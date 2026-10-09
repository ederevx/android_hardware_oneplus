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

// Host-side filter for the Bluetooth A2DP output.
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
// Dirac-QEM parity: the curve is seven peaking biquads, one per QEM band
// centre, carrying the same half-dB gains the app pushes as PARAM_EQ_BANDS
// (0x12D36). The state is read from /data/vendor/audio/dirac_qem.conf, with a
// baked preset as the initial default until the first successful read; a later
// read failure keeps the last good state rather than bypassing the effect; see
// DiracBiquadConfig. Parity covers the seven user-EQ gains and the enable flag
// only: the DAR device correction, the HDSOUND filter index and the limiter
// chain are not reproduced, so A2DP cannot sound identical to the wired route.
//
// Effect order: AudioFlinger applies the stream volume per track in
// prepareTracks_l (mMasterVolume * track port volume) and runs the output
// effect chain's process_l() afterwards, so this effect's input is already
// attenuated by the stream volume. A low-volume loudness boost therefore
// cannot clip the output, and the static headroom preamp, which answers only
// to the signature and the user EQ, stays valid. The framework never sends
// EFFECT_CMD_SET_VOLUME for this descriptor, so the attenuation arrives
// through the same state channel as the EQ, in `volume_db`.

#define LOG_TAG "dirac_biquad"

#include <errno.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>

#include <log/log.h>

#include <hardware/audio_effect.h>

#include "DiracBiquadConfig.h"
#include "DiracBiquadFilter.h"

// Effect UUID: 8f2b7c1e-4a5d-4e9b-9c3a-6d1f0b2e7a41
static const effect_uuid_t kDiracBiquadUuid = {
        0x8f2b7c1e, 0x4a5d, 0x4e9b, 0x9c3a, {0x6d, 0x1f, 0x0b, 0x2e, 0x7a, 0x41}};

enum {
    DIRAC_BIQUAD_STATE_UNINITIALIZED = 0,
    DIRAC_BIQUAD_STATE_INITIALIZED,
    DIRAC_BIQUAD_STATE_ACTIVE,
};

typedef struct dirac_biquad_object_s {
    uint32_t state;
    bool configured;
    bool enabled;

    // Updated by EFFECT_CMD_SET_DEVICE and read on every process() call.
    audio_devices_t device;

    // Dirac-QEM parity state, reloaded on configure and on every device
    // change from DiracBiquadConfig.
    int gainsHalfDb[DiracBiquadFilter::kBandCount];
    bool diracEnabled;
    // The software-fallback switch: the effect is a fallback for routes the
    // HAL/DSP Dirac topology cannot reach, so it stays bypassed unless the
    // user extends Dirac to them. Data source: DiracBiquadConfig, written only
    // by the HAL; the effect never writes ACDB/cal or any DSP state.
    bool fallback;
    // Stream attenuation in dB below the reference, from `volume_db` in the QEM
    // state file; DiracBiquadConfig::kUnknownVolumeDb means the tilt is
    // identity. Reloaded with the rest of the state.
    double volumeDb;
    // The last distinct state-file load failure, so an EACCES or a torn file is
    // logged once instead of on every reload check; 0 when the last load was
    // good. A failed reload keeps the fields above and never falls back.
    int lastLoadError;
    // Live conf reload: the state file is stat()ed at most every
    // kReloadCheckFrames frames, and re-parsed only when its mtime changes, so
    // a UI toggle applies without restarting the audio stack. No extra thread.
    time_t configMtimeSec;
    long configMtimeNsec;
    bool configMtimeValid;
    unsigned framesSinceReload;

    // Built from the stream configuration; ready only for the sample formats
    // the filter curve implements.
    DiracBiquadFilter filter;
    bool filterReady;

    effect_config_t config;
} dirac_biquad_object_t;

typedef struct dirac_biquad_module_s {
    const struct effect_interface_s *itfe;
    dirac_biquad_object_t context;
} dirac_biquad_module_t;

// Effect control interface
static int32_t DiracBiquad_Process(effect_handle_t self,
                                 audio_buffer_t *inBuffer,
                                 audio_buffer_t *outBuffer);
static int32_t DiracBiquad_Command(effect_handle_t self,
                                 uint32_t cmdCode,
                                 uint32_t cmdSize,
                                 void *pCmdData,
                                 uint32_t *replySize,
                                 void *pReplyData);
static int32_t DiracBiquad_GetDescriptor(effect_handle_t self,
                                       effect_descriptor_t *pDescriptor);

