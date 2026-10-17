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

    SetCoefficients(type, 2.0 * kPi * frequencyHz / sampleRateHz, q, gainDb);
}

void Biquad::SetPrototype(BiquadType type, double sampleRateHz, double cornerHz, double q,
                          double gainDb) {
    if (sampleRateHz <= 0.0 || cornerHz <= 0.0 || q <= 0.0) {
        return;
    }

    // Analog corner wa to digital w0: wa = 2*fs*tan(w0/2), so pre-warping the
    // prototype corner wa = 2*pi*cornerHz gives w0 = 2*atan(pi*cornerHz/fs),
    // which stays inside (0, pi) for every corner and rate.
    SetCoefficients(type, 2.0 * std::atan(kPi * cornerHz / sampleRateHz), q, gainDb);
}

void Biquad::SetCoefficients(BiquadType type, double w0, double q, double gainDb) {
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

void Biquad::SetButterworthShelf(BiquadType type, double sampleRateHz, double cornerHz,
                                double gainDb, unsigned order, unsigned stage) {
    if ((order != 2 && order != 4) || stage >= order / 2 || cornerHz <= 0.0 ||
        sampleRateHz <= 0.0) {
        SetFilter(BiquadType::kHighPass, sampleRateHz, 1.0, 0.5, 0.0);
        return;
    }

    // The analog prototype of a Butterworth shelf, normalised to a corner of 1:
    // the zeros sit on a circle of radius rhoZ and the poles on one of radius
    // rhoP, and each second-order section carries one Butterworth factor c.
    // A high shelf is the low shelf with s -> 1/s, which swaps the two radii and
    // leaves the DC gain at one.
    const double gain = std::pow(10.0, gainDb / 20.0);
    const double rhoZero = std::pow(gain, 1.0 / (2.0 * static_cast<double>(order)));
    const double rhoPole = std::pow(gain, -1.0 / (2.0 * static_cast<double>(order)));
    const double c = 2.0 * std::sin((2.0 * static_cast<double>(stage) + 1.0) * kPi /
                                    (2.0 * static_cast<double>(order)));

    double numeratorLeading;
    double numeratorConstant;
    double numeratorLinear;
    double denominatorConstant;
    double denominatorLinear;
    if (type == BiquadType::kHighShelf) {
        // The high shelf is the low shelf with s -> 1/s: the two radii swap
        // roles and the whole numerator carries the stage's high-frequency gain
        // as its leading coefficient, which is what makes it a cut rather than a
        // bump.
        // The stage's high-frequency gain is the numerator's leading
        // coefficient, so the whole numerator carries it; the denominator stays
        // normalised, which is what puts the cut above the corner and unity
        // below it.
        const double scale = (rhoZero * rhoZero) / (rhoPole * rhoPole);
        numeratorLeading = scale;
        numeratorConstant = scale / (rhoZero * rhoZero);
        numeratorLinear = scale * c / rhoZero;
        denominatorConstant = 1.0 / (rhoPole * rhoPole);
        denominatorLinear = c / rhoPole;
    } else {
        numeratorLeading = 1.0;
        numeratorConstant = rhoZero * rhoZero;
        numeratorLinear = c * rhoZero;
        denominatorConstant = rhoPole * rhoPole;
        denominatorLinear = c * rhoPole;
    }

    // Bilinear transform of (s^2 + b1 s + b0)/(s^2 + a1 s + a0) with the corner
    // pre-warped, then normalise by the denominator's leading coefficient.
    const double k = std::tan(kPi * cornerHz / sampleRateHz);
    const double k2 = k * k;
    const double denominator0 = 1.0 + denominatorLinear * k + denominatorConstant * k2;
    if (denominator0 == 0.0) {
        SetFilter(BiquadType::kHighPass, sampleRateHz, 1.0, 0.5, 0.0);
        return;
    }
    b0_ = (numeratorLeading + numeratorLinear * k + numeratorConstant * k2) / denominator0;
    b1_ = 2.0 * (numeratorConstant * k2 - numeratorLeading) / denominator0;
    b2_ = (numeratorLeading - numeratorLinear * k + numeratorConstant * k2) / denominator0;
    a1_ = 2.0 * (denominatorConstant * k2 - 1.0) / denominator0;
    a2_ = (1.0 - denominatorLinear * k + denominatorConstant * k2) / denominator0;
}

BiquadPoint Biquad::PointAt(double sampleRateHz, double frequencyHz) {
    if (sampleRateHz <= 0.0 || frequencyHz <= 0.0) {
        return BiquadPoint{1.0, 0.0, 1.0, 0.0};
    }
    const double w = 2.0 * kPi * frequencyHz / sampleRateHz;
    return BiquadPoint{std::cos(w), std::sin(w), std::cos(2.0 * w), std::sin(2.0 * w)};
}

BiquadResponse Biquad::ResponseAt(const BiquadPoint &point) const {
    const double numRe = b0_ + b1_ * point.cosW + b2_ * point.cos2W;
    const double numIm = -(b1_ * point.sinW + b2_ * point.sin2W);
    const double denRe = 1.0 + a1_ * point.cosW + a2_ * point.cos2W;
    const double denIm = -(a1_ * point.sinW + a2_ * point.sin2W);
    const double den = denRe * denRe + denIm * denIm;
    if (den <= 0.0) {
        return BiquadResponse{1.0, 0.0};
    }
    return BiquadResponse{(numRe * denRe + numIm * denIm) / den,
                          (numIm * denRe - numRe * denIm) / den};
}

BiquadCoefficients Biquad::Coefficients() const {
    return BiquadCoefficients{b0_, b1_, b2_, a1_, a2_};
}

void Biquad::Load(const BiquadCoefficients &coefficients) {
    b0_ = coefficients.b0;
    b1_ = coefficients.b1;
    b2_ = coefficients.b2;
    a1_ = coefficients.a1;
    a2_ = coefficients.a2;
    // An immediate load is the end of any ramp: the caller has just said which
    // coefficient set this filter runs.
    rampRemaining_ = 0;
}

void Biquad::LoadRamped(const BiquadCoefficients &coefficients, unsigned rampSamples) {
    if (rampSamples == 0) {
        Load(coefficients);
        return;
    }
    const double target[5] = {coefficients.b0, coefficients.b1, coefficients.b2,
                              coefficients.a1, coefficients.a2};
    const double current[5] = {b0_, b1_, b2_, a1_, a2_};
    for (size_t index = 0; index < 5; ++index) {
        target_[index] = target[index];
        step_[index] = (target[index] - current[index]) / static_cast<double>(rampSamples);
    }
    rampRemaining_ = rampSamples;
}

void Biquad::StepRamp() {
    if (rampRemaining_ == 0) {
        return;
    }
    b0_ += step_[0];
    b1_ += step_[1];
    b2_ += step_[2];
    a1_ += step_[3];
    a2_ += step_[4];
    if (--rampRemaining_ == 0) {
        // Land exactly on the set the ramp was heading for: a step repeated to the
        // end leaves a rounding residue, and the filter would then run a
        // coefficient set no caller asked for.
        b0_ = target_[0];
        b1_ = target_[1];
        b2_ = target_[2];
        a1_ = target_[3];
        a2_ = target_[4];
    }
}

float Biquad::ProcessIdentity(float x) {
    // For b0 == 1, b1 == a1, b2 == a2 the recursion is
    // y = x + a1*(x1 - y1) + a2*(x2 - y2), so with x1 == y1 and x2 == y2 it
    // returns x exactly in exact arithmetic. Carrying both halves of the state
    // the same way keeps that invariant true for the next call too, so the
    // section can return to the full recursion with the state it would have had.
    x2_ = x1_;
    x1_ = x;
    y2_ = y1_;
    y1_ = x;
    return x;
}

float Biquad::ProcessSample(float x) {
    StepRamp();
    const double y = b0_ * x + b1_ * x1_ + b2_ * x2_ - a1_ * y1_ - a2_ * y2_;
    x2_ = x1_;
    x1_ = x;
    y2_ = y1_;
    y1_ = y;
    return static_cast<float>(y);
}