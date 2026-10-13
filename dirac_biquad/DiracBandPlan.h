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
#include <stdint.h>

#include <type_traits>

#include "Biquad.h"

// One complete bend plan: the chain's answer to "which band is over the limit,
// and how much does it have to come down" for exactly one stream rate and one
// set of band gains.
//
// This is a value, not an object: it holds no pointer, reference or handle, is
// trivially copyable and trivially destructible, so it can cross from the thread
// that builds it to the thread that runs it by copy and can never leak. Its key
// (`sampleRateHz`, `gainsHalfDb` and `valid`) says what it was built for; a
// consumer adopts it only when the key matches the state it has applied.
//
// The coefficients stay `double` because the plan's certificate on the check
// grid was computed on them, and an order-4 shelf with a low corner has poles
// near the unit circle where narrowing to float would move the response the
// certificate describes. One coefficient set per stage, not per channel: every
// channel is designed from the same parameters, and the per-channel filter state
// stays with whoever runs the sections.
struct DiracBandPlan {
    static constexpr size_t kMaxSections = 4;
    static constexpr unsigned kMaxStages = 2;  // an order-4 shelf, or one peaking cut
    static constexpr size_t kBandCount = 7;

    // One bend section: the cut it applies, the depth it was designed at, and the
    // coefficients of each of its stages.
    struct Section {
        BiquadType type;
        uint32_t stageCount;                 // 1 peaking, 2 order-4 shelf
        double cornerHz;
        double q;
        double capGain;                      // the linear gain of the designed cut
        BiquadCoefficients stage[kMaxStages];
    };

    uint32_t sampleRateHz;                  // key
    int32_t gainsHalfDb[kBandCount];        // key
    bool valid;                             // key: false means "no plan"
    bool holdsLimit;                        // the certificate on the check grid
    uint32_t sectionCount;
    uint32_t residualIntervals;
    float flatGain;
    Section sections[kMaxSections];
};

// A plan is copied whole and destroyed by doing nothing; it must never carry a
// member with a lifetime of its own.
static_assert(std::is_trivially_copyable<DiracBandPlan>::value,
              "a plan crosses threads by copy");
static_assert(std::is_trivially_destructible<DiracBandPlan>::value,
              "a plan needs no destructor");
static_assert(std::is_standard_layout<DiracBandPlan>::value, "a plan is plain data");
