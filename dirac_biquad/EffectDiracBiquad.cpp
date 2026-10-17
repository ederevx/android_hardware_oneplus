/*
 * Copyright (C) 2026 The LineageOS Project
 * Copyright (C) 2026 Edrick Sinsuan <evcsinsuan@gmail.com>
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

// Host-side filter for the outputs the Dirac DSP topology cannot reach.
//
// Dirac on this platform is an ADSP module registered for the headset and the
// speaker only (capi_v2_dirac_eheadset / capi_v2_dirac_ipowersound). Every other
// output does not enter the ADM at all, so the DSP never sees it; this effect
// voices those on the host instead and gates itself to them, so the speaker and
// wired outputs keep the ADSP processing and are never processed twice.
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
// only: the fixed filter bank and the rumble high-passes are not reproduced,
// and the two limiter stages at the end of the chain are modelled rather than
// the module's own dynamics, so a host output cannot sound identical to the
// wired route.
//
// The modelled level stage reaches the signal through the bend plan, and a plan
// that cannot hold the chain's magnitude limit within its section budget is
// applied anyway, as the best that budget can do: the peak stage still bounds the
// sample peak, the watcher logs the shortfall and the dump reports it. Where the
// designed response violates nothing the plan names no section and the level law
// applies no attenuation at all, which is a deliberate departure from the
// broadband level stage this chain ran before the plan existed.
//
// Effect order: AudioFlinger applies the stream volume per track in
// prepareTracks_l (mMasterVolume * track port volume) and runs the output
// effect chain's process_l() afterwards, so this effect's input is already
// attenuated by the stream volume. The limiter stages then bound the output.
// The framework never sends EFFECT_CMD_SET_VOLUME for this descriptor;
// the QEM state file's `volume_db` is still parsed for contract compatibility
// but no longer drives any processing.
//
// This file is the C ABI alone: the effect's semantics live in
// DiracBiquadEffect, and the checks here only validate command sizes and
// pointers, write replies, and forward.

#define LOG_TAG "dirac_biquad"

#include <errno.h>
#include <new>
#include <stdint.h>
#include <string.h>

#include <type_traits>

#include <log/log.h>

#include <hardware/audio_effect.h>

#include "DiracBiquadEffect.h"

// Effect UUID: 8f2b7c1e-4a5d-4e9b-9c3a-6d1f0b2e7a41
static const effect_uuid_t kDiracBiquadUuid = {
        0x8f2b7c1e, 0x4a5d, 0x4e9b, 0x9c3a, {0x6d, 0x1f, 0x0b, 0x2e, 0x7a, 0x41}};

struct dirac_biquad_module_t {
    // First word on purpose: the effects HAL dereferences *handle for the
    // interface table.
    const struct effect_interface_s *itfe;
    DiracBiquadEffect effect;
};

// A vptr would sit in front of `itfe` and break every handle.
static_assert(!std::is_polymorphic<DiracBiquadEffect>::value,
              "the handle's first word is itfe");

static int32_t DiracBiquad_Process(effect_handle_t self, audio_buffer_t *inBuffer,
                                   audio_buffer_t *outBuffer);
static int32_t DiracBiquad_Command(effect_handle_t self, uint32_t cmdCode, uint32_t cmdSize,
                                   void *pCmdData, uint32_t *replySize, void *pReplyData);
static int32_t DiracBiquad_GetDescriptor(effect_handle_t self, effect_descriptor_t *pDescriptor);

static int32_t DiracBiquadLib_Create(const effect_uuid_t *uuid, int32_t sessionId, int32_t ioId,
                                     effect_handle_t *pHandle);
static int32_t DiracBiquadLib_Release(effect_handle_t handle);
static int32_t DiracBiquadLib_GetDescriptor(const effect_uuid_t *uuid,
                                            effect_descriptor_t *pDescriptor);

static const struct effect_interface_s gDiracBiquadInterface = {
        DiracBiquad_Process,
        DiracBiquad_Command,
        DiracBiquad_GetDescriptor,
        nullptr,  // no reverse stream
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
        kDiracBiquadUuid,  // type
        kDiracBiquadUuid,  // uuid
        EFFECT_CONTROL_API_VERSION,
        EFFECT_FLAG_TYPE_INSERT | EFFECT_FLAG_INSERT_LAST | EFFECT_FLAG_DEVICE_IND,
        0,  // cpu load
        0,  // memory usage
        "Dirac Biquad Filter",
        "The LineageOS Project",
};

static const effect_descriptor_t *const gDescriptors[] = {
        &gDiracBiquadDescriptor,
};

static const size_t kNbEffects = sizeof(gDescriptors) / sizeof(gDescriptors[0]);

static int32_t DiracBiquad_Process(effect_handle_t self, audio_buffer_t *inBuffer,
                                   audio_buffer_t *outBuffer) {
    dirac_biquad_module_t *module = reinterpret_cast<dirac_biquad_module_t *>(self);
    if (module == nullptr || !module->effect.Initialized()) {
        return -EINVAL;
    }
    if (inBuffer == nullptr || inBuffer->raw == nullptr || outBuffer == nullptr ||
        outBuffer->raw == nullptr || inBuffer->frameCount != outBuffer->frameCount) {
        return -EINVAL;
    }
    return module->effect.Process(inBuffer, outBuffer);
}

static int32_t DiracBiquad_Command(effect_handle_t self, uint32_t cmdCode, uint32_t cmdSize,
                                   void *pCmdData, uint32_t *replySize, void *pReplyData) {
    dirac_biquad_module_t *module = reinterpret_cast<dirac_biquad_module_t *>(self);
    if (module == nullptr || !module->effect.Initialized()) {
        return -EINVAL;
    }

    ALOGV("%s: command %u cmdSize %u", __func__, cmdCode, cmdSize);

    switch (cmdCode) {
        case EFFECT_CMD_INIT:
            if (pReplyData == nullptr || replySize == nullptr || *replySize != sizeof(int)) {
                return -EINVAL;
            }
            *reinterpret_cast<int *>(pReplyData) = module->effect.Init();
            break;

        case EFFECT_CMD_SET_CONFIG:
            if (pCmdData == nullptr || cmdSize != sizeof(effect_config_t) ||
                pReplyData == nullptr || replySize == nullptr ||
                *replySize != sizeof(int)) {
                return -EINVAL;
            }
            *reinterpret_cast<int *>(pReplyData) =
                    module->effect.Configure(*reinterpret_cast<effect_config_t *>(pCmdData));
            break;

        case EFFECT_CMD_GET_CONFIG:
            if (pReplyData == nullptr || replySize == nullptr ||
                *replySize != sizeof(effect_config_t)) {
                return -EINVAL;
            }
            memcpy(pReplyData, &module->effect.Config(), sizeof(effect_config_t));
            break;

        case EFFECT_CMD_RESET:
            module->effect.Reset();
            break;

        case EFFECT_CMD_ENABLE:
        case EFFECT_CMD_DISABLE:
            if (pReplyData == nullptr || replySize == nullptr || *replySize != sizeof(int)) {
                return -EINVAL;
            }
            module->effect.SetEnabled(cmdCode == EFFECT_CMD_ENABLE);
            *reinterpret_cast<int *>(pReplyData) = 0;
            break;

        case EFFECT_CMD_SET_DEVICE:
            if (pCmdData == nullptr || cmdSize != sizeof(uint32_t)) {
                return -EINVAL;
            }
            module->effect.SetDevice(
                    static_cast<audio_devices_t>(*reinterpret_cast<uint32_t *>(pCmdData)));
            break;

        case EFFECT_CMD_SET_INPUT_DEVICE:
            // The input device carries AUDIO_DEVICE_BIT_IN, and folding it into
            // the device this effect gates on would make an input look like an
            // output. The host chain gates on the output route alone, so this is
            // accepted and ignored.
            break;

        case EFFECT_CMD_DUMP: {
            if (pCmdData == nullptr || cmdSize != sizeof(uint32_t)) {
                return -EINVAL;
            }
            module->effect.Dump(static_cast<int>(*reinterpret_cast<uint32_t *>(pCmdData)));
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
            if (pCmdData == nullptr || cmdSize != sizeof(effect_param_t) ||
                pReplyData == nullptr || replySize == nullptr ||
                *replySize != sizeof(int)) {
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
    if (module == nullptr || pDescriptor == nullptr || !module->effect.Initialized()) {
        return -EINVAL;
    }
    memcpy(pDescriptor, &gDiracBiquadDescriptor, sizeof(effect_descriptor_t));
    return 0;
}

static int32_t DiracBiquadLib_Create(const effect_uuid_t *uuid, int32_t /* sessionId */,
                                     int32_t /* ioId */, effect_handle_t *pHandle) {
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

    // The module is built without exceptions, so a failed operator new would
    // reach std::terminate and take the effects HAL down with it; the module is
    // the only allocation, and a create that cannot have it reports so.
    dirac_biquad_module_t *module = new (std::nothrow) dirac_biquad_module_t{};
    if (module == nullptr) {
        return -ENOMEM;
    }

    module->itfe = &gDiracBiquadInterface;

    const int ret = module->effect.Init();
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
    module->effect.Shutdown();
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
