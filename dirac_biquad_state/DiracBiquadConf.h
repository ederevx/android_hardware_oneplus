/*
 * Copyright (c) 2026 The LineageOS Project
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

#include <cstdint>
#include <vector>

namespace dirac {

// The one owner of the A2DP fallback config file. It writes the file the
// host-side effect reads, in the format DiracBiquadConfig::Load parses.
class DiracBiquadConf {
  public:
    static constexpr const char *kPath = "/data/vendor/audio/dirac_qem.conf";
    static constexpr size_t kBandCount = 7;
    static constexpr int kMinHalfDb = -12;
    static constexpr int kMaxHalfDb = 12;

    // Writes enabled, fallback and the clamped band gains atomically.
    static bool Write(bool enabled, bool fallback, const std::vector<int32_t> &bandsHalfDb);
};

}  // namespace dirac
