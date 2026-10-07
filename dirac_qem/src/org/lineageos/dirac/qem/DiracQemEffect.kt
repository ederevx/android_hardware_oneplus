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
import android.content.SharedPreferences
import android.media.AudioManager

/**
 * Owns the Dirac QEM state and pushes it to the audio HAL on every change.
 * State lives in device-protected storage so the boot receiver can read it.
 *
 * Two output routes exist, mirroring the two Dirac topologies: the speaker
 * (module 0x12D00 on topology 0x10012D00, sound device 2) and the wired
 * headset (module 0x12D01 on topology 0x10012D01, sound device 9). The same
 * parameter sequence -- 0x12D01 enable, 0x12D35 EQ enable, 0x12D36 28-byte
 * coefficients, 0x12D67 sound-field enable, plus the headset-only 0x12D03 /
 * 0x12D04 filter select -- is written to whichever module the active route
 * names.
 */
object DiracQemEffect {
    private const val PREFS = "dirac_qem"
    private const val KEY_ENABLED = "enabled"
    private const val KEY_OUTPUT = "output"
    private const val KEY_APPLIED = "applied"
    private const val KEY_STYLE = "style"
    private const val KEY_CUSTOM = "custom"
    private const val KEY_MODEL = "model"
    private const val KEY_MOVIE = "movie"

    const val OUTPUT_INTERNAL = 0
    const val OUTPUT_EXTERNAL = 1

    /**
     * Frames for the speaker name the sound device explicitly instead of the
     * audio device, because the HAL's own output routing does not have to
     * resolve AUDIO_DEVICE_OUT_SPEAKER to this exact sound device.
     */
    private const val SND_DEVICE_OUT_SPEAKER = 2

    /**
     * Wired headset sound device. The headset stream reports acdb_dev_id 10,
     * but the frame is addressed with cal_snddevid 9 -- the snd_device the HAL
     * resolves for the wired headset -- while cal_devid stays 0. This is the
     * selector the headset topology 0x10012D01 accepts; addressing it with the
     * speaker module 0x12D00 is rejected outright.
     */
    private const val SND_DEVICE_OUT_HEADSET = 9

    private const val SCALAR_TONAL_BALANCE = 3
    private const val SCALAR_LOUDNESS = 4

    private fun prefs(context: Context): SharedPreferences =
        context.createDeviceProtectedStorageContext()
            .getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    fun isEnabled(context: Context): Boolean = prefs(context).getBoolean(KEY_ENABLED, false)

    /**
     * The route the effect drives right now. A connected wired headset always
     * wins, because the wired route can be live before HEADSET_PLUG reaches
     * this app (or without the user touching the switch), and the engine must
     * never keep driving the speaker module for a headset stream.
     */
    fun output(context: Context): Int {
        val audioManager = context.getSystemService(AudioManager::class.java)
        if (audioManager != null && audioManager.isWiredHeadsetOn) {
            return OUTPUT_EXTERNAL
        }
        return prefs(context).getInt(KEY_OUTPUT, OUTPUT_INTERNAL)
    }

    fun style(context: Context): Int = prefs(context).getInt(KEY_STYLE, DiracPresets.STYLE_NONE)

    fun model(context: Context): Int = prefs(context).getInt(KEY_MODEL, 0)

    fun isMovie(context: Context): Boolean = prefs(context).getBoolean(KEY_MOVIE, false)

    fun customBands(context: Context): FloatArray {
        val stored = prefs(context).getString(KEY_CUSTOM, null)
            ?: return FloatArray(DiracPresets.EQ_BANDS)
        val parts = stored.split(";").filter { it.isNotEmpty() }
        if (parts.size != DiracPresets.EQ_BANDS) return FloatArray(DiracPresets.EQ_BANDS)
        return parts.map { it.toFloat() }.toFloatArray()
    }

    fun currentBands(context: Context): FloatArray =
        DiracPresets.valuesFor(style(context), customBands(context))

    fun setEnabled(context: Context, enabled: Boolean) {
        prefs(context).edit().putBoolean(KEY_ENABLED, enabled).apply()
        apply(context)
    }

    fun setStyle(context: Context, style: Int) {
        prefs(context).edit().putInt(KEY_STYLE, style).apply()
        apply(context)
    }

    fun setBand(context: Context, band: Int, value: Float) {
        val bands = customBands(context).copyOf()
        bands[band] = value
        prefs(context).edit()
            .putString(KEY_CUSTOM, bands.joinToString(";"))
            .putInt(KEY_STYLE, DiracPresets.STYLE_CUSTOM)
            .apply()
        apply(context)
    }

    fun setModel(context: Context, model: Int) {
        prefs(context).edit().putInt(KEY_MODEL, model).apply()
        apply(context)
    }

    /**
     * Switch routes. The old route's module is explicitly disabled so it does
     * not stay enabled when the headset is unplugged, then the new route's
     * current state is applied.
     */
    fun setOutput(context: Context, output: Int) {
        prefs(context).edit().putInt(KEY_OUTPUT, output).apply()
        val current = output(context)
        val applied = prefs(context).getInt(KEY_APPLIED, OUTPUT_INTERNAL)
        if (applied != current) {
            sendDisable(context, applied)
        }
        apply(context)
    }

