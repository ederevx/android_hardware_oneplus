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

// Host-side A2DP voicing curve.
//
// The curve is a short minimum-phase biquad chain that approximates the
// published Dirac HD Sound treatment for a mobile output: magnitude and
// impulse response correction toward a flat curve, with deeper controlled
// bass, a clearer midrange, and a wider soundstage. It is synthesised from
// that published behaviour, not from Dirac's signed DAR coefficient data.
class DiracA2dpVoicing {
  public:
    // Builds the curve for the stream rate and channel count. Returns false
    // for an unsupported combination, in which case the caller must pass the
    // stream through untouched.
    bool Configure(unsigned sampleRateHz, unsigned channelCount);
    void Reset();

    // Applies the curve to an interleaved PCM buffer. `input` and `output` may
    // alias; `accumulate` adds the result to the existing output content, as
    // EFFECT_BUFFER_ACCESS_ACCUMULATE requires.
    void Process(const void *input, void *output, size_t frameCount, unsigned channelCount,
                 audio_format_t format, bool accumulate);

    static constexpr unsigned kStageCount = 5;

  private:
    float ProcessSample(float x, unsigned channel);

    static constexpr unsigned kMaxChannels = 8;

    Biquad stages_[kMaxChannels][kStageCount];
    unsigned channelCount_ = 0;
    bool configured_ = false;
};