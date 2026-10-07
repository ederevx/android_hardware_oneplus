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

/**
 * DEV PROBE, not a shipped feature.
 *
 * Plays a stereo tone so a mixer (path=0) ADM stream is open, then walks
 * the requested topologies against the requested HAL sound devices and sends
 * one Dirac QEM frame per combination with cal_caltype=1, which makes
 * libacdbloader dump the kernel's active RTAC ADM table and log every failed
 * lookup.
 *
 * Trigger:
 *
 *   adb shell am start -n org.lineageos.dirac.tone/.ToneProbeActivity \
 *       --ei duration 60 --es topos "0x10012d00,0x10012d01,0x10312" \
 *       --es devids "2,9" --es device speaker --es usage media
 *
 * Extras: duration seconds (default 60), topos (default 0x10012d00,
 * 0x10012d01,0x10312), devids snd_device ids (2 speaker, 9 headphones; the
 * default is the connected output resolved by [ToneTarget]), device
 * speaker|headset|earpiece and usage media|alarm select the stream's preferred
 * output and usage (default is the media output), apptype (69936),
 * rate (48000), caltype (1), persist (0), module (0x12D00, the speaker's
 * module; the headset topology 0x10012d01 instantiates 0x12D01), param
 * (0x12D01), value (1), params (a comma list of `param` or `param=value`
 * entries, hex or decimal, all sent in sequence into the one held stream,
 * e.g. module=0x12D01 params="0x12d01=1,0x12d03=3,0x12d04=3"), rawparam
 * (0x12D36) plus raw (a hex byte string, e.g. the 28-byte 0x12D36 filter blob
 * "000080c000000040000000c000000000000000c0000000c0000080c0") sent as one
 * extra frame per combo, delay ms
 * before the first frame (2000), gap ms between topo/device combos (1200),
 * seqgap ms between the params of one combo (150), freq Hz (1000). The long
 * names durationMs/delayMs/gapMs/sndDevIds/appType/sampleRate/frequencyHz are
 * accepted as aliases of the short ones.
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
        val player = TonePlayer(route, frequencyHz = request.frequencyHz.toDouble())
        val frames = QemFrames(this)
        try {
            if (!player.start()) {
                Log.e(TAG, "no tone, no frames sent")
                return
            }
            Thread.sleep(request.delayMs)
            var index = 0
            val combos = request.topos.size * request.sndDevIds.size
            val perComboMs = request.gapMs +
                (request.params.size - 1).coerceAtLeast(0) * request.seqGapMs
            for (topo in request.topos) {
                for (devId in request.sndDevIds) {
                    index++
                    Log.i(TAG, "combo $index/$combos topo=0x${Integer.toHexString(topo)}" +
                        " snddev=$devId module=0x${Integer.toHexString(request.module)}" +
                        " params=" + request.params.joinToString { "0x" + Integer.toHexString(it) })
                    request.params.forEachIndexed { i, param ->
                        frames.send(
                            topo = topo,
                            appType = request.appType,
                            sndDevId = devId,
                            sampleRate = request.sampleRate,
                            calType = request.calType,
                            persist = request.persist,
                            module = request.module,
                            param = param,
                            value = request.values.getOrElse(i) {
                                request.values.lastOrNull() ?: request.value
                            },
                        )
                        if (i < request.params.size - 1) {
                            Thread.sleep(request.seqGapMs)
                        }
                    }
                    request.raw?.let { payload ->
                        Log.i(TAG, "raw topo=0x${Integer.toHexString(topo)} snddev=$devId" +
                            " module=0x${Integer.toHexString(request.module)}" +
                            " param=0x${Integer.toHexString(request.rawParam)} bytes=${payload.size}")
                        frames.sendRaw(
                            topo = topo,
                            appType = request.appType,
                            sndDevId = devId,
                            sampleRate = request.sampleRate,
                            calType = request.calType,
                            persist = request.persist,
                            module = request.module,
                            param = request.rawParam,
                            payload = payload,
                        )
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
            player.stop()
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
        val param: Int,
        val value: Int,
        val params: List<Int>,
        val values: List<Int>,
        val seqGapMs: Long,
        val frequencyHz: Int,
        val rawParam: Int,
        val raw: ByteArray?,
    ) {
        companion object {
            fun from(intent: Intent?, defaultSndDevId: Int): Request {
                val duration = intExtra(intent, EXTRA_DURATION, EXTRA_DURATION_MS,
                    default = DEFAULT_DURATION)
                val param = intExtra(intent, EXTRA_PARAM, default = QemFrames.PARAM_ENABLE)
                val value = intExtra(intent, EXTRA_VALUE, default = QemFrames.ENABLE)
                val paramValues = parseParamValues(stringExtra(intent, EXTRA_PARAMS), value)
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
                    calType = intExtra(intent, EXTRA_CALTYPE, default = QemFrames.CAL_TYPE_POPP),
                    persist = intExtra(intent, EXTRA_PERSIST, default = DEFAULT_PERSIST),
                    module = intExtra(intent, EXTRA_MODULE, default = QemFrames.MODULE_INTERNAL),
                    param = param,
                    value = value,
                    params = paramValues.map { it.first }.ifEmpty { listOf(param) },
                    values = paramValues.map { it.second }.ifEmpty { listOf(value) },
                    seqGapMs = intExtra(intent, EXTRA_SEQGAP, default = DEFAULT_SEQGAP)
                        .coerceAtLeast(0).toLong(),
                    frequencyHz = intExtra(intent, EXTRA_FREQ, EXTRA_FREQUENCY,
                        default = DEFAULT_FREQ),
                    rawParam = intExtra(intent, EXTRA_RAWPARAM, default = DEFAULT_RAWPARAM),
                    raw = hexBytes(stringExtra(intent, EXTRA_RAW)),
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

            private fun stringExtra(intent: Intent?, vararg names: String): String? {
                for (name in names) {
                    val raw = intent?.extras?.get(name) ?: continue
                    if (raw is String) return raw
                }
                return null
            }

            private fun parseInts(raw: String?): List<Int> = raw.orEmpty()
                .split(',', ' ', ';')
                .mapNotNull { parseNumber(it.trim()) }

            /** Splits "0x12d01=1,0x12d03=3" into (param, value) pairs. */
            private fun parseParamValues(raw: String?, fallbackValue: Int): List<Pair<Int, Int>> =
                raw.orEmpty()
                    .split(',', ' ', ';')
                    .mapNotNull { token ->
                        val trimmed = token.trim()
                        if (trimmed.isEmpty()) {
                            null
                        } else {
                            val parts = trimmed.split('=', limit = 2)
                            val param = parseNumber(parts[0]) ?: return@mapNotNull null
                            val value = if (parts.size == 2) parseNumber(parts[1]) else null
                            param to (value ?: fallbackValue)
                        }
                    }

            private fun parseNumber(raw: String?): Int? = raw?.let {
                runCatching {
                    if (it.startsWith("0x", ignoreCase = true)) {
                        it.substring(2).toLong(16).toInt()
                    } else {
                        it.toLong().toInt()
                    }
                }.getOrNull()
            }

            /** Decodes a hex byte string, tolerating 0x prefixes and separators. */
            private fun hexBytes(raw: String?): ByteArray? {
                val cleaned = raw.orEmpty().replace("0x", "", ignoreCase = true)
                    .replace(Regex("[^0-9a-fA-F]"), "")
                if (cleaned.isEmpty() || cleaned.length % 2 != 0) {
                    return null
                }
                return ByteArray(cleaned.length / 2) { i ->
                    cleaned.substring(i * 2, i * 2 + 2).toInt(16).toByte()
                }
            }

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
            private const val EXTRA_FREQ = "freq"
            private const val EXTRA_FREQUENCY = "frequencyHz"
            private const val EXTRA_RAWPARAM = "rawparam"
            private const val EXTRA_RAW = "raw"

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
            private const val DEFAULT_RAWPARAM = 0x12D36
            private val DEFAULT_TOPOS = listOf(0x10012D00, 0x10012D01, 0x10312)
        }
    }

    private companion object {
        const val TAG = "DiracToneProbe"
    }
}
