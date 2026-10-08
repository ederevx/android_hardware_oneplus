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

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

namespace dirac {
namespace {

constexpr const char *kDirectory = "/data/vendor/audio";
constexpr mode_t kFileMode = 0644;

int ClampHalfDb(int value) {
    if (value < DiracBiquadConf::kMinHalfDb) {
        return DiracBiquadConf::kMinHalfDb;
    }
    if (value > DiracBiquadConf::kMaxHalfDb) {
        return DiracBiquadConf::kMaxHalfDb;
    }
    return value;
}

std::string Format(bool enabled, bool fallback, const std::vector<int32_t> &bandsHalfDb) {
    std::string body = "# dirac a2dp state: enabled/fallback plus seven half-dB band gains\n";
    body += "enabled=" + std::string(enabled ? "1" : "0") + "\n";
    body += "fallback=" + std::string(fallback ? "1" : "0") + "\n";
    body += "bands=";
    for (size_t i = 0; i < bandsHalfDb.size(); ++i) {
        if (i != 0) {
            body += ",";
        }
        body += std::to_string(ClampHalfDb(bandsHalfDb[i]));
    }
    body += "\n";
    return body;
}

}  // namespace

bool DiracBiquadConf::Write(bool enabled, bool fallback, const std::vector<int32_t> &bandsHalfDb) {
    if (bandsHalfDb.size() != kBandCount) {
        LOG(ERROR) << "expected " << kBandCount << " bands, got " << bandsHalfDb.size();
        return false;
    }

    mkdir(kDirectory, 0770);

    const std::string path = kPath;
    const std::string temp = path + ".tmp";
    const std::string body = Format(enabled, fallback, bandsHalfDb);

    int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, kFileMode);
    if (fd < 0) {
        LOG(ERROR) << "open " << temp << " failed: " << strerror(errno);
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
    return true;
}

}  // namespace dirac
