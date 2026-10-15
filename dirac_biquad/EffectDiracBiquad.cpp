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

#define LOG_TAG "dirac_biquad"

#include <errno.h>
#include <new>
#include <stdio.h>
#include <string.h>

#include <log/log.h>

#include <hardware/audio_effect.h>

#include "DiracBiquadConfig.h"
#include "DiracBiquadConfigWatcher.h"
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
    // Stream attenuation from `volume_db` in the QEM state file. The app still
    // publishes it and it is still parsed and dumped, but nothing consumes it.
    double volumeDb;
    // The snapshot generation the chain was last rebuilt from. The mixer thread
    // rebuilds only when the watcher publishes a newer one.
    uint64_t appliedGeneration;
    // False once a SET_CONFIG has replaced the stream; the next apply then
    // rebuilds the whole chain. While it holds, an apply may only rebuild the
    // user EQ, so the limiters keep their state across a live curve update.
    bool streamConfigured;
    // False while the stream is going out unprocessed: the framework's STARTING
    // bypass before the first ENABLE, and every frame this effect's gate closes.
    // The first processed frame after that resets the chain and fades it in, so a
    // re-engagement never splices the raw stream against a cold chain.
    bool chainRunning;

    // Owns the only thread that touches the QEM state file. The mixer thread
    // reads a published snapshot and takes no filesystem call.
    DiracBiquadConfigWatcher configWatcher;

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
static void DiracBiquad_ApplySnapshot(dirac_biquad_object_t *context, bool force,
                                     bool mayConfigure);

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

