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
import androidx.preference.PreferenceFragmentCompat
import androidx.preference.SeekBarPreference
import com.android.settingslib.widget.MainSwitchPreference

class DiracQemSettingsFragment : PreferenceFragmentCompat() {
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

        val bands = DiracQemEffect.currentBands(context)
        for (band in 0 until DiracPresets.EQ_BANDS) {
            findPreference<SeekBarPreference>(KEY_BAND_PREFIX + band)?.let { preference ->
                preference.min = MIN_GAIN
                preference.max = MAX_GAIN
                preference.value = bands[band].toInt()
                preference.setOnPreferenceChangeListener { _, value ->
                    DiracQemEffect.setBand(context, band, (value as Int).toFloat())
                    true
                }
            }
        }
    }

    private fun refreshBands() {
        val bands = DiracQemEffect.currentBands(requireContext())
        for (band in 0 until DiracPresets.EQ_BANDS) {
            findPreference<SeekBarPreference>(KEY_BAND_PREFIX + band)?.value = bands[band].toInt()
        }
    }

    companion object {
        private const val KEY_ENABLED = "dirac_enable"
        private const val KEY_MODEL = "dirac_model"
        private const val KEY_STYLE = "dirac_style"
        private const val KEY_BAND_PREFIX = "dirac_eq_"
        private const val MIN_GAIN = -6
        private const val MAX_GAIN = 6
    }
}
