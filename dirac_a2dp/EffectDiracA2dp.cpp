/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Host-side voicing for the Bluetooth A2DP output.
//
// Dirac on this platform is an ADSP module registered for the headset and the
// speaker only (capi_v2_dirac_eheadset / capi_v2_dirac_ipowersound), and the
// A2DP software path does not enter the ADM at all, so the DSP never sees
// Bluetooth audio. This effect voices the A2DP output on the host instead and
// gates itself to the A2DP devices, so the speaker and wired outputs keep the
// ADSP processing and are never processed twice.
//
// The gate is driven by EFFECT_CMD_SET_DEVICE, which the framework sends
// because the descriptor carries EFFECT_FLAG_DEVICE_IND.
//
// The voicing curve is a short minimum-phase biquad chain (DiracA2dpVoicing)
// synthesised from the published Dirac HD Sound behaviour: magnitude and
// impulse response correction toward a flat curve, with deeper controlled
// bass, a clearer midrange, and a wider soundstage. No Dirac-licensed
// coefficient data is used.

#define LOG_TAG "dirac_a2dp"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <log/log.h>

#include <hardware/audio_effect.h>

#include "DiracA2dpVoicing.h"

// Effect UUID: 8f2b7c1e-4a5d-4e9b-9c3a-6d1f0b2e7a41
static const effect_uuid_t kDiracA2dpUuid = {
        0x8f2b7c1e, 0x4a5d, 0x4e9b, 0x9c3a, {0x6d, 0x1f, 0x0b, 0x2e, 0x7a, 0x41}};

enum {
    DIRAC_A2DP_STATE_UNINITIALIZED = 0,
    DIRAC_A2DP_STATE_INITIALIZED,
    DIRAC_A2DP_STATE_ACTIVE,
};

typedef struct dirac_a2dp_object_s {
    uint32_t state;
    bool configured;
    bool enabled;

    // Updated by EFFECT_CMD_SET_DEVICE and read on every process() call.
    audio_devices_t device;

    // Built from the stream configuration; ready only for the sample formats
    // the voicing curve implements.
    DiracA2dpVoicing voicing;
    bool voicingReady;

    effect_config_t config;
} dirac_a2dp_object_t;

typedef struct dirac_a2dp_module_s {
    const struct effect_interface_s *itfe;
    dirac_a2dp_object_t context;
} dirac_a2dp_module_t;

// Effect control interface
static int32_t DiracA2dp_Process(effect_handle_t self,
                                 audio_buffer_t *inBuffer,
                                 audio_buffer_t *outBuffer);
static int32_t DiracA2dp_Command(effect_handle_t self,
                                 uint32_t cmdCode,
                                 uint32_t cmdSize,
                                 void *pCmdData,
                                 uint32_t *replySize,
                                 void *pReplyData);
static int32_t DiracA2dp_GetDescriptor(effect_handle_t self,
                                       effect_descriptor_t *pDescriptor);

// Effect library interface
static int32_t DiracA2dpLib_Create(const effect_uuid_t *uuid,
                                   int32_t sessionId,
                                   int32_t ioId,
                                   effect_handle_t *pHandle);
static int32_t DiracA2dpLib_Release(effect_handle_t handle);
static int32_t DiracA2dpLib_GetDescriptor(const effect_uuid_t *uuid,
                                          effect_descriptor_t *pDescriptor);

static int DiracA2dp_Init(dirac_a2dp_module_t *module);
static int DiracA2dp_Configure(dirac_a2dp_module_t *module, const effect_config_t *config);
static void DiracA2dp_Reset(dirac_a2dp_object_t *context);

static const struct effect_interface_s gDiracA2dpInterface = {
        DiracA2dp_Process,
        DiracA2dp_Command,
        DiracA2dp_GetDescriptor,
        nullptr, // no reverse stream
};

// This is the only symbol the effects HAL loads.
__attribute__((visibility("default")))
audio_effect_library_t AUDIO_EFFECT_LIBRARY_INFO_SYM = {
        .tag = AUDIO_EFFECT_LIBRARY_TAG,
        .version = EFFECT_LIBRARY_API_VERSION,
        .name = "Dirac A2DP Voicing Library",
        .implementor = "The LineageOS Project",
        .create_effect = DiracA2dpLib_Create,
        .release_effect = DiracA2dpLib_Release,
        .get_descriptor = DiracA2dpLib_GetDescriptor,
};

