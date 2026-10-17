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

#include "DiracBiquadFilter.h"

#include <algorithm>
#include <cmath>
#include <stdint.h>
#include <string.h>

#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

#include "Biquad.h"
#include "DiracBiquadTable.h"
#include "DiracChainStages.h"
#include "DiracCurveDesign.h"
#include "DiracLimiter.h"
#include "DiracSampleFormat.h"

namespace {

// Flush-to-zero for one sample-processing call. The filter and limiter states
// decay into denormals during silence, and denormal operands carry a large
// penalty on the target cores. The FP control register is saved and restored,
// so the mode lasts exactly the call and never leaks to another thread or past
// an early return.
//
// Guarded architectures: AArch64 FPCR bit 24 (FZ), 32-bit ARM FPSCR bit 24
// (FZ), and x86/x86-64 MXCSR FTZ (bit 15) | DAZ (bit 6). Every other
// architecture is left untouched rather than assumed to be protected.
class ScopedNoDenormals {
  public:
    ScopedNoDenormals() {
#if defined(__aarch64__)
        __asm__ __volatile__("mrs %0, fpcr" : "=r"(saved_));
        __asm__ __volatile__("msr fpcr, %0" : : "r"(saved_ | (1u << 24)));
#elif defined(__arm__)
        __asm__ __volatile__("vmrs %0, fpscr" : "=r"(saved_));
        __asm__ __volatile__("vmsr fpscr, %0" : : "r"(saved_ | (1u << 24)));
#elif defined(__x86_64__) || defined(__i386__)
        saved_ = _mm_getcsr();
        _mm_setcsr(saved_ | 0x8040u);
#endif
    }
    ~ScopedNoDenormals() {
#if defined(__aarch64__)
        __asm__ __volatile__("msr fpcr, %0" : : "r"(saved_));
#elif defined(__arm__)
        __asm__ __volatile__("vmsr fpscr, %0" : : "r"(saved_));
#elif defined(__x86_64__) || defined(__i386__)
        _mm_setcsr(saved_);
#endif
    }
    ScopedNoDenormals(const ScopedNoDenormals &) = delete;
    ScopedNoDenormals &operator=(const ScopedNoDenormals &) = delete;

  private:
#if defined(__aarch64__)
    uint64_t saved_ = 0;
#elif defined(__arm__)
    uint32_t saved_ = 0;
#elif defined(__x86_64__) || defined(__i386__)
    unsigned int saved_ = 0;
#endif
};

}  // namespace

const DiracLimiter::Params &DiracBiquadFilter::LevelStage() {
    return dirac_chain::LevelStage();
}

const DiracLimiter::Params &DiracBiquadFilter::PeakStage() {
    return dirac_chain::PeakStage();
}

bool DiracBiquadFilter::Configure(unsigned sampleRateHz, unsigned channelCount,
                                 const int gainsHalfDb[kBandCount]) {
    if (sampleRateHz == 0 || channelCount == 0 || channelCount > kMaxChannels ||
        gainsHalfDb == nullptr) {
        configured_ = false;
        return false;
    }

    // Below 44100 Hz the user EQ bands can be designed past Nyquist, where
    // w0 = 2*pi*f/fs makes sin(w0) negative and the peaking section's poles
    // leave the unit circle; the chain then diverges to NaN. Bypass the whole
    // chain rather than voice a rate it cannot be stable at. 44100 is the
    // lowest rate served and is included.
    if (sampleRateHz < 44100) {
        configured_ = false;
        return false;
    }

    channelCount_ = channelCount;
    sampleRateHz_ = sampleRateHz;
    // A live curve change moves the seven user bands to their new coefficients
    // over this many samples: 50 ms, the same fade the shaper uses.
    rampSamples_ = dirac_chain::FadeSamples(sampleRateHz);

    // A reconfigure replaces the curve, so nothing may stay latched across it:
    // the decision is taken from scratch below, from the incoming gains.
    for (size_t band = 0; band < kBandCount; ++band) {
        identityPath_[band] = false;
    }

    for (unsigned ch = 0; ch < channelCount; ++ch) {
        DiracCurveDesign::Signature(sampleRateHz, signature_[ch]);
    }
    // The user bands are designed here, on the command thread that reconfigures,
    // rather than on the audio path; a live gain change loads the watcher's own
    // precomputed set instead.
    Biquad designed[kBandCount];
    DiracCurveDesign::UserBands(sampleRateHz, gainsHalfDb, designed);
    BiquadCoefficients bands[kBandCount];
    for (size_t band = 0; band < kBandCount; ++band) {
        bands[band] = designed[band].Coefficients();
    }
    // A fresh chain loads immediately: its memories are cleared right after, so
    // there is no running state for a step to disturb.
    SetUserBands(gainsHalfDb, bands, 0);
    LatchIdentityBands(gainsHalfDb);

    // The chain build does not depend on the plan: the law, the peak stage and
    // the coefficients above are what the shaper runs, and the plan is built and
    // offered afterwards.
    bandShaper_.Configure(sampleRateHz, channelCount, dirac_chain::LevelStage());
    peakLimiter_.Configure(sampleRateHz, channelCount, dirac_chain::PeakStage());

    // A configured chain is a fresh chain. The coefficients above replace the
    // ones the filters were running with, so the memories behind them belong to
    // a filter that no longer exists; a caller reconfigures on a stream change
    // and does not necessarily reset, and stale memories would ring the old
    // curve into the new configuration.
    Reset();

    // The chain is built with no plan: the shaper starts in fallback (the
    // recovered level stage), and the plan arrives through AdoptBendPlan once a
    // published one matches this rate and these gains.
    memcpy(appliedGainsHalfDb_, gainsHalfDb, sizeof(appliedGainsHalfDb_));
    configured_ = true;
    return true;
}

