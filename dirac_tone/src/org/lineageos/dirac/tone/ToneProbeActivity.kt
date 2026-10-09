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
import android.media.AudioManager
import android.os.Bundle
import android.util.Log

/** One frame's parameter, its value text, and the two optional suffixes. */
private data class Entry(
    val param: Int,
    val valueText: String?,
    val isFloat: Boolean,
    val calType: Int?,
) {
    fun intOr(fallback: Int): Int = valueText?.let { parseNumber(it) } ?: fallback

    fun floatOr(fallback: Int): Float =
        valueText?.toFloatOrNull() ?: parseNumber(valueText)?.toFloat() ?: fallback.toFloat()
}

/** Splits `0x12d35=0@0,0x12d02=0.5:f@1` into entries: `<param>[=<value>][:f][@caltype]`. */
private fun parseEntries(raw: String?): List<Entry> = raw.orEmpty()
    .split(',', ' ', ';')
    .mapNotNull { token ->
        val trimmed = token.trim()
        if (trimmed.isEmpty()) {
            return@mapNotNull null
        }
        val atSplit = trimmed.split('@', limit = 2)
        val eqSplit = atSplit[0].split('=', limit = 2)
        val colonSplit = eqSplit.getOrNull(1).orEmpty().split(':', limit = 2)
        val param = parseNumber(eqSplit[0]) ?: return@mapNotNull null
        Entry(
            param = param,
            valueText = colonSplit[0].ifEmpty { null },
            isFloat = colonSplit.getOrNull(1)?.trim()?.lowercase() == "f",
            calType = atSplit.getOrNull(1)?.let { parseNumber(it.trim()) },
        )
    }

/** Accepts decimal or 0x-prefixed hex. */
private fun parseNumber(raw: String?): Int? = raw?.trim()?.let {
    runCatching {
        if (it.startsWith("0x", ignoreCase = true)) {
            it.substring(2).toLong(16).toInt()
        } else {
            it.toLong().toInt()
        }
    }.getOrNull()
}

/**
 * DEV PROBE, not a shipped feature.
 *
 * Plays a stereo tone so a mixer (path=0) ADM stream is open, then walks the
 * requested topologies against the requested HAL sound devices and sends one
 * Dirac QEM frame per (combo, parameter). One frame is one setParameters call
 * carrying one parameter, so an ADM or DSP refusal names exactly one suspect;
 * the parameters a test mixes are deliberately never packed into one call.
 *
 * Trigger:
 *
 *   adb shell am start -n org.lineageos.dirac.tone/.ToneProbeActivity \
 *       --es topos "0x10012d01" --es devids "0" --es device headset \
 *       --es module "0x12d01" --es params "0x12d35=0@0,0x12d01=1@0,0x12d02=0.5:f@1"
 *
 * Extras: duration seconds (default 60), topos (default 0x10012d00,
 * 0x10012d01,0x10312), devids snd_device_t ids (2 speaker, 9 headphones; 0 is
 * the framing the shipping app uses - cal_devid=0 alone, no cal_snddevid - and
 * the only one the wired route accepts; the default is the connected output
 * resolved by [ToneTarget]), device speaker|headset|earpiece and usage
 * media|alarm select the stream's preferred output and usage (default is the
 * media output), apptype (69936), rate (48000), caltype (1, the raw path; 0 asks
 * the ACDB table), persist (0), module (0x12D00, the speaker's module; the
 * headset topology 0x10012d01 instantiates 0x12D01), param (0x12D01), value (1).
 *
 * params is the parameter list, one entry per frame, each `param` or
 * `param=value`, with an optional `:f` selecting a float payload and an optional
 * `@caltype` overriding the request's caltype for that entry only - so one run
 * can turn the equalizer off through the table, enable the module through the
 * table, and push the balance raw, three frames apart and nothing riding along.
 * With no params the single `param`/`value` pair is sent with the request's
 * caltype. Other extras: delay ms before the first frame (2000), gap ms between
 * topo/device combos (1200), seqgap ms between the params of one combo (150),
 * freq Hz (1000).
 *
 * `--ez silent true` opens no tone stream at all: the frames go out against
 * whatever output is already live (music, for an ear test), which is also the
 * cleaner trace, because a probe stream of this app's own adds ADM stream-setup
 * errors to dmesg and masks the audio being judged. The hold then runs for
 * `duration` with nothing playing. The long names
 * durationMs/delayMs/gapMs/sndDevIds/appType/sampleRate/frequencyHz are accepted
 * as aliases of the short ones.
 */
class ToneProbeActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val audioManager = getSystemService(AudioManager::class.java)
        val request = Request.from(intent, ToneTarget.of(audioManager))
        val route = ToneRoute.of(intent, audioManager)
        Log.i(TAG, "request: $request")
        Thread { run(request, route) }.start()
    }

    private fun run(request: Request, route: ToneRoute) {
        val player = if (request.silent) null else
            TonePlayer(route, frequencyHz = request.frequencyHz.toDouble())
        val frames = QemFrames(this)
        try {
            if (player != null && !player.start()) {
                Log.e(TAG, "no tone, no frames sent")
                return
            }
            if (player == null) {
                Log.i(TAG, "silent: no tone stream opened; frames go out against the live output")
            }
            Thread.sleep(request.delayMs)
            var index = 0
            val combos = request.topos.size * request.sndDevIds.size
            val perComboMs = request.gapMs +
                (request.entries.size - 1).coerceAtLeast(0) * request.seqGapMs
            for (topo in request.topos) {
                for (devId in request.sndDevIds) {
                    index++
                    Log.i(TAG, "combo $index/$combos topo=0x${Integer.toHexString(topo)}" +
                        " snddev=$devId module=0x${Integer.toHexString(request.module)}" +
                        " params=" +
                        request.entries.joinToString { "0x" + Integer.toHexString(it.param) })
                    request.entries.forEachIndexed { i, entry ->
                        frames.send(
                            topo = topo,
                            appType = request.appType,
                            sndDevId = devId,
                            sampleRate = request.sampleRate,
                            calType = entry.calType ?: request.calType,
                            persist = request.persist,
                            module = request.module,
                            param = entry.param,
                            value = entry.intOr(request.value),
                            floatValue = if (entry.isFloat) entry.floatOr(request.value) else null,
                        )
                        if (i < request.entries.size - 1) {
                            Thread.sleep(request.seqGapMs)
                        }
                    }
                    Thread.sleep(request.gapMs)
                }
            }
            val remaining = request.durationMs - request.delayMs - index * perComboMs
            Log.i(TAG, "walk done: $index frames sent; holding the tone ${remaining}ms")
            if (remaining > 0) {
                Thread.sleep(remaining)
            }
        } catch (t: Throwable) {
            Log.e(TAG, "probe failed", t)
        } finally {
            player?.let {
                if (it.failedWrites > 0) {
                    Log.e(TAG, "tone: ${it.failedWrites} failed writes, it may not have sounded")
                }
                it.stop()
            }
            runOnUiThread { finish() }
        }
    }

    private data class Request(
        val durationMs: Long,
        val delayMs: Long,
        val gapMs: Long,
        val topos: List<Int>,
        val sndDevIds: List<Int>,
        val appType: Int,
        val sampleRate: Int,
        val calType: Int,
        val persist: Int,
        val module: Int,
        val value: Int,
        val silent: Boolean,
        val entries: List<Entry>,
        val seqGapMs: Long,
        val frequencyHz: Int,
    ) {
        companion object {
            fun from(intent: Intent?, defaultSndDevId: Int): Request {
                val duration = intExtra(intent, EXTRA_DURATION, EXTRA_DURATION_MS,
                    default = DEFAULT_DURATION)
                val param = intExtra(intent, EXTRA_PARAM, default = QemFrames.PARAM_ENABLE)
                val value = intExtra(intent, EXTRA_VALUE, default = QemFrames.ENABLE)
                return Request(
                    durationMs = duration.coerceIn(MIN_DURATION, MAX_DURATION) * 1000L,
                    delayMs = intExtra(intent, EXTRA_DELAY, EXTRA_DELAY_MS,
                        default = DEFAULT_DELAY).coerceAtLeast(0).toLong(),
                    gapMs = intExtra(intent, EXTRA_GAP, EXTRA_GAP_MS,
                        default = DEFAULT_GAP).coerceAtLeast(0).toLong(),
                    topos = parseInts(stringExtra(intent, EXTRA_TOPOS)).ifEmpty { DEFAULT_TOPOS },
                    sndDevIds = parseInts(stringExtra(intent, EXTRA_DEVIDS, EXTRA_SNDDEVIDS))
                        .ifEmpty { listOf(defaultSndDevId) },
                    appType = intExtra(intent, EXTRA_APPTYPE, EXTRA_APPTYPE_ALT,
                        default = DEFAULT_APPTYPE),
                    sampleRate = intExtra(intent, EXTRA_RATE, EXTRA_SAMPLERATE,
                        default = DEFAULT_RATE),
                    calType = intExtra(intent, EXTRA_CALTYPE, default = QemFrames.CAL_TYPE_RAW),
                    persist = intExtra(intent, EXTRA_PERSIST, default = DEFAULT_PERSIST),
                    module = intExtra(intent, EXTRA_MODULE, default = QemFrames.MODULE_INTERNAL),
                    value = value,
                    silent = boolExtra(intent, EXTRA_SILENT),
                    entries = parseEntries(stringExtra(intent, EXTRA_PARAMS))
                        .ifEmpty { listOf(Entry(param, valueText = null, isFloat = false,
                            calType = null)) },
                    seqGapMs = intExtra(intent, EXTRA_SEQGAP, default = DEFAULT_SEQGAP)
                        .coerceAtLeast(0).toLong(),
                    frequencyHz = intExtra(intent, EXTRA_FREQ, EXTRA_FREQUENCY,
                        default = DEFAULT_FREQ),
                )
            }

            /** Accepts an int extra, or a decimal/hex string extra for `--es`. */
            private fun intExtra(intent: Intent?, vararg names: String, default: Int): Int {
                for (name in names) {
                    val raw = intent?.extras?.get(name) ?: continue
                    when (raw) {
                        is Int -> return raw
                        is Long -> return raw.toInt()
                        is String -> parseNumber(raw)?.let { return it }
                    }
                }
                return default
            }

            /** Accepts a boolean extra, or the strings true/1 for `--es`. */
            private fun boolExtra(intent: Intent?, vararg names: String): Boolean {
                for (name in names) {
                    val raw = intent?.extras?.get(name) ?: continue
                    when (raw) {
                        is Boolean -> return raw
                        is Int -> return raw != 0
                        is String -> return raw.equals("true", ignoreCase = true) || raw == "1"
                    }
                }
                return false
            }

            private fun stringExtra(intent: Intent?, vararg names: String): String? {
                for (name in names) {
                    val raw = intent?.extras?.get(name) ?: continue
                    if (raw is String) return raw
                }
                return null
            }

            private fun parseInts(raw: String?): List<Int> = raw.orEmpty()
                .split(',', ' ', ';')
                .mapNotNull { parseNumber(it) }

            private const val EXTRA_DURATION = "duration"
            private const val EXTRA_DURATION_MS = "durationMs"
            private const val EXTRA_DELAY = "delay"
            private const val EXTRA_DELAY_MS = "delayMs"
            private const val EXTRA_GAP = "gap"
            private const val EXTRA_GAP_MS = "gapMs"
            private const val EXTRA_TOPOS = "topos"
            private const val EXTRA_DEVIDS = "devids"
            private const val EXTRA_SNDDEVIDS = "sndDevIds"
            private const val EXTRA_APPTYPE = "apptype"
            private const val EXTRA_APPTYPE_ALT = "appType"
            private const val EXTRA_RATE = "rate"
            private const val EXTRA_SAMPLERATE = "sampleRate"
            private const val EXTRA_CALTYPE = "caltype"
            private const val EXTRA_PERSIST = "persist"
            private const val EXTRA_MODULE = "module"
            private const val EXTRA_PARAM = "param"
            private const val EXTRA_VALUE = "value"
            private const val EXTRA_PARAMS = "params"
            private const val EXTRA_SEQGAP = "seqgap"
            private const val EXTRA_SILENT = "silent"
            private const val EXTRA_FREQ = "freq"
            private const val EXTRA_FREQUENCY = "frequencyHz"

            private const val DEFAULT_DURATION = 60
            private const val MIN_DURATION = 5
            private const val MAX_DURATION = 600
            private const val DEFAULT_DELAY = 2000
            private const val DEFAULT_GAP = 1200
            private const val DEFAULT_APPTYPE = 69936
            private const val DEFAULT_RATE = 48000
            private const val DEFAULT_PERSIST = 0
            private const val DEFAULT_SEQGAP = 150
            private const val DEFAULT_FREQ = 1000
            private val DEFAULT_TOPOS = listOf(0x10012D00, 0x10012D01, 0x10312)
        }
    }

    private companion object {
        const val TAG = "DiracToneProbe"
    }
}
