/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

package vendor.lineage.slider;

import vendor.lineage.slider.IAlertSliderCallback;

@VintfStability
interface IAlertSlider {
    /**
     * Atomically subscribe and return the current state, so a fresh or
     * restarted client never waits for the next movement to learn it.
     * State is one of 1 (top/mute), 2 (middle/dnd), 3 (bottom/normal),
     * or 0 when unknown.
     */
    int registerCallback(IAlertSliderCallback callback);

    /** Current cached state, 0 when unknown. */
    int getState();
}
