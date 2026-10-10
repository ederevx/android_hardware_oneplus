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

/**
 * DEV PROBE, not part of the shipped feature.
 *
 * The stereo-width row without its route gate and with the range widened to
 * [DiracState.SUMDIFF_MAX], so the Sum/Diff value can be dragged past the limit
 * the layers below enforce and the sweep can be read on the device.
 *
 * It drives the same owned width as [SumDiffPreference] - one state, two
 * controls - and reuses that row's layout and its slider geometry, so the
 * staged settle and the single write at the settle are the shipping row's own.
 * The push differs on purpose: [SumDiffProbeSlider] sends the raw DSP frame
 * even where the host owns the width, which is the whole point of the probe.
 *
 * What the value does above 1.0 is what this control measures: the host stage
 * clamps its side gain at width 1.0 (StereoWidth::SetWidth), so only the ADSP
 * frame carries the value further, and whether the firmware acts on it there is
 * not established by any source we have.
 */
class SumDiffProbePreference @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : Preference(context, attrs), NormalPaddingMixin {

    private var bar: SumDiffProbeSlider? = null
    private var reset: View? = null
    private var valueLabel: TextView? = null

    init {
        isSelectable = false
        layoutResource = R.layout.dirac_sumdiff_probe
    }

    override fun onBindViewHolder(holder: PreferenceViewHolder) {
        super.onBindViewHolder(holder)
        bar = holder.itemView.findViewById<SumDiffProbeSlider>(R.id.dirac_sumdiff_slider)?.apply {
            setMinMax(0f, DiracState.SUMDIFF_MAX)
            onValueChanged = { value -> showValue(value) }
        }
        reset = holder.itemView.findViewById(R.id.dirac_sumdiff_reset)
        valueLabel = holder.itemView.findViewById(R.id.dirac_sumdiff_value)
        holder.itemView.findViewById<TextView>(R.id.dirac_sumdiff_label)?.text =
            context.getString(R.string.dirac_sumdiff_probe_title)
        // The trailing button restores the shipping default through the slider,
        // so it adds no second write path.
        reset?.setOnClickListener { bar?.reset() }
        showValue(DiracState.sumDiff(context))
    }

    private fun showValue(value: Float) {
        valueLabel?.text = context.getString(R.string.dirac_sumdiff_probe_value, value)
    }
}
