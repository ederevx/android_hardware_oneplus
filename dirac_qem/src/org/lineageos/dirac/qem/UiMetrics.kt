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
import android.util.TypedValue

/**
 * The one owner for the display-metric and theme-colour conversions every
 * widget needs, so no widget re-derives dp or the theme accent on its own.
 */
internal object UiMetrics {

    /** [value] in device-independent pixels, as pixels for [context]'s display. */
    fun dp(context: Context, value: Float): Float =
        TypedValue.applyDimension(
            TypedValue.COMPLEX_UNIT_DIP, value, context.resources.displayMetrics,
        )

    /**
     * The theme accent as a colour. The attribute can resolve to a colour state
     * list, so it has to go through getColor instead of TypedValue.data.
     */
    fun accent(context: Context): Int {
        val typed = context.obtainStyledAttributes(intArrayOf(android.R.attr.colorAccent))
        val color = typed.getColor(0, 0)
        typed.recycle()
        return if (color != 0) color else FALLBACK_ACCENT
    }

    private val FALLBACK_ACCENT = 0xFF8AB4F8.toInt()
}
