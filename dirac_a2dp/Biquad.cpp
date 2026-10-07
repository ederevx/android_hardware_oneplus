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

#include "Biquad.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double kPi = 3.14159265358979323846;

}  // namespace

void Biquad::Reset() {
    x1_ = 0.0;
    x2_ = 0.0;
    y1_ = 0.0;
    y2_ = 0.0;
}

void Biquad::SetFilter(BiquadType type, double sampleRateHz, double frequencyHz, double q,
                       double gainDb) {
    if (sampleRateHz <= 0.0 || frequencyHz <= 0.0 || q <= 0.0) {
        return;
    }

    const double w0 = 2.0 * kPi * frequencyHz / sampleRateHz;
    const double cosW0 = std::cos(w0);
    const double sinW0 = std::sin(w0);
    const double a = std::pow(10.0, gainDb / 40.0);

    double alpha;
    if (type == BiquadType::kHighPass || type == BiquadType::kPeaking) {
        alpha = sinW0 / (2.0 * q);
    } else {
        alpha = (sinW0 / 2.0) * std::sqrt(std::max(0.0, (a + 1.0 / a) * (1.0 / q - 1.0) + 2.0));
    }

    double b0, b1, b2, a0, a1, a2;
    switch (type) {
        case BiquadType::kHighPass:
            b0 = (1.0 + cosW0) / 2.0;
            b1 = -(1.0 + cosW0);
            b2 = (1.0 + cosW0) / 2.0;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cosW0;
            a2 = 1.0 - alpha;
            break;
        case BiquadType::kLowShelf: {
            const double beta = 2.0 * std::sqrt(a) * alpha;
            b0 = a * ((a + 1.0) - (a - 1.0) * cosW0 + beta);
            b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cosW0);
            b2 = a * ((a + 1.0) - (a - 1.0) * cosW0 - beta);
            a0 = (a + 1.0) + (a - 1.0) * cosW0 + beta;
            a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cosW0);
            a2 = (a + 1.0) + (a - 1.0) * cosW0 - beta;
            break;
        }
        case BiquadType::kPeaking:
            b0 = 1.0 + alpha * a;
            b1 = -2.0 * cosW0;
            b2 = 1.0 - alpha * a;
            a0 = 1.0 + alpha / a;
            a1 = -2.0 * cosW0;
            a2 = 1.0 - alpha / a;
            break;
        case BiquadType::kHighShelf: {
            const double beta = 2.0 * std::sqrt(a) * alpha;
            b0 = a * ((a + 1.0) + (a - 1.0) * cosW0 + beta);
            b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cosW0);
            b2 = a * ((a + 1.0) + (a - 1.0) * cosW0 - beta);
            a0 = (a + 1.0) - (a - 1.0) * cosW0 + beta;
            a1 = 2.0 * ((a - 1.0) - (a + 1.0) * cosW0);
            a2 = (a + 1.0) - (a - 1.0) * cosW0 - beta;
            break;
        }
        default:
            return;
    }

    b0_ = b0 / a0;
    b1_ = b1 / a0;
    b2_ = b2 / a0;
    a1_ = a1 / a0;
    a2_ = a2 / a0;
}

float Biquad::ProcessSample(float x) {
    const double y = b0_ * x + b1_ * x1_ + b2_ * x2_ - a1_ * y1_ - a2_ * y2_;
    x2_ = x1_;
    x1_ = x;
    y2_ = y1_;
    y1_ = y;
    return static_cast<float>(y);
}