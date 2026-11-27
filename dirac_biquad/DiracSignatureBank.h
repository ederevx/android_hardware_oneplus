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

#include "DiracCurveDesign.h"

// One channel's state for the fixed Dirac signature, which runs as one filter.
//
// The per-rate sections are multiplied into a single equivalent filter when the
// table is generated, and this runs that filter in its parallel form: a direct
// term plus one real term per pole, so nothing is cascaded at runtime and no term
// waits on another. The coefficients come from DiracCurveDesign: the generated
// doubles at a designed rate, those terms mapped onto any other. The arithmetic
// is double so the terms run as generated, and the module builds without
// contraction so a term evaluates as it was certified.
class DiracSignatureBank {
  public:
    void Reset();
    float ProcessSample(const DiracSignatureCoefficients &coefficients, float x);

    // |H| at one unit-circle point, from the terms that run. Not on the sample
    // path; the plan builder reads the response the chain actually applies.
    static double MagnitudeAt(const DiracSignatureCoefficients &coefficients,
                              const BiquadPoint &point);

  private:
    double x1_ = 0.0;
    double z1_[DiracSignatureTable::kMaxTermCount] = {};
    double z2_[DiracSignatureTable::kMaxTermCount] = {};
};
