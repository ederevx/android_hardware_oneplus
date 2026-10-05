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
 * Default mode sends exactly ONE calibration frame per broadcast, so a topology x apptype x device
 * matrix is walked under explicit control: one id per broadcast, an explicit continue (the next
 * broadcast), and an abort (distinct result code + log line) if the HAL call does not return, so a
 * wedged setParameters is visible instead of silent.
 *
 *   adb shell am broadcast -a org.lineageos.dirac.qem.PROBE \
 *       -n org.lineageos.dirac.qem/.DiracProbeReceiver \
 *       --es topo 0x10012d00 --ei apptype 69936 --ei devid 15 --ei snddevid 0
 *
 * mode=stockseq reproduces the stock se.dirac.acs ORDER instead: it READS first, then writes.
 *   (a) getParameters "cal_persist=0;cal_topoid=268512512;cal_moduleid=77056;cal_paramid=77056;cal_caltype=0;cal_data=0"
 *   (b) getParameters the scalar slots 81920..81929 (stock Controller ctor probes with cal_persist=1)
 *   (c) setParameters the stock enable frame "cal_caltype=0;cal_topoid=268512512;cal_apptype=69936;cal_persist=0;cal_devid=2;cal_samplerate=48000;cal_data=<b64>"
 *   adb shell am broadcast -a org.lineageos.dirac.qem.PROBE \
 *       -n org.lineageos.dirac.qem/.DiracProbeReceiver --es mode stockseq
 *
 * Result codes: Activity.RESULT_OK when the calls returned, RESULT_CANCELED for bad arguments,
 * Activity.RESULT_FIRST_USER + 1 when a setParameters did not return inside the watchdog.
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
        val audioManager = context.getSystemService(AudioManager::class.java)
        if (audioManager == null) {
            Log.e(TAG, "no AudioManager")
            return Activity.RESULT_CANCELED
        }

        val mode = safeStringExtra(intent, EXTRA_MODE)
        if (mode == MODE_STOCKSEQ) {
            return stockSequence(audioManager)
        }

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

        Log.i(TAG, "send $cal")
        if (!sendWatched(audioManager, cal)) {
            return RESULT_TIMEOUT
        }
        return Activity.RESULT_OK
    }

    /**
     * The exact stock se.dirac.acs ordering: a module-version READ, then the scalar-slot probes,
     * then one enable WRITE. Every reply is logged verbatim before it is parsed.
     */
    private fun stockSequence(audioManager: AudioManager): Int {
        val topo = TOPO_INTERNAL
        val moduleId = QemProtocol.MODULE_INTERNAL

        Log.i(TAG, "stockseq: start (topo=$topo module=$moduleId)")

        // (a) module version readback: stock c.a.e.f.k.a() -> b.a(false, 77056), persist=0.
        readMatrix(audioManager, "a.version", 0, topo, moduleId, VERSION_PARAM)

        // (b) scalar-slot probes: stock Controller ctor -> b.a(true, i) for 81920..81929, persist=1.
        for (slot in DIRAC_SLOT_FIRST..DIRAC_SLOT_LAST) {
            readMatrix(audioManager, "b.slot$slot", 1, topo, moduleId, slot)
        }

        // (c) the stock enable write exactly as se.dirac.acs sends it.
        val data = QemProtocol.frame(moduleId, QemProtocol.PARAM_ENABLE, QemProtocol.intPayload(1))
        val cal =
            "cal_caltype=0;cal_topoid=$topo;cal_apptype=$ENABLE_APPTYPE;cal_persist=0;" +
                "cal_devid=$ENABLE_DEVID;cal_samplerate=$ENABLE_RATE;cal_data=${QemProtocol.encode(data)}"
        Log.i(TAG, "c.enable send $cal")
        val returned = sendWatched(audioManager, cal)
        Log.i(TAG, "c.enable returned=$returned")
        Log.i(TAG, "stockseq: done")
        return if (returned) Activity.RESULT_OK else RESULT_TIMEOUT
    }

    /**
     * Stock b.a(z, paramid) reads the base string plus";cal_apptype=..;cal_devid=..;cal_samplerate=.."
     * and iterates device x apptype x sample-rate, returning on the first successful reply. The
     * selectors are part of the query: without cal_devid the HAL sees dev_id 0 and answers -22.
     */
    private fun readMatrix(
        audioManager: AudioManager,
        label: String,
        persist: Int,
        topo: Int,
        moduleId: Int,
        paramId: Int,
    ): String? {
        for (dev in READ_DEVICES) {
            for (appType in READ_APPTYPES) {
                for (rate in READ_RATES) {
                    val query =
                        "cal_persist=$persist;cal_topoid=$topo;cal_moduleid=$moduleId;" +
                            "cal_paramid=$paramId;cal_caltype=0;cal_data=0;" +
                            "cal_apptype=$appType;cal_devid=$dev;cal_samplerate=$rate"
                    Log.i(TAG, "$label read $query")
                    val reply =
                        try {
                            audioManager.getParameters(query)
                        } catch (t: Throwable) {
                            Log.e(TAG, "$label getParameters threw", t)
                            return null
                        }
                    if (reply == null) {
                        Log.i(TAG, "$label reply <null>")
                        continue
                    }
                    Log.i(TAG, "$label reply verbatim: $reply")
                    // Stock parses ^cal_result=(-?\d*);cal_data=(.*)$ (in either key order).
                    val result = RESULT_RE.find(reply)?.groupValues?.get(1)
                    val data = DATA_RE.find(reply)?.groupValues?.get(1)
                    Log.i(TAG, "$label parsed cal_result=$result cal_data_len=${data?.length ?: -1}")
                    if (result == SUCCESS_RESULT) {
                        Log.i(TAG, "$label SUCCESS on dev=$dev apptype=$appType rate=$rate")
                        return reply
                    }
                }
            }
        }
        return null
    }

    /** Runs one setParameters on a worker thread with the stock-order watchdog. */
    private fun sendWatched(audioManager: AudioManager, cal: String): Boolean {
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
        }
        return returned
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
        const val EXTRA_MODE = "mode"
        const val MODE_STOCKSEQ = "stockseq"
        const val DEFAULT_APPTYPE = 69936
        const val DEFAULT_RATE = 48000
        const val WATCHDOG_SECONDS = 4L
        val RESULT_TIMEOUT = Activity.RESULT_FIRST_USER + 1

        // Stock se.dirac.acs constants (se/dirac/controller/qem/Controller.java, c/a/e/f/*).
        const val TOPO_INTERNAL = 0x10012D00
        const val VERSION_PARAM = 0x12D00
        const val DIRAC_SLOT_FIRST = 81920
        const val DIRAC_SLOT_LAST = 81929
        const val ENABLE_APPTYPE = 69936
        const val ENABLE_DEVID = 2
        const val ENABLE_RATE = 48000
        const val SUCCESS_RESULT = "0"
        val READ_DEVICES = intArrayOf(2)
        val READ_APPTYPES = intArrayOf(69936, 69940)
        val READ_RATES = intArrayOf(44100, 48000, 96000, 192000)

        val RESULT_RE = Regex("cal_result=(-?\\d*)")
        val DATA_RE = Regex("cal_data=([^;]*)")
    }
}
