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
import android.util.Log

/**
 * DEV PROBE, not part of the shipped feature.
 *
 * Walks candidate ACDB topologies (and, optionally, candidate acdb device ids)
 * for one app type and sends a Dirac calibration frame for each combination, so
 * the topology the live audio stream registered can be identified from the
 * audio HAL's own log.
 *
 * With caltype=0 libacdbloader takes get_audio_copp_id, which logs nothing, so
 * a miss is only visible as the generic
 *
 *   ACDB-LOADER ... active device/stream not found (result=-100) for
 *   topology 0x%x and apptype 0x%x
 *
 * line. With caltype=1 it takes get_audio_popp_id, which prints the kernel's
 * whole active RTAC ADM table first (tag is NULL, so grep the message text):
 *
 *   " app 0x%x acdb 0x%x"      one line per active RTAC ADM device
 *   " topo 0x%x "              one line per popp topology inside that device
 *   "[get_audio_popp_id] finding active popp id failed, apptype %d, acdb %d,
 *    topo %d"                  the miss, printed for every failed lookup
 *
 * so caltype=1 both dumps the table contents and reports the miss. A hit is
 * silent on that last line and the frame is applied instead.
 *
 * Trigger, with a music stream already playing:
 *
 *   adb shell am broadcast -a org.lineageos.dirac.qem.PROBE \
 *       --es topos "0x10312,0x10313,0x10012d00,0x10012d01,0x1025e" \
 *       --ei apptype 69936 --ei caltype 1 --es devids "15,10" \
 *       --ei rate 48000 --ei output 0
 *
 * Extras:
 *   topos    comma separated, hex or decimal, required
 *   apptype  default 69936
 *   rate     default 48000
 *   output   0 internal / 1 external, default is the app's current setting
 *   caltype  0 copp (silent) / 1 popp (dumps the RTAC ADM table), default 1
 *   devids   comma separated cal_devid values (acdb device ids) to sweep,
 *            default: the app's own device list, letting the HAL resolve the
 *            sound device. "15,10" probes speaker and wired headset explicitly.
 *   snddevid explicit cal_snddevid, default: automatic
 *   persist  cal_persist, default 0 (never writes a persistent calibration)
 *   param    parameter id to send, default PARAM_ENABLE (0x12D01)
 *   gap      milliseconds between frames, default 1200
 *
 * Each frame is logged under tag DiracQemProbe before it is sent, with a gap
 * between frames so the HAL's reactions can be correlated one to one.
 */
class DiracProbeReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != ACTION_PROBE) {
            return
        }
        val topos = parseInts(intent.getStringExtra(EXTRA_TOPOS).orEmpty())
        if (topos.isEmpty()) {
            Log.w(TAG, "no usable topologies in '$EXTRA_TOPOS' extra, nothing sent")
            return
        }
        val appType = intent.getIntExtra(EXTRA_APPTYPE, DEFAULT_APPTYPE)
        val rate = intent.getIntExtra(EXTRA_RATE, DEFAULT_RATE)
        val calType = intent.getIntExtra(EXTRA_CALTYPE, DEFAULT_CALTYPE)
        val persist = intent.getIntExtra(EXTRA_PERSIST, DEFAULT_PERSIST)
        val param = intent.getIntExtra(EXTRA_PARAM, QemProtocol.PARAM_ENABLE)
        val sndDevId = intent.getIntExtra(EXTRA_SNDDEVID, SNDDEV_UNSET)
        val devIds = parseInts(intent.getStringExtra(EXTRA_DEVIDS).orEmpty())
        val requestedOutput = intent.getIntExtra(EXTRA_OUTPUT, OUTPUT_UNSET)
        val output = if (requestedOutput == DiracQemEffect.OUTPUT_INTERNAL ||
            requestedOutput == DiracQemEffect.OUTPUT_EXTERNAL) {
            requestedOutput
        } else {
            DiracQemEffect.output(context)
        }
        val gap = intent.getIntExtra(EXTRA_GAP, GAP_MS).coerceAtLeast(0).toLong()

        // The walk takes seconds, so it has to outlive onReceive().
        val pending = goAsync()
        Thread {
            try {
                Log.i(TAG, "walk starting: ${topos.size} topologies x " +
                    "${devIds.size.coerceAtLeast(1)} devids, apptype=$appType rate=$rate " +
                    "output=$output caltype=$calType persist=$persist param=0x" +
                    Integer.toHexString(param) + " snddevid=$sndDevId gap=$gap")
                var index = 0
                topos.forEach { topo ->
                    // A device list is sent either as explicit cal_devid values
                    // or as one pass with the app's own list.
                    val passes: List<Int> = if (devIds.isEmpty()) listOf(DEVID_AUTO) else devIds
                    passes.forEach { devId ->
                        index++
                        val explicitDevId = devId != DEVID_AUTO
                        Log.i(TAG, "probe $index: topo=0x" + Integer.toHexString(topo) +
                            " apptype=$appType devid=" +
                            (if (explicitDevId) devId else "auto") +
                            " snddevid=$sndDevId caltype=$calType persist=$persist" +
                            " rate=$rate param=0x" + Integer.toHexString(param))
                        DiracQemEffect.probeCal(
                            context = context,
                            output = output,
                            topo = topo,
                            appType = appType,
                            rate = rate,
                            calType = calType,
                            calDevId = if (explicitDevId) devId else -1,
                            sndDevId = sndDevId,
                            persist = persist,
                            param = param,
                        )
                        if (gap > 0) {
                            Thread.sleep(gap)
                        }
                    }
                }
                Log.i(TAG, "walk done: $index frames sent")
            } catch (t: Throwable) {
                Log.e(TAG, "walk failed", t)
            } finally {
                pending.finish()
            }
        }.start()
    }

    private fun parseInts(raw: String): List<Int> = raw
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
        const val EXTRA_CALTYPE = "caltype"
        const val EXTRA_DEVIDS = "devids"
        const val EXTRA_SNDDEVID = "snddevid"
        const val EXTRA_PERSIST = "persist"
        const val EXTRA_PARAM = "param"
        const val EXTRA_GAP = "gap"
        const val DEFAULT_APPTYPE = 69936
        const val DEFAULT_RATE = 48000
        const val DEFAULT_CALTYPE = QemProtocol.CAL_TYPE_POPP
        const val DEFAULT_PERSIST = 0
        const val DEVID_AUTO = -1
        const val SNDDEV_UNSET = -1
        const val OUTPUT_UNSET = -1
        const val GAP_MS = 1200
    }
}
