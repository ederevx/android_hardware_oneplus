/*
 * Copyright (C) 2026 The LineageOS Project
 * Copyright (C) 2026 Edrick Sinsuan <evcsinsuan@gmail.com>
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

#include <algorithm>
#include <cmath>
#include <stdint.h>
#include <string.h>

// One policy per sample format, so one template carries the per-frame pipeline
// for both; the statics inline at the two dispatch sites.
namespace dirac_format {

struct FloatSamples {
    using Sample = float;

    static float Load(float sample) { return sample; }

    // The dry leg is only the sanitized input: ACCUMULATE content never enters
    // the chain, and a non-finite sample must not reach the filter state.
    static float Dry(float x) { return std::isfinite(x) ? x : 0.0f; }

    static void Store(float *out, float value, bool accumulate) {
        *out = accumulate ? *out + value : value;
    }

    static void Add(float *out, float in) { *out += in; }
};

struct Pcm16Samples {
    using Sample = int16_t;

    static float Load(int16_t sample) { return static_cast<float>(sample) / 32768.0f; }

    // An integer sample is always finite.
    static float Dry(float x) { return x; }

    static void Store(int16_t *out, float value, bool accumulate) {
        const int32_t own =
                static_cast<int32_t>(std::lround(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
        const int32_t sum = accumulate ? own + *out : own;
        *out = static_cast<int16_t>(std::clamp(sum, -32768, 32767));
    }

    static void Add(int16_t *out, int16_t in) {
        *out = static_cast<int16_t>(std::clamp(int32_t{*out} + in, -32768, 32767));
    }
};

// AudioFlinger does not copy the input into the output for an effect that
// implements process(), so a pass-through must still produce the output.
template <typename F>
void PassThroughFrames(const typename F::Sample *in, typename F::Sample *out, size_t samples,
                       bool accumulate) {
    if (!accumulate) {
        memcpy(out, in, samples * sizeof(*in));
        return;
    }
    for (size_t i = 0; i < samples; ++i) {
        F::Add(&out[i], in[i]);
    }
}

}  // namespace dirac_format
