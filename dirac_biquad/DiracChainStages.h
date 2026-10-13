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

#include "DiracLimiter.h"

// The host chain's shared stage definitions and response-grid geometry.
//
// These are the constants both the filter that runs the chain and the planner
// that reasons about its response must agree on - the two limiter laws, the
// magnitude limit and the grids the plan is read from and verified against - so
// they live in one place and the plan builder can read the law's floor and the
// limit without depending on the filter. Constants only; no state.

namespace dirac_chain {

// The level stage: the recovered safelimiter law at its recovered values -- a
// 64-sample mean square, a symmetric one-pole in linear power (tau 2000 ms),
// the dB law at a -12.5 dBFS threshold and up to 9 dB of attenuation, shaped
// by release and attack poles (both tau 1000 ms). This chain owns the law's
// actuator, not the law: the band shaper applies the attenuation through the
// bend plan, so where the plan covers the response the law lands as that plan
// and where it finds nothing to bend it applies nothing; with no plan in force
// the stage is the law's own output, bit for bit.
//
// -12.5 dBFS / 9.0 dB are the recovered 941 values; their field roles are
// INFERRED (the blob proves the values and the 9.0-then--12.5 order, not which
// is which).
inline constexpr DiracLimiter::Params kLevelStage = {
        -12.5f, 9.0f, 1.0f, 1.0e-4f, 64, true, 2000.0f, 0.0f,
        1000.0f, 1000.0f, true, 0.0f, false, 0u, 0u};

// The peak stage: the recovered pslimiter and the bound the chain ends in --
// per-sample peak, 10 ms hold, 25 ms release pole, instant attack, 6 ms attack
// pole, 8 ms look-ahead; -0.5 dBFS / 50 dB are the recovered 503 values. The
// ceiling a hardBound stage applies caps the output sample peak at the ceiling
// that threshold computes, for any input below threshold + maxAttenuation, read
// from the largest input inside the look-ahead so the sample leaving the delay
// line is always covered.
//
// The ceiling evaluates a true-peak estimate of the input, from a polyphase
// windowed-sinc detector (wide-Hann half-width 64, 16 taps per phase, 32-tap
// kernel, unit DC), because a converter reconstructs an overshoot between
// samples on broadband material. What the hard bound holds exactly, independent
// of any reconstruction kernel, is the sample-peak cap: on the 1440-record
// broadband stress it measured at worst 0.944066 under the ceiling. The true
// (inter-sample) peak is kernel-dependent, about 0.964-1.046 at 16x, so none is
// claimed. This detector departs from the blob, whose safelimiter/pslimiter
// carries no true-peak evaluation; the -0.5 dBFS ceiling is the owner
// requirement.
//
// The interpolator carries about half its kernel length of group delay, so the
// effective look-ahead margin is below the full 8 ms; that bounds the kernel
// length as much as CPU does. The kernel is evaluated folded: a phase and its
// mirror are two half-kernel dot products over the sample sums and differences,
// so 16 taps cost 112 multiply-accumulates per sample per channel instead of
// 224, with the same values. Sixteen taps is the longest the 8 ms look-ahead
// allows, and a shorter truncated-sinc kernel over-reads near Nyquist (at
// 0 dBFS over 13-20 kHz: 0.29 dB at 12 taps, 0.79 at 8, 1.52 at 6, 1.30 at 4).
inline constexpr DiracLimiter::Params kPeakStage = {
        -0.5f, 50.0f, 1.0f, 1.0e-4f, 1, false, 0.0f, 10.0f,
        25.0f, 6.0f, true, 8.0f, true, 16u, 8u};

inline const DiracLimiter::Params &LevelStage() {
    return kLevelStage;
}

inline const DiracLimiter::Params &PeakStage() {
    return kPeakStage;
}

// No static headroom margin sits in front of the stages. The shaper bends only
// the bands in which the configured response violates the magnitude limit, and
// the peak stage is configured to bound its own output, so nothing the signature
// and the curve ask for can carry the chain past that stage's threshold; a fixed
// attenuation here would only take level away from a chain that is already
// bounded.
//
// The limit the response may not exceed: digital full scale, so with the shaper
// fully engaged no frequency it bends has a composite gain above unity.
inline constexpr double kMagnitudeLimit = 1.0;

// The 50 ms every fade in this chain runs over, so the sites cannot drift apart.
inline constexpr double kFadeMs = 50.0;
inline constexpr unsigned FadeSamples(unsigned sampleRateHz) { return sampleRateHz / 20; }

// The response grids the bend plan is read from and verified against, log-spaced
// in Omega = tan(pi*f/fs) from below the lowest band the voicing lifts to just
// under Nyquist. Omega is the coordinate an RBJ section's shape is uniform in, so
// 24 points per Omega-octave resolve a bend section's own curvature at every
// frequency, and 96 verify what the coarse grid placed between its points.
inline constexpr double kBendGridLowHz = 10.0;
inline constexpr double kBendGridHighFactor = 0.49;
inline constexpr double kBendGridPerOctave = 24.0;
inline constexpr double kBendCheckPerOctave = 96.0;

}  // namespace dirac_chain
