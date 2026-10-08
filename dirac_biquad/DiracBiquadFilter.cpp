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

#include "DiracBiquadFilter.h"

#include <algorithm>
#include <cmath>

#include "Biquad.h"

namespace {

// The Dirac equalizer models each band with alpha = sin(w0)/2 (the app's
// EqCurveView "halfAlpha"), which is the RBJ peaking response at Q = 1.0.
// Matching that Q, not just the centre, is what makes this the same band shape
// the module's 0x12d36 curve asks for.
constexpr double kBandQ = 1.0;

struct SignatureSection {
    BiquadType type;
    double frequencyHz;
    double q;
    double gainDb;
};

// Approximation of the Dirac neutral signature: the FIR magnitude response of
// usecase/eheadset/defaults/941 in dirac_resource.dar, the defaults the module
// loads even with the user EQ zeroed, fitted as eight RBJ sections to 0 dB at
// 1 kHz over the whole 20 Hz-20 kHz band. On a 1/2000-octave grid with the
// extrema refined, the cascade holds 0.22 dB RMS against the 80-tap FIR's
// DTFT with a 1.14 dB worst case. The design parameters are rate independent,
// so each section is rebuilt for the stream rate.
constexpr SignatureSection kSignature[DiracBiquadFilter::kSignatureCount] = {
        {BiquadType::kLowShelf, 181.9, 0.514, -15.42},
        {BiquadType::kPeaking, 881.4, 1.183, 2.72},
        {BiquadType::kPeaking, 1321.1, 2.690, -3.30},
        {BiquadType::kPeaking, 4280.7, 1.581, -12.06},
        {BiquadType::kPeaking, 5597.8, 8.205, 1.49},
        {BiquadType::kPeaking, 7989.8, 0.961, 17.87},
        {BiquadType::kPeaking, 11276.9, 0.554, -23.41},
        {BiquadType::kHighShelf, 18472.6, 1.413, -31.06},
};

double HalfDbToDb(int halfDb) {
    return static_cast<double>(halfDb) * 0.5;
}

}  // namespace

bool DiracBiquadFilter::Configure(unsigned sampleRateHz, unsigned channelCount,
                                 const int gainsHalfDb[kBandCount]) {
    if (sampleRateHz == 0 || channelCount == 0 || channelCount > kMaxChannels ||
        gainsHalfDb == nullptr) {
        configured_ = false;
        return false;
    }

    for (size_t section = 0; section < kSignatureCount; ++section) {
        for (unsigned ch = 0; ch < channelCount; ++ch) {
            signature_[ch][section].SetFilter(kSignature[section].type, sampleRateHz,
                                              kSignature[section].frequencyHz,
                                              kSignature[section].q,
                                              kSignature[section].gainDb);
        }
    }
    for (size_t band = 0; band < kBandCount; ++band) {
        const double gainDb = HalfDbToDb(gainsHalfDb[band]);
        for (unsigned ch = 0; ch < channelCount; ++ch) {
            stages_[ch][band].SetFilter(BiquadType::kPeaking, sampleRateHz,
                                        kBandCenterHz[band], kBandQ, gainDb);
        }
    }

    preampGain_ = ComputePreampGain(sampleRateHz);

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
}

