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

struct VoicingStage {
    BiquadType type;
    double frequencyHz;
    double q;  // Q for the peaking/high-pass types, shelf slope S for the shelves
    double gainDb;
};

// Reference A2DP voicing. Bass extension, a low-mid dip for clarity, presence
// for detail and speech intelligibility, and a high shelf for the soundstage;
// the subsonic high-pass keeps the bass shelf from amplifying rumble.
constexpr VoicingStage kStages[] = {
        {BiquadType::kHighPass, 28.0, 0.707, 0.0},
        {BiquadType::kLowShelf, 105.0, 0.9, 3.5},
        {BiquadType::kPeaking, 480.0, 0.8, -1.5},
        {BiquadType::kPeaking, 3000.0, 1.0, 1.5},
        {BiquadType::kHighShelf, 8500.0, 0.9, 1.5},
};

// The chain's measured worst-case combined gain is +2.85 dB (at 55 Hz, 44.1
// and 48 kHz); this preamp normalizes the peak to below unity so a hot master
// cannot clip either output format.
constexpr float kPreampGain = 0.7079458f;  // -3.0 dB

static_assert(sizeof(kStages) / sizeof(kStages[0]) == DiracA2dpVoicing::kStageCount,
              "voicing stage table does not match kStageCount");

}  // namespace

bool DiracA2dpVoicing::Configure(unsigned sampleRateHz, unsigned channelCount) {
    if (sampleRateHz == 0 || channelCount == 0 || channelCount > kMaxChannels) {
        configured_ = false;
        return false;
    }

    for (unsigned ch = 0; ch < channelCount; ++ch) {
        for (unsigned stage = 0; stage < kStageCount; ++stage) {
            stages_[ch][stage].SetFilter(kStages[stage].type, sampleRateHz,
                                         kStages[stage].frequencyHz, kStages[stage].q,
                                         kStages[stage].gainDb);
        }
    }

    channelCount_ = channelCount;
    configured_ = true;
    return true;
}

void DiracA2dpVoicing::Reset() {
    for (unsigned ch = 0; ch < kMaxChannels; ++ch) {
        for (unsigned stage = 0; stage < kStageCount; ++stage) {
            stages_[ch][stage].Reset();
        }
    }
}

float DiracA2dpVoicing::ProcessSample(float x, unsigned channel) {
    for (unsigned stage = 0; stage < kStageCount; ++stage) {
        x = stages_[channel][stage].ProcessSample(x);
    }
    return x * kPreampGain;
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
                    out[index] = static_cast<int16_t>(std::lrintf(y * 32767.0f));
                }
            }
            break;
        }
        default:
            break;
    }
}