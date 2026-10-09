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

/**
 * The one owner of the shared slider behaviour: the range, the drag state, the
 * staged/settle scheduling, the Material bar drawing and the touch and
 * accessibility handling.
 *
 * A subclass supplies only its orientation geometry, its value mapping and the
 * two state actions [stageValue] and [persistAndPush], so the optimizations --
 * no binder work per frame, one write at the settle, a flush on detach -- are
 * implemented once and inherited by every slider.
 */
abstract class BarSlider @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0,
) : View(context, attrs, defStyleAttr) {

    /** The value range the track spans. */
    protected var minValue = 0.0f
    protected var maxValue = 1.0f

    /** Whether the track runs along y (true) or x (false). */
    protected abstract val isVertical: Boolean

    /** Track and bar extents: along the axis for the bar, across it otherwise. */
    protected abstract val trackThickness: Float
    protected abstract val barThickness: Float
    protected abstract val barLength: Float

    /** Track end points along the slider axis, in view coordinates. */
    protected abstract fun trackStart(): Float
    protected abstract fun trackEnd(): Float

    /** Axis coordinate of [value] and its inverse. */
    protected abstract fun positionForValue(value: Float): Float
    protected abstract fun valueForPosition(position: Float): Float

    /** The value this slider drives, straight from the owned state. */
    protected abstract fun currentValue(): Float

    /** Stages [value] in the owned state; true only when it changed. */
    protected abstract fun stageValue(value: Float): Boolean

    /** Writes the settled value once and pushes it to the audio layer. */
    protected abstract fun persistAndPush()

    /** The active span, in axis coordinates, for the detent and the value. */
    protected abstract fun activeSpan(detentPos: Float, valuePos: Float): Pair<Float, Float>

    /** The touch coordinate along the slider axis. */
    protected abstract fun axisPosition(event: MotionEvent): Float

    /** The value a double-tap restores. */
    protected open fun resetValue(): Float = 0f

    /** The value the detent marker sits at. */
    protected open fun detentValue(): Float = 0f

    /** Called after a flushed frame with the new value; no-op by default. */
    protected open fun onValueFlushed(value: Float) {}

    /** Accessibility range kind. */
    protected open val rangeType: Int = AccessibilityNodeInfo.RangeInfo.RANGE_TYPE_FLOAT

    private val trackPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val activePaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val thumbPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val haloPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val detentPaint = Paint(Paint.ANTI_ALIAS_FLAG)

    private val trackColors: ColorStateList
    private val haloColors: ColorStateList

    private val accent = UiMetrics.accent(context)
    private val detentHalfAlong = UiMetrics.dp(context, 1f)
    private val detentHalfAcross = UiMetrics.dp(context, 8f)
    private val haloRadius = UiMetrics.dp(context, 20f)
    private val hitMargin = UiMetrics.dp(context, 8f)

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
                updateValue(resetValue())
                return true
            }
        },
    )

    init {
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

    fun setMinMax(min: Float, max: Float) {
        minValue = min
        maxValue = max
        invalidate()
    }

    /**
     * Stages the value in the owned state and restarts the settle window. That
     * state is the only thing that moves while the finger is down: the bar
     * redraws from it on the next frame, and the HAL is not touched until the
     * position settles.
     */
    protected fun updateValue(newValue: Float) {
        val value = newValue.coerceIn(minValue, maxValue)
        if (!stageValue(value)) {
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

    /**
     * Restores [resetValue] through the same staged path a double tap uses, so
     * an external button inherits the settle: one repaint now, one write later.
     */
    fun reset() {
        updateValue(resetValue())
    }

    /** The per-frame UI step: repaint, with no binder work at all. */
    private fun flushFrame() {
        if (!staged) {
            return
        }
        staged = false
        val started = if (DiracTrace.ENABLED) System.nanoTime() else 0L
        invalidate()
        val invalidated = if (DiracTrace.ENABLED) System.nanoTime() else 0L
        onValueFlushed(currentValue())
        DiracTrace.log(TAG) {
            "flush invalidate ns=${invalidated - started} " +
                "notify ns=${System.nanoTime() - invalidated}"
        }
    }

    /**
     * The settle: flush the last frame so the row shows the settled value, then
     * write it once and hand the one push of the gesture to the HAL engine.
     * Called by the idle window, by touch-up or cancel, and on detach.
     */
    private fun settleNow() {
        removeCallbacks(settle)
        flushFrame()
        if (!dirty) {
            return
        }
        dirty = false
        persistAndPush()
    }

    private fun perpendicularCenter(): Float = if (isVertical) width / 2f else height / 2f

    /** Draws a rounded bar along the slider axis, centred across it. */
    private fun drawAlong(
        canvas: Canvas,
        paint: Paint,
        alongStart: Float,
        alongEnd: Float,
        acrossHalf: Float,
        radius: Float,
        center: Float,
    ) {
        if (isVertical) {
            canvas.drawRoundRect(
                center - acrossHalf, alongStart, center + acrossHalf, alongEnd,
                radius, radius, paint,
            )
        } else {
            canvas.drawRoundRect(
                alongStart, center - acrossHalf, alongEnd, center + acrossHalf,
                radius, radius, paint,
            )
        }
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val defaultWidth = UiMetrics.dp(context, if (isVertical) 48f else 200f).toInt()
        val defaultHeight = UiMetrics.dp(context, if (isVertical) 200f else 48f).toInt()
        setMeasuredDimension(
            resolveSizeAndState(defaultWidth, widthMeasureSpec, 0),
            resolveSizeAndState(defaultHeight, heightMeasureSpec, 0),
        )
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val center = perpendicularCenter()
        val trackHalf = trackThickness / 2f
        val valuePos = positionForValue(currentValue())

        trackPaint.color = trackColors.getColorForState(drawableState, trackColors.defaultColor)
        drawAlong(canvas, trackPaint, trackStart(), trackEnd(), trackHalf, trackHalf, center)

        val detentPos = positionForValue(detentValue())
        drawAlong(
            canvas, detentPaint, detentPos - detentHalfAlong, detentPos + detentHalfAlong,
            detentHalfAcross, 0f, center,
        )

        val (activeStart, activeEnd) = activeSpan(detentPos, valuePos)
        // Head brighter than trail, trail brighter than track: the thumb is the
        // full accent and the trail is the accent dimmed, like the response
        // curve's line and translucent fill.
        activePaint.color = accent and 0x59FFFFFF
        drawAlong(canvas, activePaint, activeStart, activeEnd, trackHalf, trackHalf, center)

        if (dragging) {
            haloPaint.color = haloColors.getColorForState(drawableState, haloColors.defaultColor)
            if (isVertical) {
                canvas.drawCircle(center, valuePos, haloRadius, haloPaint)
            } else {
                canvas.drawCircle(valuePos, center, haloRadius, haloPaint)
            }
        }

        thumbPaint.color = accent
        drawAlong(
            canvas, thumbPaint, valuePos - barLength / 2f, valuePos + barLength / 2f,
            barThickness / 2f, barThickness / 2f, center,
        )
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        gestureDetector.onTouchEvent(event)
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                // A drag that starts on the bar belongs to the bar, so the list
                // must not steal it to scroll. A touch off the bars is left
                // alone, so the page still scrolls normally.
                if (!isWithinBar(axisPosition(event))) {
                    return false
                }
                dragging = true
                parent?.requestDisallowInterceptTouchEvent(true)
                updateValue(valueForPosition(axisPosition(event)))
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                if (dragging) {
                    updateValue(valueForPosition(axisPosition(event)))
                }
                return true
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                if (dragging) {
                    dragging = false
                    parent?.requestDisallowInterceptTouchEvent(false)
                    // Run the last frame now, then write the settled value once.
                    removeCallbacks(flush)
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
     * A generous hit area along the track: the whole slider counts along the
     * axis, and the track is extended by the thumb and a margin.
     */
    private fun isWithinBar(position: Float): Boolean {
        val start = trackStart() - barLength / 2f - hitMargin
        val end = trackEnd() + barLength / 2f + hitMargin
        return position in start..end
    }

    override fun performClick(): Boolean {
        super.performClick()
        return true
    }

    override fun onInitializeAccessibilityNodeInfo(info: AccessibilityNodeInfo) {
        super.onInitializeAccessibilityNodeInfo(info)
        info.className = "android.widget.SeekBar"
        info.rangeInfo = AccessibilityNodeInfo.RangeInfo.obtain(
            rangeType, minValue, maxValue, currentValue(),
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
            persistAndPush()
        }
        super.onDetachedFromWindow()
    }

    private companion object {
        const val TAG = "DiracQemSlider"

        /** Idle after the last move that reads as a settle, not a frame gap. */
        const val SETTLE_MS = 120L
    }
}
