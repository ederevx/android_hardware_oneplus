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
 */
class VerticalSlider @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0,
) : View(context, attrs, defStyleAttr) {

    var onValueChanged: ((Float) -> Unit)? = null

    private var minValue = -6f
    private var maxValue = 6f
    private var value = 0f

    private val trackPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val activePaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val thumbPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val haloPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val detentPaint = Paint(Paint.ANTI_ALIAS_FLAG)

    private val trackColors: ColorStateList
    private val haloColors: ColorStateList

    private val accent = resolveAccent(context)

    private val density = resources.displayMetrics.density
    private val trackWidth: Float
    private val barWidth: Float
    private val barHeight: Float
    private val haloRadius: Float

    private var dragging = false

    private val gestureDetector = GestureDetector(
        context,
        object : GestureDetector.SimpleOnGestureListener() {
            override fun onDoubleTap(e: MotionEvent): Boolean {
                setValue(0f)
                return true
            }
        },
    )

    init {
        trackWidth = 4f * density
        barWidth = 4f * density
        barHeight = 28f * density
        haloRadius = 20f * density
        detentPaint.color = 0x66FFFFFF
        detentPaint.strokeWidth = density

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

    fun getValue(): Float = value

    fun setValue(newValue: Float) {
        val clamped = newValue.roundToInt().toFloat().coerceIn(minValue, maxValue)
        if (clamped != value) {
            value = clamped
            invalidate()
            onValueChanged?.invoke(value)
        }
    }

    private fun trackTop(): Float = paddingTop + barHeight / 2f

    private fun trackBottom(): Float = height - paddingBottom - barHeight / 2f

    private fun yForValue(v: Float): Float {
        val span = trackBottom() - trackTop()
        return trackTop() + span * (maxValue - v) / (maxValue - minValue)
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val width = resolveSize((48f * density).toInt(), widthMeasureSpec)
        val height = resolveSize((200f * density).toInt(), heightMeasureSpec)
        setMeasuredDimension(width, height)
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val cx = width / 2f
        val top = trackTop()
        val bottom = trackBottom()
        val half = trackWidth / 2f

        trackPaint.color = trackColors.getColorForState(drawableState, trackColors.defaultColor)
        canvas.drawRoundRect(cx - half, top, cx + half, bottom, half, half, trackPaint)

        val zeroY = yForValue(0f)
        canvas.drawRect(cx - 8f * density, zeroY - density, cx + 8f * density, zeroY + density, detentPaint)

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

    /**
     * Reads the theme accent as a colour. The attribute can resolve to a colour
     * state list, so it has to go through getColor instead of TypedValue.data.
     */
    private fun resolveAccent(context: Context): Int {
        val typed = context.obtainStyledAttributes(intArrayOf(android.R.attr.colorAccent))
        val color = typed.getColor(0, 0)
        typed.recycle()
        return if (color != 0) color else 0xFF8AB4F8.toInt()
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
        val top = trackTop() - barHeight / 2f - 8f * density
        val bottom = trackBottom() + barHeight / 2f + 8f * density
        return y >= top && y <= bottom
    }

    private fun updateFromY(y: Float) {
        val span = trackBottom() - trackTop()
        if (span <= 0f) {
            return
        }
        val ratio = ((y - trackTop()) / span).coerceIn(0f, 1f)
        setValue(maxValue - ratio * (maxValue - minValue))
    }

    override fun performClick(): Boolean {
        super.performClick()
        return true
    }

    override fun onInitializeAccessibilityNodeInfo(info: AccessibilityNodeInfo) {
        super.onInitializeAccessibilityNodeInfo(info)
        info.className = "android.widget.SeekBar"
        info.rangeInfo = AccessibilityNodeInfo.RangeInfo.obtain(
            AccessibilityNodeInfo.RangeInfo.RANGE_TYPE_FLOAT, minValue, maxValue, value,
        )
    }

    override fun onInitializeAccessibilityEvent(event: AccessibilityEvent) {
        super.onInitializeAccessibilityEvent(event)
        event.className = "android.widget.SeekBar"
    }
}
