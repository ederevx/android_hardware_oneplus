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

// Safety soft-clip for the host leg.
//
// Every Dirac topology ends in a limiter trio - pslimiter, safelimiter and
// timedomainlimiter, whose sources are named inside libdirac-capiv2.so - and the
// signature this effect approximates was fitted from a chain that had them. This
// leg reproduces the safelimiter's role and only its role: an odd-symmetric
// memoryless knee that is exactly transparent below kKnee, carries no state, adds
// no look-ahead and therefore no latency for the framework to account for, and is
// bounded by unity by construction, so it replaces the hard clamp rather than
// preceding it.
//
// It exists because the cascade in front of it is a sixteen-section bank with
// large low-frequency gain: its overshoot used to land on that hard clamp, and a
// hard-clipped bass envelope modulates everything above it, which is audible as
// crunch on bass-heavy material rather than as an overload.
//
// Faithfulness: the limiter trio's threshold, knee and time constants are not
// exposed anywhere this ROM can read - not in the cal data, not in
// dirac_resource.dar, not in the interface database - so nothing beyond the
// knee's position is chosen here, and it sits at full scale, where a safety
// limiter sits. Attack, release and look-ahead are deliberately absent rather
// than guessed.
class SoftClip {
  public:
    // Full scale less half a dB: the last point at which a sample is still
    // below the ceiling, so the knee only ever catches the overshoot the static
    // preamp cannot bound.
    static constexpr float kKnee = 0.94406088f;

    // One saturation per sample, against the nineteen biquads around it, so the
    // curve can be the honest tanh rather than a cheap approximation of it.
    float ProcessSample(float x) const;
};