    fun setMovie(context: Context, movie: Boolean) {
        prefs(context).edit().putBoolean(KEY_MOVIE, movie).apply()
        send(context, SCALAR_TONAL_BALANCE, QemProtocol.scalarPayload(
            SCALAR_TONAL_BALANCE, if (movie) MOVIE_TONAL_BALANCE else DEFAULT_TONAL_BALANCE))
    }

    fun setBluetooth(context: Context, connected: Boolean) {
        send(context, SCALAR_LOUDNESS, QemProtocol.scalarPayload(
            SCALAR_LOUDNESS, if (connected) BT_LOUDNESS else DEFAULT_LOUDNESS))
        if (connected) setOutput(context, OUTPUT_EXTERNAL)
    }

    private fun send(context: Context, key: Int, payload: ByteArray) {
        val output = output(context)
        QemTransport(context).send(
            moduleFor(output), topoFor(output), devicesFor(output),
            QemProtocol.PARAM_SCALAR_BASE + key, payload, sndDevIdFor(output))
    }

    fun apply(context: Context) {
        val output = output(context)
        if (!isEnabled(context)) {
            sendOp(context, QemProtocol.PARAM_ENABLE, QemProtocol.intPayload(0))
            markApplied(context, output)
            return
        }
        val bands = currentBands(context)
        sendOp(context, QemProtocol.PARAM_ENABLE, QemProtocol.intPayload(1))
        sendOp(context, QemProtocol.PARAM_EQ_ENABLE, QemProtocol.intPayload(1))
        sendOp(context, QemProtocol.PARAM_EQ_BANDS, QemProtocol.floatArrayPayload(bands))
        sendOp(context, QemProtocol.PARAM_SFX_ENABLE, QemProtocol.intPayload(1))
        if (output == OUTPUT_EXTERNAL) {
            sendOp(context, QemProtocol.PARAM_HDSOUND_ENABLE, QemProtocol.intPayload(1))
            val index = DiracPresets.MODEL_FILTER_INDEX[model(context)]
            sendOp(context, QemProtocol.PARAM_HDSOUND_FILTERIDX, QemProtocol.intPayload(index))
        }
        markApplied(context, output)
    }

    private fun sendOp(context: Context, param: Int, payload: ByteArray) {
        val output = output(context)
        QemTransport(context).send(
            moduleFor(output), topoFor(output), devicesFor(output), param, payload,
            sndDevIdFor(output))
    }

    private fun sendDisable(context: Context, output: Int) {
        QemTransport(context).send(
            moduleFor(output), topoFor(output), devicesFor(output),
            QemProtocol.PARAM_ENABLE, QemProtocol.intPayload(0), sndDevIdFor(output))
    }

    private fun markApplied(context: Context, output: Int) {
        prefs(context).edit().putInt(KEY_APPLIED, output).apply()
    }

    private fun moduleFor(output: Int): Int =
        if (output == OUTPUT_EXTERNAL) QemProtocol.MODULE_EXTERNAL else QemProtocol.MODULE_INTERNAL

    private fun topoFor(output: Int): Int =
        if (output == OUTPUT_EXTERNAL) QemProtocol.TOPO_EXTERNAL else QemProtocol.TOPO_INTERNAL

    /**
     * The headset frame carries an explicit cal_snddevid, so one pass is
     * enough; the speaker keeps its single-element device list.
     */
    private fun devicesFor(output: Int): IntArray =
        if (output == OUTPUT_EXTERNAL) intArrayOf(0) else QemProtocol.DEVICES_INTERNAL

    private fun sndDevIdFor(output: Int): Int =
        if (output == OUTPUT_EXTERNAL) SND_DEVICE_OUT_HEADSET else SND_DEVICE_OUT_SPEAKER

    /**
     * DEV PROBE: send a single Dirac calibration frame with an explicit
     * topology (and app type) instead of the constants this app normally uses,
     * so the topology the live audio stream registered can be identified from
     * the audio HAL's own log. Touches no stored state: it neither writes a
     * preference nor uses cal_persist=1, so a wrong candidate cannot leave a
     * bad persistent calibration behind.
     */
    fun probeCal(context: Context, output: Int, topo: Int, appType: Int, rate: Int) {
        QemTransport(context).send(
            moduleFor(output), topo, devicesFor(output), QemProtocol.PARAM_ENABLE,
            QemProtocol.intPayload(1), sndDevIdFor(output),
            appTypes = intArrayOf(appType), persistValues = intArrayOf(0),
            rates = intArrayOf(rate))
    }

    private const val MOVIE_TONAL_BALANCE = -1.0f
    private const val DEFAULT_TONAL_BALANCE = 0.0f
    private const val BT_LOUDNESS = -1.0f
    private const val DEFAULT_LOUDNESS = 0.0f
}
