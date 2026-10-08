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

// Biquad (second-order IIR) section for the A2DP filter chain.
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

    // `frequencyHz` is a digital corner: the coefficient set realises, at this
    // rate, a prototype whose corner is pre-warped to land on it.  `q` is the
    // quality factor for the peaking and high-pass types, and the shelf slope S
    // for the shelving types. `gainDb` applies to peaking and shelving only.
    void SetFilter(BiquadType type, double sampleRateHz, double frequencyHz, double q,
                   double gainDb);

    // `cornerHz` is the corner of an analog prototype.  The bilinear transform
    // is pre-warped for this rate, so the same arguments realise the same
    // analog response at every sample rate instead of a rate-specific one.
    void SetPrototype(BiquadType type, double sampleRateHz, double cornerHz, double q,
                      double gainDb);

    float ProcessSample(float x);

    // Evaluates |H(f)| from the stored coefficients, for headroom analysis.
    double MagnitudeAt(double sampleRateHz, double frequencyHz) const;

  private:
    void SetCoefficients(BiquadType type, double w0, double q, double gainDb);

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