static const effect_descriptor_t gDiracA2dpDescriptor = {
        kDiracA2dpUuid, // type
        kDiracA2dpUuid, // uuid
        EFFECT_CONTROL_API_VERSION,
        EFFECT_FLAG_TYPE_INSERT | EFFECT_FLAG_INSERT_LAST | EFFECT_FLAG_DEVICE_IND,
        0, // cpu load
        0, // memory usage
        "Dirac A2DP Voicing",
        "The LineageOS Project",
};

static const effect_descriptor_t *const gDescriptors[] = {
        &gDiracA2dpDescriptor,
};

static const size_t kNbEffects = sizeof(gDescriptors) / sizeof(gDescriptors[0]);

static size_t DiracA2dp_BytesPerSample(audio_format_t format) {
    return audio_bytes_per_sample(format);
}

// AudioFlinger does not copy the input buffer into the output buffer for an
// effect that implements process(), so an effect that does not modify the
// stream still has to produce the output itself.
static void DiracA2dp_PassThrough(const dirac_a2dp_object_t *context,
                                  audio_buffer_t *inBuffer,
                                  audio_buffer_t *outBuffer) {
    if (inBuffer->raw == outBuffer->raw) {
        return;
    }

    const size_t sampleCount = outBuffer->frameCount *
            audio_channel_count_from_out_mask(context->config.inputCfg.channels);
    const audio_format_t format =
            static_cast<audio_format_t>(context->config.inputCfg.format);

    if (context->config.outputCfg.accessMode != EFFECT_BUFFER_ACCESS_ACCUMULATE) {
        memcpy(outBuffer->raw, inBuffer->raw, sampleCount * DiracA2dp_BytesPerSample(format));
        return;
    }

    switch (format) {
        case AUDIO_FORMAT_PCM_FLOAT:
            for (size_t i = 0; i < sampleCount; i++) {
                outBuffer->f32[i] += inBuffer->f32[i];
            }
            break;
        case AUDIO_FORMAT_PCM_16_BIT:
            for (size_t i = 0; i < sampleCount; i++) {
                outBuffer->s16[i] = (int16_t)(outBuffer->s16[i] + inBuffer->s16[i]);
            }
            break;
        default:
            // Accumulation for an unsupported format would corrupt the stream,
            // so leave the output untouched rather than guess.
            ALOGW("%s: cannot accumulate format %#x", __func__, format);
            break;
    }
}

static int32_t DiracA2dp_Process(effect_handle_t self,
                                 audio_buffer_t *inBuffer,
                                 audio_buffer_t *outBuffer) {
    dirac_a2dp_module_t *module = reinterpret_cast<dirac_a2dp_module_t *>(self);

    if (module == nullptr || module->context.state == DIRAC_A2DP_STATE_UNINITIALIZED) {
        return -EINVAL;
    }
    if (inBuffer == nullptr || inBuffer->raw == nullptr || outBuffer == nullptr ||
        outBuffer->raw == nullptr || inBuffer->frameCount != outBuffer->frameCount) {
        return -EINVAL;
    }

    dirac_a2dp_object_t *context = &module->context;

    // The device is read on every call rather than only when the command
    // arrives, so a device switch mid-stream takes effect on the next buffer.
    if (!context->enabled || !audio_is_a2dp_out_device(context->device) ||
        !context->voicingReady) {
        DiracA2dp_PassThrough(context, inBuffer, outBuffer);
        return 0;
    }

    context->voicing.Process(
            inBuffer->raw, outBuffer->raw, outBuffer->frameCount,
            static_cast<unsigned>(
                    audio_channel_count_from_out_mask(context->config.inputCfg.channels)),
            static_cast<audio_format_t>(context->config.inputCfg.format),
            context->config.outputCfg.accessMode == EFFECT_BUFFER_ACCESS_ACCUMULATE);
    return 0;
}

