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

#pragma once

#include <stddef.h>

#include "Biquad.h"
#include "DiracBiquadTable.h"

// The chain's designed response: the fixed Dirac signature followed by the seven
// user EQ bands, built for a stream rate and a set of half-dB band gains.
//
// One owner, so the response a plan is read from is the response the chain runs.
// Stateless: every method is static, the same shape as DiracBiquadTable.
//
// The band shape is the Dirac equalizer's own model: the app's EqCurveView
// reduces each band to the small-signal limit of the RBJ peaking response, a
// rational Lorentzian in (cos w - cos w0), and builds it with
// alpha = sin(w0)/2. In RBJ terms alpha = sin(w0)/(2Q), so the Dirac bands are
// Q = 1.0, which is exactly what this design uses. It is therefore the same
// second-order peaking bank, at the same centres and the same Q, in the same
// low-to-high order.
class DiracCurveDesign {
  public:
    static constexpr size_t kBandCount = 7;
    static constexpr size_t kSignatureCount = DiracBiquadTable::kSectionCount;

    static constexpr double kBandCenterHz[kBandCount] = {
            68.0, 165.0, 400.0, 972.0, 2000.0, 6000.0, 14000.0};

    // The fixed signature sections for the stream rate, from the per-rate
    // prototype table, with the corner pre-warped by Biquad::SetPrototype.
    static void Signature(unsigned sampleRateHz, Biquad out[kSignatureCount]);

    // The seven user EQ sections from the half-dB band gains.
    static void UserBands(unsigned sampleRateHz, const int gainsHalfDb[kBandCount],
                          Biquad out[kBandCount]);
};
