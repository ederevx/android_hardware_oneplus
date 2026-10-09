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
import android.content.SharedPreferences
import android.util.Log
import java.util.concurrent.CopyOnWriteArraySet
import kotlin.math.roundToInt

/**
 * The one owner of the Dirac app state.
 *
 * Every published variable -- the enable flag, the A2DP fallback opt-in, the
 * Sum/Diff width, the seven band gains, the stream attenuation, the resolved
 * route and the HDSOUND filter index -- lives here and is read and written only
 * through this class.
 * It keeps the authoritative snapshot and the last payload handed to the
 * daemon, so [publish] is a no-op while the state is unchanged and opening the
 * settings app cannot rewrite an identical config. [assertState] reads the
 * daemon's copy back and reports a mismatch instead of letting the two diverge.
 *
 * State is stored in device-protected storage so the boot receiver can read it
 * before unlock. [load] is the single load step; [publish] refuses to run
 * before it, so a default-valued snapshot can never reach the daemon.
 */
object DiracState {
    /**
     * The two routes the ADSP frame family addresses: the internal module the
     * speaker route uses and the external one the wired jack uses. They are not
     * the live output -- [sink] is -- because every sink that is not the wired
     * jack, A2DP included, addresses the internal module.
     */
    const val OUTPUT_INTERNAL = 0
    const val OUTPUT_EXTERNAL = 1

    /** No usable stream volume; the effect leaves the loudness tilt at identity. */
    const val UNKNOWN_VOLUME_DB = -1.0

    private const val TAG = "DiracState"
    private const val TAG_BANDS = "DiracQemBands"

    private const val PREFS = "dirac_qem"
    private const val KEY_ENABLED = "enabled"
    private const val KEY_APPLIED = "applied"
    private const val KEY_STYLE = "style"
    private const val KEY_CUSTOM = "custom"
    private const val KEY_MODEL = "model"
    private const val KEY_MOVIE = "movie"
    private const val KEY_A2DP_FALLBACK = "a2dp_fallback"
    private const val KEY_SUMDIFF = "sumdiff"

    /**
     * The width a fresh install starts at and the reset target: the middle of
     * the range, which the slider also draws its detent under. 0.0 stays the
     * neutral value the effect bypasses and the disable path restores.
     */
    const val SUMDIFF_DEFAULT = 0.5f

    /** The loudness law is flat past this; it only bounds a mute. */
    private const val MAX_VOLUME_DB = 120.0

    private var prefsCache: SharedPreferences? = null

    private var ready = false
    private var enabled = false
    private var a2dpFallback = false
    private var sumdiff = 0.0f
    private var style = DiracPresets.STYLE_NONE
    private var model = 0
    private var movie = false
    private var appliedRoutes = 0
    private var attenuationDb = UNKNOWN_VOLUME_DB

    /**
     * The sink the observers were last told about. The sink itself is never
     * remembered: [sink] reads it live, and this only decides when a change is
     * worth a notification.
     */
    private var notifiedSink: DiracRouteResolver.Sink? = null

    /** In-process observers of [sink]; notified only when it changes. */
    private val routeListeners = CopyOnWriteArraySet<() -> Unit>()

    /**
     * The one band-state array. It is loaded once from storage, or rebuilt when
     * the preset changes, and then shared by the board, the sliders and the
     * curve, so nothing keeps a second copy that a rebind could leave stale.
     * Values are half-dB integers.
     */
    private var bandValues: IntArray? = null

    /** Bumped only when [bandValues] actually changes, for cached views. */
    var bandsGeneration: Int = 0
        private set

    /** The last payload handed to the daemon, or null when none was sent. */
    private var lastPublished: Snapshot? = null