// Effect library interface
static int32_t DiracBiquadLib_Create(const effect_uuid_t *uuid,
                                   int32_t sessionId,
                                   int32_t ioId,
                                   effect_handle_t *pHandle);
static int32_t DiracBiquadLib_Release(effect_handle_t handle);
static int32_t DiracBiquadLib_GetDescriptor(const effect_uuid_t *uuid,
                                          effect_descriptor_t *pDescriptor);

static int DiracBiquad_Init(dirac_biquad_module_t *module);
static int DiracBiquad_Configure(dirac_biquad_module_t *module, const effect_config_t *config);
static void DiracBiquad_Reset(dirac_biquad_object_t *context);
static void DiracBiquad_ReloadConfig(dirac_biquad_object_t *context);

static const struct effect_interface_s gDiracBiquadInterface = {
        DiracBiquad_Process,
        DiracBiquad_Command,
        DiracBiquad_GetDescriptor,
        nullptr, // no reverse stream
};

// This is the only symbol the effects HAL loads.
__attribute__((visibility("default")))
audio_effect_library_t AUDIO_EFFECT_LIBRARY_INFO_SYM = {
        .tag = AUDIO_EFFECT_LIBRARY_TAG,
        .version = EFFECT_LIBRARY_API_VERSION,
        .name = "Dirac Biquad Filter Library",
        .implementor = "The LineageOS Project",
        .create_effect = DiracBiquadLib_Create,
        .release_effect = DiracBiquadLib_Release,
        .get_descriptor = DiracBiquadLib_GetDescriptor,
};

static const effect_descriptor_t gDiracBiquadDescriptor = {
        kDiracBiquadUuid, // type
        kDiracBiquadUuid, // uuid
        EFFECT_CONTROL_API_VERSION,
        EFFECT_FLAG_TYPE_INSERT | EFFECT_FLAG_INSERT_LAST | EFFECT_FLAG_DEVICE_IND,
        0, // cpu load
        0, // memory usage
        "Dirac Biquad Filter",
        "The LineageOS Project",
};

static const effect_descriptor_t *const gDescriptors[] = {
        &gDiracBiquadDescriptor,
};

static const size_t kNbEffects = sizeof(gDescriptors) / sizeof(gDescriptors[0]);

static size_t DiracBiquad_BytesPerSample(audio_format_t format) {
    return audio_bytes_per_sample(format);
}

