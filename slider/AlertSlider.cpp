/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "AlertSliderHAL"

#include "AlertSlider.h"

#include <android-base/logging.h>
#include <cutils/uevent.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace aidl::vendor::lineage::slider {

namespace {
// Slider positions, matching the framework-facing convention.
constexpr int32_t kPositionTop = 1;
constexpr int32_t kPositionMiddle = 2;
constexpr int32_t kPositionBottom = 3;

bool isTriStateName(const char* name) {
    return strcmp(name, "tri-state-key") == 0 || strcmp(name, "tri_state_key") == 0;
}
}  // namespace

AlertSlider::AlertSlider() {
    mThread = std::thread(&AlertSlider::ueventLoop, this);
}

AlertSlider::~AlertSlider() {
    mStop = true;
    if (mThread.joinable()) {
        mThread.join();
    }
}

ndk::ScopedAStatus AlertSlider::registerCallback(
        const std::shared_ptr<IAlertSliderCallback>& callback, int32_t* _aidl_return) {
    if (callback == nullptr) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    std::lock_guard<std::mutex> lock(mLock);
    mCallback = callback;
    *_aidl_return = mState.load();
    return ndk::ScopedAStatus::ok();
}

void AlertSlider::ueventLoop() {
    int fd = uevent_open_socket(64 * 1024, true);
    if (fd < 0) {
        LOG(ERROR) << "uevent_open_socket failed";
        return;
    }
    std::string buffer(64 * 1024, '\0');
    while (!mStop) {
        int len = uevent_kernel_multicast_recv(fd, buffer.data(), buffer.size() - 1);
        if (len <= 0) {
            continue;
        }
        buffer[len] = '\0';
        handleUEvent(buffer.data(), len);
    }
    close(fd);
}

// Two driver families expose the same physical slider:
//  - switch class (msm8998 tri_state_key): SWITCH_NAME=tri-state-key, SWITCH_STATE=1|2|3
//  - extcon (sdm845): NAME=tri-state-key, STATE=<USB=..>; <HOST=..>; <null)=..>
// Both feed the same framework-facing 1|2|3 convention; no platform semantics are assumed.
void AlertSlider::handleUEvent(const char* buf, int len) {
    const char* switchName = nullptr;
    const char* name = nullptr;
    const char* switchState = nullptr;
    const char* state = nullptr;

    for (const char* p = buf; p < buf + len && *p; p += strlen(p) + 1) {
        if (strncmp(p, "SWITCH_NAME=", 12) == 0) {
            switchName = p + 12;
        } else if (strncmp(p, "SWITCH_STATE=", 13) == 0) {
            switchState = p + 13;
        } else if (strncmp(p, "NAME=", 5) == 0) {
            name = p + 5;
        } else if (strncmp(p, "STATE=", 6) == 0) {
            state = p + 6;
        }
    }

    // switch-class form
    if (switchName != nullptr && switchState != nullptr && isTriStateName(switchName)) {
        int32_t value = atoi(switchState);
        if (value >= kPositionTop && value <= kPositionBottom) {
            updateState(value);
        }
        return;
    }

    // extcon form
    if (name != nullptr && state != nullptr && isTriStateName(name)) {
        bool none = strstr(state, "USB=0") != nullptr;
        bool vibration = strstr(state, "HOST=0") != nullptr;
        bool silent = strstr(state, "null)=0") != nullptr;

        int32_t value = 0;
        if (none && !vibration && !silent) {
            value = kPositionBottom;
        } else if (!none && vibration && !silent) {
            value = kPositionMiddle;
        } else if (!none && !vibration && silent) {
            value = kPositionTop;
        }
        if (value != 0) {
            updateState(value);
        }
    }
}

void AlertSlider::updateState(int32_t state) {
    mState = state;
    std::lock_guard<std::mutex> lock(mLock);
    if (mCallback != nullptr) {
        mCallback->onStateChanged(state);
    }
}

}  // namespace aidl::vendor::lineage::slider
