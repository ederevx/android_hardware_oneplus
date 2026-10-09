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
import android.view.MotionEvent

/**
 * A horizontal stereo-width slider: the [BarSlider] geometry with the owned
 * Sum/Diff width (0..1) as its value, active from the left end and a thicker
 * 8 dp track with a 28x8 dp bar. The detent and the reset both mark the middle
 * of the range, so the row rests centred rather than at the zero end.
 *
 * The slider keeps no value: it draws the width it drives straight from the
 * owned state and stages that value on a drag.
 */
class HorizontalSlider @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0,
) : BarSlider(context, attrs, defStyleAttr) {

    /** Reports each flushed value so the row can relabel from it. */
    var onValueChanged: ((Float) -> Unit)? = null

    override val isVertical = false
    override val trackThickness = UiMetrics.dp(context, 8f)
    override val barThickness = UiMetrics.dp(context, 8f)
    override val barLength = UiMetrics.dp(context, 28f)

    override fun trackStart(): Float = paddingLeft + barLength / 2f

    override fun trackEnd(): Float = width - paddingRight - barLength / 2f

    override fun positionForValue(value: Float): Float {
        val span = trackEnd() - trackStart()
        return trackStart() + span * (value - minValue) / (maxValue - minValue)
    }

    override fun valueForPosition(position: Float): Float {
        val span = trackEnd() - trackStart()
        if (span <= 0f) {
            return minValue
        }
        val ratio = ((position - trackStart()) / span).coerceIn(0f, 1f)
        return minValue + ratio * (maxValue - minValue)
    }

    override fun activeSpan(detentPos: Float, valuePos: Float): Pair<Float, Float> =
        trackStart() to valuePos

    override fun axisPosition(event: MotionEvent): Float = event.x

    override fun currentValue(): Float = DiracState.sumDiff(context)

    override fun resetValue(): Float = DiracState.SUMDIFF_DEFAULT

    override fun detentValue(): Float = (minValue + maxValue) / 2f

    override fun onValueFlushed(value: Float) {
        onValueChanged?.invoke(value)
    }

    override fun stageValue(value: Float): Boolean = DiracState.setSumDiff(context, value)

    override fun persistAndPush() {
        DiracState.persistSumDiff(context)
        DiracQemEffect.setSumDiff(context)
    }
}
