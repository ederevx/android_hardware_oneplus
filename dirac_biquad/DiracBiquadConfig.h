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

// Dirac-QEM parity state for the host biquad effect.
//
// The QEM app (org.lineageos.dirac.qem) keeps its user EQ as seven half-dB
// steps and pushes them to the ADSP as PARAM_EQ_BANDS (0x12D36). The host
// software path never reaches the ADSP, so the host effect reads the same
// state from a world-readable file and applies the same seven gains.
//
// Parity scope: the seven-band high-level curve (0x12d36) and the
// enable/bypass flag. The module's separate low-level 80-byte filter bank
// (param 0x12d00) is zeroed in this ROM's ACDB and is never driven by the app,
// and the DAR correction, the HDSOUND filter index and the limiter chain are
// not reproduced.
class DiracBiquadConfig {
  public:
    static constexpr size_t kBandCount = 7;

    // The QEM band centres, in the order the app stores the gains.
    static constexpr double kBandCenterHz[kBandCount] = {
            68.0, 165.0, 400.0, 972.0, 2000.0, 6000.0, 14000.0};

    // The app's band travel, in half-dB steps (12 = +6.0 dB).
    static constexpr int kMinHalfDb = -12;
    static constexpr int kMaxHalfDb = 12;

    // Stream attenuation sentinel: no usable volume, so the loudness tilt stays
    // identity. Any parsed `volume_db` that is not a positive number maps here.
    static constexpr double kUnknownVolumeDb = -1.0;

    // Reads the shared state file. On success the outputs carry the parsed
    // state and it returns true. On any failure (absent, unreadable, torn or
    // malformed) the outputs are left untouched and it returns false, so the
    // caller keeps its last good state: an unreadable file must never silently
    // drop the effect to a full pass-through. `errorCode` receives errno on an
    // open or read failure, EINVAL on a parse failure, and 0 on success; it may
    // be null. `fallback` is the software-fallback switch and defaults OFF, so
    // a host output is voiced only when the user has explicitly extended Dirac
    // to it. `sumdiff` is the mid/side width as the conf stores it, 0..2, and
    // also defaults to 0,
    // i.e. bypass.
    // `volumeDb` is the stream attenuation in dB below the reference
    // (the app publishes it), or kUnknownVolumeDb.
    static bool Load(int gainsHalfDb[kBandCount], bool *enabled, bool *fallback, float *sumdiff,
                     double *volumeDb, int *errorCode = nullptr);

    // The baked preset used before the state file has ever been loaded. It is
    // the initial default only, never a fallback for a failed reload.
    // `fallback` is always false, `sumdiff` always 0 and `volumeDb` always
    // unknown here: the switches are opt-in and the tilt is driven only by a
    // real published volume.
    static void Fallback(int gainsHalfDb[kBandCount], bool *enabled, bool *fallback, float *sumdiff,
                         double *volumeDb);

    static const char *ConfigPath();
};
