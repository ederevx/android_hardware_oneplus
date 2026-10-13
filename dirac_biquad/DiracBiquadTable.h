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

#include <stddef.h>

#include "Biquad.h"

// One design parameter set of the fixed Dirac signature.  `cornerHz`
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
// Target: the FIR magnitude response of the hdsound slot 8 filter,
// usecase/eheadset/hdsound-filters/09-Oneplus-Earphone_General_Bluetooth_
// 170928v02 in dirac_resource.dar, referenced to 1 kHz.  That is the OEM
// "Earphone General Bluetooth" earphone voicing: a broad "smile" that lifts
// the bass about +2.9 dB at 20 Hz, dips about -4 dB near 6 kHz and rises about
// +4.6 dB at 20 kHz, in place of the old defaults/941 fallback.  The DAR's
// eheadset hdsound-filters name table runs one entry ahead of its payload
// table, so tools/generate_signature.py resolves the key by the ProtoFilter's
// own stored identity.  The blob carries a real FIR for 44100 and 48000 Hz, so
// those two entries are fitted to their own target; it has no FIR for 88200 or
// 96000,
// whose entries reuse the 48000 prototype because the DAR design is an analog
// prototype sampled per rate.
//
// Objective: sixteen RBJ sections evaluated on the digital axis of each rate
// against that rate's own target, corners realised through the pre-warped
// bilinear transform of Biquad::SetPrototype (w0 = 2*atan(pi*cornerHz/fs)),
// with the corner ceiling per rate at the analog image of 0.45*fs.  The
// residual is the dB error over the 1201-point 20 Hz - 20 kHz log grid,
// weighted 1.0 below 16 kHz and 0.5 above, minimised by p-norm continuation
// with the 1 kHz level pinned.  Because the reference FIR is flat to DC, that
// grid is extended below 20 Hz by the f = 0 tap-sum anchor (the FIR tap sum
// re 1 kHz: +2.900 dB at 44100, +2.911 dB at 48000) plus sparse 0.5-20 Hz rows,
// all at weight 1.0; without them the lowest shelf extrapolates and the 48000
// fit ships a +10.5 dB DC gain the reference does not have.  The fit is
// warm-started from tools/warm_start_signature.json (the previous shipped
// tables).  Constraints: peaking Q below 3.5, |gain| below 15 dB, and one
// section per disjoint geometric band over [15 Hz, ceiling].
//
// Measured accuracy of the synthesis path, per covered rate, over the full
// 20 Hz - 20 kHz band (RMS / max dB), quoted from the rounded values that ship:
//   44100 Hz  20-16k 0.34 / 0.72 | 16-20k 0.58 / 1.13
//   48000 Hz  20-16k 0.32 / 0.75 | 16-20k 0.74 / 1.02
//   88200 Hz  and 96000 Hz have no DAR target of their own; they reuse the
//   48000 prototype, whose analog response the same transform reproduces to
//   better than 1e-9 dB.
// These are maxima from a 20001-point log grid; the worst error sits near
// 7.15 kHz at 44.1 kHz and 7.20 kHz at 48 kHz.
//
// Behaviour, per rate - impulse tail to -60 dB for the whole cascade, the worst
// pole modulus, and the worst node of the cascade (the output response peak):
//   44100 Hz  tail 1.77 ms  |p| 0.995102  worst node 1.95x
//   48000 Hz  tail 1.40 ms  |p| 0.996624  worst node 1.90x
//   88200 Hz  tail 1.34 ms  |p| 0.998161  worst node 1.62x
//   96000 Hz  tail 1.19 ms  |p| 0.998311  worst node 1.62x
// |p| is the guard metric the fit bounds, sqrt(|a2|); the true dominant root
// modulus is 0.998909 / 0.999248 / 0.999591 / 0.999624, a low-frequency real
// pole with negligible residue, so it is benign and the tail is really set by
// the time-constant/decay bound the fit applies.  DC sits +0.04 dB (44100) and
// -0.01 dB (48000) from the reference anchor, against +0.82 dB and +10.47 dB
// before this refit.
// The sections are stored in the canonical order of CanonicalOrder() in
// tools/generate_signature.py, a pure permutation that bounds every
// intermediate node below the output response peak (the worst intermediate
// prefix peak is 1.41x at 44.1 kHz and 1.44x at 48 kHz and above).
// Full-band group delay is -0.22..+0.19 ms at 44.1 kHz and -0.27..+0.18 ms at
// 48 kHz; the negative hump sits near 6 kHz.  The signature's own peak is 1.950
// at 44.1 kHz and 1.900 at 48 kHz.
//
// The fitted entries are reproducible from this repository with the committed
// generator and tools/warm_start_signature.json, run from the repository root
// (hardware/oneplus/dirac_biquad/tools/generate_signature.py, with --dar
// vendor/oneplus/msm8998-common/proprietary/vendor/lib/rfsa/adsp/
// dirac_resource.dar):
//   --rate 44100 --dc-option B --dc-anchor-points 1,2,5,10,20 --dc-weight 1.0
//   --warm-start hardware/oneplus/dirac_biquad/tools/warm_start_signature.json
//   --rate 48000 --dc-option B --dc-anchor-points 0.5,1,2,5,10,20
//   --dc-weight 1.0 (same --warm-start)
// The documented seeds with the DC option off (--rate 44100 --seed 9,
// --rate 48000 --seed 2) print the pre-refit entries instead.
class DiracBiquadTable {
  public:
    static constexpr size_t kSectionCount = 16;

    // Sections to realise at `sampleRateHz`: the rate's own entry when it is
    // covered, otherwise the nearest covered rate's prototype.
    static const DiracBiquadSection *Sections(int sampleRateHz);
};
