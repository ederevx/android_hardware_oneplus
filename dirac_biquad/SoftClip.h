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

#pragma once

#include <cmath>

// Safety soft-clip for the A2DP effect.
//
// Every Dirac topology ends in a limiter trio - pslimiter, safelimiter and
// timedomainlimiter - and the signature this effect approximates was fitted
// from a chain that had them. This leg reproduces the safelimiter half and
// nothing more: an odd-symmetric memoryless knee that is exactly transparent
// below kKnee, carries no state, adds no look-ahead and therefore no latency
// for the framework to account for, and is bounded by unity by construction, so
// it replaces the hard clamp rather than preceding it.
//
// It exists because the cascade in front of it is a sixteen-section bank with
// large low-frequency gain: its overshoot used to land on that hard clamp, and a
// hard-clipped bass envelope modulates everything above it, which is audible as
// crunch on bass-heavy material rather than as an overload.
class SoftClip {
  public:
    // Full scale less 3 dB: where a signal has no headroom left, and early
    // enough that the knee is already working by the time the cascade's
    // overshoot would otherwise reach the ceiling.
    static constexpr float kKnee = 0.70794578f;

    // One saturation per sample, against the nineteen biquads around it, so the
    // curve can be the honest tanh rather than a cheap approximation of it.
    float ProcessSample(float x) const;
};
