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

#include "DiracBiquadEffect.h"

#include <errno.h>
#include <string.h>

#include <cmath>
#include <cstdio>

#include <log/log.h>

#include "DiracBiquadConfig.h"
#include "DiracRoute.h"
#include "DiracSampleFormat.h"

namespace {

double ToDb(float gain) {
    return gain > 0.0f ? 20.0 * std::log10(static_cast<double>(gain)) : -120.0;
}

}  // namespace

int DiracBiquadEffect::Init() {
    state_ = kInitialized;
    configured_ = false;
    enabled_ = false;
    fallback_ = false;
    device_ = AUDIO_DEVICE_NONE;
    appliedGeneration_ = 0;
    chainRunning_ = false;
    planRefusals_ = 0;
    buffersBeforeEngage_ = 0;
    DiracBiquadConfig::Fallback(gainsHalfDb_, &diracEnabled_, &fallback_, &volumeDb_);
    // The effect is created per music session, so the watcher's constructor seeds
    // its state from the last real read of the file (or the baked preset). Take
    // that before Start, so the first SET_CONFIG builds the chain with the state
    // the user already chose rather than the baked preset.
    {
        DiracBiquadConfigSnapshot snapshot;
        if (configWatcher_.Read(&snapshot)) {
            memcpy(gainsHalfDb_, snapshot.gainsHalfDb, sizeof(gainsHalfDb_));
            diracEnabled_ = snapshot.diracEnabled;
            fallback_ = snapshot.fallback;
            volumeDb_ = snapshot.volumeDb;
        }
    }
    configWatcher_.Start();

    // INIT is a full (re)initialisation, so the configuration is cleared here;
    // EFFECT_CMD_RESET must not clear it (see Reset).
    memset(&config_, 0, sizeof(config_));

    Reset();
    return 0;
}

int DiracBiquadEffect::Configure(const effect_config_t &config) {
    if (config.inputCfg.samplingRate != config.outputCfg.samplingRate ||
        config.inputCfg.channels != config.outputCfg.channels ||
        config.inputCfg.format != config.outputCfg.format) {
        return -EINVAL;
    }
    const audio_format_t format = static_cast<audio_format_t>(config.inputCfg.format);
    if (format != AUDIO_FORMAT_PCM_FLOAT && format != AUDIO_FORMAT_PCM_16_BIT) {
        // Only the two formats the chain implements are accepted. AudioFlinger
        // sends float first and retries 16-bit on a refusal, so the pass-through
        // never sees a format it would have to warn about on the audio path.
        return -EINVAL;
    }

    // AudioFlinger re-sends an unchanged configuration to a live effect: twice
    // when the effect is added and again when a neighbour leaves the chain. Only
    // the buffers and the access mode can differ then, and both are read per
    // call, so the chain itself is left alone.
    const bool sameStream = filter_.Configured() &&
            config_.inputCfg.samplingRate == config.inputCfg.samplingRate &&
            config_.inputCfg.channels == config.inputCfg.channels &&
            config_.inputCfg.format == config.inputCfg.format;

    config_ = config;
    configured_ = true;
    state_ = kInitialized;

    if (sameStream) {
        return 0;
    }

    // Re-read the QEM parity state on every configure. The rate also sets the
    // watcher's poll window, and the request is serviced within it.
    configWatcher_.SetSampleRate(config.inputCfg.samplingRate);
    configWatcher_.RequestReload();
    ApplySnapshot(/*force=*/true, /*fromSetConfig=*/true);

    return 0;
}

void DiracBiquadEffect::Reset() {
    // AOSP EFFECT_CMD_RESET contract (system/media/audio/include/system/
    // audio_effect.h): "Reset the effect engine. Keep configuration but resets
    // state and buffer content." AudioFlinger's disable/enable path sends RESET
    // and then only ENABLE; it re-sends SET_CONFIG solely on addEffect and
    // removeEffect. Dropping the configuration here therefore left the effect a
    // pass-through for the rest of the stream's life. Only the running filter
    // state is cleared: the biquad memories and the limiter's look-ahead ring.
    //
    // A RESET on a running chain arrives on RESTART and on STOPPED transitions,
    // where the gate handling is already fading the chain to the dry input;
    // flushing its look-ahead ring here would cut that fade short. A stopped
    // chain is reset on its next engagement anyway, and Init resets because
    // chainRunning_ is false there.
    if (!chainRunning_) {
        filter_.Reset();
    }
}

