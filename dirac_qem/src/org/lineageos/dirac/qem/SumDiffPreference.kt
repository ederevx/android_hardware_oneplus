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
import android.view.View
import android.widget.TextView
import androidx.preference.Preference
import androidx.preference.PreferenceViewHolder
import com.android.settingslib.widget.NormalPaddingMixin
import kotlin.math.roundToInt

/**
 * The stereo-width row: a labelled [HorizontalSlider] that reads and writes the
 * owned Sum/Diff width, so the preference keeps no width state of its own and a
 * rebind simply repaints from it. The magnitude sits above the bar and the
 * title below it, mirroring an equalizer column laid on its side.
 */
class SumDiffPreference @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : Preference(context, attrs), NormalPaddingMixin {

    private var valueLabel: TextView? = null

    init {
        isSelectable = false
        layoutResource = R.layout.dirac_sumdiff
    }

    override fun onBindViewHolder(holder: PreferenceViewHolder) {
        super.onBindViewHolder(holder)
        val bar = holder.itemView.findViewById<HorizontalSlider>(R.id.dirac_sumdiff_slider)
        valueLabel = holder.itemView.findViewById(R.id.dirac_sumdiff_value)
        bar?.onValueChanged = { value -> showValue(value) }
        bar?.invalidate()
        showValue(DiracState.sumDiff(context))
        // The trailing button restores the default through the slider, so it
        // inherits the staged settle and the one write that follows it.
        holder.itemView.findViewById<View>(R.id.dirac_sumdiff_reset)?.setOnClickListener {
            bar?.reset()
        }
    }

    private fun showValue(value: Float) {
        valueLabel?.text =
            context.getString(R.string.dirac_sumdiff_value, (value * 100f).roundToInt())
    }
}
