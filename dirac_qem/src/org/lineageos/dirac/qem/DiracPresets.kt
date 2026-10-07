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

import kotlin.math.abs
import kotlin.math.roundToInt

/**
 * User-visible Dirac models and presets, taken from the stock OnePlus
 * DiracManager resources. The model value is the external device slot, whose
 * filter data in diracvdd.bin is "FAE<value>".
 *
 * A band gain is an integer count of half-dB steps: 9 is +4.5 dB, -8 is
 * -4.0 dB. Every preset value is a multiple of 0.5 dB, so half-steps are the
 * exact resolution the curves carry and neither the model nor the stored
 * preference ever has to hold a float.
 */
object DiracPresets {
    val MODEL_NAMES = arrayOf(
        "General Enhancement",
        "OnePlus Type-C Bullets Earphones",
        "OnePlus Bullets",
        "OnePlus Bullets (V2)",
        "OnePlus Icons",
        "Feat. JBL E1+",
    )
    val MODEL_FILTER_INDEX = intArrayOf(6, 5, 2, 5, 4, 3)

    val PRESET_NAMES = arrayOf(
        "None", "Custom", "Pop", "Rock & Roll", "Country", "Jazz",
        "Classic", "Metal", "Blues", "Hip Hop", "Dance", "Electronic",
    )

    const val STYLE_NONE = 0
    const val STYLE_CUSTOM = 1
    const val EQ_BANDS = 7

    /** Half-steps a preset row may reach: the +/-6 dB the sliders travel. */
    const val MIN_HALF_DB = -12
    const val MAX_HALF_DB = 12

    /** One half-step is 0.5 dB, so the model and the ACDB frame share it. */
    const val HALF_DB_TO_DB = 0.5f

    /**
     * Centre frequency of each Dirac band, in Hz, in the order the band gains
     * are stored. Used to evaluate the real response shape of the bands.
     */
    val BAND_FREQS = floatArrayOf(68f, 165f, 400f, 972f, 2_000f, 6_000f, 14_000f)

    /** The stock preset rows, in half-dB steps. */
    private val PRESET_VALUES = arrayOf(
        intArrayOf(0, 0, 0, 0, 0, 0, 0),
        intArrayOf(0, -6, -10, 0, 0, -6, 0),
        intArrayOf(-8, 4, -4, 0, -4, -4, -8),
        intArrayOf(0, 0, -4, -4, 4, 4, 0),
        intArrayOf(0, 0, 0, -4, -6, 0, 0),
        intArrayOf(0, 0, 0, 0, 4, 9, 9),
        intArrayOf(4, 0, 0, -4, -8, 0, 0),
        intArrayOf(4, 8, -12, 8, 0, 2, 4),
        intArrayOf(6, 6, -6, 0, -6, 0, 4),
        intArrayOf(0, 8, 4, 0, -4, -4, 8),
        intArrayOf(6, 6, -2, 0, -6, 0, 0),
    )

    fun valuesFor(style: Int, custom: IntArray): IntArray = when {
        style == STYLE_NONE -> PRESET_VALUES[0].copyOf()
        style == STYLE_CUSTOM -> custom
        style in PRESET_VALUES.indices -> PRESET_VALUES[style - 1].copyOf()
        else -> PRESET_VALUES[0].copyOf()
    }

    fun encodeBands(bands: IntArray): String = bands.joinToString(";")

    /**
     * Reads the stored custom row, tolerating the dB-float form v8 wrote: a
     * value written with a decimal point or an exponent is in dB and is
     * doubled and rounded to the nearest half step. A value without either is
     * already a half-step count. Anything unreadable, or a row of the wrong
     * length, reads as all zeros, and every value is clamped to the travel.
     */
    fun parseCustomBands(stored: String?): IntArray {
        if (stored == null) return IntArray(EQ_BANDS)
        val parts = stored.split(";").filter { it.isNotEmpty() }
        if (parts.size != EQ_BANDS) return IntArray(EQ_BANDS)
        val bands = IntArray(EQ_BANDS)
        for (index in 0 until EQ_BANDS) {
            val half = parseHalfDb(parts[index]) ?: return IntArray(EQ_BANDS)
            bands[index] = half.coerceIn(MIN_HALF_DB, MAX_HALF_DB)
        }
        return bands
    }

    private fun parseHalfDb(part: String): Int? = try {
        if (part.contains('.') || part.contains('e', ignoreCase = true)) {
            (part.toFloat() * 2f).roundToInt()
        } else {
            part.toInt()
        }
    } catch (e: NumberFormatException) {
        null
    }

    /** The half-dB label: "+4.5 dB", "+6 dB", "-4 dB", "0 dB". */
    fun formatHalfDb(halfDb: Int): String {
        val magnitude = abs(halfDb)
        val text = if (magnitude % 2 == 0) (magnitude / 2).toString() else "${magnitude / 2}.5"
        val sign = when {
            halfDb > 0 -> "+"
            halfDb < 0 -> "-"
            else -> ""
        }
        return "$sign$text dB"
    }

    /** The band label, e.g. "68 Hz", "972 Hz", "2 kHz", "14 kHz". */
    fun bandLabel(index: Int): String {
        val hz = BAND_FREQS.getOrElse(index) { 0f }.toInt()
        return if (hz >= 1_000 && hz % 1_000 == 0) "${hz / 1_000} kHz" else "$hz Hz"
    }
}
