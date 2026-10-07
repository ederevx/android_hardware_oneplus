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

import android.os.Bundle
import androidx.preference.ListPreference
import com.android.settingslib.widget.MainSwitchPreference
import com.android.settingslib.widget.SettingsBasePreferenceFragment

/**
 * Extends the Settings fragment so the expressive preference group adapter is
 * used: that adapter is what gives every row its card surface and its section
 * grouping, so no row needs a hand-drawn background.
 */
class DiracQemSettingsFragment : SettingsBasePreferenceFragment() {
    override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?) {
        addPreferencesFromResource(R.xml.dirac_settings)
        val context = requireContext()

        findPreference<MainSwitchPreference>(KEY_ENABLED)?.let { preference ->
            preference.isChecked = DiracQemEffect.isEnabled(context)
            preference.setOnPreferenceChangeListener { _, value ->
                DiracQemEffect.setEnabled(context, value as Boolean)
                true
            }
        }

        findPreference<ListPreference>(KEY_MODEL)?.let { preference ->
            preference.value = DiracQemEffect.model(context).toString()
            preference.setOnPreferenceChangeListener { _, value ->
                DiracQemEffect.setModel(context, (value as String).toInt())
                true
            }
        }

        findPreference<ListPreference>(KEY_STYLE)?.let { preference ->
            preference.value = DiracQemEffect.style(context).toString()
            preference.setOnPreferenceChangeListener { _, value ->
                DiracQemEffect.setStyle(context, (value as String).toInt())
                refreshBands()
                true
            }
        }

        findPreference<EqBoardPreference>(KEY_EQ_BOARD)?.let { board ->
            board.onBandChanged = { band, value ->
                DiracQemEffect.setBand(context, band, value)
                findPreference<EqPreviewPreference>(KEY_EQ_PREVIEW)?.refresh()
                // A manual band edit is exactly what the custom preset means,
                // so the preset row must follow it.
                findPreference<ListPreference>(KEY_STYLE)?.value =
                    DiracPresets.STYLE_CUSTOM.toString()
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

    private fun refreshBands() {
        val bands = DiracQemEffect.currentBands(requireContext())
        findPreference<EqBoardPreference>(KEY_EQ_BOARD)?.setBands(bands)
        findPreference<EqPreviewPreference>(KEY_EQ_PREVIEW)?.refresh()
    }

    private companion object {
        const val KEY_ENABLED = "dirac_enable"
        const val KEY_MODEL = "dirac_model"
        const val KEY_STYLE = "dirac_style"
        const val KEY_EQ_PREVIEW = "dirac_eq_preview"
        const val KEY_EQ_BOARD = "dirac_eq_board"
    }
}
