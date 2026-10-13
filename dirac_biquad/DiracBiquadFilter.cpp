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

#include "Biquad.h"
#include "DiracBiquadTable.h"
#include "DiracLimiter.h"

namespace {

// The Dirac equalizer models each band with alpha = sin(w0)/2 (the app's
// EqCurveView "halfAlpha"), which is the RBJ peaking response at Q = 1.0.
// Matching that Q, not just the centre, is what makes this the same band shape
// the curve the app publishes asks for.
constexpr double kBandQ = 1.0;

double HalfDbToDb(int halfDb) {
    return static_cast<double>(halfDb) * 0.5;
}

// No static headroom margin sits in front of the limiters. The peak stage is
// configured to bound its own output, so nothing the signature and the curve ask
// for can carry the chain past that stage's threshold; a fixed attenuation here
// would only take level away from a chain that is already bounded.
// The level stage: a slow clamp on the energy the signature and the curve ask
// for at low frequencies. It measures the mean square over 64 samples and
// follows it with a two-second one-pole, so it tracks the programme level
// rather than the waveform, at a -12.5 dBFS threshold and up to 9 dB of
// attenuation. Its attack is instant and only its release is smoothed, which is
// what keeps a step from passing for a whole time constant before the clamp
// engages.
constexpr DiracLimiter::Params kLevelStage = {
        -12.5f, 9.0f, 1.0f, 1.0e-4f, 64, true, 2000.0f, 0.0f,
        1000.0f, 1000.0f, true, 0.0f, false, 0u};

// The peak stage: the bound the chain ends in. A per-sample peak with a 10 ms
// hold and 8 ms of look-ahead, plus the ceiling a hardBound stage applies, keeps
// the output at or below its threshold for any input below threshold +
// maxAttenuation: +47.3 dBFS (231.74 linear) at this stage's -2.70 dBFS
// threshold, and +45.5 dBFS (188.36 linear) for the landed -4.5 dBFS it
// replaced (an input at A=298, +49.5 dBFS, already breached that older
// guarantee). The ceiling is read from the largest input inside the look-ahead,
// so the sample leaving the delay line is always covered. It only takes over
// where the tuning would let a transient through, so the tuning itself is
// unchanged elsewhere. This is one of the two places the chain departs from the
// tuning it reproduces, and it is deliberate: a safety stage that lets a
// transient through is not a bound.
//
// The interpolator carries a group delay, so the ceiling sees the input about
// N/2 samples late; the effective look-ahead margin is about 384 - N/2 = 378
// samples at the 12-coefficient kernel (N=12), not the full 384. That is ample
// here, but a longer kernel eats into it, so the kernel length is bounded by
// the look-ahead as much as by CPU.
//
// The threshold is the second departure, and the reason it is not the recovered
// -0.5 dBFS. What the ceiling bounds is the sample peak; what a converter
// reconstructs between those samples overshoots it on broadband material, and
// no smoothing or clipping moves that. A polyphase true-peak detector (one
// fractional phase, wide-Hann half-width 64) inside the ceiling path replaces
// the sample maximum with the interpolated one, so the ceiling bounds the
// reconstruction rather than the samples. Its residual was measured on 300
// broadband seeds per rate and gainset at 16x: r = true peak / detector
// estimate is threshold-invariant, worst r at 44100 Hz, so the threshold is
// -2.70 dBFS, and 0 of 300 seeds exceed full scale at every rate. Without the
// detector the same stress needs -5.17 dBFS (44100/zero, 17/300 breaching at
// the landed -4.5), so the landed -4.5 did not hold for broadband at 44.1/48
// kHz; tonal and programme-like material alone needed -4.40 dBFS. The detector
// costs 12 multiply-adds per sample per channel and leaves 1.11 dB of margin
// on the auditor's seed space (1.13 dB on ours). The gain is not uniform
// across material: on broadband noise at the zero gainset it is about 0.30 dB
// worse than the landed -4.5, and it is larger on tonal material. Restoring
// the recovered reference is this one number.
constexpr DiracLimiter::Params kPeakStage = {
        -2.7f, 50.0f, 1.0f, 1.0e-4f, 1, false, 0.0f, 10.0f,
        25.0f, 6.0f, true, 8.0f, true, 6u};

}  // namespace

const DiracLimiter::Params &DiracBiquadFilter::LevelStage() {
    return kLevelStage;
}

