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

#include "DiracBiquadConfig.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

// The QEM app writes this file on every enable, style and band change. It is
// world readable so audioserver, which hosts the effect, can read it.
constexpr const char *kConfigPath = "/data/vendor/audio/dirac_qem.conf";

// Baked fallback: the app's "Hip Hop" preset row in half-dB steps, with the
// effect enabled, so a pushed .so is standalone-testable before the app
// writes the state file.
constexpr int kFallbackGains[DiracBiquadConfig::kBandCount] = {6, 6, -6, 0, -6, 0, 4};
constexpr bool kFallbackEnabled = true;

int ClampHalfDb(int value) {
    if (value < DiracBiquadConfig::kMinHalfDb) {
        return DiracBiquadConfig::kMinHalfDb;
    }
    if (value > DiracBiquadConfig::kMaxHalfDb) {
        return DiracBiquadConfig::kMaxHalfDb;
    }
    return value;
}

// Trims leading and trailing whitespace in place.
char *Trim(char *text) {
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
        ++text;
    }
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t' ||
                          text[length - 1] == '\r' || text[length - 1] == '\n')) {
        text[--length] = '\0';
    }
    return text;
}

bool ParseBands(const char *value, int gains[DiracBiquadConfig::kBandCount]) {
    char buffer[128];
    snprintf(buffer, sizeof(buffer), "%s", value);
    size_t index = 0;
    for (char *token = strtok(buffer, ";, \t"); token != nullptr; token = strtok(nullptr, ";, \t")) {
        if (index >= DiracBiquadConfig::kBandCount) {
            return false;
        }
        char *end = nullptr;
        const long parsed = strtol(token, &end, 10);
        if (end == token || *end != '\0') {
            return false;
        }
        gains[index++] = static_cast<int>(parsed);
    }
    return index == DiracBiquadConfig::kBandCount;
}

// A usable value is a finite, strictly positive attenuation. Everything else,
// including a mute's -infinity and the writer's -1 sentinel, is unknown.
double ParseVolumeDb(const char *value) {
    char *end = nullptr;
    const double parsed = strtod(value, &end);
    if (end == value || *end != '\0' || !isfinite(parsed) || parsed <= 0.0) {
        return DiracBiquadConfig::kUnknownVolumeDb;
    }
    return parsed;
}

// The 0..1 width, with anything unparseable or out of range mapped into it.
float ParseSumDiff(const char *value) {
    char *end = nullptr;
    const float parsed = strtof(value, &end);
    if (end == value || *end != '\0' || !isfinite(parsed)) {
        return 0.0f;
    }
    if (parsed < 0.0f) {
        return 0.0f;
    }
    if (parsed > 1.0f) {
        return 1.0f;
    }
    return parsed;
}

}  // namespace

const char *DiracBiquadConfig::ConfigPath() {
    return kConfigPath;
}

void DiracBiquadConfig::Fallback(int gainsHalfDb[kBandCount], bool *enabled,
                               bool *fallback, float *sumdiff, double *volumeDb) {
    for (size_t i = 0; i < kBandCount; ++i) {
        gainsHalfDb[i] = kFallbackGains[i];
    }
    if (enabled != nullptr) {
        *enabled = kFallbackEnabled;
    }
    if (fallback != nullptr) {
        *fallback = false;
    }
    if (sumdiff != nullptr) {
        *sumdiff = 0.0f;
    }
    if (volumeDb != nullptr) {
        *volumeDb = kUnknownVolumeDb;
    }
}

bool DiracBiquadConfig::Load(int gainsHalfDb[kBandCount], bool *enabled, bool *fallback,
                             float *sumdiff, double *volumeDb, int *errorCode) {
    if (errorCode != nullptr) {
        *errorCode = 0;
    }

    int gains[kBandCount] = {};
    bool parsedEnabled = kFallbackEnabled;
    bool parsedFallback = false;
    float parsedSumdiff = 0.0f;
    bool haveEnabled = false;
    bool haveBands = false;
    double parsedVolumeDb = kUnknownVolumeDb;

    FILE *file = fopen(kConfigPath, "re");
    if (file == nullptr) {
        if (errorCode != nullptr) {
            *errorCode = errno != 0 ? errno : EIO;
        }
        return false;
    }

    char line[256];
    while (fgets(line, sizeof(line), file) != nullptr) {
        char *text = Trim(line);
        if (*text == '\0' || *text == '#') {
            continue;
        }
        char *equals = strchr(text, '=');
        if (equals == nullptr) {
            continue;
        }
        *equals = '\0';
        const char *key = Trim(text);
        const char *value = Trim(equals + 1);

        if (strcmp(key, "enabled") == 0) {
            parsedEnabled = strtol(value, nullptr, 10) != 0;
            haveEnabled = true;
        } else if (strcmp(key, "fallback") == 0) {
            parsedFallback = strtol(value, nullptr, 10) != 0;
        } else if (strcmp(key, "sumdiff") == 0) {
            parsedSumdiff = ParseSumDiff(value);
        } else if (strcmp(key, "bands") == 0) {
            haveBands = ParseBands(value, gains);
        } else if (strcmp(key, "volume_db") == 0) {
            parsedVolumeDb = ParseVolumeDb(value);
        }
    }
    const int readError = ferror(file);
    fclose(file);

    if (readError != 0) {
        if (errorCode != nullptr) {
            *errorCode = readError;
        }
        return false;
    }
    if (!haveEnabled || !haveBands) {
        if (errorCode != nullptr) {
            *errorCode = EINVAL;
        }
        return false;
    }

    // Commit only a fully parsed state; a torn or malformed file leaves the
    // caller's last good state in place.
    for (size_t i = 0; i < kBandCount; ++i) {
        gainsHalfDb[i] = ClampHalfDb(gains[i]);
    }
    if (enabled != nullptr) {
        *enabled = parsedEnabled;
    }
    if (fallback != nullptr) {
        *fallback = parsedFallback;
    }
    if (sumdiff != nullptr) {
        *sumdiff = parsedSumdiff;
    }
    if (volumeDb != nullptr) {
        *volumeDb = parsedVolumeDb;
    }
    return true;
}
