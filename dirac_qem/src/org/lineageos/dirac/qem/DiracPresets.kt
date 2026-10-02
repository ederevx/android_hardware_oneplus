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

/**
 * User-visible Dirac models and presets, taken from the stock OnePlus
 * DiracManager resources. The model value is the external device slot, whose
 * filter data in diracvdd.bin is "FAE<value>".
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

    private val PRESET_VALUES = arrayOf(
        "0.0;0.0;0.0;0.0;0.0;0.0;0.0",
        "0.0;-3.0;-5.0;0.0;0.0;-3.0;0.0",
        "-4.0;2.0;-2.0;0.0;-2.0;-2.0;-4.0",
        "0.0;0.0;-2.0;-2.0;2.0;2.0;0.0",
        "0.0;0.0;0.0;-2.0;-3.0;0.0;0.0",
        "0.0;0.0;0.0;0.0;2.0;4.5;4.5",
        "2.0;0.0;0.0;-2.0;-4.0;0.0;0.0",
        "2.0;4.0;-6.0;4.0;0.0;1.0;2.0",
        "3.0;3.0;-3.0;0.0;-3.0;0.0;2.0",
        "0.0;4.0;2.0;0.0;-2.0;-2.0;4.0",
        "3.0;3.0;-1.0;0.0;-3.0;0.0;0.0",
    )

    fun valuesFor(style: Int, custom: FloatArray): FloatArray = when {
        style == STYLE_NONE -> parse(PRESET_VALUES[0])
        style == STYLE_CUSTOM -> custom
        style in PRESET_VALUES.indices -> parse(PRESET_VALUES[style - 1])
        else -> parse(PRESET_VALUES[0])
    }

    private fun parse(row: String): FloatArray =
        row.split(";").filter { it.isNotEmpty() }.map { it.toFloat() }.toFloatArray()
}
