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
 * Sends one DiRAC QEM calibration frame per (topology, sound device) through
 * the public `AudioManager.setParameters` cal_* string, the same path the stock
 * se.dirac.acs app uses. Only MODIFY_AUDIO_SETTINGS, a normal permission, is
 * needed, so nothing here requires a privileged or persistent install.
 *
 * cal_caltype=1 selects libacdbloader's get_audio_popp_id, which dumps the
 * kernel's active RTAC ADM table and always logs the failed lookup, and
 * cal_persist=0 keeps the sent calibration non-persistent. The target device is
 * named with cal_snddevid, a HAL snd_device_t supplied by [ToneTarget];
 * cal_devid stays 0, because the HAL parses a non-zero cal_devid as an
 * audio_devices_t bitmask and then falls back to a built-in device, which
 * pinned every frame to the speaker whatever was plugged in.
 */
class QemFrames(context: Context) {
    private val audioManager: AudioManager =
        context.getSystemService(AudioManager::class.java)

    fun send(
        topo: Int,
        appType: Int,
        sndDevId: Int,
        sampleRate: Int,
        calType: Int = CAL_TYPE_POPP,
        persist: Int = 0,
        param: Int = PARAM_ENABLE,
    ) {
        val frame = frame(MODULE_INTERNAL, param, enablePayload())
        val string = setString(topo, appType, persist, sndDevId, sampleRate, frame, calType)
        Log.i(TAG, "send cal_snddev=$sndDevId topo=0x${hex(topo)} apptype=$appType " +
            "caltype=$calType persist=$persist rate=$sampleRate param=0x${hex(param)} " +
            "data=${frame.size}B")
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

    private fun enablePayload(): ByteArray =
        ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(ENABLE).array()

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
    ): String =
        "cal_caltype=$calType;cal_topoid=$topo;cal_apptype=$appType;cal_persist=$persist;" +
            "cal_devid=0;cal_snddevid=$sndDevId;cal_samplerate=$sampleRate;" +
            "cal_data=${Base64.encodeToString(data, Base64.NO_WRAP)}"

    private fun hex(value: Int): String = Integer.toHexString(value)

    companion object {
        const val CAL_TYPE_POPP = 1
        const val PARAM_ENABLE = 0x12D01

        private const val TAG = "DiracToneProbe"
        private const val MODULE_INTERNAL = 0x12D00
        private const val ENABLE = 1
        private const val HEADER_SIZE = 12
    }
}
