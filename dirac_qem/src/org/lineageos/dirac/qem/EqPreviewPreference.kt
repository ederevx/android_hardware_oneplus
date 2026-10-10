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
import com.android.settingslib.widget.NormalPaddingMixin

/**
 * The preview card at the top of the Dirac page. The curve reads the engine's
 * band array itself, so this only has to trigger the redraw.
 */
class EqPreviewPreference @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : Preference(context, attrs), NormalPaddingMixin {

    private var curve: EqCurveView? = null

    init {
        isSelectable = false
        layoutResource = R.layout.dirac_eq_preview
    }

    override fun onBindViewHolder(holder: PreferenceViewHolder) {
        val started = System.nanoTime()
        super.onBindViewHolder(holder)
        curve = holder.itemView.findViewById(R.id.dirac_eq_curve)
        curve?.invalidate()
        DiracTrace.log(TAG) { "preview bind ns=${System.nanoTime() - started}" }
    }

    /** Redraws the curve from the current stored bands. */
    fun refresh() {
        val view = curve
        if (view != null) {
            view.invalidate()
        } else {
            // The view handle was never attached (or was replaced without a
            // rebind), so ask the list to bind this row again instead.
            notifyChanged()
        }
    }

    private companion object {
        const val TAG = "DiracQemPreview"
    }
}
