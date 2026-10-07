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
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference

/**
 * Owns the Dirac QEM state and pushes a settled change to the audio HAL.
 * State lives in device-protected storage so the boot receiver can read it.
 *
 * Two output routes exist, mirroring the two Dirac topologies: the speaker
 * (module 0x12D00 on topology 0x10012D00, sound device 2) and the wired
 * headset (module 0x12D01 on topology 0x10012D01, sound device 9). The same
 * parameter sequence -- 0x12D01 enable, 0x12D35 EQ enable, 0x12D36 28-byte
 * coefficients, 0x12D67 sound-field enable, plus the headset-only 0x12D03 /
 * 0x12D04 filter select -- is written to whichever module the active route
 * names.
 */
object DiracQemEffect {
    private const val PREFS = "dirac_qem"
    private const val KEY_ENABLED = "enabled"
    private const val TAG_BANDS = "DiracQemBands"

    /** Sentinel for a route that has not been resolved since process start. */
    private const val NO_ROUTE = Int.MIN_VALUE

    /** Bitmask of the routes the app has left enabled: 1 internal, 2 external. */
    private const val KEY_APPLIED = "applied"
    private const val KEY_STYLE = "style"
    private const val KEY_CUSTOM = "custom"
    private const val KEY_MODEL = "model"
    private const val KEY_MOVIE = "movie"
    private const val KEY_A2DP_FALLBACK = "a2dp_fallback"

    const val OUTPUT_INTERNAL = 0
    const val OUTPUT_EXTERNAL = 1

    /**
     * Frames for the speaker name the sound device explicitly instead of the
     * audio device, because the HAL's own output routing does not have to
     * resolve AUDIO_DEVICE_OUT_SPEAKER to this exact sound device.
     */
    private const val SND_DEVICE_OUT_SPEAKER = 2

    /**
     * Wired headset sound device. The headset stream reports acdb_dev_id 10,
     * but the frame is addressed with cal_snddevid 9 -- the snd_device the HAL
     * resolves for the wired headset -- while cal_devid stays 0. This is the
     * selector the headset topology 0x10012D01 accepts; addressing it with the
     * speaker module 0x12D00 is rejected outright.
     */
    private const val SND_DEVICE_OUT_HEADSET = 9

    private const val SCALAR_TONAL_BALANCE = 3
    private const val SCALAR_LOUDNESS = 4

    private var prefsCache: SharedPreferences? = null

    private fun prefs(context: Context): SharedPreferences {
        prefsCache?.let { return it }
        return context.createDeviceProtectedStorageContext()
            .getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .also { prefsCache = it }
    }

    fun isEnabled(context: Context): Boolean = prefs(context).getBoolean(KEY_ENABLED, false)

    /**
     * Whether this build ships the A2DP software-fallback switch. This is a
     * build-time config resource, never a library probe: a product built
     * without the dirac_a2dp effect has no switch and no propagation at all,
     * so this app has no dependency of any kind on the effect module.
     */
    fun isA2dpFallbackAvailable(context: Context): Boolean =
        context.resources.getBoolean(R.bool.config_dirac_a2dp_fallback_available)

    fun isA2dpFallbackEnabled(context: Context): Boolean =
        prefs(context).getBoolean(KEY_A2DP_FALLBACK, false)

    fun setA2dpFallback(context: Context, enabled: Boolean) {
        prefs(context).edit().putBoolean(KEY_A2DP_FALLBACK, enabled).apply()
        apply(context)
    }

    /**
     * The cal_diracfb value to carry in a QEM frame: -1 when the build has no
     * fallback, so the key is omitted and nothing is sent for a consumer that
     * does not exist, otherwise the switch state.
     */
    fun a2dpFallbackFlag(context: Context): Int = when {
        !isA2dpFallbackAvailable(context) -> -1
        isA2dpFallbackEnabled(context) -> 1
        else -> 0
    }

    /**
     * The route this app last drove. A plug or unplug is announced by [apply],
     * which is what refreshes it, so the hot band path reads this value instead
     * of re-registering for the sticky jack broadcast on every frame.
     */
    @Volatile
    private var liveRoute = NO_ROUTE

