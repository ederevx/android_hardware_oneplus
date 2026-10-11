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

// Host approximation of the Dirac PSFD dynamic range processor.
//
// This unit reproduces the static gain computer and the one-pole smoothing of
// the DSP's ps3 "psfd" compressor, recovered from libdirac-capiv2.so. The
// curve (0x1db74) is, for a level in dB:
//
//     e     = max(level - threshold, 0)
//     slope = 1 - 1/ratio
//     curve = e <= knee ? e*e*slope/(2*knee) : (e - knee/2)*slope
//     gain  = -min(curve, maxAttenuation)
//
// and the module's default template at rodata 0x52abc carries the values used
// here: threshold -40 dB, knee 3 dB, ratio 2, max attenuation 12 dB. The
// template has no relocation or code reference in the blob, so treating it as
// the active configuration is an inference (the same six values also appear in
// the descriptor default array at 0x52b14); the formula is the DSP's own law.
// The enable byte is set by "Compressors Enable" and runs from the DAR when
// the app exposes no switch.
//
// What is approximated, and therefore our own basis rather than the DSP's: the
// DSP runs the curve per band over a five-way splitter whose 129-tap kernels
// are proprietary data and are not copied here, so the detector sees the full
// band; the detector and gain recurrents both use the module's control-loop
// law 1 - exp(-1000/(rate*T_ms)) (the module derives further laws, -4.0 and
// +1000, that this unit does not model); and the detector is per-sample rather
// than the DSP's decimated control path. Only the curve and the four values
// above claim DSP provenance.
class DiracPsfdDynamics {
  public:
    static constexpr unsigned kMaxChannels = 8;

    // The module's default template values (rodata 0x52abc).
    static constexpr double kThresholdDb = -40.0;
    static constexpr double kKneeDb = 3.0;
    static constexpr double kRatio = 2.0;
    static constexpr double kMaxAttenDb = 12.0;

    // The one-pole time constants. The DSP derives each coefficient as
    // 1 - exp(-1000/(rate*T_ms)) and its template carries 10 and 30; which of
    // the two is attack and which is release is inferred.
    static constexpr double kAttackMs = 10.0;
    static constexpr double kReleaseMs = 30.0;

    // Builds the detector and gain coefficients for the stream rate and channel
    // count. Returns false for an unsupported combination, in which case the
    // caller must pass the stream through this stage untouched.
    bool Configure(unsigned sampleRateHz, unsigned channelCount);

    void Reset();

    // Applies the compressor to one sample of one channel.
    float ProcessSample(float x, unsigned channel);

  private:
    float envelope_[kMaxChannels] = {};
    float gainDb_[kMaxChannels] = {};
    double attackCoeff_ = 0.0;
    double releaseCoeff_ = 0.0;
    unsigned channelCount_ = 0;
    bool configured_ = false;
};
