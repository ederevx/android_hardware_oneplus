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

package org.lineageos.dirac.tone

import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioTrack
import android.util.Log
import kotlin.math.PI
import kotlin.math.roundToInt
import kotlin.math.sin

/**
 * Holds one stereo mixer stream open for as long as it plays, so the ADM RX
 * (path=0) copp the QEM frames target stays registered on the device while they
 * are sent. The stream usage and preferred output come from [ToneRoute].
 */
class TonePlayer(
    private val route: ToneRoute,
    private val sampleRate: Int = DEFAULT_SAMPLE_RATE,
    private val frequencyHz: Double = DEFAULT_FREQUENCY_HZ,
    private val amplitude: Double = DEFAULT_AMPLITUDE,
) {
    private var track: AudioTrack? = null
    private var writer: Thread? = null

    @Volatile
    private var playing = false

    /** Audio session of the live stream, or [AudioManager.ERROR] when idle. */
    val sessionId: Int
        get() = track?.audioSessionId ?: AudioManager.ERROR

    /** Returns false when the track cannot be initialised or started. */
    fun start(): Boolean {
        val channelMask = AudioFormat.CHANNEL_OUT_STEREO
        val encoding = AudioFormat.ENCODING_PCM_16BIT
        val bufferSize = maxOf(
            AudioTrack.getMinBufferSize(sampleRate, channelMask, encoding),
            MIN_BUFFER_FRAMES * CHANNELS,
        )
        val created = AudioTrack(
            route.audioAttributes(),
            AudioFormat.Builder()
                .setEncoding(encoding)
                .setSampleRate(sampleRate)
                .setChannelMask(channelMask)
                .build(),
            bufferSize,
            AudioTrack.MODE_STREAM,
            AudioManager.AUDIO_SESSION_ID_GENERATE,
        )
        if (created.state != AudioTrack.STATE_INITIALIZED) {
            Log.e(TAG, "AudioTrack init failed, state=${created.state}")
            created.release()
            return false
        }
        track = created
        route.device?.let { preferred ->
            if (!created.setPreferredDevice(preferred)) {
                Log.w(TAG, "preferred device rejected, staying on the default output")
            }
        }
        val block = buildToneBlock()
        playing = true
        writer = Thread { writeLoop(created, block) }.also { it.start() }
        created.play()
        Log.i(TAG, "tone up: session=${created.audioSessionId} rate=$sampleRate " +
            "$route freq=${frequencyHz}Hz buffer=$bufferSize")
        return true
    }

    fun stop() {
        playing = false
        writer?.let { runCatching { it.join(WRITER_JOIN_MS) } }
        writer = null
        track?.let {
            runCatching { it.stop() }
            it.release()
        }
        track = null
        Log.i(TAG, "tone down")
    }

    private fun writeLoop(target: AudioTrack, block: ShortArray) {
        try {
            while (playing) {
                target.write(block, 0, block.size)
            }
        } catch (t: Throwable) {
            Log.e(TAG, "tone writer stopped", t)
        }
    }

    /**
     * A whole number of sine periods, so repeating the block is continuous and
     * no sample has to be synthesised again while the tone runs.
     */
    private fun buildToneBlock(): ShortArray {
        val periodFrames = (sampleRate / frequencyHz).roundToInt()
        val frames = periodFrames * PERIODS_PER_BLOCK
        val block = ShortArray(frames * CHANNELS)
        for (frame in 0 until frames) {
            val value = (sin(2.0 * PI * frequencyHz * frame / sampleRate) * amplitude *
                Short.MAX_VALUE).roundToInt().toShort()
            for (channel in 0 until CHANNELS) {
                block[frame * CHANNELS + channel] = value
            }
        }
        return block
    }

    private companion object {
        const val TAG = "DiracToneProbe"
        const val CHANNELS = 2
        const val DEFAULT_SAMPLE_RATE = 48000
        const val DEFAULT_FREQUENCY_HZ = 1000.0
        const val DEFAULT_AMPLITUDE = 0.2
        const val PERIODS_PER_BLOCK = 64
        const val MIN_BUFFER_FRAMES = 8192
        const val WRITER_JOIN_MS = 500L
    }
}
