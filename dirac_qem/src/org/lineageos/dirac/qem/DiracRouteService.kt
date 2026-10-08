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

import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.IBinder

/**
 * Holds the one route registration for as long as the device is up.
 *
 * ACTION_HEADSET_PLUG is a registered-only broadcast, so a live process has to
 * own the registration; the route is reconciled on every start, which covers a
 * jack that was plugged while this process was down.
 */
class DiracRouteService : Service() {
    private val receiver = DiracStateReceiver()
    private val volumeObserver = DiracVolumeObserver(this)

    override fun onCreate() {
        super.onCreate()
        registerReceiver(receiver, DiracStateReceiver.intentFilter(), Context.RECEIVER_NOT_EXPORTED)
        // The effect is off unless the fallback ships, so the volume observer
        // rides this long-lived process only when there is a consumer for it.
        volumeObserver.start()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        DiracQemEffect.apply(this)
        return START_STICKY
    }

    override fun onDestroy() {
        volumeObserver.stop()
        unregisterReceiver(receiver)
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    companion object {
        fun start(context: Context) {
            context.startService(Intent(context, DiracRouteService::class.java))
        }
    }
}
