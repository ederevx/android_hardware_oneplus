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

#include "LoudnessTilt.h"

#include <algorithm>
#include <cmath>

namespace {

// Shelf slope S. 1.0 gives the steepest shelf that does not peak at the corner,
// which matches the broad, monotone shape of an equal-loudness contour.
constexpr double kShelfSlope = 1.0;

// The attenuation is quantised before it drives the coefficients, so a ramping
// volume key costs one rebuild per 0.1 dB instead of one per event.
constexpr double kAttenuationStepDb = 0.1;

// Any attenuation past this is silence; the caps already flatten the law, so
// the clamp only keeps a mute (which reports -infinity) finite.
constexpr double kMaxAttenuationDb = 120.0;

// A sentinel for "unknown", i.e. exact identity. Any non-positive value maps
// here, so the audio path has a single identity test.
constexpr double kIdentityDb = -1.0;

double QuantiseAttenuation(double attenuationDb) {
    if (!std::isfinite(attenuationDb) || attenuationDb <= 0.0) {
        return kIdentityDb;
    }
    const double clamped = std::min(attenuationDb, kMaxAttenuationDb);
    return std::round(clamped / kAttenuationStepDb) * kAttenuationStepDb;
}

}  // namespace

void LoudnessTilt::Configure(unsigned sampleRateHz, unsigned channelCount) {
    if (sampleRateHz == 0 || channelCount == 0 || channelCount > kMaxChannels) {
        configured_ = false;
        return;
    }

    sampleRateHz_ = sampleRateHz;
    channelCount_ = channelCount;
    configured_ = true;
    Rebuild();
}

void LoudnessTilt::SetAttenuationDb(double attenuationDb) {
    const double quantised = QuantiseAttenuation(attenuationDb);
    if (quantised == attenuationDb_) {
        return;
    }
    attenuationDb_ = quantised;
    if (configured_) {
        Rebuild();
    }
}

void LoudnessTilt::Reset() {
    for (unsigned ch = 0; ch < kMaxChannels; ++ch) {
        low_[ch].Reset();
        high_[ch].Reset();
        air_[ch].Reset();
    }
}

void LoudnessTilt::Rebuild() {
    // A non-positive attenuation is exact identity: ProcessSample bypasses the
    // shelves entirely, so no coefficient write is needed here.
    if (attenuationDb_ <= 0.0) {
        return;
    }

    const double lowGainDb = std::min(kLowSlope * attenuationDb_, kLowCapDb);
    const double highGainDb = std::max(kHighSlope * attenuationDb_, kHighCapDb);
    const double airGainDb = std::min(kAirSlope * attenuationDb_, kAirCapDb);

    for (unsigned ch = 0; ch < channelCount_; ++ch) {
        low_[ch].SetPrototype(BiquadType::kLowShelf, sampleRateHz_, kLowCornerHz, kShelfSlope,
                              lowGainDb);
        high_[ch].SetPrototype(BiquadType::kHighShelf, sampleRateHz_, kHighCornerHz, kShelfSlope,
                               highGainDb);
        air_[ch].SetPrototype(BiquadType::kHighShelf, sampleRateHz_, kAirCornerHz, kShelfSlope,
                              airGainDb);
    }
}

float LoudnessTilt::ProcessSample(float x, unsigned channel) {
    if (!configured_ || attenuationDb_ <= 0.0 || channel >= channelCount_) {
        return x;
    }

    x = low_[channel].ProcessSample(x);
    x = high_[channel].ProcessSample(x);
    x = air_[channel].ProcessSample(x);
    return x;
}
