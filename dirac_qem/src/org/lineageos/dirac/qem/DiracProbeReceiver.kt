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
 * Sends one burst of explicit calibration frames at an explicit candidate, so
 * the audio HAL's own log can be read for the parameter that makes the ACDB
 * loader answer. Every frame is composed by the same composer a production pass
 * uses, so for a given parameter it carries production bytes: the parameter's
 * own payload and size, the module production names for the chosen output,
 * cal_devid from the live route, and production's cal type (0 for the seven ACDB
 * parameters, 1 for the raw Sum/Diff width). Topology, app type, rate and the
 * parameter set are the caller's, which is the point of a probe; cal_persist is
 * always 0 and the burst writes no state of its own, so a wrong candidate cannot
 * leave a persistent calibration behind. The values it carries are read through
 * the payload rules production uses, which read the stored state.
 *
 * Prefer one topology per broadcast: the foreground-broadcast budget is 10 s, so
 * a multi-candidate burst should pass a small gap (--ei gap 1000) or be split
 * into one broadcast per candidate.
 *
 * Trigger, with an offload music stream already playing:
 *
 *   # The positive control: the full production set for the route, one burst.
 *   adb shell am broadcast -n org.lineageos.dirac.qem/.DiracProbeReceiver \
 *       -a org.lineageos.dirac.qem.PROBE --es topos "0x10012D01" \
 *       --ei apptype 69940 --ei rate 48000 --ei output 1 --receiver-foreground
 *
 *   # One parameter alone, repeated six times.
 *   adb shell am broadcast -n org.lineageos.dirac.qem/.DiracProbeReceiver \
 *       -a org.lineageos.dirac.qem.PROBE --es topos "0x10012D01" \
 *       --ei apptype 69940 --ei rate 48000 --ei output 1 \
 *       --es params "0x12d36" --ei repeats 6 --receiver-foreground
 *
 * The explicit component is required: a manifest-declared receiver does not get
 * an implicit broadcast on this platform, so "-a" alone enqueues with no
 * matching receiver and nothing runs.
 *
 * Extras: topos (comma separated, hex or decimal, required), apptype (default
 * 69936), rate (default 48000), output (0 internal / 1 external, default is the
 * app's current setting), params (comma separated ids to send alone, default is
 * the production set for the route), param (one id, an alternative to params),
 * repeats (frames per parameter, default 1), gap (ms between candidates, default
 * 1000 so a burst never overlaps the next while staying inside the foreground
 * broadcast budget). Each burst is logged under tag
 * DiracQemProbe before its frames, and each frame logs its parameter, cal type
 * and repeat index, so the loader's answers can be correlated one to one.
 */
class DiracProbeReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != ACTION_PROBE) {
            return
        }
        val topos = parseIntList(intent.getStringExtra(EXTRA_TOPOS).orEmpty())
        if (topos.isEmpty()) {
            return
        }
        val appType = intent.getIntExtra(EXTRA_APPTYPE, DEFAULT_APPTYPE)
        val rate = intent.getIntExtra(EXTRA_RATE, DEFAULT_RATE)
        val repeats = intent.getIntExtra(EXTRA_REPEATS, DEFAULT_REPEATS).coerceAtLeast(1)
        val gap = intent.getLongExtra(EXTRA_GAP, DEFAULT_GAP_MS).coerceAtLeast(0L)
        val requestedOutput = intent.getIntExtra(EXTRA_OUTPUT, OUTPUT_UNSET)
        val output = if (requestedOutput == DiracState.OUTPUT_INTERNAL ||
            requestedOutput == DiracState.OUTPUT_EXTERNAL) {
            requestedOutput
        } else {
            DiracState.output(context)
        }
        val params = requestedParams(intent, output)

        // A walk takes seconds, so it has to outlive onReceive().
        val pending = goAsync()
        Thread {
            try {
                topos.forEachIndexed { index, topo ->
                    // Logged BEFORE the frames, so the loader's answers line up
                    // one to one with the burst that produced them.
                    Log.i(TAG, "probe topo=0x%x apptype=%d rate=%d output=%d params=%s repeats=%d frames=%d"
                        .format(topo, appType, rate, output, QemParams.ids(params), repeats,
                            params.size * repeats))
                    DiracQemEffect.probe(
                        context, DiracQemEffect.Probe(output, topo, appType, rate, params, repeats))
                    if (index != topos.lastIndex && gap > 0) {
                        Thread.sleep(gap)
                    }
                }
            } catch (t: Throwable) {
                Log.e(TAG, "probe walk aborted", t)
            } finally {
                pending.finish()
            }
        }.start()
    }

    /**
     * The parameters one burst sends: the ones the caller named, or the
     * production set for the route, which is the positive control.
     */
    private fun requestedParams(intent: Intent, output: Int): List<QemParams.Spec> {
        val ids = parseIntList(intent.getStringExtra(EXTRA_PARAMS).orEmpty()) +
            if (intent.hasExtra(EXTRA_PARAM)) listOf(intent.getIntExtra(EXTRA_PARAM, 0)) else emptyList()
        if (ids.isEmpty()) {
            return QemParams.productionSet(output)
        }
        val specs = ids.mapNotNull { id ->
            QemParams.byId(id).also {
                if (it == null) {
                    Log.e(TAG, "probe unknown param=0x%x, the burst will send nothing".format(id))
                }
            }
        }
        // A burst that names no known parameter sends nothing rather than
        // falling back to the whole production set, so a mistyped id can never
        // be read as a full pass.
        return specs
    }

    private fun parseIntList(raw: String): List<Int> = raw
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
        const val EXTRA_PARAMS = "params"
        const val EXTRA_PARAM = "param"
        const val EXTRA_REPEATS = "repeats"
        const val EXTRA_GAP = "gap"
        const val DEFAULT_APPTYPE = 69936
        const val DEFAULT_RATE = 48000
        const val DEFAULT_REPEATS = 1

        // Comfortably inside the 10 s foreground-broadcast budget for a handful
        // of candidates, and wide enough that one burst cannot merge into the
        // next.
        const val DEFAULT_GAP_MS = 1000L
        const val OUTPUT_UNSET = -1
    }
}
