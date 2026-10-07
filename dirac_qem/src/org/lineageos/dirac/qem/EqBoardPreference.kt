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
import com.android.settingslib.widget.NormalPaddingMixin

/**
 * The inline equalizer board: one [VerticalSlider] per Dirac band with the
 * current gain above it and the band label below it. Every slider reads and
 * writes the engine's one band array, so the board keeps no band state of its
 * own and a rebind simply repaints from the engine.
 */
class EqBoardPreference @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : Preference(context, attrs), NormalPaddingMixin {

    var onBandChanged: ((Int, Int) -> Unit)? = null

    private val sliders = ArrayList<VerticalSlider>()
    private val valueLabels = ArrayList<TextView>()
    private var built = false

    init {
        isSelectable = false
        layoutResource = R.layout.dirac_eq_board
    }

    override fun onBindViewHolder(holder: PreferenceViewHolder) {
        val started = System.nanoTime()
        super.onBindViewHolder(holder)
        if (!built) {
            holder.itemView.findViewById<ViewGroup>(R.id.dirac_eq_board_row)?.let {
                build(it)
                built = true
            }
        }
        refresh()
        DiracTrace.log(TAG) { "board bind ns=${System.nanoTime() - started}" }
    }

    private fun build(row: ViewGroup) {
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
                // A fixed width keeps a new label from asking the list to
                // measure again; the weighted column already fixes the width.
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                )
                gravity = Gravity.CENTER
                textSize = 12f
                text = DiracPresets.formatHalfDb(0)
                setPadding(0, 0, 0, UiMetrics.dp(context, 6f).toInt())
            }
            val slider = VerticalSlider(sliderContext).apply {
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, UiMetrics.dp(context, 200f).toInt(),
                )
                bandIndex = band
                onValueChanged = { value ->
                    valueText.text = DiracPresets.formatHalfDb(value)
                    onBandChanged?.invoke(band, value)
                }
            }
            val labelText = TextView(context).apply {
                gravity = Gravity.CENTER
                textSize = 12f
                text = DiracPresets.bandLabel(band)
                setPadding(0, UiMetrics.dp(context, 6f).toInt(), 0, 0)
            }
            column.addView(valueText)
            column.addView(slider)
            column.addView(labelText)
            row.addView(column)
            sliders.add(slider)
            valueLabels.add(valueText)
        }
    }

    /** Repaints the bars from the engine's array and relabels them. */
    fun refresh() {
        val bands = DiracQemEffect.currentBands(context)
        sliders.forEachIndexed { index, slider ->
            slider.invalidate()
            valueLabels.getOrNull(index)?.text =
                DiracPresets.formatHalfDb(bands.getOrElse(index) { 0 })
        }
    }

    private companion object {
        const val TAG = "DiracQemBoard"
    }
}
