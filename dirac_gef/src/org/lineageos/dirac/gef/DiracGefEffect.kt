/*
 * Copyright (c) 2021 The LineageOS Project
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

package org.lineageos.dirac.gef

import android.content.Context
import android.media.audiofx.AudioEffect
import com.android.internal.util.HexDump
import java.util.UUID

/**
 * Owns the global Dirac GEF effect.
 *
 * The effect is held for the lifetime of the app process: releasing the last
 * [AudioEffect] client destroys and unregisters the engine, which would drop
 * the session-0 effect shortly after boot.
 */
object DiracGefEffect {
    private val EFFECT_TYPE_DIRAC_GEF = UUID.fromString("3799d6d1-22c5-43c3-b3ec-d664cf8d2f0d")

    private var audioEffect: AudioEffect? = null

    @Synchronized
    fun apply(context: Context) {
        val effect = audioEffect ?: createAudioEffect()
        context.assets.open("dirac_gef_init.txt").reader().forEachLine {
            val (param, value) = it.split("|")
            effect.setParameter(
                HexDump.hexStringToByteArray(param),
                HexDump.hexStringToByteArray(value),
            )
        }
        effect.enabled = true
    }

    private fun createAudioEffect(): AudioEffect =
        AudioEffect(AudioEffect.EFFECT_TYPE_NULL, EFFECT_TYPE_DIRAC_GEF, 0, 0).also {
            audioEffect = it
        }
}
