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

import android.content.Context
import android.media.AudioManager
import android.util.Base64
import android.util.Log
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Sends one DiRAC QEM calibration frame per call through the public
 * `AudioManager.setParameters` cal_* string, the same path the stock
 * se.dirac.acs app uses. Only MODIFY_AUDIO_SETTINGS, a normal permission, is
 * needed, so nothing here requires a privileged or persistent install.
 *
 * Exactly one parameter rides per call and one call means one setParameters, so
 * a refusal at the ADM or the DSP can be attributed to the parameter that call
 * named: an earlier shape that sent a parameter sequence let a rejection be
 * blamed on either member of the pair.
 *
 * The device is named the way the shipping app names it. A zero id leaves
 * cal_snddevid off the string entirely and sends `cal_devid=0` alone, which is
 * the only framing the wired route accepts: an explicit snd_device_t that
 * disagrees with the live stream's acdb device is refused with `active
 * device/stream not found`. A non-zero cal_devid is worse than either - the HAL
 * parses it as an audio_devices_t bitmask and then falls back to a built-in
 * device, which pinned every frame to the speaker whatever was plugged in.
 *
 * cal_caltype is per frame, because the parameters a QEM frame carries do not
 * all want the same one: 0 asks for the ACDB module table (which only resolves
 * for a parameter the table holds) and 1 for the raw path that bypasses it.
 * cal_persist=0 keeps the sent calibration non-persistent.
 */
class QemFrames(context: Context) {
    private val audioManager: AudioManager =
        context.getSystemService(AudioManager::class.java)

    /**
     * Sends exactly one parameter. [floatValue] selects a 4-byte little-endian
     * float payload for a scalar the module reads as float; an integer payload
     * of the same size is a different bit pattern (0 happens to equal 0.0f, but
     * 1 is a denormal, not 1.0f).
     */
    fun send(
        topo: Int,
        appType: Int,
        sndDevId: Int,
        sampleRate: Int,
        calType: Int = CAL_TYPE_RAW,
        persist: Int = 0,
        module: Int = MODULE_INTERNAL,
        param: Int = PARAM_ENABLE,
        value: Int = ENABLE,
        floatValue: Float? = null,
    ) {
        val payload = if (floatValue != null) floatPayload(floatValue) else intPayload(value)
        val frame = frame(module, param, payload)
        val string = setString(topo, appType, persist, sndDevId, sampleRate, frame, calType)
        Log.i(TAG, "send topo=0x${hex(topo)} apptype=$appType snddev=$sndDevId caltype=$calType " +
            "persist=$persist rate=$sampleRate module=0x${hex(module)} param=0x${hex(param)} " +
            "value=${floatValue ?: value} payload=${payload.size}B frame=${frame.size}B")
        audioManager.setParameters(string)
    }

    /** 12-byte little-endian header (module, parameter, size, reserved) + payload. */
    private fun frame(module: Int, param: Int, payload: ByteArray): ByteArray =
        ByteBuffer.allocate(HEADER_SIZE + payload.size)
            .order(ByteOrder.LITTLE_ENDIAN)
            .putInt(module)
            .putInt(param)
            .putShort(payload.size.toShort())
            .putShort(0)
            .put(payload)
            .array()

    private fun intPayload(value: Int): ByteArray =
        ByteBuffer.allocate(VALUE_SIZE).order(ByteOrder.LITTLE_ENDIAN).putInt(value).array()

    private fun floatPayload(value: Float): ByteArray =
        ByteBuffer.allocate(VALUE_SIZE).order(ByteOrder.LITTLE_ENDIAN).putFloat(value).array()

    /**
     * Base64 stays padded ('=') because the HAL decodes cal_data with the strict
     * b64_pton(), which rejects any length that is not a multiple of four.
     */
    private fun setString(
        topo: Int,
        appType: Int,
        persist: Int,
        sndDevId: Int,
        sampleRate: Int,
        data: ByteArray,
        calType: Int,
    ): String {
        val device = if (sndDevId > 0) "cal_devid=0;cal_snddevid=$sndDevId" else "cal_devid=0"
        return "cal_caltype=$calType;cal_topoid=$topo;cal_apptype=$appType;cal_persist=$persist;" +
            "$device;cal_samplerate=$sampleRate;cal_data=" +
            Base64.encodeToString(data, Base64.NO_WRAP)
    }

    private fun hex(value: Int): String = Integer.toHexString(value)

    companion object {
        /** The raw path: libacdbloader's get_audio_popp_id, which skips the table. */
        const val CAL_TYPE_RAW = 1

        /** The ACDB table path: only resolves for a key the module's table holds. */
        const val CAL_TYPE_ACDB = 0

        const val PARAM_ENABLE = 0x12D01
        const val PARAM_SUMDIFF = 0x12D02
        const val PARAM_EQ_ENABLE = 0x12D35

        /** Default module: the speaker topology instantiates 0x12D00. */
        const val MODULE_INTERNAL = 0x12D00

        /** The wired topology 0x10012D01 instantiates 0x12D01. */
        const val MODULE_EXTERNAL = 0x12D01
        const val ENABLE = 1

        private const val TAG = "DiracToneProbe"
        private const val HEADER_SIZE = 12
        private const val VALUE_SIZE = 4
    }
}
