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
#include <mutex>
#include <string>
#include <vector>

namespace dirac {

// The one owner of the host-output fallback config file. It writes the file the
// host-side effect reads, in the format DiracBiquadConfig::Load parses.
class DiracBiquadConf {
  public:
    static constexpr const char *kPath = "/data/vendor/audio/dirac_qem.conf";
    static constexpr size_t kBandCount = 7;
    static constexpr int kMinHalfDb = -12;
    static constexpr int kMaxHalfDb = 12;

    // Stream attenuation sentinel: no usable volume, written as -1 so the
    // effect keeps the loudness tilt at identity.
    static constexpr double kUnknownVolumeDb = -1.0;

    // The normalized values of the last state this writer put on disk.
    struct State {
        bool enabled = false;
        bool fallback = false;
        float sumdiff = 0.0f;
        std::vector<int32_t> bandsHalfDb;
        double volumeDb = kUnknownVolumeDb;
    };

    // Writes enabled, fallback, the 0..1 mid/side width, the clamped band
    // gains and the validated stream attenuation atomically. A call whose
    // serialized content matches the last one is a no-op: no temp file, no
    // rename and no mtime change, so a repeated publish cannot make the effect
    // reload. Returns true when the state is on disk, including the no-op case.
    bool Write(bool enabled, bool fallback, float sumdiff,
               const std::vector<int32_t> &bandsHalfDb, double volumeDb);

    // The last state successfully put on disk. False until the first Write.
    bool GetState(State *state) const;

  private:
    mutable std::mutex mutex_;
    std::string lastWritten_;
    bool haveLastWritten_ = false;
    State lastState_;
};

}  // namespace dirac
