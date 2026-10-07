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

#include <stddef.h>

// Dirac-QEM parity state for the A2DP host effect.
//
// The QEM app (org.lineageos.dirac.qem) keeps its user EQ as seven half-dB
// steps and pushes them to the ADSP as PARAM_EQ_BANDS (0x12D36). The A2DP
// software path never reaches the ADSP, so the host effect reads the same
// state from a world-readable file and applies the same seven gains.
//
// Parity scope: the seven-band high-level curve (0x12d36) and the
// enable/bypass flag. The module's separate low-level 80-byte filter bank
// (param 0x12d00) is zeroed in this ROM's ACDB and is never driven by the app,
// and the DAR correction, the HDSOUND filter index and the limiter chain are
// not reproduced.
class DiracA2dpConfig {
  public:
    static constexpr size_t kBandCount = 7;

    // The QEM band centres, in the order the app stores the gains.
    static constexpr double kBandCenterHz[kBandCount] = {
            68.0, 165.0, 400.0, 972.0, 2000.0, 6000.0, 14000.0};

    // The app's band travel, in half-dB steps (12 = +6.0 dB).
    static constexpr int kMinHalfDb = -12;
    static constexpr int kMaxHalfDb = 12;

    // Reads the shared state file. On any failure (absent, unreadable,
    // malformed) both outputs are filled with the baked fallback preset and it
    // returns false, so the effect stays standalone-testable.
    static bool Load(int gainsHalfDb[kBandCount], bool *enabled);

    // The baked preset used when the state file is not usable.
    static void Fallback(int gainsHalfDb[kBandCount], bool *enabled);

    static const char *ConfigPath();
};
