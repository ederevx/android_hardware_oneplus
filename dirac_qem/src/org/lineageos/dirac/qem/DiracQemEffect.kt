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
import android.util.Log
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference

/**
 * Pushes the owned Dirac state to the audio HAL.
 *
 * The state itself lives in [DiracState]; this engine only turns it into the
 * frames the two Dirac topologies accept. The speaker is module 0x12D00 on
 * topology 0x10012D00 and the wired jack is module 0x12D01 on topology
 * 0x10012D01. Every frame names the live platform audio device and lets the
 * HAL resolve it; [DiracRouteResolver.outputDevice] is that device. The same
 * parameter sequence -- 0x12D01
 * enable, 0x12D35 EQ enable, 0x12D36 28-byte coefficients, 0x12D67 sound-field
 * enable, plus the headset-only 0x12D03 / 0x12D04 filter select -- is written to
 * whichever module the active route names.
 *
 * Frames are sent only while the Dirac DSP voices the live sink. On a sink it
 * does not voice no Dirac topology has a live stream, so the whole ADSP leg is
 * skipped there and the host effect carries the state instead.
 */
object DiracQemEffect {
    private const val TAG_BANDS = "DiracQemBands"

    private const val SCALAR_TONAL_BALANCE = 3
    private const val SCALAR_LOUDNESS = 4

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

