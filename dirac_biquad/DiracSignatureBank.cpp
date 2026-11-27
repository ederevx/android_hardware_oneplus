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
#include "DiracSignatureBank.h"

#include <math.h>

void DiracSignatureBank::Reset() {
    x1_ = 0.0;
    for (size_t term = 0; term < DiracSignatureTable::kMaxTermCount; ++term) {
        z1_[term] = 0.0;
        z2_[term] = 0.0;
    }
}

float DiracSignatureBank::ProcessSample(const DiracSignatureCoefficients &coefficients, float x) {
    double y = coefficients.direct * x;
    for (size_t index = 0; index < coefficients.termCount; ++index) {
        const DiracSignatureTerm &term = coefficients.terms[index];
        const double part = term.e0 * x1_ + z1_[index];
        z1_[index] = term.e1 * x1_ - term.a1 * part + z2_[index];
        z2_[index] = -term.a2 * part;
        y += part;
    }
    x1_ = x;
    return static_cast<float>(y);
}

double DiracSignatureBank::MagnitudeAt(const DiracSignatureCoefficients &coefficients,
                                       const BiquadPoint &point) {
    double real = coefficients.direct;
    double imaginary = 0.0;
    for (size_t index = 0; index < coefficients.termCount; ++index) {
        const DiracSignatureTerm &term = coefficients.terms[index];
        const double numReal = term.e0 * point.cosW + term.e1 * point.cos2W;
        const double numImaginary = -(term.e0 * point.sinW + term.e1 * point.sin2W);
        const double denReal = 1.0 + term.a1 * point.cosW + term.a2 * point.cos2W;
        const double denImaginary = -(term.a1 * point.sinW + term.a2 * point.sin2W);
        const double square = denReal * denReal + denImaginary * denImaginary;
        real += (numReal * denReal + numImaginary * denImaginary) / square;
        imaginary += (numImaginary * denReal - numReal * denImaginary) / square;
    }
    return hypot(real, imaginary);
}
