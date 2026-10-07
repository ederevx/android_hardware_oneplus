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
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.util.AttributeSet
import android.util.TypedValue
import android.view.View

/**
 * Draws the current equalizer curve as a polyline over the -6..+6 dB range with
 * the 0 dB line marked, so the top of the Dirac page shows what the bands do.
 */
class EqCurveView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0,
) : View(context, attrs, defStyleAttr) {

    private var bands = FloatArray(DiracPresets.EQ_BANDS)
    private val density = resources.displayMetrics.density

    private val linePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2f * density
        strokeCap = Paint.Cap.ROUND
        strokeJoin = Paint.Join.ROUND
    }
    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
    }
    private val zeroPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = density
        color = 0x33FFFFFF
    }

    init {
        val typed = TypedValue()
        val accent = if (context.theme.resolveAttribute(
                android.R.attr.colorAccent, typed, true,
            )
        ) {
            typed.data
        } else {
            0xFFFFFFFF.toInt()
        }
        linePaint.color = accent
        fillPaint.color = accent and 0x33FFFFFF
    }

    fun setBands(newBands: FloatArray) {
        bands = newBands.copyOf(DiracPresets.EQ_BANDS)
        invalidate()
    }

    fun setBand(index: Int, value: Float) {
        if (index in bands.indices) {
            bands[index] = value
            invalidate()
        }
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val height = resolveSize((96f * density).toInt(), heightMeasureSpec)
        setMeasuredDimension(MeasureSpec.getSize(widthMeasureSpec), height)
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val n = bands.size
        if (n < 2) {
            return
        }
        val left = paddingLeft.toFloat()
        val right = (width - paddingRight).toFloat()
        val top = paddingTop.toFloat()
        val bottom = (height - paddingBottom).toFloat()
        val spanX = right - left
        val spanY = bottom - top

        val zeroY = top + spanY / 2f
        canvas.drawLine(left, zeroY, right, zeroY, zeroPaint)

        fun x(i: Int) = left + spanX * i / (n - 1).toFloat()
        fun y(v: Float) = top + spanY * (6f - v.coerceIn(-6f, 6f)) / 12f

        val path = Path()
        path.moveTo(x(0), y(bands[0]))
        for (i in 1 until n) {
            path.lineTo(x(i), y(bands[i]))
        }
        canvas.drawPath(path, linePaint)
    }
}
