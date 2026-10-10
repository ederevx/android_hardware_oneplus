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

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter

/**
 * Tracks the output route and the movie-mode state the stock OnePlus Dirac
 * service also reacts to.
 *
 * [DiracRouteService] owns the runtime registration the jack event needs; this
 * class stays the single owner of the action list.
 */
class DiracStateReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        when (intent.action) {
            Intent.ACTION_HEADSET_PLUG, ACTION_A2DP_CONNECTION_STATE_CHANGED -> {
                // Both are only a trigger: the pass re-reads the live output
                // from the platform, so neither carries the state itself and a
                // connection that happened while this process was down is read
                // on the next one rather than guessed from an extra.
                DiracQemEffect.apply(context)
            }
            ACTION_MOVIES_STATE_CHANGED -> {
                DiracState.setMovie(context, intent.getBooleanExtra(EXTRA_MOVIES_STATE, false))
                DiracQemEffect.setMovie(context)
            }
        }
    }

    companion object {
        fun intentFilter(): IntentFilter = IntentFilter().apply {
            addAction(Intent.ACTION_HEADSET_PLUG)
            addAction(ACTION_A2DP_CONNECTION_STATE_CHANGED)
            addAction(ACTION_MOVIES_STATE_CHANGED)
        }

        const val ACTION_A2DP_CONNECTION_STATE_CHANGED =
            "android.bluetooth.a2dp.profile.action.CONNECTION_STATE_CHANGED"
        const val ACTION_MOVIES_STATE_CHANGED =
            "com.oem.intent.action.ACTION_MOVIES_STATE_CHANGED_ACTION"
        const val EXTRA_MOVIES_STATE = "com.oem.intent.action.MOVIES_STATE"
    }
}
