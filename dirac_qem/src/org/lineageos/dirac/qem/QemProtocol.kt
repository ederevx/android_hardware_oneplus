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

    /**
     * The application types every push covers, in full.
     *
     * A stream's app type is chosen on the player/HAL side - 0x11130 (69936) is
     * what a music stream registers, 0x11134 (69940) is the other case - and this
     * process cannot read which one is live, so it covers both and lets the ADSP
     * loader reject whichever is not registered. Those rejections are the
     * "active device/stream not found" lines in the ACDB log; narrowing this list
     * would silence them at the price of never voicing the other app type.
     */
    val APP_TYPES = intArrayOf(69936, 69940)
    val SAMPLE_RATES = intArrayOf(44100, 48000, 96000, 192000)
    val PERSIST = intArrayOf(0, 1)

    const val PARAM_ENABLE = 0x12D01
    const val PARAM_SUMDIFF = 0x12D02
    const val PARAM_HDSOUND_ENABLE = 0x12D03
    const val PARAM_HDSOUND_FILTERIDX = 0x12D04
    const val PARAM_EQ_ENABLE = 0x12D35
    const val PARAM_EQ_BANDS = 0x12D36
    const val PARAM_SFX_ENABLE = 0x12D67
    const val PARAM_SCALAR_BASE = 0x14000

    /**
     * The ADSP panorama2 Sum/Diff balance is a continuous 0.0..1.0 mid/side
     * width, read as weights (1-v, 1+v); 0.0 is bypass.
     */
    const val SUMDIFF_OFF = 0.0f

    /**
     * The calibration type the HAL hands to the ACDB loader. An ACDB frame is
     * validated against the module's parameter LUT before it is sent; a raw
     * frame bypasses that lookup. The Sum/Diff param has no LUT entry, so it is
     * the one frame sent raw.
     */
    const val CALTYPE_ACDB = 0
    const val CALTYPE_RAW = 1

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

    fun floatPayload(value: Float): ByteArray =
        ByteBuffer.allocate(FLOAT_SIZE).order(ByteOrder.LITTLE_ENDIAN).putFloat(value).array()

    /**
     * The 0x12D36 band payload is seven little-endian 32-bit floats in dB.
     * The model carries half-dB integers, so the division by two happens here,
     * at the one place the frame is encoded; it is exact for every half step.
     */
    fun eqBandsPayload(halfDb: IntArray): ByteArray {
        val buffer = ByteBuffer.allocate(halfDb.size * FLOAT_SIZE).order(ByteOrder.LITTLE_ENDIAN)
        halfDb.forEach { buffer.putFloat(it * DiracPresets.HALF_DB_TO_DB) }
        return buffer.array()
    }

    fun scalarPayload(key: Int, value: Float): ByteArray =
        ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putInt(key).putFloat(value).array()

    /**
     * Base64 with '=' padding and no wrapping. The HAL decodes cal_data with
     * the strict b64_pton(), which rejects any length that is not a multiple
     * of four, so the padding is load-bearing rather than cosmetic.
     */
    fun encode(data: ByteArray): String =
        Base64.encodeToString(data, Base64.NO_WRAP)

    /**
     * A frame names the platform audio device in cal_devid and no
     * cal_snddevid. set_audiocal() in the HAL this ROM builds
     * (hardware/qcom-caf/sdm660/audio/hal/msm8974/platform.c:8242-8264) resolves cal_devid
     * through its own output routing into its own sound device, and it runs
     * that resolution only for a non-zero cal_devid: a frame carrying 0 leaves
     * the sound device unset, fails the HAL's ACDB id lookup and is dropped
     * before the calibration loader. Naming an explicit cal_snddevid instead
     * would make the app reproduce a mapping the HAL already owns, including
     * its rate-dependent cases.
     */
    fun setString(
        topo: Int,
        appType: Int,
        persist: Int,
        device: Int,
        sampleRate: Int,
        data: ByteArray,
        calType: Int = CALTYPE_ACDB,
    ): String =
        "cal_caltype=$calType;cal_topoid=$topo;cal_apptype=$appType;cal_persist=$persist;" +
            "cal_devid=$device;cal_samplerate=$sampleRate;cal_data=${encode(data)}"


    private const val HEADER_SIZE = 12
    private const val FLOAT_SIZE = 4
}
