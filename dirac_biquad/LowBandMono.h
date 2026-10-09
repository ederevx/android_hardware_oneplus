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

// Low-band mono fold for the host leg.
//
// The Dirac module's `noise` plugin carries a "Sum/Diff Balance" control, i.e.
// mid/side processing, which a per-channel biquad cascade cannot express: every
// channel of the cascade is an independent filter with identical coefficients.
// This unit restores the low-frequency half of that cross-channel behaviour by
// high-passing the SIDE signal only. Below the corner the side is rolled off and
// the pair tends to mono; above it the pair is unchanged.
//
// It is a topology step, not a voicing one: the mid path is deliberately left
// untouched, so it adds no shelf or high-pass to the main magnitude response and
// cannot overlap or double the signature's own low-frequency roll-off or the
// loudness tilt's 150 Hz shelf. The corner is a fixed analog prototype through
// Biquad::SetPrototype, so the same response is realised at every sample rate
// and there is no per-rate table.
//
// The fold's frequency-domain gain stays at or below unity - the side is
// |HP(f)| <= 1 and the mid is unchanged - so the static headroom preamp, a
// frequency-domain policy over the signature and EQ, stays valid and its probe
// is unchanged. It is not per-sample non-expansive, though: the side high-pass
// has a tail, so a hard-panned transient can overshoot the same-frame maximum
// (~1.22x on white noise, 2.42x adversarial), transiently and only in the side.
class LowBandMono {
  public:
    // The mono corner, below the loudness tilt's 150 Hz shelf so the fold and
    // the tilt shape different bands instead of overlapping.
    static constexpr double kCornerHz = 100.0;

    // 2nd-order Butterworth, maximally flat and monotone, so the fold only
    // removes side energy below the corner and never adds any.
    static constexpr double kQ = 0.7071;

    // Builds the side high-pass for the stream rate.
    void Configure(unsigned sampleRateHz, unsigned channelCount);

    void Reset();

    // Folds channels 0/1 of one interleaved frame in place. A one-channel stream
    // and the channels beyond the first pair are left untouched.
    void ProcessFrame(float *frame, unsigned channelCount);

  private:
    Biquad sideHighPass_;
    unsigned channelCount_ = 0;
    bool configured_ = false;
};
