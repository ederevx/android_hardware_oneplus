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

// The host leg's mid/side stage: the width and the low-band fold in one matrix.
//
// The Dirac module's panorama2 Sum/Diff Balance is one continuous control over
// a mid/side path - M and S are formed inline and each side is filtered per
// sample rate before the weights (1-v, 1+v) are applied - and a per-channel
// biquad cascade cannot express any of it: every channel of the cascade is an
// independent filter with identical coefficients. This unit restores it in the
// one place the chain crosses channels.
//
// The width w in 0..1 maps to the side gain in exactly one place, GainForWidth:
// g(w) = 1 + w*(10^(kMaxGainDb/20) - 1). The side is high-passed before that
// gain, so the band below the corner tends to mono while the width applies
// above it: M=(L+R)/2, S=(L-R)/2, S' = g(w)*HP(S), L'=M+S', R'=M-S'. Only the
// side is touched, so each input channel still sees the cascade's transfer H
// for every width, and w=0 returns before the filter, holds no state and is an
// exact bypass: enabling or changing the width never resets a filter upstream.
//
// The corner is a fixed analog prototype through Biquad::SetPrototype, so the
// same response is realised at every sample rate and there is no per-rate
// table. It stands in for the module's low-frequency behaviour, which is not
// decoded, not a recovered law. The stage's frequency-domain gain stays at or
// below unity - |HP(f)| <= 1 and the mid is unchanged - so the static headroom
// preamp stays valid, but it is not per-sample non-expansive: the side
// high-pass has a tail, so a hard-panned transient can overshoot same-frame
// maxima transiently and only in the side (~1.22x on white noise, 2.42x
// adversarial).
class StereoWidth {
  public:
    // Maximum side gain, in dB, reached at width 1.0. The single tuning
    // constant; the slider interpolates linearly to unity.
    static constexpr double kMaxGainDb = 3.0;

    // The mono corner, below the loudness tilt's 150 Hz shelf so the fold and
    // the tilt shape different bands instead of overlapping.
    static constexpr double kCornerHz = 100.0;

    // 2nd-order Butterworth, maximally flat and monotone, so the fold only
    // removes side energy below the corner and never adds any.
    static constexpr double kQ = 0.7071;

    // Builds the stage for the stream rate and channel count.
    void Configure(unsigned sampleRateHz, unsigned channelCount);

    void Reset();

    // Sets the widening amount. 0.0 is an exact bypass, 1.0 is kMaxGainDb;
    // values outside 0..1 are clamped here, the one owner of the mapping.
    void SetWidth(float width);

    // Applies the matrix to channels 0/1 of one interleaved frame in place. A
    // one-channel stream, a zero width, and the channels beyond the first pair
    // are left untouched.
    void ProcessFrame(float *frame, unsigned channelCount);

  private:
    // The one place the 0..1 width becomes a side gain.
    static float GainForWidth(float width);

    Biquad sideHighPass_;
    float gain_ = 1.0f;
    float width_ = 0.0f;
    unsigned channelCount_ = 0;
    bool configured_ = false;
};
