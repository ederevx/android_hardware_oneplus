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
import android.content.res.ColorStateList
import android.graphics.Canvas
import android.graphics.Paint
import android.util.AttributeSet
import android.view.GestureDetector
import android.view.MotionEvent
import android.view.View
import android.view.accessibility.AccessibilityEvent
import android.view.accessibility.AccessibilityNodeInfo
import kotlin.math.roundToInt

/**
 * A vertical equalizer slider that reuses the Material slider colour attributes
 * (trackColorActive/trackColorInactive/thumbColor/haloColor) so it inherits the
 * dynamic colour and matches the system volume sliders: a rounded track with the
 * active part drawn from the centre detent, and a thin vertical-bar thumb.
 *
 * The slider keeps no value: it draws the band it drives straight from the
 * engine's array and writes that array on a drag.
 */
class VerticalSlider @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0,
) : View(context, attrs, defStyleAttr) {

    var onValueChanged: ((Int) -> Unit)? = null

    /** Index of the engine band this slider drives. */
    var bandIndex: Int = 0

    private var minValue = DiracPresets.MIN_HALF_DB
    private var maxValue = DiracPresets.MAX_HALF_DB

    private val trackPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val activePaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val thumbPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val haloPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val detentPaint = Paint(Paint.ANTI_ALIAS_FLAG)

    private val trackColors: ColorStateList
    private val haloColors: ColorStateList

    private val accent = UiMetrics.accent(context)
    private val detentHalfWidth = UiMetrics.dp(context, 8f)
    private val detentHalfHeight = UiMetrics.dp(context, 1f)
    private val hitMargin = UiMetrics.dp(context, 8f)

    private val trackWidth: Float
    private val barWidth: Float
    private val barHeight: Float
    private val haloRadius: Float

    private var dragging = false

    /** A value has been staged since the last frame flushed it. */
    private var staged = false

    /** A value has been staged since it was last persisted. */
    private var dirty = false

    private val flush = Runnable { flushFrame() }
    private val settle = Runnable { settleNow() }

    private val gestureDetector = GestureDetector(
        context,
        object : GestureDetector.SimpleOnGestureListener() {
            override fun onDoubleTap(e: MotionEvent): Boolean {
                updateValue(0f)
                return true
            }
        },
    )

    init {
        trackWidth = UiMetrics.dp(context, 4f)
        barWidth = UiMetrics.dp(context, 4f)
        barHeight = UiMetrics.dp(context, 28f)
        haloRadius = UiMetrics.dp(context, 20f)
        detentPaint.color = 0x66FFFFFF
        detentPaint.strokeWidth = UiMetrics.dp(context, 1f)

        val ta = context.obtainStyledAttributes(
            attrs,
            intArrayOf(
                com.google.android.material.R.attr.trackColorInactive,
                com.google.android.material.R.attr.haloColor,
            ),
        )
        trackColors = ta.getColorStateList(0) ?: ColorStateList.valueOf(0x26FFFFFF)
        haloColors = ta.getColorStateList(1) ?: ColorStateList.valueOf(0x33FFFFFF)
        ta.recycle()

        isClickable = true
        isFocusable = true
    }

    fun setMinMax(min: Int, max: Int) {
        minValue = min
        maxValue = max
        invalidate()
    }

    /** The key this slider drives, in half-dB steps, from the owned band array. */
    private fun currentValue(): Int =
        DiracState.currentBands(context).getOrElse(bandIndex) { 0 }

    /**
     * Stages the value in the owned band array and restarts the settle window.
     * That array is the only thing that moves while the finger is down: the
     * curve redraws from it on the next frame, and the HAL is not touched until
     * the position settles.
     */
    private fun updateValue(newValue: Float) {
        val value = newValue.roundToInt().coerceIn(minValue, maxValue)
        if (!DiracState.setBand(context, bandIndex, value)) {
            return
        }
        dirty = true
        if (!staged) {
            staged = true
            postOnAnimation(flush)
        }
        removeCallbacks(settle)
        postDelayed(settle, SETTLE_MS)
    }

    /** The per-frame UI step: repaint and notify, with no binder work at all. */
    private fun flushFrame() {
        if (!staged) {
            return
        }
        staged = false
        val started = if (DiracTrace.ENABLED) System.nanoTime() else 0L
        invalidate()
        val invalidated = if (DiracTrace.ENABLED) System.nanoTime() else 0L
        onValueChanged?.invoke(currentValue())
        DiracTrace.log(TAG) {
            "flush invalidate ns=${invalidated - started} " +
                "notify ns=${System.nanoTime() - invalidated}"
        }
    }

    /**
     * The settle: flush the last frame so the row shows the settled value, then
     * write the tune once and hand the one push of the gesture to the HAL
     * engine's executor, off the UI thread. Called by the idle window, by touch-up or
     * cancel, and on detach.
     */
    private fun settleNow() {
        removeCallbacks(settle)
        flushFrame()
        if (!dirty) {
            return
        }
        dirty = false
        DiracState.persistBands(context)
        DiracQemEffect.pushBands(context)
    }

    private fun trackTop(): Float = paddingTop + barHeight / 2f

    private fun trackBottom(): Float = height - paddingBottom - barHeight / 2f

    private fun yForValue(v: Int): Float {
        val span = trackBottom() - trackTop()
        return trackTop() + span * (maxValue - v) / (maxValue - minValue)
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val width = resolveSizeAndState(UiMetrics.dp(context, 48f).toInt(), widthMeasureSpec, 0)
        val height = resolveSizeAndState(UiMetrics.dp(context, 200f).toInt(), heightMeasureSpec, 0)
        setMeasuredDimension(width, height)
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val cx = width / 2f
        val top = trackTop()
        val bottom = trackBottom()
        val half = trackWidth / 2f
        val value = currentValue()

        trackPaint.color = trackColors.getColorForState(drawableState, trackColors.defaultColor)
        canvas.drawRoundRect(cx - half, top, cx + half, bottom, half, half, trackPaint)

        val zeroY = yForValue(0)
        canvas.drawRect(
            cx - detentHalfWidth, zeroY - detentHalfHeight,
            cx + detentHalfWidth, zeroY + detentHalfHeight, detentPaint,
        )

        val valueY = yForValue(value)
        // Head brighter than trail, trail brighter than track: the thumb is the
        // full accent, like the response curve's line, and the trail is the
        // accent dimmed, like that curve's translucent fill.
        activePaint.color = accent and 0x59FFFFFF
        canvas.drawRoundRect(
            cx - half, minOf(zeroY, valueY), cx + half, maxOf(zeroY, valueY), half, half, activePaint,
        )

        if (dragging) {
            haloPaint.color = haloColors.getColorForState(drawableState, haloColors.defaultColor)
            canvas.drawCircle(cx, valueY, haloRadius, haloPaint)
        }

        thumbPaint.color = accent
        canvas.drawRoundRect(
            cx - barWidth / 2f, valueY - barHeight / 2f,
            cx + barWidth / 2f, valueY + barHeight / 2f,
            barWidth / 2f, barWidth / 2f, thumbPaint,
        )
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        gestureDetector.onTouchEvent(event)
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                // A drag that starts on a bar belongs to the bar, so the list
                // must not steal it to scroll. A touch off the bars is left
                // alone, so the page still scrolls normally.
                if (!isWithinBar(event.y)) {
                    return false
                }
                dragging = true
                parent?.requestDisallowInterceptTouchEvent(true)
                updateFromY(event.y)
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                if (dragging) {
                    updateFromY(event.y)
                }
                return true
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                if (dragging) {
                    dragging = false
                    parent?.requestDisallowInterceptTouchEvent(false)
                    // Run the last frame now, then write the settled tune once.
                    removeCallbacks(flush)
                    // The lift ends the settle window early and delivers now.
                    settleNow()
                    invalidate()
                    if (event.actionMasked == MotionEvent.ACTION_UP) {
                        performClick()
                    }
                }
                return true
            }
        }
        return super.onTouchEvent(event)
    }

    /**
     * A generous hit area around the track: the whole slider width counts, and
     * the track is extended vertically by the thumb and a margin.
     */
    private fun isWithinBar(y: Float): Boolean {
        val top = trackTop() - barHeight / 2f - hitMargin
        val bottom = trackBottom() + barHeight / 2f + hitMargin
        return y >= top && y <= bottom
    }

    private fun updateFromY(y: Float) {
        val span = trackBottom() - trackTop()
        if (span <= 0f) {
            return
        }
        val ratio = ((y - trackTop()) / span).coerceIn(0f, 1f)
        updateValue(maxValue - ratio * (maxValue - minValue))
    }

    override fun performClick(): Boolean {
        super.performClick()
        return true
    }

    override fun onInitializeAccessibilityNodeInfo(info: AccessibilityNodeInfo) {
        super.onInitializeAccessibilityNodeInfo(info)
        info.className = "android.widget.SeekBar"
        info.rangeInfo = AccessibilityNodeInfo.RangeInfo.obtain(
            AccessibilityNodeInfo.RangeInfo.RANGE_TYPE_INT,
            minValue.toFloat(),
            maxValue.toFloat(),
            currentValue().toFloat(),
        )
    }

    override fun onInitializeAccessibilityEvent(event: AccessibilityEvent) {
        super.onInitializeAccessibilityEvent(event)
        event.className = "android.widget.SeekBar"
    }

    /** A detach with an unsettled value must still leave the HAL on it. */
    override fun onDetachedFromWindow() {
        removeCallbacks(flush)
        removeCallbacks(settle)
        if (dirty) {
            dirty = false
            DiracState.persistBands(context)
            DiracQemEffect.pushBands(context)
        }
        super.onDetachedFromWindow()
    }

    private companion object {
        const val TAG = "DiracQemSlider"

        /** Idle after the last move that reads as a settle, not a frame gap. */
        const val SETTLE_MS = 120L
    }
}