int32_t DiracBiquadEffect::Process(const audio_buffer_t *inBuffer, audio_buffer_t *outBuffer) {
    // Lock-free: the audio deadline takes no filesystem call, and only
    // SET_CONFIG builds a stream configuration.
    ApplySnapshot(/*force=*/false, /*fromSetConfig=*/false);

    // The device is read on every call rather than only when the command
    // arrives, so a device switch mid-stream takes effect on the next buffer.
    if (!filter_.Configured()) {
        chainRunning_ = false;
        ++buffersBeforeEngage_;
        PassThrough(inBuffer, outBuffer);
        return enabled_ ? 0 : -ENODATA;
    }
    const bool gateOpen =
            enabled_ && diracEnabled_ && fallback_ && !dirac_route::IsDspOutput(device_);
    if (!gateOpen && !chainRunning_) {
        ++buffersBeforeEngage_;
        PassThrough(inBuffer, outBuffer);
        // A disabled effect that has handed back to the dry stream ends
        // AudioFlinger's STOPPED wait, whose own bypass is the same dry stream.
        return enabled_ ? 0 : -ENODATA;
    }

    // Until now the stream went out unprocessed, so the chain's look-ahead ring
    // and filter state are cold or stale from an earlier engagement. Reset drops
    // that state and re-arms the engage crossfade; it allocates nothing. A chain
    // already running is not reset: the gate closing fades it back to the dry
    // input instead of cutting, and re-opening turns the fade around.
    if (!chainRunning_) {
        filter_.Reset();
        chainRunning_ = true;
        buffersBeforeEngage_ = 0;
    } else if (gateOpen) {
        filter_.Engage();
    } else {
        filter_.Disengage();
    }

    filter_.Process(inBuffer->raw, outBuffer->raw, outBuffer->frameCount,
                    audio_channel_count_from_out_mask(config_.inputCfg.channels),
                    static_cast<audio_format_t>(config_.inputCfg.format),
                    config_.outputCfg.accessMode == EFFECT_BUFFER_ACCESS_ACCUMULATE);
    if (!gateOpen && filter_.Disengaged()) {
        chainRunning_ = false;
    }
    return 0;
}

void DiracBiquadEffect::SetEnabled(bool enabled) {
    enabled_ = enabled;
    state_ = kActive;
    // This handler runs on the mixer thread, so it applies the flags only.
    configWatcher_.RequestReload();
    ApplySnapshot(/*force=*/true, /*fromSetConfig=*/false);
}

void DiracBiquadEffect::SetDevice(audio_devices_t device) {
    device_ = device;
    ALOGV("%s: device %#x", __func__, device_);
    // Re-read the QEM parity state so a host transition picks up the app
    // setting; this runs on the mixer thread, so it builds no stream.
    configWatcher_.RequestReload();
    ApplySnapshot(/*force=*/true, /*fromSetConfig=*/false);
}

void DiracBiquadEffect::Shutdown() {
    state_ = kUninitialized;
    configWatcher_.Stop();
}

void DiracBiquadEffect::PassThrough(const audio_buffer_t *inBuffer,
                                    audio_buffer_t *outBuffer) const {
    if (inBuffer->raw == outBuffer->raw) {
        return;
    }
    const size_t sampleCount =
            outBuffer->frameCount *
            audio_channel_count_from_out_mask(config_.inputCfg.channels);
    const bool accumulate =
            config_.outputCfg.accessMode == EFFECT_BUFFER_ACCESS_ACCUMULATE;
    switch (static_cast<audio_format_t>(config_.inputCfg.format)) {
        case AUDIO_FORMAT_PCM_FLOAT:
            dirac_format::PassThroughFrames<dirac_format::FloatSamples>(
                    inBuffer->f32, outBuffer->f32, sampleCount, accumulate);
            break;
        case AUDIO_FORMAT_PCM_16_BIT:
            dirac_format::PassThroughFrames<dirac_format::Pcm16Samples>(
                    inBuffer->s16, outBuffer->s16, sampleCount, accumulate);
            break;
        default:
            // Configure accepts only the two formats above, so AudioFlinger never
            // reaches this and no log is written from the audio path.
            break;
    }
}

void DiracBiquadEffect::AdoptPlanOrClear(const DiracBandPlan &plan) {
    if (filter_.AdoptBendPlan(plan)) {
        return;
    }
    // Counted, not logged: this runs on the audio path, where a logd write is an
    // unbounded tail-latency event. Dump reports the count.
    ++planRefusals_;
    filter_.ClearBendPlan();
}

