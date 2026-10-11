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

#include "DiracPsfdDynamics.h"

#include <algorithm>
#include <cmath>

namespace {

// The energy floor that keeps the detector in the linear range of log10.
// -120 dBFS, below any audible content.
constexpr double kMinEnergy = 1.0e-12;

// The gain recurrence is unconditional release: the target gain only ever
// moves further down as the level rises, so attack is folded into the envelope
// one-pole and the gain one-pole handles recovery.
double ToDb(double amplitude) {
    return 20.0 * std::log10(amplitude);
}

}  // namespace

bool DiracPsfdDynamics::Configure(unsigned sampleRateHz, unsigned channelCount) {
    if (sampleRateHz == 0 || channelCount == 0 || channelCount > kMaxChannels) {
        configured_ = false;
        return false;
    }

    attackCoeff_ = 1.0 - std::exp(-1000.0 / (static_cast<double>(sampleRateHz) * kAttackMs));
    releaseCoeff_ = 1.0 - std::exp(-1000.0 / (static_cast<double>(sampleRateHz) * kReleaseMs));
    channelCount_ = channelCount;
    configured_ = true;
    Reset();
    return true;
}

void DiracPsfdDynamics::Reset() {
    for (unsigned ch = 0; ch < kMaxChannels; ++ch) {
        envelope_[ch] = 0.0f;
        gainDb_[ch] = 0.0f;
    }
}

float DiracPsfdDynamics::ProcessSample(float x, unsigned channel) {
    if (!configured_ || channel >= channelCount_) {
        return x;
    }

    const double energy = static_cast<double>(x) * x;
    const double envelopeCoeff = energy > envelope_[channel] ? attackCoeff_ : releaseCoeff_;
    envelope_[channel] += static_cast<float>(envelopeCoeff * (energy - envelope_[channel]));
    const double smoothedEnergy = std::max(static_cast<double>(envelope_[channel]), kMinEnergy);

    const double levelDb = ToDb(std::sqrt(smoothedEnergy));
    const double excess = std::max(levelDb - kThresholdDb, 0.0);
    const double slope = 1.0 - 1.0 / kRatio;
    const double curve = excess <= kKneeDb
                                 ? excess * excess * slope / (2.0 * kKneeDb)
                                 : (excess - kKneeDb / 2.0) * slope;
    const double targetGainDb = -std::min(curve, kMaxAttenDb);

    gainDb_[channel] += static_cast<float>(releaseCoeff_ * (targetGainDb - gainDb_[channel]));
    return x * static_cast<float>(std::pow(10.0, gainDb_[channel] / 20.0));
}
