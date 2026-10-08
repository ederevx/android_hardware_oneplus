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

#include "DiracBiquadTable.h"

#include <cmath>

namespace {

// Fitted design parameters (values rounded to the two decimals that ship; the
// accuracy figures in the header are quoted from these rounded values).  These
// blocks are verbatim generator output: tools/generate_signature.py --rate
// 44100 --seed 3 and --rate 48000 --seed 6 print exactly this, order included,
// so the table has a single source of truth.  CanonicalOrder() in that script
// picks the order - a pure permutation of one fitted set, so the transfer
// function is identical and only the intermediate headroom improves.
constexpr DiracBiquadSection kSignature44100[DiracBiquadTable::kSectionCount] = {
        {BiquadType::kPeaking, 644.72, 0.225, -14.95},
        {BiquadType::kPeaking, 17394.93, 0.200, -15.00},
        {BiquadType::kPeaking, 28.13, 0.200, -9.28},
        {BiquadType::kPeaking, 51506.07, 0.200, -15.00},
        {BiquadType::kPeaking, 58.45, 0.200, -5.61},
        {BiquadType::kLowShelf, 16.34, 0.200, -11.70},
        {BiquadType::kPeaking, 4371.57, 1.595, -13.63},
        {BiquadType::kPeaking, 1354.87, 3.383, -2.98},
        {BiquadType::kPeaking, 131.68, 0.410, -5.31},
        {BiquadType::kHighShelf, 63249.64, 2.122, -15.00},
        {BiquadType::kPeaking, 19930.69, 1.464, +8.59},
        {BiquadType::kPeaking, 110.63, 0.200, +8.28},
        {BiquadType::kPeaking, 834.79, 1.368, +5.79},
        {BiquadType::kPeaking, 8628.62, 1.106, +15.00},
        {BiquadType::kPeaking, 260.65, 0.393, +7.30},
        {BiquadType::kPeaking, 3222.48, 0.203, +12.39},

};

constexpr DiracBiquadSection kSignature48000[DiracBiquadTable::kSectionCount] = {
        {BiquadType::kPeaking, 695.37, 0.200, -14.46},
        {BiquadType::kPeaking, 30015.30, 0.837, -12.47},
        {BiquadType::kPeaking, 46.90, 0.213, -11.89},
        {BiquadType::kPeaking, 12499.19, 1.885, -8.41},
        {BiquadType::kPeaking, 53717.13, 1.259, -15.00},
        {BiquadType::kPeaking, 4379.84, 1.465, -13.63},
        {BiquadType::kPeaking, 32.72, 0.200, -9.97},
        {BiquadType::kHighShelf, 55764.89, 0.996, -15.00},
        {BiquadType::kPeaking, 232.37, 0.717, -5.16},
        {BiquadType::kLowShelf, 24.61, 0.200, +0.56},
        {BiquadType::kPeaking, 1742.81, 3.500, +1.99},
        {BiquadType::kPeaking, 106.30, 0.200, +9.33},
        {BiquadType::kPeaking, 2595.68, 0.915, +5.45},
        {BiquadType::kPeaking, 889.68, 1.299, +7.79},
        {BiquadType::kPeaking, 338.02, 0.400, +11.48},
        {BiquadType::kPeaking, 9050.07, 0.958, +11.49},

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