    /** One complete published payload; the unit [publish] dedupes on. */
    class Snapshot(
        val enabled: Boolean,
        val a2dpFallback: Boolean,
        val sumdiff: Float,
        val bandsHalfDb: IntArray,
        val volumeDb: Double,
    ) {
        fun sameAs(other: Snapshot?): Boolean = other != null &&
            enabled == other.enabled &&
            a2dpFallback == other.a2dpFallback &&
            sumdiff == other.sumdiff &&
            volumeDb == other.volumeDb &&
            bandsHalfDb.contentEquals(other.bandsHalfDb)

        override fun toString(): String =
            "enabled=$enabled fallback=$a2dpFallback sumdiff=$sumdiff volume_db=$volumeDb " +
                "bands=${bandsHalfDb.joinToString(";")}"
    }

    private fun prefs(context: Context): SharedPreferences {
        prefsCache?.let { return it }
        return context.createDeviceProtectedStorageContext()
            .getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .also { prefsCache = it }
    }

    /**
     * The single load step: reads storage once and marks the state usable. It is
     * idempotent, and [publish] refuses to run before it, so a default-valued
     * snapshot can never be published ahead of the stored values.
     */
    fun load(context: Context) {
        if (ready) {
            return
        }
        val p = prefs(context)
        enabled = p.getBoolean(KEY_ENABLED, false)
        a2dpFallback = p.getBoolean(KEY_A2DP_FALLBACK, false)
        sumdiff = p.getFloat(KEY_SUMDIFF, SUMDIFF_DEFAULT)
        style = p.getInt(KEY_STYLE, DiracPresets.STYLE_NONE)
        model = p.getInt(KEY_MODEL, 0)
        movie = p.getBoolean(KEY_MOVIE, false)
        appliedRoutes = p.getInt(KEY_APPLIED, 0)
        bandValues = DiracPresets.valuesFor(style, storedCustomBands(p))
        ready = true
    }

    private fun ensureLoaded(context: Context) {
        if (!ready) {
            load(context)
        }
    }

    /**
     * Whether this build ships the A2DP software-fallback switch. This is a
     * build-time config resource, never a library probe: a product built
     * without the dirac_biquad effect has no switch and no propagation at all.
     */
    fun isA2dpFallbackAvailable(context: Context): Boolean =
        context.resources.getBoolean(R.bool.config_dirac_biquad_fallback_available)

    fun isEnabled(context: Context): Boolean {
        ensureLoaded(context)
        return enabled
    }

    fun setEnabled(context: Context, value: Boolean) {
        ensureLoaded(context)
        enabled = value
        prefs(context).edit().putBoolean(KEY_ENABLED, value).apply()
    }

    fun isA2dpFallbackEnabled(context: Context): Boolean {
        ensureLoaded(context)
        return a2dpFallback
    }

    fun setA2dpFallback(context: Context, value: Boolean) {
        ensureLoaded(context)
        a2dpFallback = value
        prefs(context).edit().putBoolean(KEY_A2DP_FALLBACK, value).apply()
    }

    fun sumDiff(context: Context): Float {
        ensureLoaded(context)
        return sumdiff
    }

    /**
     * Stages the width in the owned state without persisting or publishing it,
     * so a drag repaints from it and the HAL is touched only at the settle. The
     * value is quantized to the thousandth the conf stores, so [assertState]
     * reads back exactly the float that was published.
     */
    fun setSumDiff(context: Context, value: Float): Boolean {
        ensureLoaded(context)
        val quantized =
            (value.coerceIn(0.0f, 1.0f) * SUMDIFF_STEPS).roundToInt() / SUMDIFF_STEPS
        if (quantized == sumdiff) {
            return false
        }
        sumdiff = quantized
        return true
    }

    /** Writes the settled width once and publishes it. */
    fun persistSumDiff(context: Context) {
        ensureLoaded(context)
        prefs(context).edit().putFloat(KEY_SUMDIFF, sumdiff).apply()
        publish(context)
    }

    fun style(context: Context): Int {
        ensureLoaded(context)
        return style
    }

    fun setStyle(context: Context, value: Int) {
        ensureLoaded(context)
        style = value
        prefs(context).edit().putInt(KEY_STYLE, value).apply()
        reloadBands(context)
    }

    fun model(context: Context): Int {
        ensureLoaded(context)
        return model
    }

    fun setModel(context: Context, value: Int) {
        ensureLoaded(context)
        model = value
        prefs(context).edit().putInt(KEY_MODEL, value).apply()
    }