    /** The live route, resolved from the jack's own connection signal. */
    fun output(context: Context): Int = resolveRoute(context, false)

    private fun resolveRoute(context: Context, force: Boolean): Int {
        if (force || liveRoute == NO_ROUTE) {
            liveRoute = DiracRouteResolver.resolve(context)
        }
        return liveRoute
    }

    /**
     * The one ordered thread every HAL push runs on, and the one slot that holds
     * the newest settled payload waiting for it. A gesture can never queue more
     * than that slot: a newer settle replaces an older one and the drain loop
     * only stops once the slot is empty, so the state the row shows is the state
     * the HAL ends on.
     */
    private val pushExecutor = Executors.newSingleThreadExecutor { runnable ->
        Thread(runnable, "dirac-qem-push").apply { isDaemon = true }
    }
    private val pendingPush = AtomicReference<Push?>(null)
    private val pushRunning = AtomicBoolean(false)

    /** One band state on its way to the HAL. */
    private class Push(val context: Context, val route: Int, val bands: IntArray)

    fun style(context: Context): Int = prefs(context).getInt(KEY_STYLE, DiracPresets.STYLE_NONE)

    fun model(context: Context): Int = prefs(context).getInt(KEY_MODEL, 0)

    fun isMovie(context: Context): Boolean = prefs(context).getBoolean(KEY_MOVIE, false)

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

    private fun storedCustomBands(context: Context): IntArray {
        val stored = prefs(context).getString(KEY_CUSTOM, null)
        val bands = DiracPresets.parseCustomBands(stored)
        val canonical = DiracPresets.encodeBands(bands)
        if (stored != null && stored != canonical) {
            prefs(context).edit().putString(KEY_CUSTOM, canonical).apply()
        }
        return bands
    }

    /** The seven band gains in effect, as the one shared array instance. */
    fun currentBands(context: Context): IntArray = bandValues ?: reloadBands(context)

