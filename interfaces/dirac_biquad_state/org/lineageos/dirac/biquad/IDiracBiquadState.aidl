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

package org.lineageos.dirac.biquad;

/**
 * The Dirac biquad fallback state the settings app hands to the host-side
 * effect.
 *
 * The effect runs outside the app, so the app sends its state here and the
 * server owns persisting it for the effect to read.
 */
interface IDiracBiquadState {
    /**
     * Persist the effect's state. Bands are seven half-dB gains. `sumdiff`
     * is the mid/side width in 0..1 (0 = bypass). `volumeDb` is the stream
     * attenuation in dB below the reference, or a value <= 0 when the volume
     * is unknown (the effect then leaves the tilt off).
     */
    void setState(boolean enabled, boolean fallback, float sumdiff, in int[] bandsHalfDb,
                  double volumeDb);

    /**
     * Whether a state has been persisted since this service started. The
     * reader methods below are only meaningful when this is true.
     */
    boolean hasState();

    /** The enabled flag of the last persisted state. */
    boolean getEnabled();

    /** The fallback flag of the last persisted state. */
    boolean getFallback();

    /** The mid/side width of the last persisted state, in 0..1. */
    float getSumDiff();

    /** The seven half-dB band gains of the last persisted state. */
    int[] getBandsHalfDb();

    /** The stream attenuation of the last persisted state, in dB. */
    double getVolumeDb();
}
