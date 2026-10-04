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

/**
 * Owns the Dirac QEM state and pushes it to the audio HAL on every change.
 * State lives in device-protected storage so the boot receiver can read it.
 */
object DiracQemEffect {
    private const val PREFS = "dirac_qem"
    private const val KEY_ENABLED = "enabled"
    private const val KEY_OUTPUT = "output"
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

    private const val SCALAR_TONAL_BALANCE = 3
    private const val SCALAR_LOUDNESS = 4

    private fun prefs(context: Context): SharedPreferences =
        context.createDeviceProtectedStorageContext()
            .getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    fun isEnabled(context: Context): Boolean = prefs(context).getBoolean(KEY_ENABLED, false)

    fun output(context: Context): Int = prefs(context).getInt(KEY_OUTPUT, OUTPUT_INTERNAL)

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

    fun setOutput(context: Context, output: Int) {
        if (output == output(context)) return
        prefs(context).edit().putInt(KEY_OUTPUT, output).apply()
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
        val module = if (output == OUTPUT_EXTERNAL) QemProtocol.MODULE_EXTERNAL else QemProtocol.MODULE_INTERNAL
        val topo = if (output == OUTPUT_EXTERNAL) QemProtocol.TOPO_EXTERNAL else QemProtocol.TOPO_INTERNAL
        val devices = if (output == OUTPUT_EXTERNAL) QemProtocol.DEVICES_EXTERNAL else QemProtocol.DEVICES_INTERNAL
        val sndDevId = if (output == OUTPUT_INTERNAL) SND_DEVICE_OUT_SPEAKER else 0
        QemTransport(context).send(
            module, topo, devices, QemProtocol.PARAM_SCALAR_BASE + key, payload, sndDevId)
    }

    fun apply(context: Context) {
        if (!isEnabled(context)) {
            sendOp(context, QemProtocol.PARAM_ENABLE, QemProtocol.intPayload(0))
            return
        }
        val output = output(context)
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
    }

    private fun sendOp(context: Context, param: Int, payload: ByteArray) {
        val output = output(context)
        val module = if (output == OUTPUT_EXTERNAL) QemProtocol.MODULE_EXTERNAL else QemProtocol.MODULE_INTERNAL
        val topo = if (output == OUTPUT_EXTERNAL) QemProtocol.TOPO_EXTERNAL else QemProtocol.TOPO_INTERNAL
        val devices = if (output == OUTPUT_EXTERNAL) QemProtocol.DEVICES_EXTERNAL else QemProtocol.DEVICES_INTERNAL
        val sndDevId = if (output == OUTPUT_INTERNAL) SND_DEVICE_OUT_SPEAKER else 0
        QemTransport(context).send(module, topo, devices, param, payload, sndDevId)
    }

    private const val MOVIE_TONAL_BALANCE = -1.0f
    private const val DEFAULT_TONAL_BALANCE = 0.0f
    private const val BT_LOUDNESS = -1.0f
    private const val DEFAULT_LOUDNESS = 0.0f
}
