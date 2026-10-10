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
// "Earphone General Bluetooth" earphone voicing, near flat with a presence
// lift, in place of the old defaults/941 fallback.  The DAR's eheadset
// hdsound-filters name table runs one entry ahead of its payload table, so
// tools/generate_signature.py resolves the key by the ProtoFilter's own stored
// identity.  The blob carries a real FIR for 44100 and 48000 Hz, so those two
// entries are fitted to their own target; it has no FIR for 88200 or 96000,
// whose entries reuse the 48000 prototype because the DAR design is an analog
// prototype sampled per rate.
//
// Objective: sixteen RBJ sections evaluated on the digital axis of each rate
// against that rate's own target over the full 20 Hz - 20 kHz band, corners
// realised through the pre-warped bilinear transform of Biquad::SetPrototype
// (w0 = 2*atan(pi*cornerHz/fs)), with the corner ceiling per rate at the analog
// image of 0.45*fs.  The residual is the dB error over a 1201-point log grid,
// weighted 1.0 below 16 kHz and 0.5 above, minimised by p-norm continuation
// with the 1 kHz level pinned.  Constraints: peaking Q below 3.5, |gain| below
// 15 dB, and one section per disjoint geometric band over [15 Hz, ceiling].
// tools/generate_signature.py reproduces these tables from the DAR blob.
//
// Measured accuracy of the synthesis path, per covered rate, over the full
// 20 Hz - 20 kHz band (RMS / max dB), quoted from the rounded values that ship:
//   44100 Hz  20-16k 0.35 / 0.72 | 16-20k 0.67 / 1.18
//   48000 Hz  20-16k 0.35 / 0.75 | 16-20k 0.76 / 1.03
//   88200 Hz  and 96000 Hz have no DAR target of their own; they reuse the
//   48000 prototype, whose analog response the same transform reproduces to
//   better than 1e-9 dB.
// These are maxima from a 20001-point log grid; the in-band peak sits near
// 8.0 kHz at 44.1 kHz and 7.2 kHz at 48 kHz.
//
// Behaviour, per rate - impulse tail to -60 dB for the whole cascade, the worst
// pole modulus, and the worst intermediate node of the cascade:
//   44100 Hz  tail 1.77 ms  |p| 0.995140  worst node 1.96x
//   48000 Hz  tail 1.81 ms  |p| 0.997249  worst node 1.81x
//   88200 Hz  tail 1.21 ms  |p| 0.998502  worst node 1.61x
//   96000 Hz  tail 1.21 ms  |p| 0.998624  worst node 1.61x
// |p| is the guard metric the fit bounds, sqrt(|a2|); the true dominant root
// modulus is 0.998926 / 0.999517 / 0.999737 / 0.999758, a low-frequency real
// pole with negligible residue, so it is benign and the tail is really set by
// the time-constant/decay bound the fit applies.
// The sections are stored in the canonical order of CanonicalOrder() in
// tools/generate_signature.py, a pure permutation that keeps the worst
// intermediate node at the output response peak.
// Full-band group delay is -0.89..+0.27 ms at 44.1 kHz and -1.85..+0.15 ms at
// 48 kHz; the negative hump sits at 20 Hz.  The signature's own peak is 1.958 at
// 44.1 kHz and 1.812 at 48 kHz, so the preamp probe attenuates -5.85 / -5.17 dB
// and the realised flat-EQ peak is 0.9988 at every rate.
//
// Every figure above is reproducible from this repository: the committed
// generator rebuilds both fitted entries exactly from the vendor blob with
//   python3 tools/generate_signature.py --rate 44100 --seed 9
//   python3 tools/generate_signature.py --rate 48000 --seed 2
class DiracBiquadTable {
  public:
    static constexpr size_t kSectionCount = 16;

    // Sections to realise at `sampleRateHz`: the rate's own entry when it is
    // covered, otherwise the nearest covered rate's prototype.
    static const DiracBiquadSection *Sections(int sampleRateHz);
};