void DiracBiquadFilter::SetUserBands(const int gainsHalfDb[kBandCount],
                                     const BiquadCoefficients bands[kBandCount],
                                     unsigned rampSamples) {
    for (unsigned ch = 0; ch < channelCount_; ++ch) {
        for (size_t band = 0; band < kBandCount; ++band) {
            // A latched band already carries the identity set, so a 0 dB load
            // would be a no-op on the coefficients, while LoadRamped would start a
            // ramp that the fast path never steps, which is the only reason for the
            // skip. It is therefore structural and order-independent: it applies
            // only when a ramp is actually requested (rampSamples != 0, i.e. a live
            // UpdateGains, never Configure) and only to a 0 dB target, because any
            // other target must take the load or the band would keep passing x
            // through. A 0 -> 0 load must also not be skipped on a RECONFIGURE: at
            // a new rate those "identity" coefficients are the old rate's, and a
            // later promotion would ramp from them.
            if (rampSamples != 0 && identityPath_[band] && gainsHalfDb[band] == 0) {
                continue;
            }
            if (rampSamples == 0) {
                stages_[ch][band].Load(bands[band]);
            } else {
                // A curve change swaps a band's coefficients under live filter
                // state, and a swap steps the output by the size of the change:
                // measured on the plan-free curve path, an un-ramped all-band
                // flip steps 1.37 to 1.40 times the signal's own largest step at a
                // band centre, and the ramp holds every measured case at or under
                // 0.95 times it. The ramp moves the running section to the new set
                // over 50 ms instead.
                stages_[ch][band].LoadRamped(bands[band], rampSamples);
            }
        }
    }
}

void DiracBiquadFilter::LatchIdentityBands(const int gainsHalfDb[kBandCount]) {
    for (size_t band = 0; band < kBandCount; ++band) {
        // The predicate is the integer control, never an epsilon on
        // coefficients. It reads the INCOMING gains: appliedGainsHalfDb_ is only
        // written at the end of Configure, so a reconfigure would otherwise latch
        // on the previous curve's gains.
        bool fast = gainsHalfDb[band] == 0;
        if (fast) {
            // Defence in depth, and the reason the fast path cannot change the
            // response: take it only when every configured channel's section
            // really carries the identity set and is not ramping. The check can
            // only keep a band on the full recursion.
            for (unsigned channel = 0; channel < channelCount_; ++channel) {
                if (stages_[channel][band].Ramping() || !stages_[channel][band].IsIdentity()) {
                    fast = false;
                    break;
                }
            }
        }
        identityPath_[band] = fast;
    }
}

bool DiracBiquadFilter::UpdateGains(const int gainsHalfDb[kBandCount],
                                    const BiquadCoefficients bands[kBandCount]) {
    if (!configured_ || gainsHalfDb == nullptr || bands == nullptr) {
        return false;
    }
    // Only the user EQ sections are rebuilt, and they are ramped to their new
    // coefficients rather than swapped under their own state. The signature and
    // both stages keep their coefficients and their memories, so a live curve
    // change neither allocates nor flushes the look-ahead ring, and it does not
    // step the output either.
    // A band whose new gain is not 0 dB leaves the identity fast path for good,
    // before the load below (which skips a band still on it). Nothing enters the
    // path here: engagement is a Configure-time decision only, because that is
    // the only point at which the state is certainly consistent.
    for (size_t band = 0; band < kBandCount; ++band) {
        if (gainsHalfDb[band] != 0) {
            identityPath_[band] = false;
        }
    }
    SetUserBands(gainsHalfDb, bands, rampSamples_);

    // Only the curve moves here. The plan is not built on this path: the effect
    // applies the curve and the matching published plan together, so a gains
    // change never leaves the shaper bending the previous response.
    memcpy(appliedGainsHalfDb_, gainsHalfDb, sizeof(appliedGainsHalfDb_));
    return true;
}

bool DiracBiquadFilter::AdoptBendPlan(const DiracBandPlan &plan) {
    if (!configured_ || !plan.valid || plan.sampleRateHz != sampleRateHz_) {
        return false;
    }
    for (size_t band = 0; band < kBandCount; ++band) {
        if (plan.gainsHalfDb[band] != appliedGainsHalfDb_[band]) {
            return false;
        }
    }
    return bandShaper_.AdoptPlan(plan);
}

