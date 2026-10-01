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
import android.content.SharedPreferences
import android.media.audiofx.AudioEffect
import android.util.Log
import com.android.internal.util.HexDump
import java.io.File
import java.io.Reader
import java.util.UUID

/**
 * Owns the global Dirac GEF effect and the persisted user state.
 *
 * The effect is held for the lifetime of the app process: releasing the last
 * [AudioEffect] client destroys and unregisters the engine, which would drop
 * the session-0 effect shortly after boot. Both writers - the boot receiver and
 * the settings screen - go through this object, so the effect keeps one owner.
 */
object DiracGefEffect {
    private val EFFECT_TYPE_DIRAC_GEF = UUID.fromString("3799d6d1-22c5-43c3-b3ec-d664cf8d2f0d")

    private const val TAG = "OnePlusDiracGef"
    private const val INIT_ASSET = "dirac_gef_init.txt"
    private const val PREFS = "dirac_gef"
    private const val KEY_ENABLED = "enabled"

    private var audioEffect: AudioEffect? = null

    /** Applies the persisted state, creating the effect on first use. */
    @Synchronized
    fun apply(context: Context) {
        val appContext = context.applicationContext
        if (!isEnabled(appContext)) {
            audioEffect?.enabled = false
            return
        }
        val effect = audioEffect ?: createAudioEffect()
        initStream(appContext).forEachLine {
            val (param, value) = it.split("|")
            effect.setParameter(
                HexDump.hexStringToByteArray(param),
                HexDump.hexStringToByteArray(value),
            )
        }
        effect.enabled = true
    }

    @Synchronized
    fun setEnabled(context: Context, enabled: Boolean) {
        val appContext = context.applicationContext
        preferences(appContext).edit().putBoolean(KEY_ENABLED, enabled).apply()
        apply(appContext)
    }

    fun isEnabled(context: Context): Boolean =
        preferences(context.applicationContext).getBoolean(KEY_ENABLED, true)

    /**
     * The GEF init stream. A same-named file in the app's device-protected data
     * directory overrides the packaged asset, so slot values can be edited on
     * device without rebuilding; removing it falls back to the asset. The file
     * cannot live under `/data/local/tmp`: the app runs as `system_app`, which
     * AOSP sepolicy forbids from opening `shell_data_file`.
     */
    private fun initStream(context: Context): Reader {
        val overrideFile = File(
            context.createDeviceProtectedStorageContext().filesDir,
            INIT_ASSET,
        )
        if (!overrideFile.isFile) {
            return context.assets.open(INIT_ASSET).reader()
        }
        Log.d(TAG, "Replaying init override $overrideFile")
        return overrideFile.reader()
    }

    // Device-protected storage: the boot receiver runs before the user unlocks.
    private fun preferences(context: Context): SharedPreferences =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    private fun createAudioEffect(): AudioEffect =
        AudioEffect(AudioEffect.EFFECT_TYPE_NULL, EFFECT_TYPE_DIRAC_GEF, 0, 0).also {
            audioEffect = it
        }
}
