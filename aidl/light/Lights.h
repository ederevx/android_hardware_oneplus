/*
 * Copyright (C) 2021 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/android/hardware/light/BnLights.h>
#include <mutex>

using ::aidl::android::hardware::light::HwLightState;
using ::aidl::android::hardware::light::HwLight;

namespace aidl {
namespace android {
namespace hardware {
namespace light {

enum light_states {
    NOTIFICATION_STATE,
    ATTENTION_STATE,
    BATTERY_STATE,
    MAX_STATES,
};

class Lights : public BnLights {
public:
    Lights();

    ndk::ScopedAStatus setLightState(int32_t id, const HwLightState& state) override;
    ndk::ScopedAStatus getLights(std::vector<HwLight> *_aidl_return) override;
private:
    void setLED(const HwLightState& state);
    void setLEDState(const HwLightState& state, light_states idx);

    std::vector<HwLight> mLights;

    std::string mBacklightPath;
    std::vector<std::string> mButtonsPaths;
    bool mWhiteLED;

    std::mutex mLEDMutex;
    std::array<HwLightState, MAX_STATES> mLastLightStates;
};

} // namespace light
} // namespace hardware
} // namespace android
} // namespace aidl
