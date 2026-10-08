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
// loads even with the user EQ zeroed, fitted as five RBJ sections (0.57 dB RMS
// over 20 Hz-19 kHz). The design parameters are rate independent, so each
// section is rebuilt for the stream rate.
constexpr SignatureSection kSignature[DiracBiquadFilter::kSignatureCount] = {
        {BiquadType::kLowShelf, 243.1, 0.522, -19.45},
        {BiquadType::kPeaking, 1397.9, 3.968, -4.18},
        {BiquadType::kPeaking, 4052.6, 2.330, -3.42},
        {BiquadType::kHighShelf, 5680.6, 4.889, 8.25},
        {BiquadType::kPeaking, 18897.9, 0.300, -38.00},
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

// The signature contributes gain too, so the preamp must answer to the real
// peak of the whole cascade, not the largest single EQ band. The signature and
// the EQ share one coefficient set across channels, so channel 0 is enough.
float DiracBiquadFilter::ComputePreampGain(unsigned sampleRateHz) const {
    constexpr size_t kProbeCount = 64;
    constexpr double kMinHz = 20.0;
    constexpr double kMaxHz = 20000.0;

    double peak = 0.0;
    for (size_t probe = 0; probe < kProbeCount; ++probe) {
        const double t = static_cast<double>(probe) / static_cast<double>(kProbeCount - 1);
        const double frequencyHz = kMinHz * std::pow(kMaxHz / kMinHz, t);
        double magnitude = 1.0;
        for (size_t section = 0; section < kSignatureCount; ++section) {
            magnitude *= signature_[0][section].MagnitudeAt(sampleRateHz, frequencyHz);
        }
        for (size_t band = 0; band < kBandCount; ++band) {
            magnitude *= stages_[0][band].MagnitudeAt(sampleRateHz, frequencyHz);
        }
        if (magnitude > peak) {
            peak = magnitude;
        }
    }

    return peak > 1.0 ? static_cast<float>(1.0 / peak) : 1.0f;
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