void DiracBiquadEffect::ApplySnapshot(bool force, bool fromSetConfig) {
    if (!configured_) {
        return;
    }
    // Nothing is marked applied here, so the gains and the plan arrive together
    // once the command path has built the stream.
    if (!filter_.Configured() && !fromSetConfig) {
        return;
    }

    // Nothing is copied while the state stands still.
    if (!force && configWatcher_.SnapshotGeneration() == appliedGeneration_) {
        return;
    }

    DiracBiquadConfigSnapshot snapshot;
    const bool haveSnapshot = configWatcher_.Read(&snapshot);
    bool gainsChanged = false;
    if (haveSnapshot) {
        if (!force && snapshot.generation == appliedGeneration_) {
            return;
        }
        // gainsChanged is against the gains still applied: when a curve change
        // cannot be adopted with its plan the applied pair is held and this stays
        // true, so the next matching publication retries.
        gainsChanged =
                memcmp(snapshot.gainsHalfDb, gainsHalfDb_, sizeof(gainsHalfDb_)) != 0;
        appliedGeneration_ = snapshot.generation;
        // The flags apply unconditionally; the gains apply only together with
        // their plan, below, so gainsHalfDb_ keeps the applied pair.
        diracEnabled_ = snapshot.diracEnabled;
        fallback_ = snapshot.fallback;
        volumeDb_ = snapshot.volumeDb;
    } else if (!force) {
        // The watcher was copying a publication at the same instant; keep the
        // state already applied and pick it up on the next call.
        return;
    }
    // A forced apply with no snapshot in flight keeps the gains already applied;
    // the watcher's requested reload lands within the poll window.

    const audio_format_t format = static_cast<audio_format_t>(config_.inputCfg.format);
    if (format != AUDIO_FORMAT_PCM_FLOAT && format != AUDIO_FORMAT_PCM_16_BIT) {
        return;
    }

    const unsigned channelCount =
            audio_channel_count_from_out_mask(config_.inputCfg.channels);
    const unsigned sampleRateHz = config_.inputCfg.samplingRate;
    const int *const targetGains = haveSnapshot ? snapshot.gainsHalfDb : gainsHalfDb_;
    // The filter's AdoptBendPlan re-checks the rate and gains; this decides
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
    // A non-identical SET_CONFIG forces the rebuild; the filter's own configured
    // state only clears when it refuses the input.
    if (fromSetConfig || !filter_.Configured()) {
        if (filter_.Configure(sampleRateHz, channelCount, targetGains)) {
            memcpy(gainsHalfDb_, targetGains, sizeof(gainsHalfDb_));
            if (planMatches) {
                filter_.AdoptBendPlan(snapshot.plan);
            }
        }
    } else if (gainsChanged && snapshot.bandsRateHz == sampleRateHz) {
        // The curve always applies; the plan is what may be refused. Holding the
        // curve back until a matching plan arrives would strand the user's
        // setting, because a plan is rebuilt only when the file's gains move
        // again, and the shortfall is the plan's to report, not the curve's. The
        // sections were designed for the snapshot's own rate, so a set built for
        // another stream is not applied to this one; the forced republish after a
        // rate change brings the matching set.
        if (filter_.UpdateGains(targetGains, snapshot.userBands)) {
            memcpy(gainsHalfDb_, targetGains, sizeof(gainsHalfDb_));
        }
        if (planMatches) {
            AdoptPlanOrClear(snapshot.plan);
        } else {
            filter_.ClearBendPlan();
        }
    } else if (!filter_.HasBendPlan() && planMatches) {
        // The chain is built but has no plan yet - the window after a configure
        // or a rate change - so adopt the matching plan without touching the
        // curve.
        AdoptPlanOrClear(snapshot.plan);
    }
    ALOGV("%s: dirac %d fallback %d volume_db %.1f gains %d;%d;%d;%d;%d;%d;%d", __func__,
          diracEnabled_, fallback_, volumeDb_, gainsHalfDb_[0], gainsHalfDb_[1],
          gainsHalfDb_[2], gainsHalfDb_[3], gainsHalfDb_[4], gainsHalfDb_[5],
          gainsHalfDb_[6]);
}

void DiracBiquadEffect::Dump(int fd) const {
    dprintf(fd, "Dirac Biquad Filter: bend plan present %d holds_limit %d"
            " residual %zu flat_db %.4f applied_flat_db %.4f engaged %zu"
            " retiring %zu\n",
            filter_.HasBendPlan(), filter_.BendPlanHoldsLimit(),
            filter_.BendResidualIntervals(), ToDb(filter_.BendFlatGain()),
            ToDb(filter_.BendAppliedFlatGain()), filter_.BendEngagedSections(),
            filter_.BendRetiringSections());
    dprintf(fd, "Dirac Biquad Filter: state %u enabled %d dirac %d fallback %d"
            " device %#x dsp %d volume_db=%.1f gains=%d;%d;%d;%d;%d;%d;%d"
            " level_stage_db %.1f/%.1f peak_stage_db %.1f/%.1f\n",
            state_, enabled_, diracEnabled_, fallback_, device_,
            dirac_route::IsDspOutput(device_), volumeDb_, gainsHalfDb_[0], gainsHalfDb_[1],
            gainsHalfDb_[2], gainsHalfDb_[3], gainsHalfDb_[4], gainsHalfDb_[5],
            gainsHalfDb_[6],
            static_cast<double>(DiracBiquadFilter::LevelStage().thresholdDb),
            static_cast<double>(DiracBiquadFilter::LevelStage().maxAttenuationDb),
            static_cast<double>(DiracBiquadFilter::PeakStage().thresholdDb),
            static_cast<double>(DiracBiquadFilter::PeakStage().maxAttenuationDb));
    dprintf(fd, "Dirac Biquad Filter: accessMode %d chainRunning %d"
            " buffersBeforeEngage %u planRefusals %u nonFiniteFlushes %u"
            " fade %s %u\n",
            static_cast<int>(config_.outputCfg.accessMode), chainRunning_,
            buffersBeforeEngage_, planRefusals_, filter_.NonFiniteFlushes(),
            filter_.FadeIn() ? "in" : "out", filter_.FadePosition());
}
