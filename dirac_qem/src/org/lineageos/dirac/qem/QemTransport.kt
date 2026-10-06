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
import android.media.AudioManager

/**
 * Sends one calibration frame to the audio HAL for every selector combination,
 * matching how the stock se.dirac.acs broadcasts a frame.
 */
class QemTransport(context: Context) {
    private val audioManager = context.getSystemService(AudioManager::class.java)

    fun send(
        module: Int,
        topo: Int,
        devices: IntArray,
        param: Int,
        payload: ByteArray,
        sndDevId: Int = 0,
        appTypes: IntArray = QemProtocol.APP_TYPES,
        persistValues: IntArray = QemProtocol.PERSIST,
        rates: IntArray = QemProtocol.SAMPLE_RATES,
        calTypes: IntArray = intArrayOf(QemProtocol.CAL_TYPE_COPP),
    ) {
        val data = QemProtocol.frame(module, param, payload)
        devices.forEach { device ->
            appTypes.forEach { appType ->
                persistValues.forEach { persist ->
                    rates.forEach { rate ->
                        calTypes.forEach { calType ->
                            audioManager.setParameters(
                                QemProtocol.setString(
                                    topo, appType, persist, device, sndDevId, rate, data, calType)
                            )
                        }
                    }
                }
            }
        }
    }
}
