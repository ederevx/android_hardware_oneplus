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
import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.exp
import kotlin.math.ln
import kotlin.math.log10
import kotlin.math.pow
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * Draws the equalizer response as the summed magnitude response of the seven
 * peaking bands, sampled on a log frequency axis, on a light grid; the curve
 * therefore shows the real rounded shape of the bands rather than straight
 * segments between them.
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
        val n = minOf(bands.size, DiracPresets.BAND_FREQS.size)
        Log.d(
            TAG,
            "curve bands n=${bands.size} first=${bands.firstOrNull()} last=${bands.lastOrNull()} " +
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

        val logMin = ln(DiracPresets.BAND_FREQS.first())
        val logSpan = ln(DiracPresets.BAND_FREQS.last()) - logMin
        fun yFor(db: Float) = top + spanY * (6f - db.coerceIn(-6f, 6f)) / 12f

        // Grid: the +6, 0 and -6 dB lines plus one line per band centre.
        canvas.drawLine(left, top, right, top, gridPaint)
        canvas.drawLine(left, yFor(0f), right, yFor(0f), zeroPaint)
        canvas.drawLine(left, bottom, right, bottom, gridPaint)
        for (i in 0 until n) {
            val x = left + spanX * (ln(DiracPresets.BAND_FREQS[i]) - logMin) / logSpan
            canvas.drawLine(x, top, x, bottom, gridPaint)
        }

        // Sample the summed response of all bands on a log frequency axis.
        val curve = Path()
        val area = Path()
        for (sample in 0..SAMPLES) {
            val t = sample / SAMPLES.toFloat()
            val freq = exp(logMin + t * logSpan)
            val x = left + spanX * t
            val y = yFor(totalResponseDb(freq))
            if (sample == 0) {
                curve.moveTo(x, y)
                area.moveTo(x, bottom)
                area.lineTo(x, y)
            } else {
                curve.lineTo(x, y)
                area.lineTo(x, y)
            }
        }
        area.lineTo(right, bottom)
        area.close()
        canvas.drawPath(area, areaPaint)
        canvas.drawPath(curve, curvePaint)
    }

    /** Sum of the seven band responses, in dB, at [freq]. */
    private fun totalResponseDb(freq: Float): Float {
        var total = 0f
        for (i in DiracPresets.BAND_FREQS.indices) {
            total += peakingDb(DiracPresets.BAND_FREQS[i], bands[i], freq)
        }
        return total
    }

    /**
     * Magnitude response of one peaking band at [freq], in dB, from the RBJ
     * audio EQ cookbook at a nominal 48 kHz rate: the shape is what the graph
     * shows, so the exact rate only nudges where the band tapers off.
     */
    private fun peakingDb(f0: Float, gainDb: Float, freq: Float): Float {
        if (gainDb == 0f) {
            return 0f
        }
        val a = 10f.pow(gainDb / 40f)
        val w0 = 2f * PI.toFloat() * f0 / RATE
        val alpha = sin(w0) / (2f * BAND_Q)
        val cosW0 = cos(w0)
        val b0 = 1f + alpha * a
        val b1 = -2f * cosW0
        val b2 = 1f - alpha * a
        val a0 = 1f + alpha / a
        val a1 = -2f * cosW0
        val a2 = 1f - alpha / a

        val w = 2f * PI.toFloat() * freq / RATE
        val cos1 = cos(w)
        val sin1 = sin(w)
        val cos2 = cos(2f * w)
        val sin2 = sin(2f * w)
        val numRe = b0 + b1 * cos1 + b2 * cos2
        val numIm = -(b1 * sin1 + b2 * sin2)
        val denRe = a0 + a1 * cos1 + a2 * cos2
        val denIm = -(a1 * sin1 + a2 * sin2)
        val num = sqrt(numRe * numRe + numIm * numIm)
        val den = sqrt(denRe * denRe + denIm * denIm)
        return 20f * log10(num / den)
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
        const val RATE = 48_000f
        const val SAMPLES = 192

        /**
         * The decoded band payload carries the seven gains and nothing else,
         * so the real bandwidth is not available. The bands are roughly 1.3
         * octaves apart, which is a peaking Q of about 1, so use that.
         */
        const val BAND_Q = 1.0f
    }
}
