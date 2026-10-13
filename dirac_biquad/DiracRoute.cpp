/*
 * Copyright (C) 2026 The LineageOS Project
 * Copyright (C) 2026 Edrick Sinsuan <evcsinsuan@gmail.com>
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

#include "DiracRoute.h"

namespace dirac_route {

// The wired speaker and headset outputs are voiced by the ADSP Dirac topology
// (ACDB devices 15 and 10), so the host chain stays out of them. Everything else
// bypasses the ADM and is ours: Bluetooth A2DP, the remote submix that Android
// Auto and other projection consumers use, USB, and the rest.
//
// The value the framework hands us may be the OR of several patch sinks, and
// since Android R an audio device type is an exclusive value that must not be
// used as a mask - system/media/audio/include/system/audio.h says exactly that on
// audio_is_output_devices. The two rules cannot both be satisfied by a bit test:
// AUDIO_DEVICE_OUT_BLE_BROADCAST (0x20000002) carries the speaker's bit and
// AUDIO_DEVICE_OUT_BLE_SPEAKER (0x20000001) the earpiece's, so a Bluetooth LE
// route read as a set of sink bits is indistinguishable from a duplicated
// speaker output. So the test is membership of the DSP set by whole value, using
// the framework's own audio_is_output_device() for "is this exactly one output
// type", and anything else - a combination, or an encoding this effect does not
// know - is left to the DSP. That is the directive's rule too: a route whose
// Dirac support is unknown must not be voiced here, because voicing a route the
// ADSP already voices is the failure this gate exists to prevent.
//
// The set itself is route data this effect does not own: which ACDB device the
// ADSP Dirac modules are registered for is a HAL and calibration property. The
// HAL-published form of it is what this predicate should read once that data
// exists; until then the set is written out here. It includes the safe-speaker
// type, which is the same physical speaker reached through the volume limit.
bool IsDspOutput(audio_devices_t device) {
    static const audio_devices_t kDspDevices[] = {
            AUDIO_DEVICE_OUT_SPEAKER,
            AUDIO_DEVICE_OUT_SPEAKER_SAFE,
            AUDIO_DEVICE_OUT_WIRED_HEADSET,
            AUDIO_DEVICE_OUT_WIRED_HEADPHONE,
            AUDIO_DEVICE_OUT_LINE,
    };
    for (const audio_devices_t candidate : kDspDevices) {
        if (device == candidate) {
            return true;
        }
    }
    // Exactly one output type, and not one of the DSP's: the ADM does not carry
    // it, so it is ours to voice.
    if (audio_is_output_device(device)) {
        return false;
    }
    return true;
}

}  // namespace dirac_route
