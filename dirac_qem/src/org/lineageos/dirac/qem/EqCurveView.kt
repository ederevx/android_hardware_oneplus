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
import android.util.Log
import android.view.View

/**
 * Draws the current equalizer curve as a filled area over the -6..+6 dB range
 * on a light grid, so the top of the Dirac page reads as a curve even when
 * every band sits at 0 dB.
 *
 * The bands are read from the engine while drawing, from the same accessor the
 * board reads, so the curve can never render a stale array or one that was
 * never attached to this view instance.
 */
class EqCurveView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0,
) : View(context, attrs, defStyleAttr) {

    /** Test seam; when null the engine is read directly. */
    var bandProvider: (() -> FloatArray)? = null

    private var bands = FloatArray(DiracPresets.EQ_BANDS)
    private val density = resources.displayMetrics.density
    private val accent = resolveAccent(context)

    private val curvePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2.5f * density
        strokeCap = Paint.Cap.ROUND
        strokeJoin = Paint.Join.ROUND
        color = accent
    }
    private val areaPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        color = accent and 0x33FFFFFF
    }
    private val gridPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = density
        color = 0x1AFFFFFF
    }
    private val zeroPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = density
        color = 0x59FFFFFF
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val height = resolveSize((120f * density).toInt(), heightMeasureSpec)
        setMeasuredDimension(MeasureSpec.getSize(widthMeasureSpec), height)
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)

        bands = (bandProvider?.invoke() ?: DiracQemEffect.currentBands(context))
            .copyOf(DiracPresets.EQ_BANDS)
        val n = bands.size
        Log.d(
            TAG,
            "curve bands n=$n first=${bands.firstOrNull()} last=${bands.lastOrNull()} " +
                "provider=${bandProvider != null}",
        )
        if (n < 2) {
            // Never leave a bare baseline behind: it reads as a stray divider.
            return
        }

        // Keep the graph inset from the card even if the row padding was
        // replaced while binding, so the grid never sits on the rounded edge.
        val inset = (16f * density).toInt()
        val left = paddingLeft.toFloat()
        val right = (width - paddingRight).toFloat()
        val top = maxOf(paddingTop, inset).toFloat()
        val bottom = (height - maxOf(paddingBottom, inset)).toFloat()
        val spanX = right - left
        val spanY = bottom - top
        if (spanX <= 0f || spanY <= 0f) {
            return
        }

        fun x(i: Int) = left + spanX * i / (n - 1).toFloat()
        fun y(v: Float) = top + spanY * (6f - v.coerceIn(-6f, 6f)) / 12f

        // Grid: the +6, 0 and -6 dB lines plus one line per band, so a flat
        // 0 dB curve is still legible.
        canvas.drawLine(left, top, right, top, gridPaint)
        canvas.drawLine(left, y(0f), right, y(0f), zeroPaint)
        canvas.drawLine(left, bottom, right, bottom, gridPaint)
        for (i in 0 until n) {
            canvas.drawLine(x(i), top, x(i), bottom, gridPaint)
        }

        // Filled area under the curve.
        val area = Path()
        area.moveTo(x(0), bottom)
        area.lineTo(x(0), y(bands[0]))
        for (i in 1 until n) {
            area.lineTo(x(i), y(bands[i]))
        }
        area.lineTo(x(n - 1), bottom)
        area.close()
        canvas.drawPath(area, areaPaint)

        // The curve itself.
        val curve = Path()
        curve.moveTo(x(0), y(bands[0]))
        for (i in 1 until n) {
            curve.lineTo(x(i), y(bands[i]))
        }
        canvas.drawPath(curve, curvePaint)
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

    private companion object {
        const val TAG = "DiracQemCurve"
    }
}
