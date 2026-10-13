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

#include "DiracBiquadTable.h"

#include <cmath>

namespace {

// Fitted design parameters (values rounded to the two decimals that ship; the
// accuracy figures in the header are quoted from these rounded values).  These
// blocks are verbatim generator output: the DC-anchored refit commands in the
// header print exactly this, order included, so the table has a single source
// of truth.  The documented seeds with --dc-option off print the pre-refit
// blocks instead.  CanonicalOrder() in that script picks the order - a pure
// permutation of one fitted set, so the transfer function is identical and only
// the intermediate headroom improves.
constexpr DiracBiquadSection kSignature44100[DiracBiquadTable::kSectionCount] = {
        {BiquadType::kPeaking, 1984.05, 0.327, -10.31},
        {BiquadType::kPeaking, 218.34, 0.645, -4.91},
        {BiquadType::kPeaking, 17394.93, 0.859, -6.57},
        {BiquadType::kPeaking, 58.98, 0.200, -0.01},
        {BiquadType::kPeaking, 589.42, 1.370, -0.60},
        {BiquadType::kPeaking, 2330.80, 3.500, -2.06},
        {BiquadType::kPeaking, 6384.49, 3.500, -9.75},
        {BiquadType::kPeaking, 10108.92, 3.336, -2.67},
        {BiquadType::kPeaking, 922.30, 2.008, +0.83},
        {BiquadType::kPeaking, 120.35, 0.272, +0.24},
        {BiquadType::kPeaking, 4795.45, 0.202, +10.07},
        {BiquadType::kPeaking, 34.78, 0.200, +0.88},
        {BiquadType::kHighShelf, 86796.28, 2.553, +2.68},
        {BiquadType::kLowShelf, 23.65, 0.200, +3.05},
        {BiquadType::kPeaking, 277.30, 0.408, +7.71},
        {BiquadType::kPeaking, 29940.84, 0.202, +4.74},

};

constexpr DiracBiquadSection kSignature48000[DiracBiquadTable::kSectionCount] = {
        {BiquadType::kPeaking, 1854.19, 0.214, -12.71},
        {BiquadType::kPeaking, 401.96, 0.406, -9.20},
        {BiquadType::kPeaking, 69.09, 0.200, -0.22},
        {BiquadType::kPeaking, 6351.42, 3.500, -13.21},
        {BiquadType::kPeaking, 32236.13, 0.645, -7.91},
        {BiquadType::kPeaking, 10772.26, 1.367, -2.73},
        {BiquadType::kHighShelf, 84588.14, 2.362, -3.88},
        {BiquadType::kPeaking, 3328.91, 3.153, +4.79},
        {BiquadType::kPeaking, 6227.14, 1.111, +11.11},
        {BiquadType::kPeaking, 1202.92, 0.568, +7.55},
        {BiquadType::kPeaking, 165.60, 0.377, +0.13},
        {BiquadType::kPeaking, 78.01, 0.200, +0.93},
        {BiquadType::kPeaking, 31.47, 0.200, +1.05},
        {BiquadType::kLowShelf, 17.69, 0.200, +2.96},
        {BiquadType::kPeaking, 414.74, 0.401, +15.00},
        {BiquadType::kPeaking, 28526.58, 0.215, +11.79},

};

// 88200 and 96000 carry the 48000 prototype, so they alias that one array
// instead of duplicating it.
struct RateEntry {
    int rateHz;
    const DiracBiquadSection *sections;
};

constexpr RateEntry kEntries[] = {
        {44100, kSignature44100},
        {48000, kSignature48000},
        {88200, kSignature48000},
        {96000, kSignature48000},
};

constexpr size_t kEntryCount = sizeof(kEntries) / sizeof(kEntries[0]);

}  // namespace

// Fallback policy: a rate outside the table runs the *nearest covered rate's
// prototype* through Biquad::SetPrototype.  Because the entries are analog
// prototypes, the pre-warped bilinear transform then realises that prototype's
// analog response at the requested rate, which is exactly what the synthesis
// path produces for a covered rate too - so an unlisted rate is as well
// behaved as the rate the prototype came from, and no coefficient is invented.
// "Nearest" is the closest rate in ratio, which is the meaningful distance for
// a filter: 192000 therefore falls back to 96000 and 22050 to 44100.  The
// signature is rate independent by construction, so this needs no extra
// tolerance band around the covered rates.
const DiracBiquadSection *DiracBiquadTable::Sections(int sampleRateHz) {
    if (sampleRateHz <= 0) {
        return kEntries[0].sections;
    }

    size_t nearest = 0;
    double nearestRatio = 0.0;
    for (size_t entry = 0; entry < kEntryCount; ++entry) {
        if (kEntries[entry].rateHz == sampleRateHz) {
            return kEntries[entry].sections;
        }
        const double ratio = std::fabs(std::log(static_cast<double>(sampleRateHz) /
                                                static_cast<double>(kEntries[entry].rateHz)));
        if (entry == 0 || ratio < nearestRatio) {
            nearest = entry;
            nearestRatio = ratio;
        }
    }
    return kEntries[nearest].sections;
}
