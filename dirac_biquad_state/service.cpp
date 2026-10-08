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

#include <aidl/org/lineageos/dirac/biquad/BnDiracBiquadState.h>
#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "DiracBiquadConf.h"

using aidl::org::lineageos::dirac::biquad::BnDiracBiquadState;

namespace {

constexpr const char *kServiceName = "dirac_biquad_state";

class DiracBiquadStateService : public BnDiracBiquadState {
  public:
    ndk::ScopedAStatus setState(bool enabled, bool fallback,
                                const std::vector<int32_t> &bandsHalfDb,
                                double volumeDb) override {
        if (!dirac::DiracBiquadConf::Write(enabled, fallback, bandsHalfDb, volumeDb)) {
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
        return ndk::ScopedAStatus::ok();
    }
};

}  // namespace

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(0);
    std::shared_ptr<DiracBiquadStateService> service =
            ndk::SharedRefBase::make<DiracBiquadStateService>();

    const std::string instance = kServiceName;
    binder_status_t status = AServiceManager_addService(service->asBinder().get(), instance.c_str());
    CHECK_EQ(status, STATUS_OK) << "Failed to add service " << instance << ": " << status;

    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;  // should not reach
}
