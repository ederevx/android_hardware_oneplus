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

import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Bundle
import androidx.preference.ListPreference
import androidx.preference.Preference
import androidx.preference.SwitchPreferenceCompat
import com.android.settingslib.widget.MainSwitchPreference
import com.android.settingslib.widget.SettingsBasePreferenceFragment

/**
 * Extends the Settings fragment so the expressive preference group adapter is
 * used: that adapter is what gives every row its card surface and its section
 * grouping, so no row needs a hand-drawn background.
 */
class DiracQemSettingsFragment : SettingsBasePreferenceFragment() {

    private var previewPreference: EqPreviewPreference? = null
    private var stylePreference: ListPreference? = null
    private var modelPreference: ListPreference? = null
    private var sumDiffPreference: SumDiffPreference? = null

    /**
     * Re-evaluates the rows that follow the output when it changes while the
     * page is open. The post drops the update when the view is gone.
     */
    private val routeListener: () -> Unit = {
        view?.post { refreshRouteRows() }
    }

    /**
     * Re-reconciles the route while the page is open. A plug or unplug changes
     * the device set the audio policy reports; the push then re-reads the
     * jack's own state, so the engine follows it without a manual toggle.
     */
    private val routeCallback = object : AudioDeviceCallback() {
        override fun onAudioDevicesAdded(addedDevices: Array<out AudioDeviceInfo>) {
            context?.let { DiracQemEffect.apply(it) }
        }

        override fun onAudioDevicesRemoved(removedDevices: Array<out AudioDeviceInfo>) {
            context?.let { DiracQemEffect.apply(it) }
        }
    }

    override fun onStart() {
        super.onStart()
        requireContext().getSystemService(AudioManager::class.java)
            ?.registerAudioDeviceCallback(routeCallback, null)
        DiracState.addRouteListener(routeListener)
    }

    override fun onStop() {
        DiracState.removeRouteListener(routeListener)
        context?.getSystemService(AudioManager::class.java)
            ?.unregisterAudioDeviceCallback(routeCallback)
        super.onStop()
    }

    override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?) {
        addPreferencesFromResource(R.xml.dirac_settings)
        val context = requireContext()

        findPreference<MainSwitchPreference>(KEY_ENABLED)?.let { preference ->
            preference.isChecked = DiracState.isEnabled(context)
            preference.setOnPreferenceChangeListener { _, value ->
                DiracState.setEnabled(context, value as Boolean)
                DiracQemEffect.apply(context)
                true
            }
        }

        // The host-output fallback row exists only when the build ships the
        // dirac_biquad effect. Absent that, the row is removed: no dead
        // preference, no listener, and nothing that would ever be pushed.
        val fallback = findPreference<SwitchPreferenceCompat>(KEY_BIQUAD_FALLBACK)
        if (fallback != null) {
            if (!DiracState.isBiquadFallbackAvailable(context)) {
                preferenceScreen.removePreference(fallback)
            } else {
                fallback.isChecked = DiracState.isBiquadFallbackEnabled(context)
                fallback.setOnPreferenceChangeListener { _, value ->
                    DiracState.setBiquadFallback(context, value as Boolean)
                    DiracQemEffect.apply(context)
                    true
                }
            }
        }

        modelPreference = findPreference<ListPreference>(KEY_MODEL)?.also { preference ->
            preference.value = DiracState.model(context).toString()
            // Live text: the provider is consulted on every bind, and both the
            // enable flip and a value change notify, so no manual rebind is
            // needed. It also means setSummary must never be called on this row.
            preference.summaryProvider =
                Preference.SummaryProvider<ListPreference> { row ->
                    if (row.isEnabled) row.entry else row.entries.firstOrNull()
                }
            preference.setOnPreferenceChangeListener { _, value ->
                DiracState.setModel(context, (value as String).toInt())
                DiracQemEffect.apply(context)
                true
            }
        }
        refreshModelPreference()

        stylePreference = findPreference<ListPreference>(KEY_STYLE)?.also { preference ->
            preference.value = DiracState.style(context).toString()
            preference.setOnPreferenceChangeListener { _, value ->
                DiracState.setStyle(context, (value as String).toInt())
                refreshBands()
                DiracQemEffect.apply(context)
                true
            }
        }

        previewPreference = findPreference(KEY_EQ_PREVIEW)
        sumDiffPreference = findPreference(KEY_SUMDIFF)

        findPreference<EqBoardPreference>(KEY_EQ_BOARD)?.let { board ->
            board.onBandChanged = { _, _ ->
                // The slider already staged the engine's array; the preview has
                // to redraw, and the preset row only has to move the once, when
                // it stops naming the preset the drag is editing.
                previewPreference?.refresh()
                stylePreference?.let { preference ->
                    val custom = DiracPresets.STYLE_CUSTOM.toString()
                    if (preference.value != custom) {
                        preference.value = custom
                    }
                }
            }
        }

        refreshBands()
    }

    /**
     * The stored preference is the single source of truth, so re-apply it when
     * the page opens: if the app data was reset while the HAL kept a
     * persistent calibration, this brings the engine back in line with the
     * switch instead of leaving the UI and the audio disagreeing.
     */
    override fun onResume() {
        super.onResume()
        DiracQemEffect.apply(requireContext())
    }

    /** Repaints the board and the curve from the engine's current array. */
    private fun refreshBands() {
        findPreference<EqBoardPreference>(KEY_EQ_BOARD)?.refresh()
        previewPreference?.refresh()
    }

    /**
     * Re-reads every row whose state follows the live output: the headset model
     * and the stereo-width row, which is inert wherever the DSP owns the width.
     */
    private fun refreshRouteRows() {
        if (!isAdded) {
            return
        }
        refreshModelPreference()
        sumDiffPreference?.refresh()
    }

    /**
     * The headset-model row is selectable only while the app's own route is the
     * external one -- the same condition under which the effect pushes
     * PARAM_HDSOUND_ENABLE and PARAM_HDSOUND_FILTERIDX. On speaker, Bluetooth
     * and every other route it is greyed out, and its summary then shows the
     * first entry, "General Enhancement", without touching the stored model.
     */
    private fun refreshModelPreference() {
        // The summary follows the row's own state through its provider, so only
        // the enable flip is driven here; setEnabled notifies on its own.
        modelPreference?.isEnabled = isWiredSink()
    }

    private fun isWiredSink(): Boolean {
        val context = context ?: return false
        return DiracState.isWiredSink(context)
    }

    private companion object {
        const val KEY_ENABLED = "dirac_enable"
        const val KEY_BIQUAD_FALLBACK = "dirac_biquad_fallback"
        const val KEY_MODEL = "dirac_model"
        const val KEY_STYLE = "dirac_style"
        const val KEY_EQ_PREVIEW = "dirac_eq_preview"
        const val KEY_EQ_BOARD = "dirac_eq_board"
        const val KEY_SUMDIFF = "dirac_sumdiff"
    }
}
