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
 * title below it with the reset beside the title, so the bar owns the full
 * width of the row.
 *
 * On an output the DSP voices the row is inert: the host stage the bar drives
 * never runs there, so the bar and its reset are greyed and the magnitude slot
 * reads N/A instead of the stored width.
 */
class SumDiffPreference @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : Preference(context, attrs), NormalPaddingMixin {

    private var bar: HorizontalSlider? = null
    private var reset: View? = null
    private var valueLabel: TextView? = null
    private var dspHandled = false

    init {
        isSelectable = false
        layoutResource = R.layout.dirac_sumdiff
    }

    override fun onBindViewHolder(holder: PreferenceViewHolder) {
        super.onBindViewHolder(holder)
        bar = holder.itemView.findViewById<HorizontalSlider>(R.id.dirac_sumdiff_slider)
        reset = holder.itemView.findViewById(R.id.dirac_sumdiff_reset)
        valueLabel = holder.itemView.findViewById(R.id.dirac_sumdiff_value)
        bar?.onValueChanged = { value -> showValue(value) }
        refresh()
        // The trailing button restores the default through the slider, so it
        // inherits the staged settle and the one write that follows it.
        reset?.setOnClickListener { bar?.reset() }
    }

    /**
     * Re-reads whether the DSP owns the widening and repaints the row. A rebind,
     * a jack change and an A2DP change all land here; caching the answer keeps
     * the per-frame relabel from resolving the route again.
     */
    fun refresh() {
        dspHandled = DiracState.isWideningDspHandled(context)
        bar?.let {
            it.isEnabled = !dspHandled
            it.alpha = if (dspHandled) DISABLED_ALPHA else 1f
            it.invalidate()
        }
        reset?.let {
            it.isEnabled = !dspHandled
            it.alpha = if (dspHandled) DISABLED_ALPHA else 1f
        }
        showValue(DiracState.sumDiff(context))
    }

    private fun showValue(value: Float) {
        valueLabel?.text = if (dspHandled) {
            context.getString(R.string.dirac_sumdiff_na)
        } else {
            context.getString(R.string.dirac_sumdiff_value, (value * 100f).roundToInt())
        }
    }

    private companion object {
        /** The standard inert look; the bar draws its own colours. */
        const val DISABLED_ALPHA = 0.38f
    }
}
