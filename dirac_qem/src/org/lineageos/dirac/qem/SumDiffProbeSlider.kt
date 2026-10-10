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

/**
 * DEV PROBE, not part of the shipped feature.
 *
 * The width slider with the instrument's push: it persists and republishes the
 * width exactly as the shipping slider does, and then sends the raw DSP frame
 * whatever owns the width, so the probe can measure what the ADSP does with the
 * value. The shipping paths keep exclusive ownership; this is the one caller
 * allowed to send the frame where the host owns the width.
 */
class SumDiffProbeSlider @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0,
) : HorizontalSlider(context, attrs, defStyleAttr) {

    override fun persistAndPush() {
        DiracState.persistSumDiff(context)
        DiracQemEffect.probeSumDiff(context)
    }
}