static int32_t DiracA2dp_Command(effect_handle_t self,
                                 uint32_t cmdCode,
                                 uint32_t cmdSize,
                                 void *pCmdData,
                                 uint32_t *replySize,
                                 void *pReplyData) {
    dirac_a2dp_module_t *module = reinterpret_cast<dirac_a2dp_module_t *>(self);

    if (module == nullptr || module->context.state == DIRAC_A2DP_STATE_UNINITIALIZED) {
        return -EINVAL;
    }

    dirac_a2dp_object_t *context = &module->context;

    ALOGV("%s: command %u cmdSize %u", __func__, cmdCode, cmdSize);

    switch (cmdCode) {
        case EFFECT_CMD_INIT:
            if (pReplyData == nullptr || replySize == nullptr || *replySize != sizeof(int)) {
                return -EINVAL;
            }
            *reinterpret_cast<int *>(pReplyData) = DiracA2dp_Init(module);
            break;

        case EFFECT_CMD_SET_CONFIG:
            if (pCmdData == nullptr || cmdSize != sizeof(effect_config_t) || pReplyData == nullptr ||
                replySize == nullptr || *replySize != sizeof(int)) {
                return -EINVAL;
            }
            *reinterpret_cast<int *>(pReplyData) =
                    DiracA2dp_Configure(module, reinterpret_cast<effect_config_t *>(pCmdData));
            break;

        case EFFECT_CMD_GET_CONFIG:
            if (pReplyData == nullptr || replySize == nullptr ||
                *replySize != sizeof(effect_config_t)) {
                return -EINVAL;
            }
            memcpy(pReplyData, &context->config, sizeof(effect_config_t));
            break;

        case EFFECT_CMD_RESET:
            DiracA2dp_Reset(context);
            break;

        case EFFECT_CMD_ENABLE:
        case EFFECT_CMD_DISABLE:
            if (pReplyData == nullptr || replySize == nullptr || *replySize != sizeof(int)) {
                return -EINVAL;
            }
            context->enabled = cmdCode == EFFECT_CMD_ENABLE;
            context->state = DIRAC_A2DP_STATE_ACTIVE;
            *reinterpret_cast<int *>(pReplyData) = 0;
            break;

        case EFFECT_CMD_SET_DEVICE:
        case EFFECT_CMD_SET_INPUT_DEVICE:
            if (pCmdData == nullptr || cmdSize != sizeof(uint32_t)) {
                return -EINVAL;
            }
            context->device = static_cast<audio_devices_t>(*reinterpret_cast<uint32_t *>(pCmdData));
            ALOGV("%s: device %#x", __func__, context->device);
            break;

        case EFFECT_CMD_SET_AUDIO_MODE:
        case EFFECT_CMD_SET_VOLUME:
            break;

        case EFFECT_CMD_DUMP: {
            if (pCmdData == nullptr || cmdSize != sizeof(uint32_t)) {
                return -EINVAL;
            }
            const int fd = static_cast<int>(*reinterpret_cast<uint32_t *>(pCmdData));
            dprintf(fd, "Dirac A2DP Voicing: state %u enabled %d device %#x a2dp %d\n",
                    context->state, context->enabled, context->device,
                    audio_is_a2dp_out_device(context->device));
            break;
        }

        case EFFECT_CMD_GET_PARAM: {
            if (pCmdData == nullptr || replySize == nullptr || pReplyData == nullptr ||
                cmdSize != sizeof(effect_param_t) || *replySize < sizeof(effect_param_t)) {
                return -EINVAL;
            }
            effect_param_t *param = reinterpret_cast<effect_param_t *>(pCmdData);
            memcpy(pReplyData, pCmdData, sizeof(effect_param_t) + param->psize);
            param = reinterpret_cast<effect_param_t *>(pReplyData);
            param->status = -EINVAL;
            param->vsize = sizeof(int);
            *replySize = sizeof(effect_param_t) + sizeof(int);
            break;
        }

        case EFFECT_CMD_SET_PARAM:
            if (pCmdData == nullptr || cmdSize != sizeof(effect_param_t) || pReplyData == nullptr ||
                replySize == nullptr || *replySize != sizeof(int)) {
                return -EINVAL;
            }
            *reinterpret_cast<int *>(pReplyData) = -EINVAL;
            break;

        default:
            ALOGW("%s: unhandled command %u", __func__, cmdCode);
            return -EINVAL;
    }

    return 0;
}

