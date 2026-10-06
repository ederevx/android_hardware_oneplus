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

package org.lineageos.dirac.tone

import android.media.AudioDeviceInfo
import android.media.AudioManager

/**
 * Resolves which HAL sound device the tone is routed to, so a QEM frame names
 * the device the calibration will actually be applied to.
 *
 * These are snd_device_t values from the primary HAL (msm8974/platform.h), not
 * acdb device ids: the HAL derives the acdb id itself
 * (SND_DEVICE_OUT_SPEAKER -> 15, SND_DEVICE_OUT_HEADPHONES -> 10).
 */
object ToneTarget {
    const val SPEAKER = 2
    const val HEADPHONES = 9

    /** The connected output the media stream follows: wired or the speaker. */
    fun of(audioManager: AudioManager): Int =
        if (hasWiredHeadset(audioManager)) HEADPHONES else SPEAKER

    private fun hasWiredHeadset(audioManager: AudioManager): Boolean =
        audioManager.getDevices(AudioManager.GET_DEVICES_OUTPUTS).any {
            it.type == AudioDeviceInfo.TYPE_WIRED_HEADSET ||
                it.type == AudioDeviceInfo.TYPE_WIRED_HEADPHONES
        }
}
