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

#include "DiracA2dpVoicing.h"

#include <cmath>

#include "Biquad.h"

namespace {

// The Dirac equalizer models each band with alpha = sin(w0)/2 (the app's
// EqCurveView "halfAlpha"), which is the RBJ peaking response at Q = 1.0.
// Matching that Q, not just the centre, is what makes this the same band shape
// the module's 0x12d36 curve asks for.
constexpr double kBandQ = 1.0;

double HalfDbToDb(int halfDb) {
    return static_cast<double>(halfDb) * 0.5;
}

}  // namespace

bool DiracA2dpVoicing::Configure(unsigned sampleRateHz, unsigned channelCount,
                                 const int gainsHalfDb[kBandCount]) {
    if (sampleRateHz == 0 || channelCount == 0 || channelCount > kMaxChannels ||
        gainsHalfDb == nullptr) {
        configured_ = false;
        return false;
    }

    double maxGainDb = 0.0;
    for (size_t band = 0; band < kBandCount; ++band) {
        const double gainDb = HalfDbToDb(gainsHalfDb[band]);
        if (gainDb > maxGainDb) {
            maxGainDb = gainDb;
        }
        for (unsigned ch = 0; ch < channelCount; ++ch) {
            stages_[ch][band].SetFilter(BiquadType::kPeaking, sampleRateHz,
                                        kBandCenterHz[band], kBandQ, gainDb);
        }
    }

    // Normalize only the boost: a preset that only cuts keeps unity, and a
    // preset that boosts cannot drive the stream into clipping.
    preampGain_ = static_cast<float>(std::pow(10.0, -maxGainDb / 20.0));

    channelCount_ = channelCount;
    configured_ = true;
    return true;
}

void DiracA2dpVoicing::Reset() {
    for (unsigned ch = 0; ch < kMaxChannels; ++ch) {
        for (size_t band = 0; band < kBandCount; ++band) {
            stages_[ch][band].Reset();
        }
    }
}

float DiracA2dpVoicing::ProcessSample(float x, unsigned channel) {
    for (size_t band = 0; band < kBandCount; ++band) {
        x = stages_[channel][band].ProcessSample(x);
    }
    return x * preampGain_;
}

void DiracA2dpVoicing::Process(const void *input, void *output, size_t frameCount,
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