    private fun reloadBands(context: Context): IntArray {
        val loaded = DiracPresets.valuesFor(style(context), storedCustomBands(context))
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

    fun setEnabled(context: Context, enabled: Boolean) {
        prefs(context).edit().putBoolean(KEY_ENABLED, enabled).apply()
        apply(context)
    }

    fun setStyle(context: Context, style: Int) {
        prefs(context).edit().putInt(KEY_STYLE, style).apply()
        reloadBands(context)
        apply(context)
    }

    /**
     * A slider drag is the only edit of the stored custom tune. The canonical
     * array is the sole judge of whether anything changed, so a drag that
     * lands on the value a band already holds persists and pushes nothing.
     * Dragging while Custom is active keeps the rest of the stored tune;
     * dragging while a preset is shown adopts the displayed curve as the
     * custom baseline, so the band that moves and the band that was there both
     * survive. Selecting a preset never comes through here. Only the in-memory
     * array moves here; [pushBands] and [persistBands] run once per settle, and
     * the return value says whether the canonical array actually changed.
     */
    fun setBand(context: Context, band: Int, value: Int): Boolean {
        val clamped = value.coerceIn(DiracPresets.MIN_HALF_DB, DiracPresets.MAX_HALF_DB)
        val bands = currentBands(context)
        if (bands.getOrElse(band) { 0 } == clamped) {
            // Always on: the audio verification greps this short-circuit.
            Log.d(TAG_BANDS, "band=$band value=$clamped unchanged")
            return false
        }
        // The displayed array is the baseline in every case: on Custom it is
        // the stored tune, and on a preset adopting it is exactly what makes a
        // drag an edit of that preset. The persist and the push are held back
        // to the settle, so nothing here touches disk or the HAL.
        val base = bands.copyOf()
        base[band] = clamped
        publishBands(base)
        Log.d(TAG_BANDS, "band=$band value=$clamped applied")
        return true
    }

    /**
     * Hands the settled band state to the push thread. The UI thread only stores
     * the payload in the single slot (the engine's arrays are write-once, so no
     * copy is needed) and makes sure the drain loop is running; the 16
     * synchronous setParameters run on the worker. A step costs 16 HAL writes
     * where the full enable set costs 80 on the speaker route and 112 on the
     * headset route.
     */
    fun pushBands(context: Context) {
        if (!isEnabled(context)) {
            return
        }
        val appContext = context.applicationContext ?: context
        pendingPush.set(Push(appContext, output(context), currentBands(context)))
        if (pushRunning.compareAndSet(false, true)) {
            pushExecutor.execute(::drainPushes)
        }
    }

    /**
     * The ordered drain loop. It takes whatever is in the slot, sends it, and
     * keeps going until the slot is empty, so a settle that lands while a push
     * is in flight is sent after it rather than stranded. It re-arms itself if a
     * payload arrives in the gap before [pushRunning] is cleared.
     */
    private fun drainPushes() {
        try {
            while (true) {
                val push = pendingPush.getAndSet(null) ?: break
                sendBands(push)
            }
        } finally {
            pushRunning.set(false)
            if (pendingPush.get() != null && pushRunning.compareAndSet(false, true)) {
                pushExecutor.execute(::drainPushes)
            }
        }
    }

    private fun sendBands(push: Push) {
        val started = System.nanoTime()
        val frames = QemTransport(push.context).send(
            moduleFor(push.route), topoFor(push.route), devicesFor(push.route),
            QemProtocol.PARAM_EQ_BANDS, QemProtocol.eqBandsPayload(push.bands),
            sndDevIdFor(push.route))
        // Always on, and on the worker: this is the HAL-side proof, not a
        // frame-level measurement, so it costs the UI thread nothing.
        Log.d(TAG_BANDS, "push route=${push.route} frames=$frames ns=${System.nanoTime() - started}")
    }

    /** Writes the settled custom tune and its style once, when the drag ends. */
    fun persistBands(context: Context) {
        val started = System.nanoTime()
        prefs(context).edit()
            .putString(KEY_CUSTOM, DiracPresets.encodeBands(currentBands(context)))
            .putInt(KEY_STYLE, DiracPresets.STYLE_CUSTOM)
            .apply()
        DiracTrace.log(TAG_BANDS) { "persist ns=${System.nanoTime() - started}" }
    }

    fun setModel(context: Context, model: Int) {
        prefs(context).edit().putInt(KEY_MODEL, model).apply()
        apply(context)
    }

    fun setMovie(context: Context, movie: Boolean) {
        prefs(context).edit().putBoolean(KEY_MOVIE, movie).apply()
        send(context, SCALAR_TONAL_BALANCE, QemProtocol.scalarPayload(
            SCALAR_TONAL_BALANCE, if (movie) MOVIE_TONAL_BALANCE else DEFAULT_TONAL_BALANCE))
    }

    fun setBluetooth(context: Context, connected: Boolean) {
        send(context, SCALAR_LOUDNESS, QemProtocol.scalarPayload(
            SCALAR_LOUDNESS, if (connected) BT_LOUDNESS else DEFAULT_LOUDNESS))
    }

    private fun send(context: Context, key: Int, payload: ByteArray) {
        val output = output(context)
        QemTransport(context).send(
            moduleFor(output), topoFor(output), devicesFor(output),
            QemProtocol.PARAM_SCALAR_BASE + key, payload, sndDevIdFor(output))
    }

    /**
     * Pushes the stored state to the audio layer. The route is resolved once so
     * that every frame of a pass addresses the same module, and the disable
     * path never follows the route resolved here: it turns off the module the
     * enable path actually turned on, which is not necessarily the current one
     * (the headset can be unplugged between the two passes).
     */
    fun apply(context: Context) {
        // A pass is where the route is re-read; the band path then reuses it.
        val route = resolveRoute(context, true)
        val applied = appliedRoutes(context)
        if (!isEnabled(context)) {
            // Clear every route the app enabled. An empty record means the app
            // state was reset while the audio layer kept a calibration, so both
            // modules are cleared rather than trusting the record.
            if (applied == 0) {
                sendDisable(context, OUTPUT_INTERNAL)
                sendDisable(context, OUTPUT_EXTERNAL)
            } else {
                for (candidate in 0..1) {
                    if (applied and routeBit(candidate) != 0) {
                        sendDisable(context, candidate)
                    }
                }
            }
            setAppliedRoutes(context, 0)
            return
        }

        // Only the route being driven may stay enabled. A route this app never
        // enabled has nothing to clear, so only a route that is on record (or
        // an empty record from a reset, which clears both) is disabled here.
        for (candidate in 0..1) {
            if (candidate != route && (applied == 0 || applied and routeBit(candidate) != 0)) {
                sendDisable(context, candidate)
            }
        }
        sendEnable(context, route)
        setAppliedRoutes(context, routeBit(route))
    }

    private fun sendEnable(context: Context, output: Int) {
        val bands = currentBands(context)
        sendOp(context, output, QemProtocol.PARAM_ENABLE, QemProtocol.intPayload(1))
        sendOp(context, output, QemProtocol.PARAM_EQ_ENABLE, QemProtocol.intPayload(1))
        sendOp(context, output, QemProtocol.PARAM_EQ_BANDS, QemProtocol.eqBandsPayload(bands))
        sendOp(context, output, QemProtocol.PARAM_SFX_ENABLE, QemProtocol.intPayload(1))
        if (output == OUTPUT_EXTERNAL) {
            sendOp(context, output, QemProtocol.PARAM_HDSOUND_ENABLE, QemProtocol.intPayload(1))
            val index = DiracPresets.MODEL_FILTER_INDEX[model(context)]
            sendOp(context, output, QemProtocol.PARAM_HDSOUND_FILTERIDX, QemProtocol.intPayload(index))
        }
    }

    private fun sendOp(context: Context, output: Int, param: Int, payload: ByteArray) {
        QemTransport(context).send(
            moduleFor(output), topoFor(output), devicesFor(output), param, payload,
            sndDevIdFor(output))
    }

    private fun sendDisable(context: Context, output: Int) {
        QemTransport(context).send(
            moduleFor(output), topoFor(output), devicesFor(output),
            QemProtocol.PARAM_ENABLE, QemProtocol.intPayload(0), sndDevIdFor(output))
    }

    private fun appliedRoutes(context: Context): Int = prefs(context).getInt(KEY_APPLIED, 0)

    private fun setAppliedRoutes(context: Context, routes: Int) {
        prefs(context).edit().putInt(KEY_APPLIED, routes).apply()
    }

    private fun routeBit(output: Int): Int = 1 shl output

    private fun moduleFor(output: Int): Int =
        if (output == OUTPUT_EXTERNAL) QemProtocol.MODULE_EXTERNAL else QemProtocol.MODULE_INTERNAL

    private fun topoFor(output: Int): Int =
        if (output == OUTPUT_EXTERNAL) QemProtocol.TOPO_EXTERNAL else QemProtocol.TOPO_INTERNAL

    /**
     * The headset frame carries an explicit cal_snddevid, so one pass is
     * enough; the speaker keeps its single-element device list.
     */
    private fun devicesFor(output: Int): IntArray =
        if (output == OUTPUT_EXTERNAL) intArrayOf(0) else QemProtocol.DEVICES_INTERNAL

    private fun sndDevIdFor(output: Int): Int =
        if (output == OUTPUT_EXTERNAL) SND_DEVICE_OUT_HEADSET else SND_DEVICE_OUT_SPEAKER

    /**
     * DEV PROBE: send a single Dirac calibration frame with an explicit
     * topology (and app type) instead of the constants this app normally uses,
     * so the topology the live audio stream registered can be identified from
     * the audio HAL's own log. Touches no stored state: it neither writes a
     * preference nor uses cal_persist=1, so a wrong candidate cannot leave a
     * bad persistent calibration behind.
     */
    fun probeCal(context: Context, output: Int, topo: Int, appType: Int, rate: Int) {
        QemTransport(context).send(
            moduleFor(output), topo, devicesFor(output), QemProtocol.PARAM_ENABLE,
            QemProtocol.intPayload(1), sndDevIdFor(output),
            appTypes = intArrayOf(appType), persistValues = intArrayOf(0),
            rates = intArrayOf(rate))
    }

    private const val MOVIE_TONAL_BALANCE = -1.0f
    private const val DEFAULT_TONAL_BALANCE = 0.0f
    private const val BT_LOUDNESS = -1.0f
    private const val DEFAULT_LOUDNESS = 0.0f
}