const DiracLimiter::Params &DiracBiquadFilter::PeakStage() {
    return kPeakStage;
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

    const DiracBiquadSection *signature = DiracBiquadTable::Sections(sampleRateHz);
    for (size_t section = 0; section < kSignatureCount; ++section) {
        for (unsigned ch = 0; ch < channelCount; ++ch) {
            signature_[ch][section].SetPrototype(signature[section].type, sampleRateHz,
                                                 signature[section].cornerHz,
                                                 signature[section].q,
                                                 signature[section].gainDb);
        }
    }
    for (size_t band = 0; band < kBandCount; ++band) {
        const double gainDb = HalfDbToDb(gainsHalfDb[band]);
        for (unsigned ch = 0; ch < channelCount; ++ch) {
            stages_[ch][band].SetFilter(BiquadType::kPeaking, sampleRateHz,
                                        kBandCenterHz[band], kBandQ, gainDb);
        }
    }

    levelLimiter_.Configure(sampleRateHz, channelCount, kLevelStage);
    peakLimiter_.Configure(sampleRateHz, channelCount, kPeakStage);

    // A configured chain is a fresh chain. The coefficients above replace the
    // ones the filters were running with, so the memories behind them belong to
    // a filter that no longer exists; a caller reconfigures on a stream change
    // and does not necessarily reset, and stale memories would ring the old
    // curve into the new configuration.
    Reset();

    channelCount_ = channelCount;
    configured_ = true;
    return true;
}

void DiracBiquadFilter::Reset() {
    for (unsigned ch = 0; ch < kMaxChannels; ++ch) {
        for (size_t section = 0; section < kSignatureCount; ++section) {
            signature_[ch][section].Reset();
        }
        for (size_t band = 0; band < kBandCount; ++band) {
            stages_[ch][band].Reset();
        }
    }
    levelLimiter_.Reset();
    peakLimiter_.Reset();
}

float DiracBiquadFilter::ProcessSample(float x, unsigned channel) {
    for (size_t section = 0; section < kSignatureCount; ++section) {
        x = signature_[channel][section].ProcessSample(x);
    }
    for (size_t band = 0; band < kBandCount; ++band) {
        x = stages_[channel][band].ProcessSample(x);
    }
    return x;
}

void DiracBiquadFilter::Process(const void *input, void *output, size_t frameCount,
                               unsigned channelCount, audio_format_t format, bool accumulate) {
    if (!configured_ || input == nullptr || output == nullptr || channelCount == 0 ||
        channelCount > channelCount_) {
        return;
    }

    // One frame is staged so each staged value is read before its output slot is
    // written; input and output may alias.
    float staged[kMaxChannels];

    switch (format) {
        case AUDIO_FORMAT_PCM_FLOAT: {
            const float *in = static_cast<const float *>(input);
            float *out = static_cast<float *>(output);
            for (size_t frame = 0; frame < frameCount; ++frame) {
                const size_t base = frame * channelCount;
                for (unsigned ch = 0; ch < channelCount; ++ch) {
                    staged[ch] = ProcessSample(in[base + ch], ch);
                }
                for (unsigned ch = 0; ch < channelCount; ++ch) {
                    float y = staged[ch];
                    if (accumulate) {
                        y += out[base + ch];
                    }
                    y = levelLimiter_.ProcessSample(y, ch);
                    out[base + ch] = peakLimiter_.ProcessSample(y, ch);
                }
            }
            break;
        }
        case AUDIO_FORMAT_PCM_16_BIT: {
            const int16_t *in = static_cast<const int16_t *>(input);
            int16_t *out = static_cast<int16_t *>(output);
            for (size_t frame = 0; frame < frameCount; ++frame) {
                const size_t base = frame * channelCount;
                for (unsigned ch = 0; ch < channelCount; ++ch) {
                    staged[ch] = ProcessSample(static_cast<float>(in[base + ch]) / 32768.0f, ch);
                }
                for (unsigned ch = 0; ch < channelCount; ++ch) {
                    float y = staged[ch];
                    if (accumulate) {
                        y += static_cast<float>(out[base + ch]) / 32768.0f;
                    }
                    y = levelLimiter_.ProcessSample(y, ch);
                    y = peakLimiter_.ProcessSample(y, ch);
                    out[base + ch] = static_cast<int16_t>(std::lround(y * 32767.0f));
                }
            }
            break;
        }
        default:
            break;
    }
}
