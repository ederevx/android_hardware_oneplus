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
import android.content.Intent
import android.content.IntentFilter
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.util.Log

/**
 * The one owner of the live output the user hears, and of what that output
 * means for the widening.
 *
 * The sink is read, never remembered: every call re-reads the platform, so a
 * page opened while a route is already live still sees it, and a connection
 * this process was not running for is never missed. [DiracState] keeps a copy
 * only to know when a change is worth telling a live page about.
 *
 * The two sinks need different sources, which is why neither the policy's
 * device list nor a broadcast is enough on its own. The audio policy's device
 * list is not a connection source for the wired sinks on this platform:
 * AudioPolicyManager attaches every output device the HAL declares to its
 * available-output set as the HW module loads, so AudioManager.isWiredHeadsetOn()
 * and getDevices(GET_DEVICES_OUTPUTS) both report the wired headset with
 * nothing in the jack; the sticky HEADSET_PLUG broadcast that AudioService's
 * AudioDeviceInventory sends on every wired connect and disconnect is the true
 * jack state. A host sink is the opposite case: a Bluetooth output device
 * exists only while a device is connected, so its presence in that same list is
 * the connection signal, while the profile state would need a second source --
 * and its broadcast is not delivered to a process that was not running.
 */
object DiracRouteResolver {
    private const val TAG = "DiracQemRoute"
    private const val EXTRA_STATE = "state"
    private const val EXTRA_MICROPHONE = "microphone"

    /**
     * The platform audio devices the sinks are, as the HAL itself names them
     * (AUDIO_DEVICE_OUT_*, system/media/audio/include/system/audio-base-utils.h).
     * A calibration frame carries one of these and the HAL resolves it to its
     * own sound device and ACDB device, which is the only place that knows
     * which sound device a given stream needs; the app names no sound device.
     */
    private const val AUDIO_DEVICE_OUT_SPEAKER = 0x2
    private const val AUDIO_DEVICE_OUT_WIRED_HEADSET = 0x4
    private const val AUDIO_DEVICE_OUT_WIRED_HEADPHONE = 0x8
    private const val AUDIO_DEVICE_OUT_BLUETOOTH_A2DP = 0x80

    private val WIRED_SINK_TYPES = intArrayOf(
        AudioDeviceInfo.TYPE_WIRED_HEADSET,
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES,
    )

    /**
     * The live output. The route carries what it means, so the app's questions
     * about it -- whether the DSP voices the widening, which ADSP module the
     * frames address -- are answered next to the route instead of by a second
     * copy of the same knowledge somewhere else.
     */
    enum class Sink {
        /** The on-device sinks, speaker first among them. */
        SPEAKER,

        /** The wired jack. */
        WIRED,

        /**
         * Every sink the Dirac DSP does not voice. Bluetooth A2DP is the one this
         * platform reports honestly: its output device appears only while
         * connected, unlike the wired sinks above, which the policy lists whether
         * or not anything is plugged. The other host outputs - USB, the remote
         * submix - are not detected yet, and classify as SPEAKER.
         */
        HOST;

        /**
         * Whether the Dirac DSP voices this sink at all, so the ADSP frames can
         * land on it: the biquad effect gates itself off for speaker, wired
         * headset, wired headphone and line, owns the widening on the sinks it
         * does not gate, and on those same sinks no Dirac topology has a live
         * stream for a frame to reach.
         */
        val dspVoiced: Boolean get() = this != HOST
    }

    /** Reads the live sink. */
    fun sink(context: Context): Sink = route(context).sink

    /**
     * The platform audio device of the live sink, for the frames that address
     * it. It comes from the same resolution as [sink], so the sink the app
     * describes and the device it names can never disagree.
     */
    fun outputDevice(context: Context): Int = route(context).audioDevice

    /** The live sink and the platform audio device it is, resolved once. */
    private class Route(val sink: Sink, val audioDevice: Int)

    private fun route(context: Context): Route {
        val sticky = stickyJack(context)
        val plugged = sticky?.getIntExtra(EXTRA_STATE, 0)?.let { it != 0 } ?: false
        val host = !plugged && bluetoothSinkAttached(context)
        val route = when {
            plugged -> Route(Sink.WIRED, wiredAudioDevice(sticky))
            host -> Route(Sink.HOST, AUDIO_DEVICE_OUT_BLUETOOTH_A2DP)
            else -> Route(Sink.SPEAKER, AUDIO_DEVICE_OUT_SPEAKER)
        }
        // Information, not debug: this is the decision the settings row and the
        // HAL push both follow, and it is cheap only on a route event.
        Log.i(
            TAG,
            "sink=${route.sink} device=${route.audioDevice} jack_plugged=$plugged " +
                "bluetooth_sink=$host wired_available=[${wiredCandidates(context)}]",
        )
        return route
    }

    /**
     * Which wired audio device the jack is. Both resolve to the same platform
     * sound device, and that resolution belongs to the HAL: a headset with a
     * microphone is AUDIO_DEVICE_OUT_WIRED_HEADSET, a plain headphone is
     * AUDIO_DEVICE_OUT_WIRED_HEADPHONE.
     */
    private fun wiredAudioDevice(sticky: Intent?): Int =
        if (sticky?.getBooleanExtra(EXTRA_MICROPHONE, false) == true) {
            AUDIO_DEVICE_OUT_WIRED_HEADSET
        } else {
            AUDIO_DEVICE_OUT_WIRED_HEADPHONE
        }

    /**
     * The jack's own connection signal, read once per resolution. A platform
     * that refuses the sticky read, or has not sent one since boot, leaves the
     * sink unwired, which is the safe answer for a jack with no known state.
     */
    private fun stickyJack(context: Context): Intent? = try {
        context.registerReceiver(null, IntentFilter(Intent.ACTION_HEADSET_PLUG))
    } catch (e: Exception) {
        null
    }

    /**
     * Whether a Bluetooth A2DP sink is attached: the one host output this
     * platform reports honestly. Its device is created on connection and torn
     * down on disconnect, unlike the wired sinks above.
     */
    fun bluetoothSinkAttached(context: Context): Boolean =
        outputDevices(context).any { it.type == AudioDeviceInfo.TYPE_BLUETOOTH_A2DP }

    private fun outputDevices(context: Context): List<AudioDeviceInfo> =
        context.getSystemService(AudioManager::class.java)
            ?.getDevices(AudioManager.GET_DEVICES_OUTPUTS)?.toList() ?: emptyList()

    /**
     * The wired sink devices the policy reports as available. They are logged
     * only so the device check can see that the list is not the connection
     * signal; it carries the wired headset with nothing plugged.
     */
    private fun wiredCandidates(context: Context): String =
        outputDevices(context).filter { it.type in WIRED_SINK_TYPES }
            .joinToString(",") { "type=${it.type}" }
            .ifEmpty { "none" }
}
