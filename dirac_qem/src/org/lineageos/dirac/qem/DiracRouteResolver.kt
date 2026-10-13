/*
 * Copyright (C) 2026 The LineageOS Project
 * Copyright (C) 2026 Edrick Sinsuan <evcsinsuan@gmail.com>
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
import android.content.Intent
import android.content.IntentFilter
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.util.Log

/**
 * The one owner of the live output the user hears, and of what that output
 * means for the widening.
 *
 * The sink is read, never remembered: every call re-reads the platform, so a
 * page opened while a route is already live still sees it, and a connection
 * this process was not running for is never missed. [DiracState] keeps a copy
 * only to know when a change is worth telling a live page about.
 *
 * The two sinks need different sources, which is why neither the policy's
 * device list nor a broadcast is enough on its own. The audio policy's device
 * list is not a connection source for the wired sinks on this platform:
 * AudioPolicyManager attaches every output device the HAL declares to its
 * available-output set as the HW module loads, so AudioManager.isWiredHeadsetOn()
 * and getDevices(GET_DEVICES_OUTPUTS) both report the wired headset with
 * nothing in the jack; the sticky HEADSET_PLUG broadcast that AudioService's
 * AudioDeviceInventory sends on every wired connect and disconnect is the true
 * jack state. A host output is the opposite case: its device is created only
 * while it is attached -- Bluetooth A2DP and SCO, USB, the remote submix and the
 * hearing aid alike -- so its presence in that same list is the connection
 * signal, while a profile or mix state would need a second source, and its
 * broadcast is not delivered to a process that was not running. The built-in
 * speaker and earpiece are the exception that keeps presence from being a
 * liveness test: the policy attaches them as the HW module loads, so they are
 * excluded from host detection and classified by their own device instead.
 */
object DiracRouteResolver {
    private const val TAG = "DiracQemRoute"
    private const val EXTRA_STATE = "state"
    private const val EXTRA_MICROPHONE = "microphone"

    /**
     * The platform audio devices the app names, as the HAL itself names them
     * (AUDIO_DEVICE_OUT_*, system/media/audio/include/system/audio-base-utils.h).
     * A calibration frame carries one of these and the HAL resolves it to its
     * own sound device and ACDB device, which is the only place that knows
     * which sound device a given stream needs; the app names no sound device.
     */
    private const val AUDIO_DEVICE_OUT_SPEAKER = 0x2
    private const val AUDIO_DEVICE_OUT_WIRED_HEADSET = 0x4
    private const val AUDIO_DEVICE_OUT_WIRED_HEADPHONE = 0x8
    private const val AUDIO_DEVICE_OUT_BLUETOOTH_SCO = 0x10
    private const val AUDIO_DEVICE_OUT_BLUETOOTH_A2DP = 0x80
    private const val AUDIO_DEVICE_OUT_USB_ACCESSORY = 0x2000
    private const val AUDIO_DEVICE_OUT_USB_DEVICE = 0x4000
    private const val AUDIO_DEVICE_OUT_REMOTE_SUBMIX = 0x8000
    private const val AUDIO_DEVICE_OUT_LINE = 0x20000
    private const val AUDIO_DEVICE_OUT_USB_HEADSET = 0x4000000
    private const val AUDIO_DEVICE_OUT_HEARING_AID = 0x8000000

    /**
     * The one owner of "does the Dirac DSP voice this output": the four devices
     * the effect's own gate excludes from the host chain, DiracBiquad_IsDspOutput
     * in ../dirac_biquad/EffectDiracBiquad.cpp:231-234. Everything outside the
     * mask -- Bluetooth A2DP and SCO, USB, the remote submix, hearing aid,
     * earpiece, line and speaker variants alike -- is voiced by the host
     * fallback, so no ADSP frame may be pushed to it. The app and the effect are
     * separate modules with no shared header, so the mask is restated here; this
     * is the only copy on the app side and nothing else may name the four.
     */
    private const val DSP_VOICED_DEVICES =
        AUDIO_DEVICE_OUT_SPEAKER or AUDIO_DEVICE_OUT_WIRED_HEADSET or
            AUDIO_DEVICE_OUT_WIRED_HEADPHONE or AUDIO_DEVICE_OUT_LINE

    private val WIRED_SINK_TYPES = intArrayOf(
        AudioDeviceInfo.TYPE_WIRED_HEADSET,
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES,
    )

    /**
     * The host outputs the app detects, as the AudioDeviceInfo type the platform
     * reports and the platform audio device it is. The order is the tie-break
     * when several are attached; Bluetooth A2DP keeps the head of the list it
     * already had. Unlike the wired devices, these are created when attached and
     * torn down when removed, so their presence in the policy's output list is
     * the connection signal. The built-in earpiece and speaker are deliberately
     * absent: presence is not liveness for them (see the class comment), and
     * [DSP_VOICED_DEVICES] classifies them on their own device instead.
     */
    private val A2DP_SINK =
        AudioDeviceInfo.TYPE_BLUETOOTH_A2DP to AUDIO_DEVICE_OUT_BLUETOOTH_A2DP

