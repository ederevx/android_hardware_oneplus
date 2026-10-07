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

import android.bluetooth.BluetoothProfile
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

/**
 * Tracks the output route and the movie-mode state the stock OnePlus Dirac
 * service also reacts to.
 */
class DiracStateReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        when (intent.action) {
            Intent.ACTION_HEADSET_PLUG -> {
                // The route is resolved from the jack's own state, so a plug or
                // unplug only has to re-run the push.
                DiracQemEffect.apply(context)
            }
            ACTION_A2DP_CONNECTION_STATE_CHANGED -> {
                val state = intent.getIntExtra(EXTRA_A2DP_STATE, BluetoothProfile.STATE_DISCONNECTED)
                DiracQemEffect.setBluetooth(context, state == BluetoothProfile.STATE_CONNECTED)
            }
            ACTION_MOVIES_STATE_CHANGED -> {
                DiracQemEffect.setMovie(context, intent.getBooleanExtra(EXTRA_MOVIES_STATE, false))
            }
        }
    }

    private companion object {
        const val ACTION_A2DP_CONNECTION_STATE_CHANGED =
            "android.bluetooth.a2dp.profile.action.CONNECTION_STATE_CHANGED"
        const val EXTRA_A2DP_STATE = "android.bluetooth.profile.extra.STATE"
        const val ACTION_MOVIES_STATE_CHANGED =
            "com.oem.intent.action.ACTION_MOVIES_STATE_CHANGED_ACTION"
        const val EXTRA_MOVIES_STATE = "com.oem.intent.action.MOVIES_STATE"
    }
}
