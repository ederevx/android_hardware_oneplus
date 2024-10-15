/*
 * Copyright (C) 2021 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "LED.h"

#include "Utils.h"

namespace aidl {
namespace android {
namespace hardware {
namespace light {

static const uint32_t kDefaultMaxLedBrightness = 255;

LED::LED(std::string type) : mBasePath("/sys/class/leds/" + type + "/") {
    if (!readFromFile(mBasePath + "max_brightness", &mMaxBrightness))
        mMaxBrightness = kDefaultMaxLedBrightness;
    mBreath = fileWriteable(mBasePath + "breath");
}

bool LED::exists() {
    return fileWriteable(mBasePath + "brightness");
}

bool LED::setBreath(uint32_t value) {
    return writeToFile(mBasePath + (mBreath ? "breath" : "blink"), value);
}

bool LED::setTimed(uint32_t value, uint32_t onMs, uint32_t offMs, uint32_t idx) {
    // Abort if we have mBreath in use instead.
    if (mBreath)
        return false;

    // LUT has 63 entries, we could theoretically use them as 3 (colors) * 21 (steps).
    // However, the last LUT entries don't seem to behave correctly for unknown
    // reasons, so we use 17 steps for a total of 51 LUT entries only.
    static constexpr int kRampSteps = 16;
    static constexpr int kRampMaxStepDurationMs = 15;
    int pauseLo, pauseHi, stepDuration, start_idx = 0;

    auto getScaledDutyPercent = [](int brightness) -> std::string {
        std::string output;
        for (int i = 0; i <= kRampSteps; i++) {
            if (i != 0) {
                output += ",";
            }
            output += std::to_string(i * 512 * brightness / (255 * kRampSteps));
        }
        return output;
    };

    if (kRampMaxStepDurationMs * kRampSteps > onMs) {
        stepDuration = onMs / kRampSteps;
        pauseHi = 0;
        pauseLo = offMs;
    } else {
        stepDuration = kRampMaxStepDurationMs;
        pauseHi = onMs - kRampSteps * stepDuration;
        pauseLo = offMs - kRampSteps * stepDuration;
    }

    if (idx)
        start_idx = (kRampSteps + 1) * idx;

    return writeToFile(mBasePath + "start_idx", start_idx) &&
            writeToFile(mBasePath + "duty_pcts", getScaledDutyPercent(value)) &&
            writeToFile(mBasePath + "pause_lo", pauseLo) &&
            writeToFile(mBasePath + "pause_hi", pauseHi) &&
            writeToFile(mBasePath + "ramp_step_ms", stepDuration) &&
            writeToFile(mBasePath + "blink", 1);
}

bool LED::setBrightness(uint32_t value) {
    return writeToFile(mBasePath + "brightness", value * mMaxBrightness / 0xFF);
}

} // namespace light
} // namespace hardware
} // namespace android
} // namespace aidl
