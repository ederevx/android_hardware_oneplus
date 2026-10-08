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

package org.lineageos.dirac.qem

import android.content.Context
import android.database.ContentObserver
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.provider.Settings

/**
 * Observes the STREAM_MUSIC volume and publishes the matching stream
 * attenuation to the host-side effect.
 *
 * The effect's descriptor carries no EFFECT_FLAG_VOLUME_IND and the framework
 * never sends EFFECT_CMD_SET_VOLUME for it, so the attenuation cannot reach the
 * effect through the effect API. Instead this observer watches the volume
 * setting, resolves the active output device, asks AudioManager for the true
 * frame attenuation and republishes the state the effect reads.
 *
 * The attenuation is the positive number of dB below the reference:
 * `getStreamVolumeDb` returns the framework gain (0 dB at the maximum index,
 * negative below it), so the sign is flipped. A missing device or a failed
 * query publishes the unknown sentinel, never 0.
 *
 * Lifecycle: [start] registers and [stop] unregisters; both are idempotent. The
 * observer does nothing unless the build ships the A2DP fallback, and it is
 * throttled so a volume ramp cannot flood the state file with writes.
 */
class DiracVolumeObserver(context: Context) {
    private val appContext = context.applicationContext ?: context
    private val audioManager = appContext.getSystemService(AudioManager::class.java)
    private val handler = Handler(Looper.getMainLooper())

    private var started = false
    private var lastPublishMs = 0L
    private var publishScheduled = false
    private var lastAttenuationDb = Double.NaN

    private val volumeObserver = object : ContentObserver(handler) {
        override fun onChange(selfChange: Boolean) {
            schedule()
        }
    }

    private val deviceCallback = object : AudioDeviceCallback() {
        override fun onAudioDevicesAdded(addedDevices: Array<out AudioDeviceInfo>) {
            schedule()
        }

        override fun onAudioDevicesRemoved(removedDevices: Array<out AudioDeviceInfo>) {
            schedule()
        }
    }

    private val publishRunnable = Runnable {
        publishScheduled = false
        lastPublishMs = SystemClock.uptimeMillis()
        publish()
    }

    fun start() {
        if (started || audioManager == null ||
            !DiracQemEffect.isA2dpFallbackAvailable(appContext)) {
            return
        }
        started = true
        appContext.contentResolver.registerContentObserver(volumeUri(), false, volumeObserver)
        audioManager.registerAudioDeviceCallback(deviceCallback, handler)
        publish()
    }

    fun stop() {
        if (!started) {
            return
        }
        started = false
        handler.removeCallbacks(publishRunnable)
        publishScheduled = false
        appContext.contentResolver.unregisterContentObserver(volumeObserver)
        audioManager?.unregisterAudioDeviceCallback(deviceCallback)
    }

    /** Rate-limits to one publish per [THROTTLE_MS], without starving a ramp. */
    private fun schedule() {
        val elapsed = SystemClock.uptimeMillis() - lastPublishMs
        if (elapsed >= THROTTLE_MS) {
            handler.removeCallbacks(publishRunnable)
            publishScheduled = false
            publishRunnable.run()
        } else if (!publishScheduled) {
            publishScheduled = true
            handler.postDelayed(publishRunnable, THROTTLE_MS - elapsed)
        }
    }

    private fun publish() {
        val attenuationDb = currentAttenuationDb() ?: DiracBiquadState.UNKNOWN_VOLUME_DB
        if (attenuationDb == lastAttenuationDb) {
            return
        }
        lastAttenuationDb = attenuationDb
        DiracBiquadState.publish(appContext, attenuationDb)
    }

    /**
     * The attenuation in dB below the reference, or null when it cannot be
     * resolved. Muting reports -infinity; the effect is silent then, so the
     * largest finite attenuation is published instead.
     */
    private fun currentAttenuationDb(): Double? {
        val manager = audioManager ?: return null
        val device = activeOutputDevice(manager) ?: return null
        val index = manager.getStreamVolume(AudioManager.STREAM_MUSIC)
        return try {
            val attenuation = -manager.getStreamVolumeDb(
                AudioManager.STREAM_MUSIC,
                index,
                device.type,
            ).toDouble()
            if (attenuation.isFinite()) {
                attenuation.coerceIn(0.0, MAX_ATTENUATION_DB)
            } else {
                MAX_ATTENUATION_DB
            }
        } catch (e: IllegalArgumentException) {
            DiracTrace.log(TAG) { "volume db failed: ${e.message}" }
            null
        }
    }

    /**
     * The output device the media volume currently follows. The preference
     * order matches how the route rises: Bluetooth, then the wired sinks, then
     * the speaker.
     */
    private fun activeOutputDevice(manager: AudioManager): AudioDeviceInfo? {
        val devices = manager.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
        return devices.firstOrNull { it.type == AudioDeviceInfo.TYPE_BLUETOOTH_A2DP }
            ?: devices.firstOrNull {
                it.type == AudioDeviceInfo.TYPE_WIRED_HEADSET ||
                    it.type == AudioDeviceInfo.TYPE_WIRED_HEADPHONES
            }
            ?: devices.firstOrNull { it.type == AudioDeviceInfo.TYPE_BUILTIN_SPEAKER }
            ?: devices.firstOrNull()
    }

    private fun volumeUri() =
        Settings.System.getUriFor(Settings.System.VOLUME_SETTINGS[AudioManager.STREAM_MUSIC])

    private companion object {
        const val TAG = "DiracVolumeObserver"

        /** One publish per this interval; the effect quantises to 0.1 dB. */
        const val THROTTLE_MS = 100L

        /** The loudness law is flat past this; it only bounds a mute. */
        const val MAX_ATTENUATION_DB = 120.0
    }
}
