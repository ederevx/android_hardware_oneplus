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
// 44100 --seed 9 and --rate 48000 --seed 2 print exactly this, order included,
// so the table has a single source of truth.  CanonicalOrder() in that script
// picks the order - a pure permutation of one fitted set, so the transfer
// function is identical and only the intermediate headroom improves.
constexpr DiracBiquadSection kSignature44100[DiracBiquadTable::kSectionCount] = {
        {BiquadType::kPeaking, 1984.05, 0.319, -10.37},
        {BiquadType::kPeaking, 59.99, 0.200, -6.54},
        {BiquadType::kPeaking, 17394.93, 0.858, -6.54},
        {BiquadType::kPeaking, 6384.47, 3.500, -9.75},
        {BiquadType::kPeaking, 10108.92, 3.323, -2.66},
        {BiquadType::kPeaking, 2306.39, 3.500, -2.28},
        {BiquadType::kPeaking, 224.67, 0.669, -4.54},
        {BiquadType::kPeaking, 543.25, 1.368, -1.64},
        {BiquadType::kPeaking, 119.02, 0.292, -1.19},
        {BiquadType::kLowShelf, 23.91, 0.200, +3.82},
        {BiquadType::kPeaking, 934.98, 2.111, +0.20},
        {BiquadType::kPeaking, 5345.98, 0.223, +10.05},
        {BiquadType::kPeaking, 257.93, 0.305, +11.52},
        {BiquadType::kHighShelf, 86345.93, 1.879, +2.98},
        {BiquadType::kPeaking, 33.26, 0.200, +5.87},
        {BiquadType::kPeaking, 30239.68, 0.200, +4.63},

};

constexpr DiracBiquadSection kSignature48000[DiracBiquadTable::kSectionCount] = {
        {BiquadType::kPeaking, 1825.37, 0.201, -13.07},
        {BiquadType::kPeaking, 31.11, 0.200, -6.77},
        {BiquadType::kPeaking, 32236.13, 0.631, -7.85},
        {BiquadType::kPeaking, 10772.26, 1.368, -2.70},
        {BiquadType::kPeaking, 6351.57, 3.500, -13.22},
        {BiquadType::kPeaking, 397.67, 0.418, -10.57},
        {BiquadType::kPeaking, 69.40, 0.200, +1.58},
        {BiquadType::kPeaking, 169.45, 0.361, +1.27},
        {BiquadType::kPeaking, 3340.45, 3.242, +4.80},
        {BiquadType::kPeaking, 1202.92, 0.564, +7.88},
        {BiquadType::kHighShelf, 84918.63, 2.331, -0.46},
        {BiquadType::kPeaking, 6227.14, 1.102, +11.27},
        {BiquadType::kPeaking, 78.23, 0.200, +3.46},
        {BiquadType::kPeaking, 425.46, 0.411, +15.00},
        {BiquadType::kLowShelf, 17.50, 0.200, +13.38},
        {BiquadType::kPeaking, 27381.04, 0.221, +12.03},

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