    /** The external-device filter slot the current model selects. */
    fun hdsoundIndex(context: Context): Int =
        DiracPresets.MODEL_FILTER_INDEX.getOrElse(model(context)) { 0 }

    fun isMovie(context: Context): Boolean {
        ensureLoaded(context)
        return movie
    }

    fun setMovie(context: Context, value: Boolean) {
        ensureLoaded(context)
        movie = value
        prefs(context).edit().putBoolean(KEY_MOVIE, value).apply()
    }

    /**
     * The live output the user hears, resolved from the platform on every read,
     * so a page opened while a route is already live sees it and a connection
     * this process was not running for is never missed.
     */
    fun sink(context: Context): DiracRouteResolver.Sink = DiracRouteResolver.sink(context)

    /**
     * Whether the DSP owns the widening on the live sink. The host stage the
     * width row drives runs only on the sinks the DSP does not voice, so on a
     * sink the DSP voices the row has nothing to act on and stays inert.
     */
    fun isWideningDspHandled(context: Context): Boolean = sink(context).dspVoiced

    /** Whether the live sink is the Bluetooth one, which has its own loudness. */
    fun isBluetoothConnected(context: Context): Boolean =
        sink(context) == DiracRouteResolver.Sink.BLUETOOTH

    /** Whether the wired jack is the live sink, the one the headset model drives. */
    fun isWiredSink(context: Context): Boolean =
        sink(context) == DiracRouteResolver.Sink.WIRED

    fun appliedRoutes(context: Context): Int {
        ensureLoaded(context)
        return appliedRoutes
    }

    fun setAppliedRoutes(context: Context, routes: Int) {
        ensureLoaded(context)
        appliedRoutes = routes
        prefs(context).edit().putInt(KEY_APPLIED, routes).apply()
    }

    /** The stream attenuation the effect should apply, in dB below reference. */
    fun volumeDb(context: Context): Double {
        ensureLoaded(context)
        return attenuationDb
    }

    /**
     * Updates the stream attenuation and publishes it. A missing, non-finite or
     * non-positive volume stores the unknown sentinel, never 0. Returns whether
     * the state reached the daemon.
     */
    fun setVolumeDb(context: Context, value: Double): Boolean {
        ensureLoaded(context)
        val sanitized = sanitizeVolumeDb(value)
        if (sanitized == attenuationDb) {
            return false
        }
        attenuationDb = sanitized
        return publish(context)
    }

    /** The ADSP route the live sink addresses. */
    fun output(context: Context): Int =
        if (isWiredSink(context)) OUTPUT_EXTERNAL else OUTPUT_INTERNAL

    /**
     * Registers an in-process observer that is called whenever the resolved
     * output route changes, a jack plug and an A2DP connection alike. It exists
     * so a live settings page can follow the output without owning a second
     * HEADSET_PLUG or Bluetooth registration.
     */
    fun addRouteListener(listener: () -> Unit) {
        routeListeners.add(listener)
    }

    fun removeRouteListener(listener: () -> Unit) {
        routeListeners.remove(listener)
    }

    /**
     * Re-reads the live sink and tells the observers when it changed, so a page
     * that is already open follows a jack plug or an A2DP connection. It is
     * called on every pass, so nothing has to remember what the route was.
     */
    fun refreshRoute(context: Context) {
        val live = sink(context)
        if (live == notifiedSink) {
            return
        }
        notifiedSink = live
        // A listener that reads the sink sees the new value already, so this
        // cannot recurse.
        routeListeners.forEach { it() }
    }

    /** The seven band gains in effect, as the one shared array instance. */
    fun currentBands(context: Context): IntArray {
        ensureLoaded(context)
        return bandValues ?: reloadBands(context)
    }

    private fun reloadBands(context: Context): IntArray {
        val loaded = DiracPresets.valuesFor(style, storedCustomBands(prefs(context)))
        publishBands(loaded)
        return loaded
    }

    private fun publishBands(bands: IntArray) {
        val previous = bandValues
        bandValues = bands
        if (previous == null || !previous.contentEquals(bands)) {
            bandsGeneration++
        }
    }

