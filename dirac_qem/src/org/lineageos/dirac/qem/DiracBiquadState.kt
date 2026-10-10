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

import android.os.IBinder
import android.os.RemoteException
import android.os.ServiceManager
import org.lineageos.dirac.biquad.IDiracBiquadState

/**
 * Carries a fully formed [DiracState] payload to the daemon that owns the
 * effect's config file, and reads the daemon's copy back for verification.
 *
 * This class composes no state of its own: the app's values live in
 * [DiracState], and every send is the payload it hands over. A missing or
 * failing service must never fail the audio path, so every call degrades to a
 * false/null result.
 */
object DiracBiquadState {
    private const val SERVICE_NAME = "dirac_biquad_state"

    /** The daemon's copy of the state, read back for [DiracState.assertState]. */
    class Reading(
        val enabled: Boolean,
        val fallback: Boolean,
        val widthOwnerHost: Boolean,
        val sumDiff: Float,
        val bandsHalfDb: IntArray,
        val volumeDb: Double,
    )

    /** Sends one payload; true only when the daemon accepted it. */
    fun send(
        enabled: Boolean,
        fallback: Boolean,
        widthOwnerHost: Boolean,
        sumdiff: Float,
        bandsHalfDb: IntArray,
        volumeDb: Double,
    ): Boolean {
        return try {
            val binder: IBinder = ServiceManager.getService(SERVICE_NAME) ?: return false
            IDiracBiquadState.Stub.asInterface(binder).setState(
                enabled,
                fallback,
                widthOwnerHost,
                sumdiff,
                bandsHalfDb,
                volumeDb,
            )
            true
        } catch (e: RemoteException) {
            DiracTrace.log(TAG) { "send failed: ${e.message}" }
            false
        } catch (e: NullPointerException) {
            DiracTrace.log(TAG) { "send failed: ${e.message}" }
            false
        }
    }

    /** The state the daemon last persisted, or null when it has none or is absent. */
    fun read(): Reading? {
        return try {
            val binder: IBinder = ServiceManager.getService(SERVICE_NAME) ?: return null
            val service = IDiracBiquadState.Stub.asInterface(binder)
            if (!service.hasState()) {
                return null
            }
            val bands = service.bandsHalfDb ?: return null
            Reading(
                service.enabled,
                service.fallback,
                service.widthOwnerHost,
                service.sumDiff,
                bands,
                service.volumeDb,
            )
        } catch (e: RemoteException) {
            DiracTrace.log(TAG) { "read failed: ${e.message}" }
            null
        } catch (e: NullPointerException) {
            DiracTrace.log(TAG) { "read failed: ${e.message}" }
            null
        }
    }

    private const val TAG = "DiracBiquadState"
}