    private val HOST_SINK_DEVICES = listOf(
        A2DP_SINK,
        AudioDeviceInfo.TYPE_BLUETOOTH_SCO to AUDIO_DEVICE_OUT_BLUETOOTH_SCO,
        AudioDeviceInfo.TYPE_USB_HEADSET to AUDIO_DEVICE_OUT_USB_HEADSET,
        AudioDeviceInfo.TYPE_USB_DEVICE to AUDIO_DEVICE_OUT_USB_DEVICE,
        AudioDeviceInfo.TYPE_USB_ACCESSORY to AUDIO_DEVICE_OUT_USB_ACCESSORY,
        AudioDeviceInfo.TYPE_HEARING_AID to AUDIO_DEVICE_OUT_HEARING_AID,
        AudioDeviceInfo.TYPE_REMOTE_SUBMIX to AUDIO_DEVICE_OUT_REMOTE_SUBMIX,
    )

    /**
     * The live output. The route carries what it means, so the app's questions
     * about it -- whether the DSP voices the widening, which ADSP module the
     * frames address -- are answered next to the route instead of by a second
     * copy of the same knowledge somewhere else.
     */
    enum class Sink {
        /** The on-device sinks, speaker first among them. */
        SPEAKER,

        /** The wired jack. */
        WIRED,

        /**
         * Every output the Dirac DSP does not voice: Bluetooth A2DP and SCO, USB,
         * the remote submix Android Auto and other projection consumers use,
         * hearing aid, earpiece, and the rest. The host biquad chain is the
         * fallback for all of them.
         */
        HOST,
    }

    /** Reads the live sink. */
    fun sink(context: Context): Sink = route(context).sink

    /**
     * The platform audio device of the live sink, for the frames that address
     * it. It comes from the same resolution as [sink], so the sink the app
     * describes and the device it names can never disagree.
     */
    fun outputDevice(context: Context): Int = route(context).audioDevice

    /**
     * Whether the Dirac DSP voices the live sink, so the ADSP frames can land on
     * it. It is the effect's own gate, [DSP_VOICED_DEVICES], applied to the
     * device [route] resolved: the app skips the ADSP leg on exactly the outputs
     * the effect leaves to the host.
     */
    fun isDspVoiced(context: Context): Boolean = route(context).dspVoiced

    /** The live sink and the platform audio device it is, resolved once. */
    private class Route(val sink: Sink, val audioDevice: Int) {
        val dspVoiced: Boolean get() = audioDevice and DSP_VOICED_DEVICES != 0
    }

    private fun route(context: Context): Route {
        val sticky = stickyJack(context)
        val plugged = sticky?.getIntExtra(EXTRA_STATE, 0)?.let { it != 0 } ?: false
        val host = if (plugged) null else hostRoute(context)
        val route = when {
            plugged -> Route(Sink.WIRED, wiredAudioDevice(sticky))
            host != null -> host
            else -> Route(Sink.SPEAKER, AUDIO_DEVICE_OUT_SPEAKER)
        }
        // Information, not debug: this is the decision the settings row and the
        // HAL push both follow, and it is cheap only on a route event.
        Log.i(
            TAG,
            "sink=${route.sink} device=${route.audioDevice} dsp=${route.dspVoiced} " +
                "jack_plugged=$plugged host_output=${host?.audioDevice ?: 0} " +
                "wired_available=[${wiredCandidates(context)}]",
        )
        return route
    }

    /**
     * The live host output, or null when nothing but the built-in speaker is
     * attached. The wired jack is not consulted here: [route] resolves it first
     * from its own connection signal, so a policy-listed wired device can never
     * be mistaken for the live output.
     */
    private fun hostRoute(context: Context): Route? {
        val attached = outputDevices(context).map { it.type }.toSet()
        for ((type, audioDevice) in HOST_SINK_DEVICES) {
            if (type in attached) {
                return Route(Sink.HOST, audioDevice)
            }
        }
        return null
    }

    /**
     * Which wired audio device the jack is. Both resolve to the same platform
     * sound device, and that resolution belongs to the HAL: a headset with a
     * microphone is AUDIO_DEVICE_OUT_WIRED_HEADSET, a plain headphone is
     * AUDIO_DEVICE_OUT_WIRED_HEADPHONE.
     */
    private fun wiredAudioDevice(sticky: Intent?): Int =
        if (sticky?.getBooleanExtra(EXTRA_MICROPHONE, false) == true) {
            AUDIO_DEVICE_OUT_WIRED_HEADSET
        } else {
            AUDIO_DEVICE_OUT_WIRED_HEADPHONE
        }

    /**
     * The jack's own connection signal, read once per resolution. A platform
     * that refuses the sticky read, or has not sent one since boot, leaves the
     * sink unwired, which is the safe answer for a jack with no known state.
     */
    private fun stickyJack(context: Context): Intent? = try {
        context.registerReceiver(null, IntentFilter(Intent.ACTION_HEADSET_PLUG))
    } catch (e: Exception) {
        null
    }

    /**
     * Whether a Bluetooth A2DP sink is attached. This is the loudness scalar's
     * question, independent of which output is live; the live output itself is
     * resolved by [route], which reads the same [A2DP_SINK] entry.
     */
    fun bluetoothSinkAttached(context: Context): Boolean =
        outputDevices(context).any { it.type == A2DP_SINK.first }

    private fun outputDevices(context: Context): List<AudioDeviceInfo> =
        context.getSystemService(AudioManager::class.java)
            ?.getDevices(AudioManager.GET_DEVICES_OUTPUTS)?.toList() ?: emptyList()

    /**
     * The wired sink devices the policy reports as available. They are logged
     * only so the device check can see that the list is not the connection
     * signal; it carries the wired headset with nothing plugged.
     */
    private fun wiredCandidates(context: Context): String =
        outputDevices(context).filter { it.type in WIRED_SINK_TYPES }
            .joinToString(",") { "type=${it.type}" }
            .ifEmpty { "none" }
}
