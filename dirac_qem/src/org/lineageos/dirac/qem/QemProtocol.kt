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

import android.util.Base64
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Encodes the QEM calibration frames the stock se.dirac.acs sends to the audio
 * HAL and the cal_* parameter string that carries them.
 */
object QemProtocol {
    const val MODULE_INTERNAL = 0x12D00
    const val MODULE_EXTERNAL = 0x12D01
    const val TOPO_INTERNAL = 0x10012D00
    const val TOPO_EXTERNAL = 0x10012D01

    val DEVICES_INTERNAL = intArrayOf(2)
    val DEVICES_EXTERNAL = intArrayOf(4, 8, 131072, 2097152, 4096, 8192, 16384, 67108864)
    val APP_TYPES = intArrayOf(69936, 69940)
    val SAMPLE_RATES = intArrayOf(44100, 48000, 96000, 192000)
    val PERSIST = intArrayOf(0, 1)

    const val PARAM_ENABLE = 0x12D01
    const val PARAM_HDSOUND_ENABLE = 0x12D03
    const val PARAM_HDSOUND_FILTERIDX = 0x12D04
    const val PARAM_EQ_ENABLE = 0x12D35
    const val PARAM_EQ_BANDS = 0x12D36
    const val PARAM_SFX_ENABLE = 0x12D67
    const val PARAM_SCALAR_BASE = 0x14000

    /**
     * A calibration frame is a 12-byte little-endian header followed by the
     * payload: u32 module id, u32 parameter id, u16 size, u16 reserved.
     */
    fun frame(module: Int, param: Int, payload: ByteArray): ByteArray {
        val buffer = ByteBuffer.allocate(HEADER_SIZE + payload.size).order(ByteOrder.LITTLE_ENDIAN)
        buffer.putInt(module).putInt(param).putShort(payload.size.toShort()).putShort(0)
        buffer.put(payload)
        return buffer.array()
    }

    fun intPayload(value: Int): ByteArray =
        ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(value).array()

    fun floatArrayPayload(values: FloatArray): ByteArray {
        val buffer = ByteBuffer.allocate(values.size * 4).order(ByteOrder.LITTLE_ENDIAN)
        values.forEach { buffer.putFloat(it) }
        return buffer.array()
    }

    fun scalarPayload(key: Int, value: Float): ByteArray =
        ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putInt(key).putFloat(value).array()

    /** Base64 without wrapping or padding, matching the stock encoder. */
    fun encode(data: ByteArray): String =
        Base64.encodeToString(data, Base64.NO_WRAP or Base64.NO_PADDING)

    fun setString(
        topo: Int,
        appType: Int,
        persist: Int,
        device: Int,
        sampleRate: Int,
        data: ByteArray,
    ): String =
        "cal_caltype=0;cal_topoid=$topo;cal_apptype=$appType;cal_persist=$persist;" +
            "cal_devid=$device;cal_samplerate=$sampleRate;cal_data=${encode(data)}"

    private const val HEADER_SIZE = 12
}
