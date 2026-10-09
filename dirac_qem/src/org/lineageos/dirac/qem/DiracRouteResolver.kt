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
 * jack state. The Bluetooth sink is the opposite case: an A2DP output device
 * exists only while a device is connected, so its presence in that same list is
 * the connection signal, while the profile state would need a second source --
 * and its broadcast is not delivered to a process that was not running.
 */
object DiracRouteResolver {
    private const val TAG = "DiracQemRoute"
    private const val EXTRA_STATE = "state"

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

        /** A2DP, and any other sink the Dirac DSP does not voice. */
        BLUETOOTH;

        /**
         * Whether the Dirac DSP voices the widening here, so the host
         * StereoWidth stage must stay off: the biquad effect gates itself off
         * for speaker, wired headset, wired headphone and line, and owns the
         * widening on the sinks it does not gate.
         */
        val dspVoicesWidening: Boolean get() = this != BLUETOOTH
    }

    /** Reads the live sink. */
    fun sink(context: Context): Sink {
        val plugged = wiredPlugged(context)
        val bluetooth = !plugged && bluetoothAttached(context)
        val sink = when {
            plugged -> Sink.WIRED
            bluetooth -> Sink.BLUETOOTH
            else -> Sink.SPEAKER
        }
        // Information, not debug: this is the decision the settings row and the
        // HAL push both follow, and it is cheap only on a route event.
        Log.i(
            TAG,
            "sink=$sink jack_plugged=$plugged a2dp_attached=$bluetooth " +
                "wired_available=[${wiredCandidates(context)}]",
        )
        return sink
    }

    /**
     * The jack's own connection signal. A platform that refuses the sticky
     * read, or has not sent one since boot, leaves the sink unwired, which is
     * the safe answer for a jack with no known state.
     */
    private fun wiredPlugged(context: Context): Boolean {
        val sticky = try {
            context.registerReceiver(null, IntentFilter(Intent.ACTION_HEADSET_PLUG))
        } catch (e: Exception) {
            null
        } ?: return false
        return sticky.getIntExtra(EXTRA_STATE, 0) != 0
    }

    /**
     * Whether an A2DP sink is attached. Its output device is created on
     * connection and torn down on disconnect, unlike the wired sinks above.
     */
    private fun bluetoothAttached(context: Context): Boolean =
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
