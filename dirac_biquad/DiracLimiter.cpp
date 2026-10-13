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

#include "DiracLimiter.h"

#include <algorithm>
#include <cmath>

namespace {

// ln(10)/20: gain = 10^(-attenuationDb/20).
constexpr float kLn10Over20 = 0.115129254758358f;

// log2 to dB, for an amplitude and for a power respectively.
constexpr float kAmplitudeKdbPerLog2 = 6.020599913279624f;
constexpr float kPowerKdbPerLog2 = 3.010299956639812f;

// The law's time base: a configured time constant is expressed per second and
// the smoothing updates once per detector window.
constexpr double kRatePerSecond = 1000.0;

constexpr float kLevelFloorDb = -120.0f;

// One-pole coefficient for a windowed update, a = 1 - exp(-1000 / (Q * tau)),
// where Q is the number of updates per second (rate / window).
float TimeConstantCoef(unsigned sampleRateHz, unsigned window, float tauMs) {
    if (sampleRateHz == 0 || window == 0 || tauMs <= 0.0f) {
        return 0.0f;
    }
    const double updatesPerSecond = static_cast<double>(sampleRateHz) / static_cast<double>(window);
    return static_cast<float>(1.0 - std::exp(-kRatePerSecond / (updatesPerSecond * tauMs)));
}

unsigned SamplesForMs(float milliseconds, unsigned sampleRateHz) {
    return static_cast<unsigned>(
            std::lround(static_cast<double>(sampleRateHz) * milliseconds / 1000.0));
}

}  // namespace

void DiracLimiter::Configure(unsigned sampleRateHz, unsigned channelCount, const Params &params) {
    if (sampleRateHz == 0 || channelCount == 0 || channelCount > kMaxChannels ||
        params.window == 0 || params.slope == 0.0f) {
        configured_ = false;
        return;
    }

    params_ = params;
    kdbPerLog2_ = params.energyDetector ? kPowerKdbPerLog2 : kAmplitudeKdbPerLog2;
    levelMemoryCoef_ = TimeConstantCoef(sampleRateHz, params.window, params.levelMemoryMs);
    releaseCoef_ = TimeConstantCoef(sampleRateHz, params.window, params.releaseMs);
    attackCoef_ = TimeConstantCoef(sampleRateHz, params.window, params.attackMs);
    holdSamples_ = SamplesForMs(params.peakHoldMs, sampleRateHz);
    delaySamples_ = SamplesForMs(params.lookaheadMs, sampleRateHz);

    delay_.assign(static_cast<size_t>(kMaxChannels) * delaySamples_, 0.0f);

    // The 2x true-peak detector: one fractional phase, evaluated at the
    // half-sample position. The window is a Hann of half-width 64 samples,
    // which is nearly flat over the 12-tap support, so the kernel approaches
    // the truncated ideal sinc; it was the best of a windowed/Kaiser/reference-
    // matched sweep and beats the narrow Hann by about 1.5-2 dB of true-peak
    // residual at the same multiply count. Each phase is normalised to unit DC
    // gain, so a constant input is reproduced at every phase.
    truePeakTaps_ = params.truePeakTaps;
    if (truePeakTaps_ > kMaxTruePeakTaps) {
        truePeakTaps_ = kMaxTruePeakTaps;
    }
    truePeakLength_ = 2 * truePeakTaps_;
    if (truePeakLength_ > 0) {
        const double center = static_cast<double>(truePeakTaps_ - 1) + 0.5;
        double unnormalised[2 * kMaxTruePeakTaps] = {};
        double sum = 0.0;
        for (unsigned k = 0; k < truePeakLength_; ++k) {
            const double t = center - static_cast<double>(k);
            const double sinc = (t == 0.0) ? 1.0
                                           : std::sin(M_PI * t) / (M_PI * t);
            const double window = 0.5 * (1.0 + std::cos(M_PI * t / 64.0));
            unnormalised[k] = sinc * window;
            sum += unnormalised[k];
        }
        if (sum != 0.0) {
            for (unsigned k = 0; k < truePeakLength_; ++k) {
                const double scaled = unnormalised[k] / sum;
                truePeakCoef_[k] = static_cast<float>(scaled);
            }
        }
    }

    channelCount_ = channelCount;
    Reset();
    configured_ = true;
}

void DiracLimiter::Reset() {
    for (unsigned ch = 0; ch < kMaxChannels; ++ch) {
        channels_[ch] = Envelope{};
    }
    std::fill(delay_.begin(), delay_.end(), 0.0f);
}

float DiracLimiter::TargetDb(float levelDb) const {
    const float excess = std::max(0.0f, params_.slope * (levelDb - params_.thresholdDb));
    float attenuation;
    if (excess <= 0.0f) {
        attenuation = 0.0f;
    } else if (params_.kneeDb > 0.0f && excess < params_.kneeDb) {
        attenuation = (excess * excess) / (2.0f * params_.kneeDb);
    } else {
        attenuation = excess - 0.5f * params_.kneeDb;
    }
    return std::min(attenuation, params_.maxAttenuationDb);
}