void DiracBiquadFilter::Reset() {
    for (unsigned ch = 0; ch < kMaxChannels; ++ch) {
        ResetBiquads(ch);
    }
    bandShaper_.Reset();
    peakLimiter_.Reset();
    // A reset is also a re-engagement: the next processed buffer must not splice
    // the live stream against a chain whose look-ahead ring is cold and whose
    // filters start from rest. Reset runs from Configure (rampSamples_ is set and
    // the peak stage is configured) and from Init (both are zero, so no fade).
    engageDelay_ = peakLimiter_.LatencySamples();
    fadePosition_ = 0;
    fadeIn_ = true;
}

void DiracBiquadFilter::ResetBiquads(unsigned channel) {
    for (size_t section = 0; section < kSignatureCount; ++section) {
        signature_[channel][section].Reset();
    }
    for (size_t band = 0; band < kBandCount; ++band) {
        stages_[channel][band].Reset();
    }
}

float DiracBiquadFilter::EngageWet() {
    if (engageDelay_ > 0) {
        --engageDelay_;
        return 0.0f;
    }
    if (rampSamples_ == 0) {
        return fadeIn_ ? 1.0f : 0.0f;
    }
    if (fadeIn_) {
        if (fadePosition_ >= rampSamples_) {
            return 1.0f;
        }
        return static_cast<float>(fadePosition_++) / static_cast<float>(rampSamples_);
    }
    if (fadePosition_ > 0) {
        --fadePosition_;
    }
    return static_cast<float>(fadePosition_) / static_cast<float>(rampSamples_);
}

float DiracBiquadFilter::ProcessSample(float x, unsigned channel) {
    // A non-finite sample stored in the cascade's IIR state persists there, so
    // every later output stays non-finite; an ACCUMULATE buffer is one source.
    // Flush it before it reaches any memory.
    if (!std::isfinite(x)) {
        ++nonFiniteFlushes_;
        x = 0.0f;
    }
    for (size_t section = 0; section < kSignatureCount; ++section) {
        x = signature_[channel][section].ProcessSample(x);
    }
    for (size_t band = 0; band < kBandCount; ++band) {
        x = identityPath_[band] ? stages_[channel][band].ProcessIdentity(x)
                                : stages_[channel][band].ProcessSample(x);
    }
    // The input guard should keep this unreachable, but a coefficient set can
    // still produce a non-finite value. Recover the offending channel instead
    // of carrying it into the limiters. This is an emergency flush of that
    // channel's state, not a smooth operation.
    if (!std::isfinite(x)) {
        ++nonFiniteFlushes_;
        ResetBiquads(channel);
        return 0.0f;
    }
    return x;
}

void DiracBiquadFilter::Process(const void *input, void *output, size_t frameCount,
                               unsigned channelCount, audio_format_t format, bool accumulate) {
    if (!configured_ || input == nullptr || output == nullptr || channelCount == 0 ||
        channelCount > channelCount_) {
        return;
    }

    // Keep the whole buffer's processing in flush-to-zero mode; the guard
    // restores the previous control register on every return path.
    ScopedNoDenormals noDenormals;

    switch (format) {
        case AUDIO_FORMAT_PCM_FLOAT:
            ProcessFrames<dirac_format::FloatSamples>(static_cast<const float *>(input),
                                                      static_cast<float *>(output), frameCount,
                                                      channelCount, accumulate);
            break;
        case AUDIO_FORMAT_PCM_16_BIT:
            ProcessFrames<dirac_format::Pcm16Samples>(static_cast<const int16_t *>(input),
                                                      static_cast<int16_t *>(output), frameCount,
                                                      channelCount, accumulate);
            break;
        default:
            break;
    }
}

template <typename F>
void DiracBiquadFilter::ProcessFrames(const typename F::Sample *in, typename F::Sample *out,
                                      size_t frameCount, unsigned channelCount,
                                      bool accumulate) {
    // One frame is staged so each staged value is read before its output slot is
    // written; input and output may alias. `dry` is the sample the bypass path
    // would have written this frame, for the engage crossfade: it never goes
    // through the chain, so it is the policy's job to sanitize it.
    float staged[kMaxChannels];
    float dry[kMaxChannels];
    for (size_t frame = 0; frame < frameCount; ++frame) {
        const size_t base = frame * channelCount;
        const float wet = EngageWet();
        for (unsigned ch = 0; ch < channelCount; ++ch) {
            const float x = F::Load(in[base + ch]);
            dry[ch] = F::Dry(x);
            staged[ch] = ProcessSample(x, ch);
        }
        bandShaper_.BeginFrame();
        for (unsigned ch = 0; ch < channelCount; ++ch) {
            float y = bandShaper_.ProcessSample(staged[ch], ch);
            y = peakLimiter_.ProcessSample(y, ch);
            const float d = dry[ch];
            F::Store(&out[base + ch], wet >= 1.0f ? y : d + wet * (y - d), accumulate);
        }
    }
}
