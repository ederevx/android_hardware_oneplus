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

import android.app.Activity
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.media.AudioManager
import android.util.Base64
import android.util.Log
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * Development-only QEM calibration ladder driver.
 *
 * Sends exactly ONE calibration frame per broadcast, so a topology x apptype x device matrix is
 * walked under explicit control: one id per broadcast, an explicit continue (the next broadcast),
 * and an abort (distinct result code + log line) if the HAL call does not return, so a wedged
 * setParameters is visible instead of silent. This receiver is never part of the shipped app's
 * normal behaviour; it only reacts to the explicit org.lineageos.dirac.qem.PROBE action.
 *
 *   adb shell am broadcast -a org.lineageos.dirac.qem.PROBE \
 *       -n org.lineageos.dirac.qem/.DiracProbeReceiver \
 *       --es topo 0x10012d00 --ei apptype 69936 --ei devid 15 --ei snddevid 0
 *
 * Extras (all optional except topo):
 *   topo      String "0x..." or decimal (an int extra is also accepted)   [required]
 *   apptype   int    application type                                     default 69936
 *   devid     int    ACDB device id -> "cal_devid=<n>"                     default 0
 *   snddevid  int    when > 0, ";cal_snddevid=<n>" is appended             default 0
 *   moduleid  int    frame module id                                      default 0x12d00
 *   paramid   int    frame parameter id                                   default 0x12d01
 *   rate      int    sample rate                                          default 48000
 *   persist   int    cal_persist                                          default 0
 *   data      String base64 payload, default frame(moduleid, paramid, int 1)
 *
 * Result codes: Activity.RESULT_OK when the HAL call returned, RESULT_CANCELED for bad arguments,
 * Activity.RESULT_FIRST_USER + 1 when the call did not return inside the watchdog.
 */
class DiracProbeReceiver : BroadcastReceiver() {

    override fun onReceive(context: Context, intent: Intent) {
        val pending = goAsync()
        val appContext = context.applicationContext
        Thread {
            var code = Activity.RESULT_CANCELED
            try {
                code = probe(appContext, intent)
            } catch (t: Throwable) {
                Log.e(TAG, "probe threw", t)
            } finally {
                try {
                    pending.setResultCode(code)
                } catch (t: Throwable) {
                    Log.e(TAG, "setResultCode failed", t)
                }
                pending.finish()
            }
        }.start()
    }

    private fun probe(context: Context, intent: Intent): Int {
        val topo =
            safeStringExtra(intent, EXTRA_TOPO)?.let { parseId(it) }
                ?: intent.getIntExtra(EXTRA_TOPO, -1).takeIf { it >= 0 }
        if (topo == null) {
            Log.e(TAG, "no usable 'topo' extra, nothing sent")
            return Activity.RESULT_CANCELED
        }

        val appType = intent.getIntExtra(EXTRA_APPTYPE, DEFAULT_APPTYPE)
        val devId = intent.getIntExtra(EXTRA_DEVID, 0)
        val sndDevId = intent.getIntExtra(EXTRA_SNDDEVID, 0)
        val moduleId = intent.getIntExtra(EXTRA_MODULEID, QemProtocol.MODULE_INTERNAL)
        val paramId = intent.getIntExtra(EXTRA_PARAMID, QemProtocol.PARAM_ENABLE)
        val rate = intent.getIntExtra(EXTRA_RATE, DEFAULT_RATE)
        val persist = intent.getIntExtra(EXTRA_PERSIST, 0)

        val data =
            safeStringExtra(intent, EXTRA_DATA)?.let {
                try {
                    Base64.decode(it, Base64.DEFAULT)
                } catch (t: Throwable) {
                    Log.e(TAG, "bad 'data' extra, using default", t)
                    null
                }
            } ?: QemProtocol.frame(moduleId, paramId, QemProtocol.intPayload(1))

        val selectors = StringBuilder("cal_devid=").append(devId)
        if (sndDevId > 0) {
            selectors.append(";cal_snddevid=").append(sndDevId)
        }
        val cal =
            "cal_caltype=0;cal_topoid=$topo;cal_apptype=$appType;cal_persist=$persist;" +
                "$selectors;cal_samplerate=$rate;cal_data=${QemProtocol.encode(data)}"

        val audioManager = context.getSystemService(AudioManager::class.java)
        if (audioManager == null) {
            Log.e(TAG, "no AudioManager")
            return Activity.RESULT_CANCELED
        }

        Log.i(TAG, "send $cal")
        val done = CountDownLatch(1)
        Thread {
            try {
                audioManager.setParameters(cal)
            } catch (t: Throwable) {
                Log.e(TAG, "setParameters threw", t)
            } finally {
                done.countDown()
            }
        }.start()

        val returned =
            try {
                done.await(WATCHDOG_SECONDS, TimeUnit.SECONDS)
            } catch (t: InterruptedException) {
                false
            }
        if (!returned) {
            Log.e(TAG, "ABORT: setParameters did not return within ${WATCHDOG_SECONDS}s (HAL wedged)")
            return RESULT_TIMEOUT
        }
        Log.i(TAG, "returned")
        return Activity.RESULT_OK
    }

    /** getStringExtra() throws on a non-string extra, so normalise that to null. */
    private fun safeStringExtra(intent: Intent, key: String): String? =
        try {
            intent.getStringExtra(key)
        } catch (t: Throwable) {
            null
        }

    private fun parseId(value: String?): Int? {
        val s = value?.trim()?.takeIf { it.isNotEmpty() } ?: return null
        return try {
            if (s.startsWith("0x") || s.startsWith("0X")) {
                s.substring(2).toLong(16).toInt()
            } else {
                s.toInt()
            }
        } catch (t: Throwable) {
            Log.e(TAG, "bad topo '$s'", t)
            null
        }
    }

    private companion object {
        const val TAG = "DiracQemProbe"
        const val EXTRA_TOPO = "topo"
        const val EXTRA_APPTYPE = "apptype"
        const val EXTRA_DEVID = "devid"
        const val EXTRA_SNDDEVID = "snddevid"
        const val EXTRA_MODULEID = "moduleid"
        const val EXTRA_PARAMID = "paramid"
        const val EXTRA_RATE = "rate"
        const val EXTRA_PERSIST = "persist"
        const val EXTRA_DATA = "data"
        const val DEFAULT_APPTYPE = 69936
        const val DEFAULT_RATE = 48000
        const val WATCHDOG_SECONDS = 4L
        val RESULT_TIMEOUT = Activity.RESULT_FIRST_USER + 1
    }
}
