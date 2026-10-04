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

// Biquad (second-order IIR) section for the A2DP voicing chain.
//
// Coefficients follow the Audio EQ Cookbook (Robert Bristow-Johnson, W3C
// NOTE-audio-eq-cookbook) normalized by a0, and the runtime uses the direct
// form 1 recursion, which the cookbook recommends for floating point.

enum class BiquadType {
    kHighPass,
    kLowShelf,
    kPeaking,
    kHighShelf,
};

class Biquad {
  public:
    void Reset();

    // `q` is the quality factor for the peaking and high-pass types, and the
    // shelf slope S for the shelving types. `gainDb` applies to peaking and
    // shelving only.
    void SetFilter(BiquadType type, double sampleRateHz, double frequencyHz, double q,
                   double gainDb);

    float ProcessSample(float x);

  private:
    double b0_ = 1.0;
    double b1_ = 0.0;
    double b2_ = 0.0;
    double a1_ = 0.0;
    double a2_ = 0.0;

    double x1_ = 0.0;
    double x2_ = 0.0;
    double y1_ = 0.0;
    double y2_ = 0.0;
};