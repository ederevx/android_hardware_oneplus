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

#include <stddef.h>

#include <hardware/audio.h>

#include "Biquad.h"

// Host-side A2DP curve with Dirac-QEM parity.
//
// One peaking biquad per Dirac band centre (68/165/400/972/2000/6000/14000 Hz)
// carrying the same half-dB gains the QEM app pushes as PARAM_EQ_BANDS
// (0x12D36), plus a headroom preamp that only reduces the level when a band
// boosts. The Dirac DAR device correction, the HDSOUND filter index and the
// limiter chain are not reproduced, so this is a parity of the user EQ, not of
// the wired-route sound.
class DiracA2dpVoicing {
  public:
    static constexpr size_t kBandCount = 7;

    static constexpr double kBandCenterHz[kBandCount] = {
            68.0, 165.0, 400.0, 972.0, 2000.0, 6000.0, 14000.0};

    static constexpr unsigned kMaxChannels = 8;

    // Builds the curve for the stream rate and channel count from the half-dB
    // band gains. Returns false for an unsupported combination, in which case
    // the caller must pass the stream through untouched.
    bool Configure(unsigned sampleRateHz, unsigned channelCount,
                   const int gainsHalfDb[kBandCount]);

    void Reset();

    // Applies the curve to an interleaved PCM buffer. `input` and `output` may
    // alias; `accumulate` adds the result to the existing output content, as
    // EFFECT_BUFFER_ACCESS_ACCUMULATE requires.
    void Process(const void *input, void *output, size_t frameCount, unsigned channelCount,
                 audio_format_t format, bool accumulate);

  private:
    float ProcessSample(float x, unsigned channel);

    Biquad stages_[kMaxChannels][kBandCount];
    float preampGain_ = 1.0f;
    unsigned channelCount_ = 0;
    bool configured_ = false;
};
