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
    private const val TAG_PROBE = "DiracQemProbe"

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
            QemParams.EQ_BANDS.id, QemProtocol.eqBandsPayload(push.bands))
        // Always on, and on the worker: this is the HAL-side proof, not a
        // frame-level measurement, so it costs the UI thread nothing.
        Log.d(TAG_BANDS, "push route=${push.route} frames=$frames ns=${System.nanoTime() - started}")
    }

    /** Pushes the stored movie-mode scalar from the owned state. */
    fun setMovie(context: Context) {
        if (!adspVoiced(context)) {
            return
        }
        sendSpec(context, DiracState.output(context), QemParams.TONAL_BALANCE)
    }

    /** Sends one parameter to the module the live route names, as a full pass does. */
    private fun sendSpec(context: Context, output: Int, spec: QemParams.Spec) {
        sendFrame(context, moduleFor(output), topoFor(output), deviceFor(context), spec,
            QemProtocol.APP_TYPES, QemProtocol.PERSIST, QemProtocol.SAMPLE_RATES)
    }

    /**
     * The one place a pass or probe frame is composed: a parameter, the module
     * and topology it addresses, the platform devices, and the selector fan-out.
     * The band push composes from its gesture snapshot directly, and both paths
     * end in QemProtocol.frame, so the pass and the dev probe differ only in the
     * arguments they pass here, never in how a frame is composed.
     */
    private fun sendFrame(
        context: Context,
        module: Int,
        topo: Int,
        devices: IntArray,
        spec: QemParams.Spec,
        appTypes: IntArray,
        persistValues: IntArray,
        rates: IntArray,
    ) {
        QemTransport(context).send(
            module, topo, devices, spec.id, spec.payloadFor(context),
            appTypes = appTypes, persistValues = persistValues, rates = rates,
            calType = spec.calType)
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

    /**
     * The production pass, in the order the module expects: the set [QemParams]
     * says a route is sent, each frame composed by [sendSpec]. The loudness
     * scalar rides the pass instead of a route broadcast of its own, so the
     * value the DSP holds always matches the sink, including the return to the
     * default loudness when Bluetooth goes away.
     */
    private fun sendEnable(context: Context, output: Int) {
        for (spec in QemParams.productionSet(output)) {
            sendSpec(context, output, spec)
        }
    }

    private fun sendDisable(context: Context, output: Int) {
        sendSpec(context, output, QemParams.ENABLE_OFF)
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
     * DEV PROBE: what one burst sends. The parameter's own payload and cal type
     * come from [QemParams], the module from [output], and the platform device
     * from the live route exactly as a pass takes them; the topology, app type,
     * rate and parameter set are the caller's, which is the point of a probe and
     * the one way its addressing can differ from a pass.
     */
    class Probe(
        val output: Int,
        val topo: Int,
        val appType: Int,
        val rate: Int,
        val params: List<QemParams.Spec>,
        val repeats: Int = 1,
    )

    /**
     * DEV PROBE: one burst of [override]'s parameters, each composed by
     * [sendFrame]. It holds no state and does not run the pass, so a concurrent
     * production [apply] is untouched; the stored values are read through the
     * same payload rules production uses, not written here.
     */
    fun probe(context: Context, override: Probe) {
        val frames = override.repeats.coerceAtLeast(1)
        val devices = deviceFor(context)
        for (repeat in 1..frames) {
            for (spec in override.params) {
                // Logged BEFORE the frame so the loader's answers line up with
                // the frame that produced them.
                Log.i(TAG_PROBE, ("probe frame topo=0x%x apptype=%d rate=%d output=%d devid=%d" +
                    " param=0x%x caltype=%d repeat=%d/%d").format(
                    override.topo, override.appType, override.rate, override.output, devices.first(),
                    spec.id, spec.calType, repeat, frames))
                sendFrame(context, moduleFor(override.output), override.topo, devices, spec,
                    intArrayOf(override.appType), intArrayOf(0), intArrayOf(override.rate))
            }
        }
    }
}