float DiracLimiter::ProcessSample(float x, unsigned channel) {
    if (!configured_ || channel >= channelCount_) {
        return x;
    }

    Envelope &env = channels_[channel];
    float *delay = delaySamples_ > 0 ? delay_.data() + static_cast<size_t>(channel) * delaySamples_
                                     : nullptr;

    // Look-ahead: emit the input written delaySamples_ ago, then store this one.
    float delayed = x;
    if (delay != nullptr) {
        delayed = delay[env.write];
        delay[env.write] = x;
        if (++env.write == delaySamples_) {
            env.write = 0;
        }
    }

    // The ceiling a bounding stage applies. Its own detector cannot be the source:
    // a hold keeps a maximum for a fixed time and then drops to the current input,
    // so a peak that never became the maximum is forgotten while it is still
    // inside the look-ahead, which is how a sample leaves the delay line
    // unattended. Two block maxima do span it: the sample about to leave the
    // delay line always lies in the block before the current one, and the maximum
    // is read before the boundary closes the current block, because once the
    // boundary has passed, the block that sample lives in is the one just
    // dropped and the sample would leave with whatever the law alone allowed.
    float boundGain = 1.0f;
    if (params_.hardBound) {
        const unsigned boundWindow = std::max(delaySamples_, 1u);
        double localPeak = static_cast<double>(std::fabs(x));
        if (truePeakLength_ > 0) {
            // Interpolate the half-sample position and let the ceiling see it
            // too, so the bound is on the reconstruction and not only on the
            // sample. The history ring is mirrored so the tap loop has no wrap
            // branch, and it is short compared with the block the two-block
            // maximum below covers, so the departing sample is still counted in
            // a block whose maximum already includes every half-sample estimate
            // of its own reconstruction.
            const unsigned length = truePeakLength_;
            env.truePeakHistory[env.truePeakWrite] = x;
            env.truePeakHistory[env.truePeakWrite + length] = x;
            unsigned write = env.truePeakWrite + 1;
            if (write == length) {
                write = 0;
            }
            env.truePeakWrite = write;
            const float *history = env.truePeakHistory;
            const float *coefficient = truePeakCoef_;
            double interpolated = 0.0;
            for (unsigned k = 0; k < length; ++k) {
                interpolated += static_cast<double>(coefficient[k]) *
                                static_cast<double>(history[write + k]);
            }
            localPeak = std::max(localPeak, std::fabs(interpolated));
        }
        env.boundAccumulator = std::max(env.boundAccumulator, localPeak);
        const double boundLevel = std::max(env.boundPrevious, env.boundAccumulator);
        if (++env.boundFill >= boundWindow) {
            env.boundPrevious = env.boundAccumulator;
            env.boundAccumulator = 0.0;
            env.boundFill = 0;
        }
        const float boundDb = boundLevel > 0.0
                                      ? kdbPerLog2_ * static_cast<float>(std::log2(boundLevel))
                                      : kLevelFloorDb;
        boundGain = std::exp(-TargetDb(boundDb) * kLn10Over20);
    }

    // Detect over the window. The gain ramps across it, one step per sample, so
    // only a window boundary recomputes it.
    if (params_.energyDetector) {
        env.accumulator += static_cast<double>(x) * static_cast<double>(x);
    } else {
        env.accumulator = std::max(env.accumulator, static_cast<double>(std::fabs(x)));
    }
    if (++env.fill < params_.window) {
        env.gain += env.rampStep;
        return delayed * std::min(env.gain, boundGain);
    }
    env.fill = 0;

    double windowed = env.accumulator;
    env.accumulator = 0.0;
    if (params_.energyDetector) {
        windowed /= static_cast<double>(params_.window);
    }

    // Hold the largest detected value, when the stage asks for a hold.
    if (holdSamples_ > 0) {
        if (!env.heldValid || windowed >= env.held) {
            env.held = windowed;
            env.heldValid = true;
            env.holdCount = holdSamples_;
        } else if (env.holdCount > 0) {
            --env.holdCount;
        } else {
            env.held = windowed;
        }
        windowed = env.held;
    }

    // Follow the detected level with a one-pole, when the stage asks for one.
    if (levelMemoryCoef_ > 0.0f) {
        if (!env.levelValid) {
            env.level = windowed;
            env.levelValid = true;
        } else {
            env.level += static_cast<double>(levelMemoryCoef_) * (windowed - env.level);
        }
        windowed = env.level;
    }

    const float levelValue = static_cast<float>(windowed);
    const float levelDb = levelValue > 0.0f ? kdbPerLog2_ * std::log2(levelValue) : kLevelFloorDb;
    const float target = TargetDb(levelDb);

    const float released = (1.0f - releaseCoef_) * env.s1 + releaseCoef_ * target;
    env.s1 = params_.instantAttack ? std::max(target, released) : released;
    env.s2 += attackCoef_ * (env.s1 - env.s2);

    const float newGain = std::exp(-env.s2 * kLn10Over20);
    env.rampStep = (newGain - env.gain) / static_cast<float>(params_.window);
    env.gain += env.rampStep;
    // Whatever the law asks for, the ceiling wins.
    return delayed * std::min(env.gain, boundGain);
}
