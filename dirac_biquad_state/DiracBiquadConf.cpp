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

#define LOG_TAG "dirac_biquad_state"

#include "DiracBiquadConf.h"

#include <android-base/logging.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace dirac {
namespace {

constexpr const char *kDirectory = "/data/vendor/audio";
constexpr mode_t kFileMode = 0644;

// The law flattens past this, so the clamp only keeps a mute's infinity from
// ever reaching the file.
constexpr double kMaxVolumeDb = 120.0;

int ClampHalfDb(int value) {
    if (value < DiracBiquadConf::kMinHalfDb) {
        return DiracBiquadConf::kMinHalfDb;
    }
    if (value > DiracBiquadConf::kMaxHalfDb) {
        return DiracBiquadConf::kMaxHalfDb;
    }
    return value;
}

double ClampVolumeDb(double volumeDb) {
    if (!std::isfinite(volumeDb) || volumeDb <= 0.0) {
        return DiracBiquadConf::kUnknownVolumeDb;
    }
    return std::min(volumeDb, kMaxVolumeDb);
}

// The 0..1 width; a non-finite or negative value is the bypass.
float ClampSumDiff(float sumdiff) {
    if (!std::isfinite(sumdiff) || sumdiff <= 0.0f) {
        return 0.0f;
    }
    return std::min(sumdiff, 1.0f);
}

std::string Format(const DiracBiquadConf::State &state) {
    char volume[32];
    snprintf(volume, sizeof(volume), "%.1f", state.volumeDb);
    char sumdiff[32];
    snprintf(sumdiff, sizeof(sumdiff), "%.3f", state.sumdiff);

    std::string body = "# dirac host biquad state: enabled/fallback/sumdiff, seven half-dB band gains"
                        ", stream attenuation\n";
    body += "enabled=" + std::string(state.enabled ? "1" : "0") + "\n";
    body += "fallback=" + std::string(state.fallback ? "1" : "0") + "\n";
    body += "bands=";
    for (size_t i = 0; i < state.bandsHalfDb.size(); ++i) {
        if (i != 0) {
            body += ",";
        }
        body += std::to_string(state.bandsHalfDb[i]);
    }
    body += "\n";
    body += "volume_db=" + std::string(volume) + "\n";
    body += "sumdiff=" + std::string(sumdiff) + "\n";
    return body;
}

}  // namespace

bool DiracBiquadConf::Write(bool enabled, bool fallback, float sumdiff,
                            const std::vector<int32_t> &bandsHalfDb, double volumeDb) {
    if (bandsHalfDb.size() != kBandCount) {
        LOG(ERROR) << "expected " << kBandCount << " bands, got " << bandsHalfDb.size();
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    State state;
    state.enabled = enabled;
    state.fallback = fallback;
    state.sumdiff = ClampSumDiff(sumdiff);
    state.bandsHalfDb.resize(bandsHalfDb.size());
    for (size_t i = 0; i < bandsHalfDb.size(); ++i) {
        state.bandsHalfDb[i] = ClampHalfDb(bandsHalfDb[i]);
    }
    state.volumeDb = ClampVolumeDb(volumeDb);

    const std::string body = Format(state);
    if (haveLastWritten_ && body == lastWritten_) {
        // Byte-identical: leaving the file untouched keeps its mtime, and with
        // it the effect's reload check, away from a needless re-read.
        return true;
    }

    mkdir(kDirectory, 0770);

    const std::string path = kPath;
    const std::string temp = path + ".tmp";

    int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, kFileMode);
    if (fd < 0) {
        LOG(ERROR) << "open " << temp << " failed: " << strerror(errno);
        return false;
    }

    // init forks services with umask(077), so the mode argument alone leaves the
    // file 0600 and the effect, which runs as audioserver, cannot read it. Set
    // the mode on the descriptor actually written, before the rename publishes
    // it.
    if (fchmod(fd, kFileMode) != 0) {
        LOG(ERROR) << "fchmod " << temp << " failed: " << strerror(errno);
        close(fd);
        unlink(temp.c_str());
        return false;
    }

    size_t written = 0;
    while (written < body.size()) {
        const ssize_t count = write(fd, body.data() + written, body.size() - written);
        if (count < 0) {
            LOG(ERROR) << "write " << temp << " failed: " << strerror(errno);
            close(fd);
            unlink(temp.c_str());
            return false;
        }
        written += static_cast<size_t>(count);
    }

    if (fsync(fd) != 0) {
        LOG(ERROR) << "fsync " << temp << " failed: " << strerror(errno);
        close(fd);
        unlink(temp.c_str());
        return false;
    }
    close(fd);

    if (rename(temp.c_str(), path.c_str()) != 0) {
        LOG(ERROR) << "rename " << temp << " to " << path << " failed: " << strerror(errno);
        unlink(temp.c_str());
        return false;
    }

    lastWritten_ = body;
    haveLastWritten_ = true;
    // The cache holds the normalized values, so a read-back reports exactly what
    // the file now carries.
    lastState_ = std::move(state);
    return true;
}

bool DiracBiquadConf::GetState(State *state) const {
    if (state == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!haveLastWritten_) {
        return false;
    }
    *state = lastState_;
    return true;
}

}  // namespace dirac
