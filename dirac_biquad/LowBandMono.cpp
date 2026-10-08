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

#include "LowBandMono.h"

#include "Biquad.h"

void LowBandMono::Configure(unsigned sampleRateHz, unsigned channelCount) {
    if (sampleRateHz == 0 || channelCount == 0) {
        configured_ = false;
        return;
    }

    sideHighPass_.SetPrototype(BiquadType::kHighPass, sampleRateHz, kCornerHz, kQ, 0.0);
    channelCount_ = channelCount;
    configured_ = true;
}

void LowBandMono::Reset() {
    sideHighPass_.Reset();
}

void LowBandMono::ProcessFrame(float *frame, unsigned channelCount) {
    if (!configured_ || frame == nullptr || channelCount < 2 || channelCount > channelCount_) {
        return;
    }

    const float mid = 0.5f * (frame[0] + frame[1]);
    const float side = sideHighPass_.ProcessSample(0.5f * (frame[0] - frame[1]));
    frame[0] = mid + side;
    frame[1] = mid - side;
}