// The wired speaker and headset outputs are voiced by the ADSP Dirac topology
// (ACDB devices 15 and 10), so the host chain stays out of them. Everything else
// bypasses the ADM and is ours: Bluetooth A2DP, the remote submix that Android
// Auto and other projection consumers use, USB, and the rest.
//
// The value the framework hands us may be the OR of several patch sinks, and
// since Android R an audio device type is an exclusive value that must not be
// used as a mask - system/media/audio/include/system/audio.h says exactly that on
// audio_is_output_devices. The two rules cannot both be satisfied by a bit test:
// AUDIO_DEVICE_OUT_BLE_BROADCAST (0x20000002) carries the speaker's bit and
// AUDIO_DEVICE_OUT_BLE_SPEAKER (0x20000001) the earpiece's, so a Bluetooth LE
// route read as a set of sink bits is indistinguishable from a duplicated
// speaker output. So the test is membership of the DSP set by whole value, using
// the framework's own audio_is_output_device() for "is this exactly one output
// type", and anything else - a combination, or an encoding this effect does not
// know - is left to the DSP. That is the directive's rule too: a route whose
// Dirac support is unknown must not be voiced here, because voicing a route the
// ADSP already voices is the failure this gate exists to prevent.
//
// The set itself is route data this effect does not own: which ACDB device the
// ADSP Dirac modules are registered for is a HAL and calibration property. The
// HAL-published form of it is what this predicate should read once that data
// exists; until then the set is written out here. It includes the safe-speaker
// type, which is the same physical speaker reached through the volume limit.
static bool DiracBiquad_IsDspOutput(audio_devices_t device) {
    static const audio_devices_t kDspDevices[] = {
            AUDIO_DEVICE_OUT_SPEAKER,
            AUDIO_DEVICE_OUT_SPEAKER_SAFE,
            AUDIO_DEVICE_OUT_WIRED_HEADSET,
            AUDIO_DEVICE_OUT_WIRED_HEADPHONE,
            AUDIO_DEVICE_OUT_LINE,
    };
    for (const audio_devices_t candidate : kDspDevices) {
        if (device == candidate) {
            return true;
        }
    }
    // Exactly one output type, and not one of the DSP's: the ADM does not carry
    // it, so it is ours to voice.
    if (audio_is_output_device(device)) {
        return false;
    }
    return true;
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

    // Apply whatever the watcher has published. Reading the snapshot is
    // lock-free, so the audio deadline never takes a filesystem call; a switch
    // toggle still flips the fallback without a restart. The audio path never
    // builds a stream configuration: that is the command handlers' work and it
    // allocates.
    DiracBiquad_ApplySnapshot(context, /*force=*/false, /*mayConfigure=*/false);

    // The device is read on every call rather than only when the command
    // arrives, so a device switch mid-stream takes effect on the next buffer.
    if (!context->enabled || !context->diracEnabled || !context->fallback ||
        DiracBiquad_IsDspOutput(context->device) || !context->filterReady) {
        context->chainRunning = false;
        DiracBiquad_PassThrough(context, inBuffer, outBuffer);
        return 0;
    }

    // Until now the stream went out unprocessed, so the chain's look-ahead ring
    // and filter state are cold or stale from an earlier engagement. Reset drops
    // that state and re-arms the engage crossfade; it allocates nothing.
    if (!context->chainRunning) {
        context->filter.Reset();
        context->chainRunning = true;
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
            context->configWatcher.RequestReload();
            DiracBiquad_ApplySnapshot(context, /*force=*/true, /*mayConfigure=*/true);
            *reinterpret_cast<int *>(pReplyData) = 0;
            break;

        case EFFECT_CMD_SET_DEVICE:
            if (pCmdData == nullptr || cmdSize != sizeof(uint32_t)) {
                return -EINVAL;
            }
            context->device = static_cast<audio_devices_t>(*reinterpret_cast<uint32_t *>(pCmdData));
            ALOGV("%s: device %#x", __func__, context->device);
            // Re-read the QEM parity state on every device change so a host
            // transition picks up the current app setting.
            context->configWatcher.RequestReload();
            DiracBiquad_ApplySnapshot(context, /*force=*/true, /*mayConfigure=*/true);
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
            const int fd = static_cast<int>(*reinterpret_cast<uint32_t *>(pCmdData));
            dprintf(fd, "Dirac Biquad Filter: bend plan present %d holds_limit %d"
                    " residual %zu flat_db %.4f applied_flat_db %.4f engaged %zu"
                    " retiring %zu\n",
                    context->filter.HasBendPlan(), context->filter.BendPlanHoldsLimit(),
                    context->filter.BendResidualIntervals(),
                    context->filter.BendFlatGain() > 0.0f
                            ? 20.0 * std::log10(
                                      static_cast<double>(context->filter.BendFlatGain()))
                            : -120.0,
                    context->filter.BendAppliedFlatGain() > 0.0f
                            ? 20.0 * std::log10(static_cast<double>(
                                      context->filter.BendAppliedFlatGain()))
                            : -120.0,
                    context->filter.BendEngagedSections(),
                    context->filter.BendRetiringSections());
            dprintf(fd, "Dirac Biquad Filter: state %u enabled %d dirac %d fallback %d"
                    " device %#x dsp %d volume_db=%.1f gains=%d;%d;%d;%d;%d;%d;%d"
                    " level_stage_db %.1f/%.1f peak_stage_db %.1f/%.1f\n",
                    context->state, context->enabled, context->diracEnabled, context->fallback,
                    context->device, DiracBiquad_IsDspOutput(context->device),
                    context->volumeDb, context->gainsHalfDb[0], context->gainsHalfDb[1],
                    context->gainsHalfDb[2], context->gainsHalfDb[3], context->gainsHalfDb[4],
                    context->gainsHalfDb[5], context->gainsHalfDb[6],
                    static_cast<double>(DiracBiquadFilter::LevelStage().thresholdDb),
                    static_cast<double>(DiracBiquadFilter::LevelStage().maxAttenuationDb),
                    static_cast<double>(DiracBiquadFilter::PeakStage().thresholdDb),
                    static_cast<double>(DiracBiquadFilter::PeakStage().maxAttenuationDb));
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

    // The module is built without exceptions, so a failed operator new would
    // reach std::terminate and take the effects HAL down with it; the context is
    // the module's only allocation, and a create that cannot have it reports so.
    dirac_biquad_module_t *module = new (std::nothrow) dirac_biquad_module_t{};
    if (module == nullptr) {
        return -ENOMEM;
    }

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
    module->context.configWatcher.Stop();
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

// Adopts a published plan, or drops whatever plan is in force: a plan that
// cannot be adopted must not leave the shaper bending a response the curve no
// longer asks for, and a plan that cannot be adopted is dropped rather than
// allowed to hold the curve back.
static void DiracBiquad_ApplyPlan(dirac_biquad_object_t *context, const DiracBandPlan &plan) {
    if (context->filter.AdoptBendPlan(plan)) {
        return;
    }
    ALOGW("%s: refusing the bend plan for this curve; the level stage stays broadband",
          __func__);
    context->filter.ClearBendPlan();
}

static void DiracBiquad_ApplySnapshot(dirac_biquad_object_t *context, bool force,
                                     bool mayConfigure) {
    if (!context->configured) {
        return;
    }
    // A stream configuration is built only on the command path, so the audio
    // path applies what is already built and leaves an unbuilt stream to the
    // next command. Nothing is marked applied here, so the gains and the plan
    // arrive together once the build has happened.
    if (!context->streamConfigured && !mayConfigure) {
        return;
    }

    // A publication earns a copy only when it is newer than what has been
    // applied: the snapshot carries the plan, the audio path reads it on every
    // process() call, and nothing has to be copied while the state stands still.
    if (!force && context->configWatcher.SnapshotGeneration() == context->appliedGeneration) {
        return;
    }

    DiracBiquadConfigSnapshot snapshot;
    const bool haveSnapshot = context->configWatcher.Read(&snapshot);
    bool gainsChanged = false;
    if (haveSnapshot) {
        if (!force && snapshot.generation == context->appliedGeneration) {
            return;
        }
        // gainsChanged is against the gains still applied: when a curve change
        // cannot be adopted with its plan the applied pair is held and this
        // stays true, so the next matching publication retries.
        gainsChanged = memcmp(snapshot.gainsHalfDb, context->gainsHalfDb,
                              sizeof(context->gainsHalfDb)) != 0;
        context->appliedGeneration = snapshot.generation;
        // The flags apply unconditionally; the gains apply only together with
        // their plan, below, so context->gainsHalfDb keeps the applied pair.
        context->diracEnabled = snapshot.diracEnabled;
        context->fallback = snapshot.fallback;
        context->volumeDb = snapshot.volumeDb;
    } else if (!force) {
        // The watcher was copying a publication at the same instant; keep the
        // state already applied and pick it up on the next call.
        return;
    }
    // A forced apply with no snapshot in flight keeps the gains already applied;
    // the watcher's requested reload lands within the poll window.

    const audio_format_t format =
            static_cast<audio_format_t>(context->config.inputCfg.format);
    if (format != AUDIO_FORMAT_PCM_FLOAT && format != AUDIO_FORMAT_PCM_16_BIT) {
        context->filterReady = false;
        context->streamConfigured = false;
        return;
    }

    const unsigned channelCount = static_cast<unsigned>(
            audio_channel_count_from_out_mask(context->config.inputCfg.channels));
    const unsigned sampleRateHz = context->config.inputCfg.samplingRate;
    // Without a snapshot there is no new plan and the gains already applied are
    // kept; a matching plan can only come from a snapshot.
    const int *const targetGains = haveSnapshot ? snapshot.gainsHalfDb : context->gainsHalfDb;
    // A plan may be applied only with the rate and the gains it was built for.
    // The filter's AdoptBendPlan re-checks both against the rate and gains it has
    // actually applied, which is the invariant that matters; this check decides
    // whether the pair can be applied together at all.
    const bool planMatches =
            haveSnapshot && snapshot.plan.valid && snapshot.plan.sampleRateHz == sampleRateHz &&
            memcmp(snapshot.plan.gainsHalfDb, snapshot.gainsHalfDb,
                   sizeof(snapshot.gainsHalfDb)) == 0;

    // A stream change rebuilds the whole chain: coefficients, signature and both
    // limiter stages, with no plan (the shaper starts in fallback). Every other
    // apply may rebuild only the user EQ sections, which leaves the limiter's
    // ring, write index, true-peak history, bound maxima, envelope and gain in
    // place, so a live curve update does not flush the look-ahead ring and cut an
    // ~8 ms hole in the stream.
    if (!context->streamConfigured) {
        context->filterReady =
                context->filter.Configure(sampleRateHz, channelCount, targetGains);
        context->streamConfigured = context->filterReady;
        if (context->filterReady) {
            memcpy(context->gainsHalfDb, targetGains, sizeof(context->gainsHalfDb));
            if (planMatches) {
                context->filter.AdoptBendPlan(snapshot.plan);
            }
        }
    } else if (gainsChanged) {
        // The curve always applies; the plan is what may be refused. Holding the
        // curve back until a matching plan arrives would strand the user's
        // setting, because a plan is rebuilt only when the file's gains move
        // again, and the shortfall is the plan's to report, not the curve's.
        context->filterReady = context->filter.UpdateGains(targetGains);
        if (context->filterReady) {
            memcpy(context->gainsHalfDb, targetGains, sizeof(context->gainsHalfDb));
        }
        if (context->filterReady && planMatches) {
            DiracBiquad_ApplyPlan(context, snapshot.plan);
        } else if (context->filterReady) {
            context->filter.ClearBendPlan();
        }
    } else if (!context->filter.HasBendPlan() && planMatches) {
        // The chain is built but has no plan yet - the window after a configure
        // or a rate change - so adopt the matching plan without touching the
        // curve.
        DiracBiquad_ApplyPlan(context, snapshot.plan);
    }
    ALOGV("%s: dirac %d fallback %d volume_db %.1f gains %d;%d;%d;%d;%d;%d;%d", __func__,
          context->diracEnabled, context->fallback, context->volumeDb,
          context->gainsHalfDb[0], context->gainsHalfDb[1], context->gainsHalfDb[2],
          context->gainsHalfDb[3], context->gainsHalfDb[4], context->gainsHalfDb[5],
          context->gainsHalfDb[6]);
}

static int DiracBiquad_Init(dirac_biquad_module_t *module) {
    module->context.state = DIRAC_BIQUAD_STATE_INITIALIZED;
    module->context.configured = false;
    module->context.enabled = false;
    module->context.fallback = false;
    module->context.device = AUDIO_DEVICE_NONE;
    module->context.filterReady = false;
    module->context.appliedGeneration = 0;
    module->context.streamConfigured = false;
    module->context.chainRunning = false;
    DiracBiquadConfig::Fallback(module->context.gainsHalfDb, &module->context.diracEnabled,
                              &module->context.fallback, &module->context.volumeDb);
    module->context.configWatcher.Start();

    // INIT is a full (re)initialisation, so the configuration is cleared here;
    // EFFECT_CMD_RESET must not clear it (see DiracBiquad_Reset).
    memset(&module->context.config, 0, sizeof(effect_config_t));

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

    // The stream configuration changed, so the next apply must rebuild the
    // whole chain rather than just the user EQ sections.
    context->streamConfigured = false;

    // Re-read the QEM parity state on every configure. The rate also sets the
    // watcher's poll window, and the request is serviced within it.
    context->configWatcher.SetSampleRate(config->inputCfg.samplingRate);
    context->configWatcher.RequestReload();
    DiracBiquad_ApplySnapshot(context, /*force=*/true, /*mayConfigure=*/true);

    return 0;
}

static void DiracBiquad_Reset(dirac_biquad_object_t *context) {
    // AOSP EFFECT_CMD_RESET contract (system/media/audio/include/system/
    // audio_effect.h): "Reset the effect engine. Keep configuration but resets
    // state and buffer content." AudioFlinger's disable/enable path sends RESET
    // and then only ENABLE; it re-sends SET_CONFIG solely on addEffect and
    // removeEffect. Dropping the configuration here therefore left the effect a
    // pass-through for the rest of the stream's life. Only the running filter
    // state is cleared: the biquad memories and the limiter's look-ahead ring.
    context->filter.Reset();
}
