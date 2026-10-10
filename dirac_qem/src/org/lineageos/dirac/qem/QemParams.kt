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

/**
 * Every calibration parameter this app sends: its id, how its payload is built
 * from the live state, and the calibration type the audio HAL must use for it.
 *
 * One description per parameter, so the enable pass, the disable pass and the
 * dev probe compose their frames in the same place. A parameter's id, payload
 * rule and cal type cannot then drift between a normal push and a probe, which
 * is what makes a probe frame comparable with a pass frame.
 *
 * Payload rules read [DiracState], the single owner of the live values. A caller
 * that holds a snapshot instead (the band push, whose values come from the
 * gesture) uses only the id from here.
 */
object QemParams {
    private const val SCALAR_TONAL_BALANCE = 3
    private const val SCALAR_LOUDNESS = 4
    private const val MOVIE_TONAL_BALANCE = -1.0f
    private const val DEFAULT_TONAL_BALANCE = 0.0f
    private const val BT_LOUDNESS = -1.0f
    private const val DEFAULT_LOUDNESS = 0.0f

    /**
     * One parameter: what it is called, whether the ACDB loader validates it,
     * and the payload rule.
     */
    class Spec(
        val id: Int,
        val calType: Int = QemProtocol.CALTYPE_ACDB,
        private val payload: (Context) -> ByteArray,
    ) {
        fun payloadFor(context: Context): ByteArray = payload(context)
    }

    val ENABLE = Spec(QemProtocol.PARAM_ENABLE) { QemProtocol.intPayload(1) }
    val ENABLE_OFF = Spec(QemProtocol.PARAM_ENABLE) { QemProtocol.intPayload(0) }
    val EQ_ENABLE = Spec(QemProtocol.PARAM_EQ_ENABLE) { QemProtocol.intPayload(1) }
    val EQ_BANDS = Spec(QemProtocol.PARAM_EQ_BANDS) {
        QemProtocol.eqBandsPayload(DiracState.currentBands(it))
    }
    val SFX_ENABLE = Spec(QemProtocol.PARAM_SFX_ENABLE) { QemProtocol.intPayload(1) }

    val LOUDNESS = Spec(QemProtocol.PARAM_SCALAR_BASE + SCALAR_LOUDNESS) { context ->
        QemProtocol.scalarPayload(SCALAR_LOUDNESS,
            if (DiracState.isBluetoothConnected(context)) BT_LOUDNESS else DEFAULT_LOUDNESS)
    }
    val TONAL_BALANCE = Spec(QemProtocol.PARAM_SCALAR_BASE + SCALAR_TONAL_BALANCE) { context ->
        QemProtocol.scalarPayload(SCALAR_TONAL_BALANCE,
            if (DiracState.isMovie(context)) MOVIE_TONAL_BALANCE else DEFAULT_TONAL_BALANCE)
    }

    val HDSOUND_ENABLE = Spec(QemProtocol.PARAM_HDSOUND_ENABLE) { QemProtocol.intPayload(1) }
    val HDSOUND_FILTERIDX = Spec(QemProtocol.PARAM_HDSOUND_FILTERIDX) {
        QemProtocol.intPayload(DiracState.hdsoundIndex(it))
    }

    /** The enable pass, in the order the module expects it. */
    val ENABLE_SET = listOf(ENABLE, EQ_ENABLE, EQ_BANDS, SFX_ENABLE, LOUDNESS)

    /** The two headset-only parameters, written only on the external route. */
    val HDSOUND_SET = listOf(HDSOUND_ENABLE, HDSOUND_FILTERIDX)

    /** What the enable pass emits on a route: the full set the module expects. */
    fun productionSet(output: Int): List<Spec> =
        if (output == DiracState.OUTPUT_EXTERNAL) ENABLE_SET + HDSOUND_SET else ENABLE_SET

    /** Every parameter the dev probe can be asked for by id. */
    val ALL = ENABLE_SET + HDSOUND_SET + TONAL_BALANCE

    private val BY_ID = ALL.associateBy { it.id }

    fun byId(id: Int): Spec? = BY_ID[id]

    /** The ids of a parameter list, for a log line. */
    fun ids(params: List<Spec>): String =
        params.joinToString(",") { "0x%x".format(it.id) }
}
