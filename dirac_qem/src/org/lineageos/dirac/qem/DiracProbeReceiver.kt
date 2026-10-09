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

/**
 * DEV PROBE, not part of the shipped feature.
 *
 * Walks a list of candidate ACDB topologies for one app type and sends a Dirac
 * calibration frame for each, so the topology a live audio stream registered can
 * be identified from the audio HAL's own log. The ACDB loader answers
 * "active device/stream not found ... for topology 0x%x and apptype 0x%x" for
 * every pair that matches no active stream, and stays silent for the pair that
 * does, so the candidate that stops producing that line is the topology the
 * stream registered.
 *
 * Trigger, with an offload music stream already playing:
 *
 *   adb shell am broadcast -a org.lineageos.dirac.qem.PROBE \
 *       --es topos "0x10312,0x10313,0x10314,0x11000000,0x11000001,0x10012d00,0x1025e" \
 *       --ei apptype 69936 --ei rate 48000 --ei output 0
 *
 * Extras: topos (comma separated, hex or decimal, required), apptype (default
 * 69936), rate (default 48000), output (0 internal / 1 external, default is the
 * app's current setting). Each candidate is logged under tag DiracQemProbe
 * before it is sent, with a gap between candidates so the HAL's reactions can be
 * correlated one to one.
 */
class DiracProbeReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != ACTION_PROBE) {
            return
        }
        val topos = parseTopos(intent.getStringExtra(EXTRA_TOPOS).orEmpty())
        if (topos.isEmpty()) {
            return
        }
        val appType = intent.getIntExtra(EXTRA_APPTYPE, DEFAULT_APPTYPE)
        val rate = intent.getIntExtra(EXTRA_RATE, DEFAULT_RATE)
        val requestedOutput = intent.getIntExtra(EXTRA_OUTPUT, OUTPUT_UNSET)
        val output = if (requestedOutput == DiracState.OUTPUT_INTERNAL ||
            requestedOutput == DiracState.OUTPUT_EXTERNAL) {
            requestedOutput
        } else {
            DiracState.output(context)
        }

        // The walk takes seconds, so it has to outlive onReceive().
        val pending = goAsync()
        Thread {
            try {
                topos.forEachIndexed { index, topo ->
                    DiracQemEffect.probeCal(context, output, topo, appType, rate)
                    if (index != topos.lastIndex) {
                        Thread.sleep(GAP_MS)
                    }
                }
            } catch (t: Throwable) {
            } finally {
                pending.finish()
            }
        }.start()
    }

    private fun parseTopos(raw: String): List<Int> = raw
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

    private companion object {
        const val TAG = "DiracQemProbe"
        const val ACTION_PROBE = "org.lineageos.dirac.qem.PROBE"
        const val EXTRA_TOPOS = "topos"
        const val EXTRA_APPTYPE = "apptype"
        const val EXTRA_RATE = "rate"
        const val EXTRA_OUTPUT = "output"
        const val DEFAULT_APPTYPE = 69936
        const val DEFAULT_RATE = 48000
        const val OUTPUT_UNSET = -1
        const val GAP_MS = 1500L
    }
}
