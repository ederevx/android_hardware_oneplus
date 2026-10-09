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
import android.view.accessibility.AccessibilityNodeInfo
import kotlin.math.roundToInt

/**
 * A vertical equalizer slider: the [BarSlider] geometry with the band's half-dB
 * integers as its value, active from the centre detent and a 4 dp track with a
 * 4x28 dp bar.
 *
 * The slider keeps no value: it draws the band it drives straight from the
 * engine's array and stages that array on a drag.
 */
class VerticalSlider @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0,
) : BarSlider(context, attrs, defStyleAttr) {

    var onValueChanged: ((Int) -> Unit)? = null

    /** Index of the engine band this slider drives. */
    var bandIndex: Int = 0

    override val isVertical = true
    override val trackThickness = UiMetrics.dp(context, 4f)
    override val barThickness = UiMetrics.dp(context, 4f)
    override val barLength = UiMetrics.dp(context, 28f)
    override val rangeType = AccessibilityNodeInfo.RangeInfo.RANGE_TYPE_INT

    init {
        setMinMax(DiracPresets.MIN_HALF_DB, DiracPresets.MAX_HALF_DB)
    }

    fun setMinMax(min: Int, max: Int) {
        super.setMinMax(min.toFloat(), max.toFloat())
    }

    override fun trackStart(): Float = paddingTop + barLength / 2f

    override fun trackEnd(): Float = height - paddingBottom - barLength / 2f

    override fun positionForValue(value: Float): Float {
        val span = trackEnd() - trackStart()
        return trackStart() + span * (maxValue - value) / (maxValue - minValue)
    }

    override fun valueForPosition(position: Float): Float {
        val span = trackEnd() - trackStart()
        if (span <= 0f) {
            return minValue
        }
        val ratio = ((position - trackStart()) / span).coerceIn(0f, 1f)
        return maxValue - ratio * (maxValue - minValue)
    }

    override fun activeSpan(detentPos: Float, valuePos: Float): Pair<Float, Float> =
        minOf(detentPos, valuePos) to maxOf(detentPos, valuePos)

    override fun axisPosition(event: MotionEvent): Float = event.y

    override fun currentValue(): Float =
        DiracState.currentBands(context).getOrElse(bandIndex) { 0 }.toFloat()

    override fun stageValue(value: Float): Boolean =
        DiracState.setBand(context, bandIndex, value.roundToInt())

    override fun persistAndPush() {
        DiracState.persistBands(context)
        DiracQemEffect.pushBands(context)
    }

    override fun onValueFlushed(value: Float) {
        onValueChanged?.invoke(value.roundToInt())
    }
}
