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

package org.lineageos.dirac.tone

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.util.Log

/**
 * DEV PROBE, not a shipped feature.
 *
 * Plays a USAGE_MEDIA tone so a mixer (path=0) ADM stream is open, then walks
 * the requested topologies against the requested acdb device ids and sends one
 * Dirac QEM frame per combination with cal_caltype=1, which makes libacdbloader
 * dump the kernel's active RTAC ADM table and log every failed lookup.
 *
 * Trigger (topos and devids are the only extras worth changing):
 *
 *   adb shell am start -n org.lineageos.dirac.tone/.ToneProbeActivity \
 *       --ei duration 60 --es topos "0x10012d00,0x10012d01,0x10312" \
 *       --es devids "15,10"
 *
 * Extras: duration seconds (default 60), topos (default 0x10012d00,
 * 0x10012d01,0x10312), devids acdb ids (default 15,10), apptype (69936),
 * rate (48000), caltype (1), persist (0), param (0x12D01), delay ms before the
 * first frame (2000), gap ms between frames (1200), freq Hz (1000).
 */
class ToneProbeActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val request = Request.from(intent)
        Log.i(TAG, "request: $request")
        Thread { run(request) }.start()
    }

    private fun run(request: Request) {
        val player = TonePlayer(frequencyHz = request.frequencyHz.toDouble())
        val frames = QemFrames(this)
        try {
            if (!player.start()) {
                Log.e(TAG, "no tone, no frames sent")
                return
            }
            Thread.sleep(request.delayMs)
            var index = 0
            for (topo in request.topos) {
                for (devId in request.acdbDevIds) {
                    index++
                    Log.i(TAG, "frame $index/${request.topos.size * request.acdbDevIds.size}" +
                        " topo=0x${Integer.toHexString(topo)} acdb=$devId")
                    frames.send(
                        topo = topo,
                        appType = request.appType,
                        acdbDevId = devId,
                        sampleRate = request.sampleRate,
                        calType = request.calType,
                        persist = request.persist,
                        param = request.param,
                    )
                    Thread.sleep(request.gapMs)
                }
            }
            val remaining = request.durationMs - request.delayMs - index * request.gapMs
            Log.i(TAG, "walk done: $index frames sent; holding the tone ${remaining}ms")
            if (remaining > 0) {
                Thread.sleep(remaining)
            }
        } catch (t: Throwable) {
            Log.e(TAG, "probe failed", t)
        } finally {
            player.stop()
            runOnUiThread { finish() }
        }
    }

    private data class Request(
        val durationMs: Long,
        val delayMs: Long,
        val gapMs: Long,
        val topos: List<Int>,
        val acdbDevIds: List<Int>,
        val appType: Int,
        val sampleRate: Int,
        val calType: Int,
        val persist: Int,
        val param: Int,
        val frequencyHz: Int,
    ) {
        companion object {
            fun from(intent: Intent?): Request {
                val duration = intent?.getIntExtra(EXTRA_DURATION, DEFAULT_DURATION) ?: DEFAULT_DURATION
                return Request(
                    durationMs = duration.coerceIn(MIN_DURATION, MAX_DURATION) * 1000L,
                    delayMs = (intent?.getIntExtra(EXTRA_DELAY, DEFAULT_DELAY) ?: DEFAULT_DELAY)
                        .coerceAtLeast(0).toLong(),
                    gapMs = (intent?.getIntExtra(EXTRA_GAP, DEFAULT_GAP) ?: DEFAULT_GAP)
                        .coerceAtLeast(0).toLong(),
                    topos = parseInts(intent?.getStringExtra(EXTRA_TOPOS)).ifEmpty { DEFAULT_TOPOS },
                    acdbDevIds = parseInts(intent?.getStringExtra(EXTRA_DEVIDS)).ifEmpty {
                        DEFAULT_DEVIDS
                    },
                    appType = intent?.getIntExtra(EXTRA_APPTYPE, DEFAULT_APPTYPE) ?: DEFAULT_APPTYPE,
                    sampleRate = intent?.getIntExtra(EXTRA_RATE, DEFAULT_RATE) ?: DEFAULT_RATE,
                    calType = intent?.getIntExtra(EXTRA_CALTYPE, QemFrames.CAL_TYPE_POPP)
                        ?: QemFrames.CAL_TYPE_POPP,
                    persist = intent?.getIntExtra(EXTRA_PERSIST, DEFAULT_PERSIST) ?: DEFAULT_PERSIST,
                    param = intent?.getIntExtra(EXTRA_PARAM, QemFrames.PARAM_ENABLE)
                        ?: QemFrames.PARAM_ENABLE,
                    frequencyHz = intent?.getIntExtra(EXTRA_FREQ, DEFAULT_FREQ) ?: DEFAULT_FREQ,
                )
            }

            private fun parseInts(raw: String?): List<Int> = raw.orEmpty()
                .split(',', ' ', ';')
                .mapNotNull { token ->
                    val trimmed = token.trim()
                    if (trimmed.isEmpty()) {
                        null
                    } else {
                        runCatching {
                            if (trimmed.startsWith("0x", ignoreCase = true)) {
                                trimmed.substring(2).toLong(16).toInt()
                            } else {
                                trimmed.toLong().toInt()
                            }
                        }.getOrNull()
                    }
                }

            private const val EXTRA_DURATION = "duration"
            private const val EXTRA_DELAY = "delay"
            private const val EXTRA_GAP = "gap"
            private const val EXTRA_TOPOS = "topos"
            private const val EXTRA_DEVIDS = "devids"
            private const val EXTRA_APPTYPE = "apptype"
            private const val EXTRA_RATE = "rate"
            private const val EXTRA_CALTYPE = "caltype"
            private const val EXTRA_PERSIST = "persist"
            private const val EXTRA_PARAM = "param"
            private const val EXTRA_FREQ = "freq"

            private const val DEFAULT_DURATION = 60
            private const val MIN_DURATION = 5
            private const val MAX_DURATION = 600
            private const val DEFAULT_DELAY = 2000
            private const val DEFAULT_GAP = 1200
            private const val DEFAULT_APPTYPE = 69936
            private const val DEFAULT_RATE = 48000
            private const val DEFAULT_PERSIST = 0
            private const val DEFAULT_FREQ = 1000
            private val DEFAULT_TOPOS = listOf(0x10012D00, 0x10012D01, 0x10312)
            private val DEFAULT_DEVIDS = listOf(15, 10)
        }
    }

    private companion object {
        const val TAG = "DiracToneProbe"
    }
}