// Static headroom policy: the signature contributes gain too, so the preamp
// answers to the real peak of the whole cascade rather than to the largest
// single EQ band, and attenuates the chain only when that peak exceeds unity.
// A coarse probe can step over a narrow resonance, so its maximum is refined
// on a log-frequency axis before the gain is set. The signature and the EQ
// share one coefficient set across channels, so channel 0 is enough.
float DiracBiquadFilter::ComputePreampGain(unsigned sampleRateHz) const {
    constexpr size_t kCoarsePoints = 961;  // ~1/96 octave over 20 Hz-20 kHz
    constexpr double kMinHz = 20.0;
    constexpr double kMaxHz = 20000.0;
    constexpr double kInvPhi = 0.6180339887498949;
    constexpr double kProbeMargin = 1.0012;  // 0.01 dB of conservative headroom

    const double logMin = std::log(kMinHz);
    const double logMax = std::log(kMaxHz);
    const double step = (logMax - logMin) / static_cast<double>(kCoarsePoints - 1);

    double peak = CascadeMagnitudeAt(sampleRateHz, kMinHz);
    double peakLogHz = logMin;
    for (size_t point = 1; point < kCoarsePoints; ++point) {
        const double logHz = logMin + step * static_cast<double>(point);
        const double magnitude = CascadeMagnitudeAt(sampleRateHz, std::exp(logHz));
        if (magnitude > peak) {
            peak = magnitude;
            peakLogHz = logHz;
        }
    }

    // Golden-section refinement of the coarse maximum, bracketed by its
    // neighbouring probe frequencies.
    double low = std::max(logMin, peakLogHz - step);
    double high = std::min(logMax, peakLogHz + step);
    double x1 = high - kInvPhi * (high - low);
    double x2 = low + kInvPhi * (high - low);
    double m1 = CascadeMagnitudeAt(sampleRateHz, std::exp(x1));
    double m2 = CascadeMagnitudeAt(sampleRateHz, std::exp(x2));
    for (int iteration = 0; iteration < 40; ++iteration) {
        if (m1 < m2) {
            low = x1;
            x1 = x2;
            m1 = m2;
            x2 = low + kInvPhi * (high - low);
            m2 = CascadeMagnitudeAt(sampleRateHz, std::exp(x2));
        } else {
            high = x2;
            x2 = x1;
            m2 = m1;
            x1 = high - kInvPhi * (high - low);
            m1 = CascadeMagnitudeAt(sampleRateHz, std::exp(x1));
        }
    }
    peak = std::max(peak, std::max(m1, m2));

    const double guardedPeak = peak * kProbeMargin;
    return guardedPeak > 1.0 ? static_cast<float>(1.0 / guardedPeak) : 1.0f;
}

double DiracBiquadFilter::CascadeMagnitudeAt(double sampleRateHz, double frequencyHz) const {
    double magnitude = 1.0;
    for (size_t section = 0; section < kSignatureCount; ++section) {
        magnitude *= signature_[0][section].MagnitudeAt(sampleRateHz, frequencyHz);
    }
    for (size_t band = 0; band < kBandCount; ++band) {
        magnitude *= stages_[0][band].MagnitudeAt(sampleRateHz, frequencyHz);
    }
    return magnitude;
}

float DiracBiquadFilter::ProcessSample(float x, unsigned channel) {
    for (size_t section = 0; section < kSignatureCount; ++section) {
        x = signature_[channel][section].ProcessSample(x);
    }
    for (size_t band = 0; band < kBandCount; ++band) {
        x = stages_[channel][band].ProcessSample(x);
    }
    return x * preampGain_;
}

void DiracBiquadFilter::Process(const void *input, void *output, size_t frameCount,
                               unsigned channelCount, audio_format_t format, bool accumulate) {
    if (!configured_ || input == nullptr || output == nullptr || channelCount == 0 ||
        channelCount > channelCount_) {
        return;
    }

    switch (format) {
        case AUDIO_FORMAT_PCM_FLOAT: {
            const float *in = static_cast<const float *>(input);
            float *out = static_cast<float *>(output);
            for (size_t frame = 0; frame < frameCount; ++frame) {
                for (unsigned ch = 0; ch < channelCount; ++ch) {
                    const size_t index = frame * channelCount + ch;
                    float y = ProcessSample(in[index], ch);
                    if (accumulate) {
                        y += out[index];
                    }
                    out[index] = std::fmin(1.0f, std::fmax(-1.0f, y));
                }
            }
            break;
        }
        case AUDIO_FORMAT_PCM_16_BIT: {
            const int16_t *in = static_cast<const int16_t *>(input);
            int16_t *out = static_cast<int16_t *>(output);
            for (size_t frame = 0; frame < frameCount; ++frame) {
                for (unsigned ch = 0; ch < channelCount; ++ch) {
                    const size_t index = frame * channelCount + ch;
                    float y = ProcessSample(static_cast<float>(in[index]) / 32768.0f, ch);
                    if (accumulate) {
                        y += static_cast<float>(out[index]) / 32768.0f;
                    }
                    y = std::fmin(1.0f, std::fmax(-1.0f, y));
                    out[index] = static_cast<int16_t>(std::lround(y * 32767.0f));
                }
            }
            break;
        }
        default:
            break;
    }
}
