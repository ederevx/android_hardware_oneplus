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

// The level stage: the recovered safelimiter law, kept at its recovered
// values. It measures the mean square over a 64-sample window, follows it with
// a symmetric one-pole in linear power (tau 2000 ms), and maps that level
// through the dB law at a -12.5 dBFS threshold and up to 9 dB of attenuation;
// the resulting attenuation is then held by an instant attack and shaped by a
// release and an attack one-pole (both tau 1000 ms). The kernel's compiled
// apply path carries all three time constants as immediates, so they are part
// of the recovered stage and the law runs unchanged. What this chain owns is the
// law's actuator, not the law: the band shaper applies the attenuation the law
// asks for through the bend plan, so on a route the plan covers the law reaches
// the signal as that plan rather than as a broadband gain, and where the plan
// finds nothing to bend it applies nothing. With no plan in force the stage is
// the law's own output, bit for bit. The host falsification
// separates three configurations, none of which is the recovered law. Removing
// the level memory alone, with this peak stage, keeps the full-scale worst-case
// bass THD at the parity 0.33% and measures 1.08 dB of 1 kHz probe ducking
// against a 2.90 dB stock baseline, but shortens the probe recovery from ~4.8 s
// to ~2.24 s. Pairing that memoryless level stage with the old stock peak stage
// measures 8.35 dB of ducking at 6.9% THD. Removing the 1000 ms gain pole as
// well raises the worst-case bass THD to 7.2% at 44.1 kHz and 8.6% at 48 kHz.
// Those measurements describe the law itself, which is what the host chain runs
// when it has no plan to apply; the shaper's band-selective actuator is the
// departure this chain declares, and it is a departure in where the attenuation
// lands, never in how much the law may ask for.
//
// The two 941 numbers are the only DAR-sourced values: threshold -12.5 dBFS and
// maximum attenuation 9.0 dB. Their field roles are INFERRED (the blob proves
// the values and the 9.0-then--12.5 order, not which is which); the literal
// role swap produces no ducking at all on real programme, so this mapping is
// the one supported by the observed symptom.
inline constexpr DiracLimiter::Params kLevelStage = {
        -12.5f, 9.0f, 1.0f, 1.0e-4f, 64, true, 2000.0f, 0.0f,
        1000.0f, 1000.0f, true, 0.0f, false, 0u, 0u};

// The peak stage: the recovered pslimiter and the bound the chain ends in. A
// per-sample peak with a 10 ms hold, a 25 ms release pole, an instant attack, a
// 6 ms attack pole and 8 ms of look-ahead; the parameters are the recovered 503
// values, so the threshold is back at -0.5 dBFS with a 50 dB cap. The ceiling a
// hardBound stage applies caps the output sample peak at the ceiling that
// threshold computes, for any input below threshold + maxAttenuation. It reads
// the ceiling from the largest input inside the look-ahead, so the sample
// leaving the delay line is always covered.
//
// The threshold alone cannot bound the output: what the ceiling bounds is the
// sample peak, while a converter reconstructs an overshoot between those
// samples on broadband material. The ceiling therefore evaluates a true-peak
// estimate of the input, from a polyphase windowed-sinc detector (wide-Hann
// half-width 64, P fractional phases at p/P, truePeakTaps coefficients per
// phase (16), a kernel length of 2*truePeakTaps (32), every phase normalised to
// unit DC), and takes the largest |interpolated| value seen in the look-ahead.
//
// What the hard bound holds exactly, independent of any reconstruction kernel,
// is that sample-peak cap: on the 1440-record broadband stress the output
// sample peak measured at worst 0.944066, under the -0.5 dBFS ceiling. The
// output's true (inter-sample) peak is a kernel-dependent property of the gain
// path: the same stress cells measured about 0.964 to 1.046 at 16x depending on
// the reconstruction kernel, so it is not a single number and no true-peak
// guarantee is claimed. This detector is our own host-side departure from the
// blob, whose safelimiter/pslimiter carry no true-peak evaluation at all; the
// -0.5 dBFS ceiling is the owner requirement.
//
// The interpolator carries a group delay of about half its kernel length, so
// the ceiling sees the input about truePeakTaps samples late; the effective
// look-ahead margin is smaller than the full 8 ms. That is ample here, but a
// longer kernel eats into it, so the kernel length is bounded by the look-ahead
// as much as by CPU.
//
// The detector is also the chain's dominant cost, so the kernel is evaluated
// folded: the phases mirror each other, and a phase and its mirror are two
// half-kernel dot products over the sample sums and differences, so eight phases
// of a 16-tap kernel cost 112 multiply-accumulates per sample per channel
// instead of 224, with the same values. Sixteen taps is the longest kernel the
// 8 ms look-ahead allows, and a kernel short enough to need fewer over-reads the
// reconstruction - a truncated sinc ripples above unit gain near Nyquist - so a
// shorter kernel's ceiling engages on loud high frequencies where the 16-tap
// estimate would not. At 0 dBFS on the baked curve that over-reading costs, at
// worst over 13-20 kHz against 16 taps: 0.29 dB at 12 taps (chain 278.7 ns
// against 319.6), 0.79 dB at 8 (253.6), 1.52 dB at 6 (230.4), 1.30 dB at 4
// (214.1).
//
// Phases are the wrong lever for the inter-sample margin, because halving them
// raises the inter-sample peak instead. No kernel length is over-trigger-free:
// the ceiling legitimately attenuates a full-scale high-frequency sine by
// max(0, 20*log10(A/0.944)) where A is the amplitude arriving at this stage, so
// the margin stays empirical, at 16x and kernel-dependent, and none is claimed.
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
