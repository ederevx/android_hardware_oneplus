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

#include <cmath>

namespace {

// The Dirac equalizer models each band with alpha = sin(w0)/2 (the app's
// EqCurveView "halfAlpha"), which is the RBJ peaking response at Q = 1.0.
// Matching that Q, not just the centre, is what makes this the same band shape
// the curve the app publishes asks for.
constexpr double kBandQ = 1.0;

// The frequency the map from a designed rate to another rate is matched at: the
// target's own reference frequency, where the fit pins the level.
constexpr double kSkewMatchHz = 1000.0;

// One generated term carried onto another rate through z_d^-1 = (w + alpha) /
// (1 + alpha*w), w = z_a^-1, and the constant it adds to the direct term.
void SkewTerm(const DiracSignatureTerm &term, double alpha, DiracSignatureTerm *out,
              double *constant) {
    const double d0 = 1.0 + alpha * (term.a1 + alpha * term.a2);
    const double d1 = 2.0 * alpha * (1.0 + term.a2) + term.a1 * (1.0 + alpha * alpha);
    const double d2 = alpha * (alpha + term.a1) + term.a2;
    const double p = term.e0 + alpha * term.e1;
    const double q = alpha * term.e0 + term.e1;
    const double n0 = alpha * p / d0;
    const double n1 = (p + alpha * q) / d0;
    const double n2 = q / d0;
    out->a1 = d1 / d0;
    out->a2 = d2 / d0;
    out->e0 = n1 - n0 * out->a1;
    out->e1 = n2 - n0 * out->a2;
    *constant = n0;
}

// The allpass that carries a design from `designedHz` to `rateHz`. It matches
// the two bilinear warps at kSkewMatchHz, fixes DC and Nyquist, and keeps every
// pole inside the circle because |alpha| < 1.
double SkewAlpha(unsigned designedHz, unsigned rateHz) {
    const double ratio = std::tan(M_PI * kSkewMatchHz / rateHz) /
                         std::tan(M_PI * kSkewMatchHz / designedHz);
    const double inverse = 1.0 / ratio;
    return (1.0 - inverse) / (1.0 + inverse);
}

// The designed rate closest to `rateHz` in log frequency: the map is a whole
// circle, so the nearer design is the one it distorts least.
const DiracSignatureSpec *NearestDesign(unsigned rateHz) {
    const DiracSignatureSpec *nearest = nullptr;
    double best = 0.0;
    for (size_t index = 0; index < DiracSignatureTable::kDesignCount; ++index) {
        const DiracSignatureSpec &design = DiracSignatureTable::kDesigns[index];
        const double distance =
                std::fabs(std::log(static_cast<double>(rateHz) / design.rateHz));
        if (nearest == nullptr || distance < best) {
            nearest = &design;
            best = distance;
        }
    }
    return nearest;
}

}  // namespace

bool DiracCurveDesign::Signature(unsigned sampleRateHz, DiracSignatureCoefficients *out) {
    if (out == nullptr || sampleRateHz == 0) {
        return false;
    }
    const DiracSignatureSpec *design = NearestDesign(sampleRateHz);
    if (design == nullptr) {
        return false;
    }

    // A designed rate runs the certified doubles unchanged, so the measured
    // accuracy in the generated table is the accuracy the chain has.
    *out = DiracSignatureCoefficients{};
    out->direct = design->direct;
    out->termCount = design->termCount;
    if (sampleRateHz == design->rateHz) {
        for (size_t term = 0; term < design->termCount; ++term) {
            out->terms[term] = design->terms[term];
        }
        return true;
    }

    const double alpha = SkewAlpha(design->rateHz, sampleRateHz);
    for (size_t term = 0; term < design->termCount; ++term) {
        double constant = 0.0;
        SkewTerm(design->terms[term], alpha, &out->terms[term], &constant);
        out->direct += constant;
        // The map is an allpass, so a term it pushes out of the stable triangle
        // means the arithmetic went wrong rather than the rate being unusable.
        if (!(std::fabs(out->terms[term].a2) < 1.0 &&
              std::fabs(out->terms[term].a1) < 1.0 + out->terms[term].a2)) {
            return false;
        }
    }
    return true;
}

void DiracCurveDesign::UserBands(unsigned sampleRateHz, const int gainsHalfDb[kBandCount],
                                 Biquad out[kBandCount]) {
    for (size_t band = 0; band < kBandCount; ++band) {
        const double gainDb = static_cast<double>(gainsHalfDb[band]) * 0.5;
        out[band].SetFilter(BiquadType::kPeaking, sampleRateHz, kBandCenterHz[band], kBandQ,
                            gainDb);
    }
}