// AudioFlinger does not copy the input buffer into the output buffer for an
// effect that implements process(), so an effect that does not modify the
// stream still has to produce the output itself.
static void DiracBiquad_PassThrough(const dirac_biquad_object_t *context,
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
        memcpy(outBuffer->raw, inBuffer->raw, sampleCount * DiracBiquad_BytesPerSample(format));
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

// Cheap throttle for the live conf check: about every 85 ms at 48 kHz.
static const unsigned kReloadCheckFrames = 4096;

// Re-parses the QEM state file when its mtime has moved. Called from the
// process path so a switch toggle takes effect on the next buffer without a
// device change or an audioserver restart.
static void DiracBiquad_ReloadIfChanged(dirac_biquad_object_t *context) {
    struct stat st;

    if (stat(DiracBiquadConfig::ConfigPath(), &st) != 0) {
        return;
    }
    if (context->configMtimeValid && st.st_mtim.tv_sec == context->configMtimeSec &&
        st.st_mtim.tv_nsec == context->configMtimeNsec) {
        return;
    }
    context->configMtimeSec = st.st_mtim.tv_sec;
    context->configMtimeNsec = st.st_mtim.tv_nsec;
    context->configMtimeValid = true;
    DiracBiquad_ReloadConfig(context);
}

static int32_t DiracBiquad_Process(effect_handle_t self,
                                 audio_buffer_t *inBuffer,
                                 audio_buffer_t *outBuffer) {
    dirac_biquad_module_t *module = reinterpret_cast<dirac_biquad_module_t *>(self);

    if (module == nullptr || module->context.state == DIRAC_BIQUAD_STATE_UNINITIALIZED) {
        return -EINVAL;
    }
    if (inBuffer == nullptr || inBuffer->raw == nullptr || outBuffer == nullptr ||
        outBuffer->raw == nullptr || inBuffer->frameCount != outBuffer->frameCount) {
        return -EINVAL;
    }

    dirac_biquad_object_t *context = &module->context;

    // Re-read the QEM state file, throttled, before the gate: a switch toggle
    // mid-stream must flip the fallback without a restart.
    context->framesSinceReload += inBuffer->frameCount;
    if (context->framesSinceReload >= kReloadCheckFrames) {
        context->framesSinceReload = 0;
        DiracBiquad_ReloadIfChanged(context);
    }

    // The device is read on every call rather than only when the command
    // arrives, so a device switch mid-stream takes effect on the next buffer.
    if (!context->enabled || !context->diracEnabled || !context->fallback ||
        !audio_is_a2dp_out_device(context->device) || !context->filterReady) {
        DiracBiquad_PassThrough(context, inBuffer, outBuffer);
        return 0;
    }

    context->filter.Process(
            inBuffer->raw, outBuffer->raw, outBuffer->frameCount,
            static_cast<unsigned>(
                    audio_channel_count_from_out_mask(context->config.inputCfg.channels)),
            static_cast<audio_format_t>(context->config.inputCfg.format),
            context->config.outputCfg.accessMode == EFFECT_BUFFER_ACCESS_ACCUMULATE);
    return 0;
}

static int32_t DiracBiquad_Command(effect_handle_t self,
                                 uint32_t cmdCode,
                                 uint32_t cmdSize,
                                 void *pCmdData,
                                 uint32_t *replySize,
                                 void *pReplyData) {
    dirac_biquad_module_t *module = reinterpret_cast<dirac_biquad_module_t *>(self);

    if (module == nullptr || module->context.state == DIRAC_BIQUAD_STATE_UNINITIALIZED) {
        return -EINVAL;
    }

    dirac_biquad_object_t *context = &module->context;

    ALOGV("%s: command %u cmdSize %u", __func__, cmdCode, cmdSize);

    switch (cmdCode) {
        case EFFECT_CMD_INIT:
            if (pReplyData == nullptr || replySize == nullptr || *replySize != sizeof(int)) {
                return -EINVAL;
            }
            *reinterpret_cast<int *>(pReplyData) = DiracBiquad_Init(module);
            break;

        case EFFECT_CMD_SET_CONFIG:
            if (pCmdData == nullptr || cmdSize != sizeof(effect_config_t) || pReplyData == nullptr ||
                replySize == nullptr || *replySize != sizeof(int)) {
                return -EINVAL;
            }
            *reinterpret_cast<int *>(pReplyData) =
                    DiracBiquad_Configure(module, reinterpret_cast<effect_config_t *>(pCmdData));
            break;

        case EFFECT_CMD_GET_CONFIG:
            if (pReplyData == nullptr || replySize == nullptr ||
                *replySize != sizeof(effect_config_t)) {
                return -EINVAL;
            }
            memcpy(pReplyData, &context->config, sizeof(effect_config_t));
            break;

        case EFFECT_CMD_RESET:
            DiracBiquad_Reset(context);
            break;

        case EFFECT_CMD_ENABLE:
        case EFFECT_CMD_DISABLE:
            if (pReplyData == nullptr || replySize == nullptr || *replySize != sizeof(int)) {
                return -EINVAL;
            }
            context->enabled = cmdCode == EFFECT_CMD_ENABLE;
            context->state = DIRAC_BIQUAD_STATE_ACTIVE;
            // Pick up a toggle made while the effect was disabled.
            DiracBiquad_ReloadConfig(context);
            *reinterpret_cast<int *>(pReplyData) = 0;
            break;

        case EFFECT_CMD_SET_DEVICE:
        case EFFECT_CMD_SET_INPUT_DEVICE:
            if (pCmdData == nullptr || cmdSize != sizeof(uint32_t)) {
                return -EINVAL;
            }
            context->device = static_cast<audio_devices_t>(*reinterpret_cast<uint32_t *>(pCmdData));
            ALOGV("%s: device %#x", __func__, context->device);
            // Re-read the QEM parity state on every device change so an A2DP
            // transition picks up the current app setting.
            DiracBiquad_ReloadConfig(context);
            break;

        case EFFECT_CMD_SET_AUDIO_MODE:
        case EFFECT_CMD_SET_VOLUME:
            break;

        case EFFECT_CMD_DUMP: {
            if (pCmdData == nullptr || cmdSize != sizeof(uint32_t)) {
                return -EINVAL;
            }
            const int fd = static_cast<int>(*reinterpret_cast<uint32_t *>(pCmdData));
            dprintf(fd, "Dirac Biquad Filter: state %u enabled %d dirac %d fallback %d device %#x a2dp %d"
                    " volume_db=%.1f gains=%d;%d;%d;%d;%d;%d;%d\n",
                    context->state, context->enabled, context->diracEnabled, context->fallback,
                    context->device, audio_is_a2dp_out_device(context->device), context->volumeDb,
                    context->gainsHalfDb[0], context->gainsHalfDb[1], context->gainsHalfDb[2],
                    context->gainsHalfDb[3], context->gainsHalfDb[4], context->gainsHalfDb[5],
                    context->gainsHalfDb[6]);
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

static int32_t DiracBiquad_GetDescriptor(effect_handle_t self,
                                       effect_descriptor_t *pDescriptor) {
    dirac_biquad_module_t *module = reinterpret_cast<dirac_biquad_module_t *>(self);
    if (module == nullptr || pDescriptor == nullptr ||
        module->context.state == DIRAC_BIQUAD_STATE_UNINITIALIZED) {
        return -EINVAL;
    }
    memcpy(pDescriptor, &gDiracBiquadDescriptor, sizeof(effect_descriptor_t));
    return 0;
}

static int32_t DiracBiquadLib_Create(const effect_uuid_t *uuid,
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

    dirac_biquad_module_t *module = new dirac_biquad_module_t{};
    module->itfe = &gDiracBiquadInterface;

    const int ret = DiracBiquad_Init(module);
    if (ret < 0) {
        delete module;
        return ret;
    }

    *pHandle = reinterpret_cast<effect_handle_t>(module);
    return 0;
}

static int32_t DiracBiquadLib_Release(effect_handle_t handle) {
    dirac_biquad_module_t *module = reinterpret_cast<dirac_biquad_module_t *>(handle);
    if (module == nullptr) {
        return -EINVAL;
    }
    module->context.state = DIRAC_BIQUAD_STATE_UNINITIALIZED;
    delete module;
    return 0;
}

static int32_t DiracBiquadLib_GetDescriptor(const effect_uuid_t *uuid,
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

static void DiracBiquad_ReloadConfig(dirac_biquad_object_t *context) {
    if (!context->configured) {
        return;
    }

    int error = 0;
    if (DiracBiquadConfig::Load(context->gainsHalfDb, &context->diracEnabled, &context->fallback,
                                &context->volumeDb, &error)) {
        context->lastLoadError = 0;
    } else if (error != context->lastLoadError) {
        // Keep the last good state: an unreadable or torn file must never
        // silently convert the effect to a full pass-through. Log each distinct
        // failure once, so an EACCES is visible without flooding logcat.
        context->lastLoadError = error;
        ALOGW("%s: state file %s unusable (%s); keeping last good state", __func__,
              DiracBiquadConfig::ConfigPath(), strerror(error));
    }

    const audio_format_t format =
            static_cast<audio_format_t>(context->config.inputCfg.format);
    const unsigned channelCount = static_cast<unsigned>(
            audio_channel_count_from_out_mask(context->config.inputCfg.channels));
    context->filterReady =
            (format == AUDIO_FORMAT_PCM_FLOAT || format == AUDIO_FORMAT_PCM_16_BIT) &&
            context->filter.Configure(context->config.inputCfg.samplingRate, channelCount,
                                       context->gainsHalfDb);
    // Forward the parsed attenuation on every reload so a volume step applies
    // without a configure or a device change.
    context->filter.SetAttenuationDb(context->volumeDb);
    ALOGV("%s: dirac %d volume_db %.1f gains %d;%d;%d;%d;%d;%d;%d", __func__, context->diracEnabled,
          context->volumeDb, context->gainsHalfDb[0], context->gainsHalfDb[1],
          context->gainsHalfDb[2], context->gainsHalfDb[3], context->gainsHalfDb[4],
          context->gainsHalfDb[5], context->gainsHalfDb[6]);
}

static int DiracBiquad_Init(dirac_biquad_module_t *module) {
    module->context.state = DIRAC_BIQUAD_STATE_INITIALIZED;
    module->context.configured = false;
    module->context.enabled = false;
    module->context.fallback = false;
    module->context.device = AUDIO_DEVICE_NONE;
    module->context.lastLoadError = 0;
    DiracBiquadConfig::Fallback(module->context.gainsHalfDb, &module->context.diracEnabled,
                              &module->context.fallback, &module->context.volumeDb);

    DiracBiquad_Reset(&module->context);
    return 0;
}

static int DiracBiquad_Configure(dirac_biquad_module_t *module, const effect_config_t *config) {
    dirac_biquad_object_t *context = &module->context;

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
    context->state = DIRAC_BIQUAD_STATE_INITIALIZED;

    // Reload the QEM parity state on every configure.
    DiracBiquad_ReloadConfig(context);

    return 0;
}

static void DiracBiquad_Reset(dirac_biquad_object_t *context) {
    memset(&context->config, 0, sizeof(effect_config_t));
    context->configured = false;
    context->enabled = false;
    context->filterReady = false;
    context->filter.Reset();
}
