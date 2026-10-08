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
import android.os.IBinder
import android.os.RemoteException
import android.os.ServiceManager
import org.lineageos.dirac.biquad.IDiracBiquadState

/**
 * Hands the Dirac biquad fallback state to the host-side effect.
 *
 * The effect runs outside the app and cannot share memory with it, so the app
 * sends the state to the service that owns the effect's config file. The app
 * holds every value, so nothing has to be intercepted in the audio HAL.
 *
 * Sending is inert unless this build ships the effect, matching the switch
 * itself: a product without the effect sends nothing for a consumer that does
 * not exist. A missing or failing service must never fail the audio path.
 */
object DiracBiquadState {
    private const val SERVICE_NAME = "dirac_biquad_state"

    fun publish(context: Context) {
        if (!DiracQemEffect.isA2dpFallbackAvailable(context)) {
            return
        }
        try {
            val binder: IBinder = ServiceManager.getService(SERVICE_NAME) ?: return
            IDiracBiquadState.Stub.asInterface(binder).setState(
                DiracQemEffect.isEnabled(context),
                DiracQemEffect.isA2dpFallbackEnabled(context),
                DiracQemEffect.currentBands(context),
            )
        } catch (e: RemoteException) {
            DiracTrace.log(TAG) { "publish failed: ${e.message}" }
        } catch (e: NullPointerException) {
            DiracTrace.log(TAG) { "publish failed: ${e.message}" }
        }
    }

    private const val TAG = "DiracBiquadState"
}
