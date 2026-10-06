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

import android.content.Intent
import android.media.AudioAttributes
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.util.Log

/**
 * Resolves the requested output device and stream usage from the intent, so a
 * run can be aimed at one output and one usage over adb without a HAL or
 * framework change.
 *
 * No extras keep today's behaviour: a USAGE_MEDIA tone on the default output.
 * A requested device type that is not currently attached leaves the track on
 * the default output and logs the miss.
 */
class ToneRoute private constructor(
    val device: AudioDeviceInfo?,
    private val usage: Int,
    private val description: String,
) {
    fun audioAttributes(): AudioAttributes = AudioAttributes.Builder()
        .setUsage(usage)
        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
        .build()

    override fun toString(): String = description

    private enum class Output(val extra: String, val type: Int, val label: String) {
        SPEAKER("speaker", AudioDeviceInfo.TYPE_BUILTIN_SPEAKER, "TYPE_BUILTIN_SPEAKER"),
        HEADSET("headset", AudioDeviceInfo.TYPE_WIRED_HEADSET, "TYPE_WIRED_HEADSET"),
        EARPIECE("earpiece", AudioDeviceInfo.TYPE_BUILTIN_EARPIECE, "TYPE_BUILTIN_EARPIECE");

        fun find(audioManager: AudioManager): AudioDeviceInfo? =
            audioManager.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
                .firstOrNull { it.type == type }

        companion object {
            fun from(raw: String?): Output? = values().firstOrNull { it.extra == raw }
        }
    }

    private enum class Usage(val extra: String, val value: Int, val label: String) {
        MEDIA("media", AudioAttributes.USAGE_MEDIA, "USAGE_MEDIA"),
        ALARM("alarm", AudioAttributes.USAGE_ALARM, "USAGE_ALARM");

        companion object {
            fun from(raw: String?): Usage? = values().firstOrNull { it.extra == raw }
        }
    }

    companion object {
        fun of(intent: Intent?, audioManager: AudioManager): ToneRoute {
            val deviceExtra = intent?.getStringExtra(EXTRA_DEVICE)
            val usageExtra = intent?.getStringExtra(EXTRA_USAGE)
            val output = Output.from(deviceExtra)
            val usage = Usage.from(usageExtra)
            if (deviceExtra != null && output == null) {
                Log.w(TAG, "unknown device '$deviceExtra', using the default output")
            }
            if (usageExtra != null && usage == null) {
                Log.w(TAG, "unknown usage '$usageExtra', using USAGE_MEDIA")
            }
            val device = output?.find(audioManager)
            if (output != null && device == null) {
                Log.w(TAG, "${output.label} is not present, using the default output")
            }
            val applied = if (output != null && device != null) {
                "${output.label}(${device.id})"
            } else {
                "default"
            }
            val selected = usage ?: Usage.MEDIA
            val description = "output=$applied usage=${selected.label}"
            Log.i(TAG, "route: $description")
            return ToneRoute(device, selected.value, description)
        }

        private const val TAG = "DiracToneProbe"
        private const val EXTRA_DEVICE = "device"
        private const val EXTRA_USAGE = "usage"
    }
}
