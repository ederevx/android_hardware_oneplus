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
import android.media.AudioAttributes
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
 * The source is the device the policy routes media to,
 * AudioManager.getAudioDevicesForAttributes(USAGE_MEDIA), which is the same
 * value AudioFlinger hands the effect as EFFECT_CMD_SET_DEVICE. The two
 * sources this used to need are each wrong for the job. The policy's output
 * list is not a connection signal: AudioPolicyManager attaches every output
 * device the HAL declares as the HW module loads, so it lists the wired
 * headset with nothing in the jack. The sticky HEADSET_PLUG broadcast is not
 * an output identity either: AudioDeviceInventory sends it for a USB headset
 * too (DEVICE_OUT_USB_HEADSET), so a broadcast cannot tell a USB-C headset
 * from the 3.5 mm jack and the app would push the wired ADSP frames at a USB
 * output. The routed device is the one input the app and the effect both act
 * on.
 */
object DiracRouteResolver {
    private const val TAG = "DiracQemRoute"

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
    private const val AUDIO_DEVICE_OUT_SPEAKER_SAFE = 0x400000
    private const val AUDIO_DEVICE_OUT_USB_HEADSET = 0x4000000
    private const val AUDIO_DEVICE_OUT_HEARING_AID = 0x8000000

    /**
     * Does the Dirac DSP voice this output; the exact set the effect's gate
     * cedes to it (dirac_route::IsDspOutput in ../dirac_biquad/DiracRoute.cpp).
     * A set, not a bit mask: a device type is an exclusive value and a Bluetooth
     * LE type carries a built-in type's bit, so membership is by whole value.
     */
    private val DSP_VOICED_DEVICES = setOf(
        AUDIO_DEVICE_OUT_SPEAKER,
        AUDIO_DEVICE_OUT_SPEAKER_SAFE,
        AUDIO_DEVICE_OUT_WIRED_HEADSET,
        AUDIO_DEVICE_OUT_WIRED_HEADPHONE,
        AUDIO_DEVICE_OUT_LINE,
    )

    /** The wired jack's own devices, the ones the headset model drives. */
    private val WIRED_DEVICES = setOf(
        AUDIO_DEVICE_OUT_WIRED_HEADSET,
        AUDIO_DEVICE_OUT_WIRED_HEADPHONE,
        AUDIO_DEVICE_OUT_LINE,
    )

    /**
     * The host outputs, as the AudioDeviceInfo type the platform reports and the
     * platform audio device it is. Only the A2DP entry is read on its own, by
     * the loudness scalar's question; the rest are here so [OUTPUT_DEVICES] can
     * name every routed device. The built-in speaker is named directly in
     * [OUTPUT_DEVICES] instead, because for it the routed device is the routing
     * answer and not a presence question; the earpiece is deliberately unnamed,
     * so it falls to the host exactly as the effect routes it.
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
     * The AudioDeviceInfo type the policy routes to, mapped to the platform
     * audio device the HAL and the effect name. It is built after
     * [HOST_SINK_DEVICES] because it extends it, and it is the only place the
     * app turns a routed type into a device.
     */
    private val OUTPUT_DEVICES: Map<Int, Int> = mapOf(
        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER to AUDIO_DEVICE_OUT_SPEAKER,
        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER_SAFE to AUDIO_DEVICE_OUT_SPEAKER_SAFE,
        AudioDeviceInfo.TYPE_WIRED_HEADSET to AUDIO_DEVICE_OUT_WIRED_HEADSET,
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES to AUDIO_DEVICE_OUT_WIRED_HEADPHONE,
        AudioDeviceInfo.TYPE_LINE_ANALOG to AUDIO_DEVICE_OUT_LINE,
    ) + HOST_SINK_DEVICES

    /** The attributes the route question is asked for: media, the stream this app voices. */
    private val MEDIA = AudioAttributes.Builder()
        .setUsage(AudioAttributes.USAGE_MEDIA)
        .build()

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
     * devices [route] resolved: the app skips the ADSP leg on exactly the
     * outputs the effect leaves to the host.
     */
    fun isDspVoiced(context: Context): Boolean = route(context).dspVoiced

    /** The live sink, the platform audio device it is, and whether the DSP voices it. */
    private class Route(val sink: Sink, val audioDevice: Int, val dspVoiced: Boolean)

    private fun route(context: Context): Route {
        // The device the policy routes media to: the same one AudioFlinger hands
        // the effect, so the app and the effect gate on one input.
        val routed = try {
            context.getSystemService(AudioManager::class.java)
                ?.getAudioDevicesForAttributes(MEDIA)?.map { OUTPUT_DEVICES[it.type] ?: 0 }
                .orEmpty()
        } catch (e: Exception) {
            emptyList()
        }
        // IsDspOutput's rule: exactly one output outside the set is
        // the host's to voice; a member, a combination, or nothing routed is
        // left to the DSP.
        val route = if (routed.size == 1 && routed[0] !in DSP_VOICED_DEVICES) {
            Route(Sink.HOST, routed[0], false)
        } else {
            val wired = routed.firstOrNull { it in WIRED_DEVICES } ?: 0
            val device = if (wired != 0) {
                wired
            } else {
                routed.firstOrNull { it in DSP_VOICED_DEVICES } ?: AUDIO_DEVICE_OUT_SPEAKER
            }
            Route(if (wired != 0) Sink.WIRED else Sink.SPEAKER, device, true)
        }
        // Information, not debug: this is the decision the settings row and the
        // HAL push both follow, and it is cheap only on a route event.
        Log.i(
            TAG,
            "sink=${route.sink} device=${route.audioDevice} dsp=${route.dspVoiced} " +
                "routed=[${routed.joinToString(",")}]",
        )
        return route
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
}
