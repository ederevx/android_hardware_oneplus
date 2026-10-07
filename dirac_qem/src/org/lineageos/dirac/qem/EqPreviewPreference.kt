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
import androidx.preference.Preference
import androidx.preference.PreferenceViewHolder

/**
 * The preview card at the top of the Dirac page, drawing the live equalizer
 * curve so the current tuning is visible without reading seven sliders.
 */
class EqPreviewPreference @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : Preference(context, attrs) {

    private var curve: EqCurveView? = null
    private var pendingBands: FloatArray? = null

    init {
        isSelectable = false
        layoutResource = R.layout.dirac_eq_preview
    }

    override fun onBindViewHolder(holder: PreferenceViewHolder) {
        super.onBindViewHolder(holder)
        curve = holder.itemView.findViewById(R.id.dirac_eq_curve)
        pendingBands?.let { curve?.setBands(it) }
    }

    fun setBands(bands: FloatArray) {
        pendingBands = bands.copyOf()
        curve?.setBands(bands)
    }

    fun setBand(index: Int, value: Float) {
        pendingBands?.set(index, value)
        curve?.setBand(index, value)
    }
}
