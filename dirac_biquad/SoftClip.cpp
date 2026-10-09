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

#include "SoftClip.h"

float SoftClip::ProcessSample(float x) const {
    const float magnitude = std::fabs(x);
    if (magnitude <= kKnee) {
        return x;
    }
    // The overshoot past the knee is mapped onto the span left to full scale,
    // so the curve leaves the knee at unit slope and approaches unity without
    // ever reaching it: no input can drive this stage past full scale.
    const float span = 1.0f - kKnee;
    return std::copysign(kKnee + span * std::tanh((magnitude - kKnee) / span), x);
}
