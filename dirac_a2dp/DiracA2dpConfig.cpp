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

#include "DiracA2dpConfig.h"

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
constexpr int kFallbackGains[DiracA2dpConfig::kBandCount] = {6, 6, -6, 0, -6, 0, 4};
constexpr bool kFallbackEnabled = true;

int ClampHalfDb(int value) {
    if (value < DiracA2dpConfig::kMinHalfDb) {
        return DiracA2dpConfig::kMinHalfDb;
    }
    if (value > DiracA2dpConfig::kMaxHalfDb) {
        return DiracA2dpConfig::kMaxHalfDb;
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

bool ParseBands(const char *value, int gains[DiracA2dpConfig::kBandCount]) {
    char buffer[128];
    snprintf(buffer, sizeof(buffer), "%s", value);
    size_t index = 0;
    for (char *token = strtok(buffer, ";, \t"); token != nullptr; token = strtok(nullptr, ";, \t")) {
        if (index >= DiracA2dpConfig::kBandCount) {
            return false;
        }
        char *end = nullptr;
        const long parsed = strtol(token, &end, 10);
        if (end == token || *end != '\0') {
            return false;
        }
        gains[index++] = static_cast<int>(parsed);
    }
    return index == DiracA2dpConfig::kBandCount;
}

}  // namespace

const char *DiracA2dpConfig::ConfigPath() {
    return kConfigPath;
}

void DiracA2dpConfig::Fallback(int gainsHalfDb[kBandCount], bool *enabled) {
    for (size_t i = 0; i < kBandCount; ++i) {
        gainsHalfDb[i] = kFallbackGains[i];
    }
    if (enabled != nullptr) {
        *enabled = kFallbackEnabled;
    }
}

bool DiracA2dpConfig::Load(int gainsHalfDb[kBandCount], bool *enabled) {
    int gains[kBandCount];
    for (size_t i = 0; i < kBandCount; ++i) {
        gains[i] = kFallbackGains[i];
    }
    bool parsedEnabled = kFallbackEnabled;
    bool haveEnabled = false;
    bool haveBands = false;

    FILE *file = fopen(kConfigPath, "re");
    if (file == nullptr) {
        Fallback(gainsHalfDb, enabled);
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
        } else if (strcmp(key, "bands") == 0) {
            haveBands = ParseBands(value, gains);
        }
    }
    fclose(file);

    for (size_t i = 0; i < kBandCount; ++i) {
        gainsHalfDb[i] = ClampHalfDb(gains[i]);
    }
    if (enabled != nullptr) {
        *enabled = parsedEnabled;
    }
    return haveEnabled && haveBands;
}
