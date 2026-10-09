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

#include "StereoWidth.h"

#include "Biquad.h"

#include <cmath>

float StereoWidth::GainForWidth(float width) {
    const double maxGain = std::pow(10.0, kMaxGainDb / 20.0);
    return static_cast<float>(1.0 + width * (maxGain - 1.0));
}

void StereoWidth::Configure(unsigned sampleRateHz, unsigned channelCount) {
    if (sampleRateHz == 0 || channelCount == 0) {
        configured_ = false;
        return;
    }

    sideHighPass_.SetPrototype(BiquadType::kHighPass, sampleRateHz, kCornerHz, kQ, 0.0);
    channelCount_ = channelCount;
    configured_ = true;
}

void StereoWidth::Reset() {
    sideHighPass_.Reset();
}

void StereoWidth::SetWidth(float width) {
    // The negated comparison also maps a NaN to the bypass.
    if (!(width > 0.0f)) {
        width_ = 0.0f;
    } else if (width > 1.0f) {
        width_ = 1.0f;
    } else {
        width_ = width;
    }
    gain_ = GainForWidth(width_);
}

void StereoWidth::ProcessFrame(float *frame, unsigned channelCount) {
    if (!configured_ || width_ <= 0.0f || frame == nullptr || channelCount < 2 ||
        channelCount > channelCount_) {
        return;
    }

    const float mid = 0.5f * (frame[0] + frame[1]);
    const float side = gain_ * sideHighPass_.ProcessSample(0.5f * (frame[0] - frame[1]));
    frame[0] = mid + side;
    frame[1] = mid - side;
}
