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
import android.graphics.Picture
import android.util.AttributeSet
import android.view.View
import java.lang.Math

/**
 * Draws the equalizer response as the summed magnitude response of the seven
 * peaking bands on a log frequency axis, on a light grid; the curve therefore
 * shows the real rounded shape of the bands rather than straight segments
 * between them.
 *
 * Each band is reduced to a closed-form bump instead of a sampled table. The
 * decoded unit shape is the small-signal limit of the RBJ peaking response, a
 * rational Lorentzian in cos w - cos w0 that needs no transcendental once the
 * sample's sin/cos are cached, so a band's whole shape is two constants. The
 * response is evaluated at thirty-three knots and drawn with Path.cubicTo
 * Hermite segments, so Skia owns the smoothing and tessellation, and the grid
 * and fill are recorded once into a Picture and replayed.
 *
 * The bands are read from the engine's one half-dB array, the same one the
 * board and the sliders use, so the curve can never render a stale copy.
 */
class EqCurveView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0,
) : View(context, attrs, defStyleAttr) {

    private val accent = UiMetrics.accent(context)

    /** The graph inset from the card edge, resolved once. */
    private val inset = UiMetrics.dp(context, 16f).toInt()

    private val curvePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = UiMetrics.dp(context, 2.5f)
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
        strokeWidth = UiMetrics.dp(context, 1f)
        color = 0x1AFFFFFF
    }
    private val zeroPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = UiMetrics.dp(context, 1f)
        color = 0x59FFFFFF
    }

    private var left = 0f
    private var right = 0f
    private var top = 0f
    private var bottom = 0f
    private var spanX = 0f
    private var spanY = 0f

    /** The grid and fill, recorded once for this view's geometry and accent. */
    private var scene: Picture? = null
    private var sceneReady = false

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val width = resolveSizeAndState(suggestedMinimumWidth, widthMeasureSpec, 0)
        val height = resolveSizeAndState(
            UiMetrics.dp(context, MIN_HEIGHT_DP).toInt(), heightMeasureSpec, 0,
        )
        setMeasuredDimension(width, height)
    }

    override fun onDraw(canvas: Canvas) {
        val started = if (DiracTrace.ENABLED) System.nanoTime() else 0L
        super.onDraw(canvas)

        updateGeometry()
        val reason = rebuildReason()
        if (reason != null) {
            rebuild(reason)
        }
        if (!Cache.hasCurve) {
            // Never leave a bare baseline behind: it reads as a stray divider.
            return
        }

        // The geometry, bands and accent are unchanged, so a fresh view reuses
        // the cached paths and only has to record its own Picture once.
        val grid = if (sceneReady) scene else recordScene().also { sceneReady = true }
        if (grid != null) {
            canvas.drawPicture(grid)
        }
        canvas.drawPath(Cache.curve, curvePaint)

        if (DiracTrace.ENABLED) {
            val elapsed = System.nanoTime() - started
            when {
                reason != null -> DiracTrace.log(TAG) { "curve draw ns=$elapsed recompute" }
                elapsed >= DiracTrace.SLOW_FRAME_NS ->
                    DiracTrace.log(TAG) { "curve draw cached ns=$elapsed" }
            }
        }
    }

    /** Maps the view box the cached paths were built for, without rebuilding. */
    private fun updateGeometry() {
        // Keep the graph inset from the card even if the row padding was
        // replaced while binding, so the grid never sits on the rounded edge.
        left = paddingLeft.toFloat()
        right = (width - paddingRight).toFloat()
        top = maxOf(paddingTop, inset).toFloat()
        bottom = (height - maxOf(paddingBottom, inset)).toFloat()
        spanX = right - left
        spanY = bottom - top
    }

    private fun rebuildReason(): String? = when {
        Cache.pathGeneration != DiracState.bandsGeneration -> "bands"
        Cache.width != width || Cache.height != height -> "size"
        Cache.paddingLeft != paddingLeft ||
            Cache.paddingTop != paddingTop ||
            Cache.paddingRight != paddingRight ||
            Cache.paddingBottom != paddingBottom -> "padding"
        else -> null
    }

    /** Rebuilds the cached paths from the owned band array and the view geometry. */
    private fun rebuild(reason: String) {
        val generation = DiracState.bandsGeneration
        val bands = DiracState.currentBands(context)
        val count = minOf(bands.size, DiracPresets.BAND_FREQS.size)

        val started = System.nanoTime()
        responseFor(generation, bands)
        val ready = System.nanoTime()

        Cache.curve.rewind()
        Cache.area.rewind()
        Cache.hasCurve = count >= 2 && spanX > 0f && spanY > 0f
        sceneReady = false
        if (Cache.hasCurve) {
            val y = Cache.y
            val slope = Cache.slope
            val step = 1f / (KNOTS - 1)
            val segment = spanX * step
            val startX = left
            val startY = yFor(y[0])
            Cache.curve.moveTo(startX, startY)
            Cache.area.moveTo(startX, bottom)
            Cache.area.lineTo(startX, startY)
            for (knot in 0 until KNOTS - 1) {
                val x0 = left + spanX * step * knot
                val x1 = x0 + segment
                // Control points from the analytic slope in dB per log-Hz, so
                // the cubic is C1 across knots and tracks the band response.
                val rise = LOG_SPAN.toFloat() * step / 3f
                val c0 = yFor(y[knot] + slope[knot] * rise)
                val c1 = yFor(y[knot + 1] - slope[knot + 1] * rise)
                val endY = yFor(y[knot + 1])
                Cache.curve.cubicTo(x0 + segment / 3f, c0, x1 - segment / 3f, c1, x1, endY)
                Cache.area.cubicTo(x0 + segment / 3f, c0, x1 - segment / 3f, c1, x1, endY)
            }
            Cache.area.lineTo(right, bottom)
            Cache.area.close()
        }

        Cache.pathGeneration = generation
        Cache.width = width
        Cache.height = height
        Cache.paddingLeft = paddingLeft
        Cache.paddingTop = paddingTop
        Cache.paddingRight = paddingRight
        Cache.paddingBottom = paddingBottom
        val finished = System.nanoTime()
        DiracTrace.log(TAG) { "curve recompute n=$count reason=$reason" }
        DiracTrace.log(TAG) { "eq response ns=${ready - started} rebuild ns=${finished - ready}" }
    }

    private fun recordScene(): Picture {
        val picture = scene ?: Picture()
        val canvas = picture.beginRecording(width, height)
        drawGrid(canvas)
        canvas.drawPath(Cache.area, areaPaint)
        picture.endRecording()
        scene = picture
        return picture
    }

    /**
     * The total dB response and its slope in dB per log-Hz at each knot for a
     * band generation. The sum is kept across geometry changes, so only a real
     * band change pays for it, and it is derived from the half-dB integers the
     * model carries.
     */
    private fun responseFor(generation: Int, bands: IntArray) {
        if (Cache.responseGeneration == generation) {
            return
        }

        val count = minOf(bands.size, DiracPresets.BAND_FREQS.size)
        val gain = Cache.gain
        for (band in 0 until count) {
            gain[band] = bands[band] * DiracPresets.HALF_DB_TO_DB
        }
        val y = Cache.y
        val slope = Cache.slope
        for (knot in 0 until KNOTS) {
            val cos = KNOT_COS[knot]
            val sin = KNOT_SIN[knot]
            val sin2 = KNOT_SIN2[knot]
            val omega = KNOT_OMEGA[knot]
            var sum = 0f
            var deriv = 0f
            for (band in 0 until count) {
                val u = cos - BAND_COS0[band]
                val r2 = BAND_ALPHA2[band] * sin2
                val den = u * u + r2
                sum += gain[band] * (r2 / den)
                deriv += gain[band] * omega *
                    (2f * BAND_ALPHA2[band] * u * sin * (1f - cos * BAND_COS0[band]) / (den * den))
            }
            y[knot] = sum
            slope[knot] = deriv
        }
        Cache.responseGeneration = generation
    }

    private fun yFor(db: Float): Float =
        top + spanY * (6f - db.coerceIn(-6f, 6f)) / 12f

    private fun drawGrid(canvas: Canvas) {
        canvas.drawLine(left, top, right, top, gridPaint)
        canvas.drawLine(left, yFor(0f), right, yFor(0f), zeroPaint)
        canvas.drawLine(left, bottom, right, bottom, gridPaint)
        for (band in BAND_X.indices) {
            val x = left + spanX * BAND_X[band]
            canvas.drawLine(x, top, x, bottom, gridPaint)
        }
    }

    /**
     * The built response and paths, keyed by the band generation and the view
     * geometry they were built for. One cache serves every instance: a rebind
     * that produces the same geometry reuses what the previous one built.
     */
    private object Cache {
        var responseGeneration = Int.MIN_VALUE
        val gain = FloatArray(DiracPresets.EQ_BANDS)
        val y = FloatArray(EqCurveView.KNOTS)
        val slope = FloatArray(EqCurveView.KNOTS)
        var pathGeneration = Int.MIN_VALUE
        var width = -1
        var height = -1
        var paddingLeft = -1
        var paddingTop = -1
        var paddingRight = -1
        var paddingBottom = -1
        var hasCurve = false
        val curve = Path()
        val area = Path()
    }

    private companion object {
        const val TAG = "DiracQemCurve"
        const val RATE = 48_000.0
        const val KNOTS = 33
        const val MIN_HEIGHT_DP = 120f

        /** Log-frequency span of the axis, in nats, and its lower end. */
        val LOG_MIN = Math.log(DiracPresets.BAND_FREQS.first().toDouble())
        val LOG_SPAN = Math.log(DiracPresets.BAND_FREQS.last().toDouble()) - LOG_MIN

        /** Fraction of the log axis at which each band centre sits. */
        val BAND_X = FloatArray(DiracPresets.EQ_BANDS)

        /**
         * The one-time shape data: the sample's sin/cos and w at each knot,
         * plus each band's two bump constants. The band shape itself is the
         * closed-form Lorentzian evaluated from those, so no per-band sample
         * table is stored.
         */
        val KNOT_COS = FloatArray(KNOTS)
        val KNOT_SIN = FloatArray(KNOTS)
        val KNOT_SIN2 = FloatArray(KNOTS)
        val KNOT_OMEGA = FloatArray(KNOTS)
        val BAND_ALPHA2 = FloatArray(DiracPresets.EQ_BANDS)
        val BAND_COS0 = FloatArray(DiracPresets.EQ_BANDS)

        init {
            val started = System.nanoTime()
            for (band in DiracPresets.BAND_FREQS.indices) {
                val f0 = DiracPresets.BAND_FREQS[band].toDouble()
                BAND_X[band] = ((Math.log(f0) - LOG_MIN) / LOG_SPAN).toFloat()
                val w0 = 2.0 * Math.PI * f0 / RATE
                val halfAlpha = Math.sin(w0) / 2.0
                BAND_ALPHA2[band] = (halfAlpha * halfAlpha).toFloat()
                BAND_COS0[band] = Math.cos(w0).toFloat()
            }
            for (knot in 0 until KNOTS) {
                val logF = LOG_MIN + LOG_SPAN * knot / (KNOTS - 1)
                val w = 2.0 * Math.PI * Math.exp(logF) / RATE
                val sin = Math.sin(w)
                KNOT_COS[knot] = Math.cos(w).toFloat()
                KNOT_SIN[knot] = sin.toFloat()
                KNOT_SIN2[knot] = (sin * sin).toFloat()
                KNOT_OMEGA[knot] = w.toFloat()
            }
            DiracTrace.log(TAG) { "eq table build ns=${System.nanoTime() - started}" }
        }
    }
}
