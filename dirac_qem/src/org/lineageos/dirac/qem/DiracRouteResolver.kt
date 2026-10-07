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
 * Decides whether the wired output is live, from the jack's own connection
 * signal only.
 *
 * The audio policy's device list is not a connection source on this platform:
 * AudioPolicyManager attaches every output device the HAL declares to its
 * available-output set as the HW module loads, and AudioManager.isWiredHeadsetOn()
 * and getDevices(GET_DEVICES_OUTPUTS) both read that set, so they report the
 * wired headset with nothing in the jack. The sticky HEADSET_PLUG broadcast
 * that AudioService's AudioDeviceInventory sends on every wired connect and
 * disconnect carries the true jack state; it is the only input here. Nothing
 * is remembered from an earlier resolution.
 */
object DiracRouteResolver {
    private const val TAG = "DiracQemRoute"
    private const val EXTRA_STATE = "state"

    private val WIRED_SINK_TYPES = intArrayOf(
        AudioDeviceInfo.TYPE_WIRED_HEADSET,
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES,
    )

    fun resolve(context: Context): Int {
        val plugged = wiredPlugged(context)
        val route = if (plugged) DiracQemEffect.OUTPUT_EXTERNAL else DiracQemEffect.OUTPUT_INTERNAL
        Log.d(
            TAG,
            "considered=[${wiredCandidates(context)}] plugged=$plugged " +
                "route=${if (plugged) "external" else "internal"}",
        )
        return route
    }

    private fun wiredPlugged(context: Context): Boolean {
        // A platform that refuses the sticky read, or has not sent one since
        // boot, leaves the route internal, which is the safe answer for a jack
        // with no known state.
        val sticky = try {
            context.registerReceiver(null, IntentFilter(Intent.ACTION_HEADSET_PLUG))
        } catch (e: Exception) {
            null
        } ?: return false
        return sticky.getIntExtra(EXTRA_STATE, 0) != 0
    }

    /**
     * The wired sink devices the policy reports as available. They are logged
     * only so the device check can see that the list is not the connection
     * signal; it carries the wired headset with nothing plugged.
     */
    private fun wiredCandidates(context: Context): String {
        val devices = context.getSystemService(AudioManager::class.java)
            ?.getDevices(AudioManager.GET_DEVICES_OUTPUTS) ?: return "none"
        return devices.filter { it.type in WIRED_SINK_TYPES }
            .joinToString(",") { "type=${it.type}" }
            .ifEmpty { "none" }
    }
}
