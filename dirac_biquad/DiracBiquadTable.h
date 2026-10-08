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

#include <stddef.h>

#include "Biquad.h"

// One design parameter set of the fixed Dirac flat-EQ signature.  `cornerHz`
// is an analog prototype corner and `q` is the peaking Q, or the shelf slope S
// for the shelving types; Biquad::SetPrototype turns the triple into the
// coefficients of the stream rate.
struct DiracBiquadSection {
    BiquadType type;
    double cornerHz;
    double q;
    double gainDb;
};

// Per-rate signature tables, and the single owner of rate selection.
//
// Target: the flat-EQ ("defaults/941") magnitude response of
// usecase/eheadset/defaults/941 in dirac_resource.dar, referenced to 1 kHz.
// That blob carries a real FIR for 44100 and 48000 Hz, so those two entries are
// fitted to their own target; it has no FIR for 88200 or 96000, whose entries
// reuse the 48000 prototype because the DAR design is an analog prototype
// sampled per rate, so its 48 kHz prototype already describes them.
//
// Objective: sixteen RBJ sections evaluated on the digital axis of each rate
// against that rate's own target over the full 20 Hz - 20 kHz band, corners
// realised through the pre-warped bilinear transform of Biquad::SetPrototype
// (w0 = 2*atan(pi*cornerHz/fs)), with the corner ceiling per rate at the analog
// image of 0.45*fs so the top sections can follow the near-Nyquist cliff.  The
// residual is the dB error over a 1201-point log grid, weighted 1.0 below
// 16 kHz and 0.5 above, minimised by p-norm continuation with the 1 kHz level
// pinned.  Constraints: peaking Q below 3.5, |gain| below 15 dB, and one
// section per disjoint geometric band over [15 Hz, ceiling], which holds
// adjacent corners apart by the band ratio (>= 1.7x) so no pair can cancel.
// tools/generate_signature.py reproduces these tables from the DAR blob.
//
// Measured accuracy of the synthesis path, per covered rate, over the full
// 20 Hz - 20 kHz band (RMS / max dB), quoted from the rounded values that ship:
//   44100 Hz  20-16k 0.48 / 0.89 | 16-20k 1.03 / 2.03
//   48000 Hz  20-16k 0.33 / 0.81 | 16-20k 0.62 / 1.09
//   88200 Hz  and 96000 Hz have no DAR target of their own; they reuse the
//   48000 prototype, whose analog response the same transform reproduces to
//   better than 1e-9 dB.
// These are maxima from a 20001-point log grid; the 44.1 kHz 20-16k peak is a
// plateau of near-equal lobes near 4.9 and 15.7 kHz, so its peak and argmax
// move by a few hundredths of a dB with the evaluation grid.
//
// Behaviour, per rate - impulse tail to -60 dB for the whole cascade, the worst
// pole modulus, and the worst intermediate node of the cascade:
//   44100 Hz  tail 3.65 ms  |p| 0.994396  worst node 1.23x
//   48000 Hz  tail 3.06 ms  |p| 0.994999  worst node 1.21x
//   88200 Hz  tail 2.59 ms  |p| 0.997275  worst node 1.21x
//   96000 Hz  tail 2.58 ms  |p| 0.997497  worst node 1.21x
// |p| is the guard metric the fit bounds, sqrt(|a2|); the true dominant root
// modulus is 0.999524 / 0.999511 / 0.999734 / 0.999756, a low-frequency real
// pole with negligible residue, so it is benign and the tail is really set by
// the time-constant/decay bound the fit applies.
// The sections are stored in the canonical order of CanonicalOrder() in
// tools/generate_signature.py, a pure permutation that keeps every intermediate
// node below the output response peak; the reusable guard makes it ~1.2x.
// Full-band group delay is -3.73..+0.42 ms at 44.1 kHz and -6.28..+0.40 ms at
// 48 kHz; the negative hump sits at 20 Hz.  The realised flat-EQ peak is
// 0.9988 at every rate.
//
// Every figure above is reproducible from this repository: the committed
// generator rebuilds both fitted entries exactly from the vendor blob with
//   python3 tools/generate_signature.py --rate 44100 --seed 3
//   python3 tools/generate_signature.py --rate 48000 --seed 6
class DiracBiquadTable {
  public:
    static constexpr size_t kSectionCount = 16;

    // Sections to realise at `sampleRateHz`: the rate's own entry when it is
    // covered, otherwise the nearest covered rate's prototype.
    static const DiracBiquadSection *Sections(int sampleRateHz);
};