    /**
     * Hands the settled band state to the push thread. The UI thread only stores
     * the payload in the single slot (the engine's arrays are write-once, so no
     * copy is needed) and makes sure the drain loop is running; the 16
     * synchronous setParameters run on the worker. A step costs 16 HAL writes
     * where the full enable set costs 80 on the speaker route and 112 on the
     * headset route.
     */
    fun pushBands(context: Context) {
        if (!DiracState.isEnabled(context) || !adspVoiced(context)) {
            return
        }
        val appContext = context.applicationContext ?: context
        pendingPush.set(Push(appContext, DiracState.output(context), DiracState.currentBands(context)))
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
            moduleFor(push.route), topoFor(push.route), deviceFor(push.context),
            QemProtocol.PARAM_EQ_BANDS, QemProtocol.eqBandsPayload(push.bands))
        // Always on, and on the worker: this is the HAL-side proof, not a
        // frame-level measurement, so it costs the UI thread nothing.
        Log.d(TAG_BANDS, "push route=${push.route} frames=$frames ns=${System.nanoTime() - started}")
    }

    /** Pushes the stored movie-mode scalar from the owned state. */
    fun setMovie(context: Context) {
        if (!adspVoiced(context)) {
            return
        }
        send(context, SCALAR_TONAL_BALANCE, QemProtocol.scalarPayload(
            SCALAR_TONAL_BALANCE, if (DiracState.isMovie(context)) MOVIE_TONAL_BALANCE else DEFAULT_TONAL_BALANCE))
    }

    /**
     * Publishes the stored Sum/Diff width for the live route. Where the host
     * stage owns the width this only republishes the effect state, which is
     * where the width is then applied; the DSP frame goes out only where the
     * host cannot widen, so one value is never applied twice.
     */
    fun setSumDiff(context: Context) {
        if (DiracState.isWideningActive(context)) {
            DiracState.publish(context)
            return
        }
        if (!adspVoiced(context)) {
            return
        }
        sendSumDiff(context, DiracState.output(context))
    }

    /**
     * DEV PROBE: sends the stored width as the raw DSP frame whatever owns the
     * width, so the instrument can still measure what the ADSP does with it.
     * Nothing in the shipping paths calls this.
     */
    fun probeSumDiff(context: Context) {
        sendSumDiff(context, DiracState.output(context))
    }

    /**
     * The Sum/Diff frame is the one param the ACDB LUT does not carry, so it is
     * sent raw: cal_caltype=0 makes the ADM reject it with ADSP_EBADPARAM from
     * ADM_CMD_SET_PP_PARAMS, while cal_caltype=1 bypasses the lookup and the
     * same frame is accepted. Every other frame stays on the ACDB path.
     */
    private fun sendSumDiff(context: Context, output: Int) {
        sendOp(context, output, QemProtocol.PARAM_SUMDIFF,
            QemProtocol.floatPayload(DiracState.sumDiff(context)), QemProtocol.CALTYPE_RAW)
    }

    private fun send(context: Context, key: Int, payload: ByteArray) {
        val output = DiracState.output(context)
        QemTransport(context).send(
            moduleFor(output), topoFor(output), deviceFor(context),
            QemProtocol.PARAM_SCALAR_BASE + key, payload)
    }

    /**
     * Pushes the stored state to the audio layer. The route is resolved once so
     * that every frame of a pass addresses the same module, and the disable
     * path never follows the route resolved here: it turns off the module the
     * enable path actually turned on, which is not necessarily the current one
     * (the headset can be unplugged between the two passes).
     *
     * The host effect's state is published first, on every pass, and is a
     * no-op when nothing changed.
     */
    fun apply(context: Context) {
        // The single load step: the stored values are read before anything is
        // composed or published.
        DiracState.load(context)
        // A pass is where the live sink is re-read and a page that is open is
        // told it changed; the frames below then reuse the route it resolves.
        DiracState.refreshRoute(context)
        val route = DiracState.output(context)
        // Exclusivity is mechanical, never inferred: before the state that hands
        // the width to the host is published, the DSP's own value is cleared, so
        // the DSP contributes nothing even if it is applying that parameter. The
        // order is load-bearing - clear, then hand over.
        if (adspVoiced(context) && DiracState.isWideningActive(context)) {
            clearSumDiff(context, route)
        }
        DiracState.publish(context)
        // On the host-owned sink no Dirac topology has a live stream, so both
        // the enable set and the disable frames could only fail there. The host
        // effect carries the state on that sink, and was published above.
        if (!adspVoiced(context)) {
            return
        }
        val applied = DiracState.appliedRoutes(context)
        if (!DiracState.isEnabled(context)) {
            // Clear every route the app enabled. An empty record means the app
            // state was reset while the audio layer kept a calibration, so both
            // modules are cleared rather than trusting the record.
            if (applied == 0) {
                sendDisable(context, DiracState.OUTPUT_INTERNAL)
                sendDisable(context, DiracState.OUTPUT_EXTERNAL)
            } else {
                for (candidate in 0..1) {
                    if (applied and routeBit(candidate) != 0) {
                        sendDisable(context, candidate)
                    }
                }
            }
            DiracState.setAppliedRoutes(context, 0)
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
        DiracState.setAppliedRoutes(context, routeBit(route))
    }

    private fun sendEnable(context: Context, output: Int) {
        val bands = DiracState.currentBands(context)
        sendOp(context, output, QemProtocol.PARAM_ENABLE, QemProtocol.intPayload(1))
        sendOp(context, output, QemProtocol.PARAM_EQ_ENABLE, QemProtocol.intPayload(1))
        sendOp(context, output, QemProtocol.PARAM_EQ_BANDS, QemProtocol.eqBandsPayload(bands))
        sendOp(context, output, QemProtocol.PARAM_SFX_ENABLE, QemProtocol.intPayload(1))
        // Exclusive ownership: the host stage widens wherever it runs, so the
        // DSP frame is sent only where the host cannot. On the speaker and the
        // wired jack the host owns the width, which is why the enable set
        // leaves that one parameter to it.
        if (!DiracState.isWideningActive(context)) {
            sendSumDiff(context, output)
        }
        // The loudness scalar is derived from the live sink, so it rides the
        // pass instead of a route broadcast of its own: the value the DSP holds
        // then always matches the sink, including the return to the default
        // loudness when Bluetooth goes away.
        sendOp(context, output, QemProtocol.PARAM_SCALAR_BASE + SCALAR_LOUDNESS,
            QemProtocol.scalarPayload(SCALAR_LOUDNESS,
                if (DiracState.isBluetoothConnected(context)) BT_LOUDNESS else DEFAULT_LOUDNESS))
        if (output == DiracState.OUTPUT_EXTERNAL) {
            sendOp(context, output, QemProtocol.PARAM_HDSOUND_ENABLE, QemProtocol.intPayload(1))
            sendOp(context, output, QemProtocol.PARAM_HDSOUND_FILTERIDX,
                QemProtocol.intPayload(DiracState.hdsoundIndex(context)))
        }
    }

    private fun sendOp(
        context: Context,
        output: Int,
        param: Int,
        payload: ByteArray,
        calType: Int = QemProtocol.CALTYPE_ACDB,
    ) {
        QemTransport(context).send(
            moduleFor(output), topoFor(output), deviceFor(context), param, payload,
            calType = calType)
    }

    private fun sendDisable(context: Context, output: Int) {
        QemTransport(context).send(
            moduleFor(output), topoFor(output), deviceFor(context),
            QemProtocol.PARAM_ENABLE, QemProtocol.intPayload(0))
        clearSumDiff(context, output)
    }

    /**
     * Clears the DSP's own width value. One implementation, used by the disable
     * path and by the handover to the host, which is the exclusivity rule: clear
     * the DSP first, then hand the width over.
     */
    private fun clearSumDiff(context: Context, output: Int) {
        sendOp(context, output, QemProtocol.PARAM_SUMDIFF,
            QemProtocol.floatPayload(QemProtocol.SUMDIFF_OFF), QemProtocol.CALTYPE_RAW)
    }

    /**
     * Whether the ADSP leg can run at all: the live sink has to be one the Dirac
     * DSP voices. On a sink it does not voice, every Dirac topology has no live
     * stream, so a frame can only fail; [apply] publishes the host effect's
     * state before this decides anything.
     */
    private fun adspVoiced(context: Context): Boolean = DiracState.sink(context).dspVoiced

    private fun routeBit(output: Int): Int = 1 shl output

    private fun moduleFor(output: Int): Int =
        if (output == DiracState.OUTPUT_EXTERNAL) QemProtocol.MODULE_EXTERNAL else QemProtocol.MODULE_INTERNAL

    private fun topoFor(output: Int): Int =
        if (output == DiracState.OUTPUT_EXTERNAL) QemProtocol.TOPO_EXTERNAL else QemProtocol.TOPO_INTERNAL

    /**
     * The wired route carries no cal_snddevid, so it needs one pass; the
     * speaker keeps its single-element device list.
     */
    /**
     * The one device every frame names: the live platform audio device, which
     * the HAL resolves into its own sound device and ACDB device. The route
     * selects module and topology only, so a frame cannot address a sink the
     * live stream is not on, and the app names no sound device of its own.
     */
    private fun deviceFor(context: Context): IntArray =
        intArrayOf(DiracRouteResolver.outputDevice(context))

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
            moduleFor(output), topo, deviceFor(context), QemProtocol.PARAM_ENABLE,
            QemProtocol.intPayload(1),
            appTypes = intArrayOf(appType), persistValues = intArrayOf(0),
            rates = intArrayOf(rate))
    }

    private const val MOVIE_TONAL_BALANCE = -1.0f
    private const val DEFAULT_TONAL_BALANCE = 0.0f
    private const val BT_LOUDNESS = -1.0f
    private const val DEFAULT_LOUDNESS = 0.0f
}