static int32_t DiracA2dp_GetDescriptor(effect_handle_t self,
                                       effect_descriptor_t *pDescriptor) {
    dirac_a2dp_module_t *module = reinterpret_cast<dirac_a2dp_module_t *>(self);
    if (module == nullptr || pDescriptor == nullptr ||
        module->context.state == DIRAC_A2DP_STATE_UNINITIALIZED) {
        return -EINVAL;
    }
    memcpy(pDescriptor, &gDiracA2dpDescriptor, sizeof(effect_descriptor_t));
    return 0;
}

static int32_t DiracA2dpLib_Create(const effect_uuid_t *uuid,
                                   int32_t /* sessionId */,
                                   int32_t /* ioId */,
                                   effect_handle_t *pHandle) {
    if (pHandle == nullptr || uuid == nullptr) {
        return -EINVAL;
    }

    size_t i;
    for (i = 0; i < kNbEffects; i++) {
        if (memcmp(uuid, &gDescriptors[i]->uuid, sizeof(effect_uuid_t)) == 0) {
            break;
        }
    }
    if (i == kNbEffects) {
        return -ENOENT;
    }

    dirac_a2dp_module_t *module = new dirac_a2dp_module_t{};
    module->itfe = &gDiracA2dpInterface;

    const int ret = DiracA2dp_Init(module);
    if (ret < 0) {
        delete module;
        return ret;
    }

    *pHandle = reinterpret_cast<effect_handle_t>(module);
    return 0;
}

static int32_t DiracA2dpLib_Release(effect_handle_t handle) {
    dirac_a2dp_module_t *module = reinterpret_cast<dirac_a2dp_module_t *>(handle);
    if (module == nullptr) {
        return -EINVAL;
    }
    module->context.state = DIRAC_A2DP_STATE_UNINITIALIZED;
    delete module;
    return 0;
}

static int32_t DiracA2dpLib_GetDescriptor(const effect_uuid_t *uuid,
                                          effect_descriptor_t *pDescriptor) {
    if (uuid == nullptr || pDescriptor == nullptr) {
        return -EINVAL;
    }
    for (size_t i = 0; i < kNbEffects; i++) {
        if (memcmp(uuid, &gDescriptors[i]->uuid, sizeof(effect_uuid_t)) == 0) {
            memcpy(pDescriptor, gDescriptors[i], sizeof(effect_descriptor_t));
            return 0;
        }
    }
    return -EINVAL;
}

static int DiracA2dp_Init(dirac_a2dp_module_t *module) {
    module->context.state = DIRAC_A2DP_STATE_INITIALIZED;
    module->context.configured = false;
    module->context.enabled = false;
    module->context.device = AUDIO_DEVICE_NONE;

    DiracA2dp_Reset(&module->context);
    return 0;
}

static int DiracA2dp_Configure(dirac_a2dp_module_t *module, const effect_config_t *config) {
    dirac_a2dp_object_t *context = &module->context;

    if (config->inputCfg.samplingRate != config->outputCfg.samplingRate ||
        config->inputCfg.channels != config->outputCfg.channels ||
        config->inputCfg.format != config->outputCfg.format) {
        return -EINVAL;
    }
    if (!audio_is_linear_pcm(static_cast<audio_format_t>(config->inputCfg.format))) {
        return -EINVAL;
    }

    memcpy(&context->config, config, sizeof(effect_config_t));
    context->configured = true;
    context->state = DIRAC_A2DP_STATE_INITIALIZED;

    const audio_format_t format = static_cast<audio_format_t>(config->inputCfg.format);
    const unsigned channelCount =
            static_cast<unsigned>(audio_channel_count_from_out_mask(config->inputCfg.channels));
    context->voicingReady =
            (format == AUDIO_FORMAT_PCM_FLOAT || format == AUDIO_FORMAT_PCM_16_BIT) &&
            context->voicing.Configure(config->inputCfg.samplingRate, channelCount);

    return 0;
}

static void DiracA2dp_Reset(dirac_a2dp_object_t *context) {
    memset(&context->config, 0, sizeof(effect_config_t));
    context->configured = false;
    context->enabled = false;
    context->voicingReady = false;
    context->voicing.Reset();
}
