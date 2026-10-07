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
import android.util.AttributeSet
import android.view.ContextThemeWrapper
import android.view.Gravity
import android.view.ViewGroup
import android.widget.LinearLayout
import android.widget.TextView
import androidx.preference.Preference
import androidx.preference.PreferenceViewHolder

/**
 * The inline equalizer board: one [VerticalSlider] per Dirac band with the
 * current gain above it and the band label below it. Bands are written straight
 * to [DiracQemEffect], so the board never needs its own state.
 */
class EqBoardPreference @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : Preference(context, attrs) {

    var onBandChanged: ((Int, Float) -> Unit)? = null

    private val sliders = ArrayList<VerticalSlider>()
    private val valueLabels = ArrayList<TextView>()
    private var pendingBands: FloatArray? = null
    private var updating = false
    private var built = false

    init {
        isSelectable = false
        layoutResource = R.layout.dirac_eq_board
    }

    override fun onBindViewHolder(holder: PreferenceViewHolder) {
        super.onBindViewHolder(holder)
        if (!built) {
            build(holder.itemView as ViewGroup)
            built = true
        }
        pendingBands?.let { applyBands(it) }
    }

    private fun build(row: ViewGroup) {
        val density = context.resources.displayMetrics.density
        val sliderContext = ContextThemeWrapper(context, R.style.SettingslibSliderStyle_Expressive)

        sliders.clear()
        valueLabels.clear()
        for (band in 0 until DiracPresets.EQ_BANDS) {
            val column = LinearLayout(context).apply {
                orientation = LinearLayout.VERTICAL
                gravity = Gravity.CENTER_HORIZONTAL
                layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
            }
            val valueText = TextView(context).apply {
                gravity = Gravity.CENTER
                textSize = 12f
                text = formatDb(0f)
            }
            val slider = VerticalSlider(sliderContext).apply {
                layoutParams = LinearLayout.LayoutParams(
                    (48 * density).toInt(), (200 * density).toInt(),
                )
                onValueChanged = { value ->
                    valueText.text = formatDb(value)
                    if (!updating) {
                        onBandChanged?.invoke(band, value)
                    }
                }
            }
            val labelText = TextView(context).apply {
                gravity = Gravity.CENTER
                textSize = 12f
                text = BAND_LABELS[band]
            }
            column.addView(valueText)
            column.addView(slider)
            column.addView(labelText)
            row.addView(column)
            sliders.add(slider)
            valueLabels.add(valueText)
        }
    }

    fun setBands(bands: FloatArray) {
        pendingBands = bands.copyOf()
        if (built) {
            applyBands(bands)
        }
    }

    private fun applyBands(bands: FloatArray) {
        updating = true
        sliders.forEachIndexed { index, slider ->
            val value = bands.getOrElse(index) { 0f }
            slider.setValue(value)
            valueLabels.getOrNull(index)?.text = formatDb(value)
        }
        updating = false
    }

    private companion object {
        val BAND_LABELS = arrayOf(
            "68 Hz", "165 Hz", "400 Hz", "972 Hz", "2 kHz", "6 kHz", "14 kHz",
        )

        fun formatDb(value: Float): String =
            if (value > 0f) "+${value.toInt()} dB" else "${value.toInt()} dB"
    }
}
