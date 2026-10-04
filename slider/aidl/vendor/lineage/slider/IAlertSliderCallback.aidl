/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package vendor.lineage.slider;

@VintfStability
interface IAlertSliderCallback {
    oneway void onStateChanged(int state);
}
