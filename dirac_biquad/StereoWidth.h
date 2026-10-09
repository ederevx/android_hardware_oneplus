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

// Mid/side stereo width for the A2DP effect.
//
// The Dirac module's `noise` plugin carries a "Sum/Diff Balance" control, i.e.
// mid/side processing, that a per-channel biquad cascade cannot express. This
// unit realises the widening half of it as a pure, frequency-independent
// matrix: M=(L+R)/2, S=(L-R)/2, S'=S*g(w), L'=M+S', R'=M-S'. Only the side is
// scaled, so each input channel still sees the cascade's transfer H: the
// signature, EQ, tilt and low-band fold keep their exact per-channel magnitude
// response and group delay for every width. There is no filter, delay line or
// look-ahead, so the stage adds no latency and no phase of its own.
//
// The width w in 0..1 maps to the side gain in exactly one place, GainForWidth:
// g(w) = 1 + w*(10^(kMaxGainDb/20) - 1). w=0 gives g=1, and the stage then holds
// no signal state and returns before it reads the frame, so off is an exact
// bypass; enabling or changing the width never resets or perturbs a filter
// upstream.
class StereoWidth {
  public:
    // Maximum side gain, in dB, reached at width 1.0. The single tuning
    // constant; the slider interpolates linearly to unity.
    static constexpr double kMaxGainDb = 3.0;

    // Builds the stage for the stream's channel count.
    void Configure(unsigned channelCount);

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

    float gain_ = 1.0f;
    float width_ = 0.0f;
    unsigned channelCount_ = 0;
    bool configured_ = false;
};
