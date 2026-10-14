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
// The level stage: the recovered safelimiter law, kept at its recovered
// values. It measures the mean square over a 64-sample window, follows it with
// a symmetric one-pole in linear power (tau 2000 ms), and maps that level
// through the dB law at a -12.5 dBFS threshold and up to 9 dB of attenuation;
// the resulting attenuation is then held by an instant attack and shaped by a
// release and an attack one-pole (both tau 1000 ms). The kernel's compiled
// apply path carries all three time constants as immediates, so they are part
// of the recovered stage and are not ours to drop: removing the level memory
// was falsified on the host (it turns the stage into a 64-sample waveshaper:
// the full-scale worst-case bass THD rises from 0.31% to 7.2% and the 1 kHz
// probe ducking rises from 2.90 dB to 8.35 dB, while keeping the memory and
// raising only the peak threshold cuts that ducking to 0.99 dB).
//
// The two 941 numbers are the only DAR-sourced values: threshold -12.5 dBFS and
// maximum attenuation 9.0 dB. Their field roles are INFERRED (the blob proves
// the values and the 9.0-then--12.5 order, not which is which); the literal
// role swap produces no ducking at all on real programme, so this mapping is
// the one supported by the observed symptom.
constexpr DiracLimiter::Params kLevelStage = {
        -12.5f, 9.0f, 1.0f, 1.0e-4f, 64, true, 2000.0f, 0.0f,
        1000.0f, 1000.0f, true, 0.0f, false, 0u, 0u};

// The peak stage: the recovered pslimiter and the bound the chain ends in. A
// per-sample peak with a 10 ms hold, a 25 ms release pole, an instant attack, a
// 6 ms attack pole and 8 ms of look-ahead; the parameters are the recovered 503
// values, so the threshold is back at -0.5 dBFS with a 50 dB cap. The ceiling a
// hardBound stage applies caps the output sample peak at the ceiling that
// threshold computes, for any input below threshold + maxAttenuation. It reads
// the ceiling from the largest input inside the look-ahead, so the sample
// leaving the delay line is always covered.
//
// The threshold alone cannot bound the output: what the ceiling bounds is the
// sample peak, while a converter reconstructs an overshoot between those
// samples on broadband material. The ceiling therefore evaluates a true-peak
// estimate of the input, from a polyphase windowed-sinc detector (wide-Hann
// half-width 64, P fractional phases at p/P, truePeakTaps coefficients per
// phase (16), a kernel length of 2*truePeakTaps (32), every phase normalised to
// unit DC), and takes the largest |interpolated| value seen in the look-ahead.
//
// What the hard bound holds exactly, independent of any reconstruction kernel,
// is that sample-peak cap: on the 1440-record broadband stress the output
// sample peak measured at worst 0.944066, under the -0.5 dBFS ceiling. The
// output's true (inter-sample) peak is a kernel-dependent property of the gain
// path: the same stress cells measured about 0.964 to 1.046 at 16x depending on
// the reconstruction kernel, so it is not a single number and no true-peak
// guarantee is claimed. This detector is our own host-side departure from the
// blob, whose safelimiter/pslimiter carry no true-peak evaluation at all; the
// -0.5 dBFS ceiling is the owner requirement.
//
// The interpolator carries a group delay of about half its kernel length, so
// the ceiling sees the input about truePeakTaps samples late; the effective
// look-ahead margin is smaller than the full 8 ms. That is ample here, but a
// longer kernel eats into it, so the kernel length is bounded by the look-ahead
// as much as by CPU.
constexpr DiracLimiter::Params kPeakStage = {
        -0.5f, 50.0f, 1.0f, 1.0e-4f, 1, false, 0.0f, 10.0f,
        25.0f, 6.0f, true, 8.0f, true, 16u, 8u};

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
        ResetBiquads(ch);
    }
    levelLimiter_.Reset();
    peakLimiter_.Reset();
}

void DiracBiquadFilter::ResetBiquads(unsigned channel) {
    for (size_t section = 0; section < kSignatureCount; ++section) {
        signature_[channel][section].Reset();
    }
    for (size_t band = 0; band < kBandCount; ++band) {
        stages_[channel][band].Reset();
    }
}

float DiracBiquadFilter::ProcessSample(float x, unsigned channel) {
    // A non-finite sample stored in the cascade's IIR state persists there, so
    // every later output stays non-finite; an ACCUMULATE buffer is one source.
    // Flush it before it reaches any memory.
    if (!std::isfinite(x)) {
        x = 0.0f;
    }
    for (size_t section = 0; section < kSignatureCount; ++section) {
        x = signature_[channel][section].ProcessSample(x);
    }
    for (size_t band = 0; band < kBandCount; ++band) {
        x = stages_[channel][band].ProcessSample(x);
    }
    // The input guard should keep this unreachable, but a coefficient set can
    // still produce a non-finite value. Recover the offending channel instead
    // of carrying it into the limiters. This is an emergency flush of that
    // channel's state, not a smooth operation.
    if (!std::isfinite(x)) {
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
