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

#include "DiracCurveDesign.h"

namespace {

// The Dirac equalizer models each band with alpha = sin(w0)/2 (the app's
// EqCurveView "halfAlpha"), which is the RBJ peaking response at Q = 1.0.
// Matching that Q, not just the centre, is what makes this the same band shape
// the curve the app publishes asks for.
constexpr double kBandQ = 1.0;

}  // namespace

void DiracCurveDesign::Signature(unsigned sampleRateHz, Biquad out[kSignatureCount]) {
    const DiracBiquadSection *sections =
            DiracBiquadTable::Sections(static_cast<int>(sampleRateHz));
    for (size_t section = 0; section < kSignatureCount; ++section) {
        out[section].SetPrototype(sections[section].type, sampleRateHz,
                                  sections[section].cornerHz, sections[section].q,
                                  sections[section].gainDb);
    }
}

void DiracCurveDesign::UserBands(unsigned sampleRateHz, const int gainsHalfDb[kBandCount],
                                 Biquad out[kBandCount]) {
    for (size_t band = 0; band < kBandCount; ++band) {
        const double gainDb = static_cast<double>(gainsHalfDb[band]) * 0.5;
        out[band].SetFilter(BiquadType::kPeaking, sampleRateHz, kBandCenterHz[band], kBandQ,
                            gainDb);
    }
}