    private fun storedCustomBands(p: SharedPreferences): IntArray {
        val stored = p.getString(KEY_CUSTOM, null)
        val bands = DiracPresets.parseCustomBands(stored)
        val canonical = DiracPresets.encodeBands(bands)
        if (stored != null && stored != canonical) {
            p.edit().putString(KEY_CUSTOM, canonical).apply()
        }
        return bands
    }

    /**
     * A slider drag is the only edit of the stored custom tune. The canonical
     * array is the sole judge of whether anything changed, so a drag that lands
     * on the value a band already holds pushes nothing. The persist and the
     * publish are held back to the settle; only the in-memory array moves here.
     */
    fun setBand(context: Context, band: Int, value: Int): Boolean {
        ensureLoaded(context)
        val clamped = value.coerceIn(DiracPresets.MIN_HALF_DB, DiracPresets.MAX_HALF_DB)
        val bands = currentBands(context)
        if (bands.getOrElse(band) { 0 } == clamped) {
            // Always on: the audio verification greps this short-circuit.
            Log.d(TAG_BANDS, "band=$band value=$clamped unchanged")
            return false
        }
        val base = bands.copyOf()
        base[band] = clamped
        publishBands(base)
        Log.d(TAG_BANDS, "band=$band value=$clamped applied")
        return true
    }

    /** Writes the settled custom tune and its style once, when the drag ends. */
    fun persistBands(context: Context) {
        ensureLoaded(context)
        val started = System.nanoTime()
        prefs(context).edit()
            .putString(KEY_CUSTOM, DiracPresets.encodeBands(currentBands(context)))
            .putInt(KEY_STYLE, DiracPresets.STYLE_CUSTOM)
            .apply()
        style = DiracPresets.STYLE_CUSTOM
        publish(context)
        DiracTrace.log(TAG_BANDS) { "persist ns=${System.nanoTime() - started}" }
    }

    /**
     * Hands the current state to the daemon the effect reads. A no-op while the
     * payload matches [lastPublished], so an unchanged state never rewrites the
     * config file and never makes the effect reload it.
     */
    fun publish(context: Context): Boolean {
        if (!isA2dpFallbackAvailable(context)) {
            return false
        }
        if (!ready) {
            Log.w(TAG, "publish before load ignored")
            return false
        }
        val next = compose(context)
        if (next.sameAs(lastPublished)) {
            return false
        }
        if (!DiracBiquadState.send(next.enabled, next.a2dpFallback, next.sumdiff, next.bandsHalfDb, next.volumeDb)) {
            return false
        }
        lastPublished = next
        assertState(context)
        return true
    }

    /**
     * Verifies the last published payload against the daemon's copy and reports
     * a mismatch instead of letting the two diverge silently. When no state has
     * been published, or the daemon cannot answer, it is inconclusive and
     * returns true.
     */
    fun assertState(context: Context): Boolean {
        if (!ready) {
            return true
        }
        val published = lastPublished ?: return true
        val echoed = DiracBiquadState.read()
        if (echoed == null) {
            Log.w(TAG, "daemon state unavailable; cannot verify [$published]")
            return true
        }
        val reading =
            Snapshot(echoed.enabled, echoed.fallback, echoed.sumDiff, echoed.bandsHalfDb, echoed.volumeDb)
        if (reading.sameAs(published)) {
            return true
        }
        Log.e(TAG, "published state diverged: published=[$published] daemon=[$reading]")
        return false
    }

    private fun compose(context: Context): Snapshot = Snapshot(
        enabled,
        a2dpFallback,
        sumdiff,
        currentBands(context).copyOf(),
        attenuationDb,
    )

    private fun sanitizeVolumeDb(value: Double): Double =
        if (value.isFinite() && value > 0.0) value.coerceAtMost(MAX_VOLUME_DB) else UNKNOWN_VOLUME_DB

    /** The thousand-step grid the conf's %.3f round-trips the width on. */
    private const val SUMDIFF_STEPS = 1000f
}
