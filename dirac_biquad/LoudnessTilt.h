/*
 * Copyright (C) 2026 The LineageOS Project
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

#pragma once

#include "Biquad.h"

// Volume-linked loudness contour for the host leg.
//
// The ISO 226 equal-loudness contours are level dependent: below the 80-phon
// reference the ear loses low- and high-frequency sensitivity faster than the
// midrange, so a quiet reproduction is not just a scaled-down loud one. This
// unit contours the effect output by the stream attenuation to put the lost
// weight back.
//
// This class owns the whole loudness tilt: the law, the three shelf corners and
// the caps. Nothing else in the effect may own any part of it, so the feature is
// independent of the signature approximation, the user EQ and the preamp.
//
// Corners are fixed analog prototypes, set through Biquad::SetPrototype, so
// the same response is realised at every sample rate and there is no per-rate
// table.
class LoudnessTilt {
  public:
    static constexpr unsigned kMaxChannels = 8;

    static constexpr double kLowCornerHz = 150.0;
    static constexpr double kHighCornerHz = 4000.0;
    static constexpr double kAirCornerHz = 10000.0;

    // The law: `a` is the attenuation in dB below the 80-phon reference.
    // The ear's bass loss is the largest term; the 4 kHz term is a cut because
    // the midrange is relatively more sensitive as level falls; the 10 kHz air
    // term returns the top end.
    static constexpr double kLowSlope = 0.23;
    static constexpr double kLowCapDb = 12.0;
    static constexpr double kHighSlope = -0.045;
    static constexpr double kHighCapDb = -2.5;
    static constexpr double kAirSlope = 0.055;
    static constexpr double kAirCapDb = 2.8;

    // Builds the shelves for the stream rate and channel count. A stored
    // attenuation is re-derived for the new rate; an unknown one stays identity.
    void Configure(unsigned sampleRateHz, unsigned channelCount);

    // Sets the contour's drive. `attenuationDb` is the attenuation below the
    // reference; any value <= 0, non-finite or unknown means exact identity.
    // It is quantised, and the coefficients are rebuilt only when the rounded
    // value actually changes, so a stream of volume steps costs one rebuild per
    // distinct rounded attenuation.
    void SetAttenuationDb(double attenuationDb);

    void Reset();

    float ProcessSample(float x, unsigned channel);

  private:
    // Recomputes the three shelf coefficients for the current attenuation.
    void Rebuild();

    Biquad low_[kMaxChannels];
    Biquad high_[kMaxChannels];
    Biquad air_[kMaxChannels];
    unsigned sampleRateHz_ = 0;
    unsigned channelCount_ = 0;
    double attenuationDb_ = -1.0;
    bool configured_ = false;
};
