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

#include <hardware/audio.h>

#include "Biquad.h"
#include "DiracBiquadTable.h"
#include "LoudnessTilt.h"
#include "LowBandMono.h"
#include "SoftClip.h"
#include "StereoWidth.h"

// Host-side biquad curve with Dirac-QEM parity.
//
// The Dirac CAPIv2 module (0x12d00 ipowersound / 0x12d01 eheadset) exposes two
// independent EQ surfaces: a low-level 80-byte bank (param 0x12d00, ten
// filters of {enable, fchz, gaindb, q}) and a high-level seven-float band
// curve (param 0x12d36). The QEM app drives only 0x12d36, and this ROM's ACDB
// defaults the bank to all-zero, so the audible user EQ is the seven-band
// curve: seven peaking bands at 68/165/400/972/2000/6000/14000 Hz.
//
// The band shape is the Dirac equalizer's own model: the app's EqCurveView
// reduces each band to the small-signal limit of the RBJ peaking response, a
// rational Lorentzian in (cos w - cos w0), and builds it with
// alpha = sin(w0)/2. In RBJ terms alpha = sin(w0)/(2Q), so the Dirac bands are
// Q = 1.0, which is exactly what the chain below uses. It is therefore the
// same second-order peaking bank, at the same centres and the same Q, in the
// same low-to-high order.
//
// Not reproduced, and not observable from the shipped blobs: the 10-filter
// bank (the DAR device correction and the stock filter presets, zeroed in
// this MTP cal set), the module's Input/Output HP fchz rumble filters (also
// zeroed), the HDSOUND filter index (its data lives in diracvdd.bin), and the
// pslimiter/safelimiter/timedomainlimiter chain. The preamp below is a static
// headroom stand-in for those limiters, not a limiter.
//
// One stage stands in for what this leg cannot have: the clamp became SoftClip,
// the safelimiter's stand-in, so the cascade's overshoot can no longer modulate
// the whole band. The module's own input and output rumble filters ("Input HP
// fchz" / "Output HP fchz", named in libdirac-capiv2.so) are zeroed in this cal
// set, so this leg adds no high-pass either: a corner would be an invention,
// and faithfulness here means doing what the OEM's own calibration does.
//
// Before the user EQ the chain applies a fixed approximation of one Dirac
// signature: the FIR response of the hdsound slot 8 filter
// usecase/eheadset/hdsound-filters/09-Oneplus-Earphone_General_Bluetooth_
// 170928v02 in dirac_resource.dar, the OEM "Earphone General Bluetooth"
// earphone voicing. The design parameters live in DiracBiquadTable, one set
// per covered rate; that unit owns rate selection and its comment records the
// target, the objective and the measured per-rate accuracy. Only the filter's
// FIR magnitude is modelled; its IIR sections and the module's dynamics are
// not, exactly as for the earlier defaults/941 target.
//
// After the signature and the user EQ, a volume-linked loudness tilt contours
// the output by the stream attenuation the app publishes. It is applied before
// the preamp and is deliberately excluded from the preamp probe, so the static
// headroom stays a property of the signature and EQ alone. The tilt belongs to
// LoudnessTilt, which owns its law, shelves and caps.
//
// The module's cross-channel `noise` Sum/Diff behaviour is not expressible by
// the per-channel cascade; LowBandMono restores its low-frequency half by
// high-passing the side signal, folding the low band to mono, and StereoWidth
// applies the widening half as a final side gain. Both act on the mid/side
// pair only, so the signature, EQ and tilt keep their exact per-channel
// response: the mid path is untouched and the widening scales only the side.
class DiracBiquadFilter {
  public:
    static constexpr size_t kBandCount = 7;

    static constexpr double kBandCenterHz[kBandCount] = {
            68.0, 165.0, 400.0, 972.0, 2000.0, 6000.0, 14000.0};

    static constexpr unsigned kMaxChannels = 8;

    // Fixed Dirac signature sections. The parameters come from
    // DiracBiquadTable, which selects them for the stream rate; the
    // coefficients are rebuilt for that rate in Configure.
    static constexpr size_t kSignatureCount = DiracBiquadTable::kSectionCount;

    // Builds the curve for the stream rate and channel count from the half-dB
    // band gains. Returns false for an unsupported combination, in which case
    // the caller must pass the stream through untouched.
    bool Configure(unsigned sampleRateHz, unsigned channelCount,
                   const int gainsHalfDb[kBandCount]);

    // Drive for the volume-linked loudness tilt. Forwarded to LoudnessTilt;
    // the preamp probe never sees it.
    void SetAttenuationDb(double attenuationDb);

    // Enables the final mid/side widening. `sumDiff` is the width in 0..1; it
    // is a pure side gain applied after every filter stage, so it never changes
    // the cascade's per-channel response or group delay.
    void SetSumDiff(float sumDiff);

    void Reset();

    // Applies the curve to an interleaved PCM buffer. `input` and `output` may
    // alias; `accumulate` adds the result to the existing output content, as
    // EFFECT_BUFFER_ACCESS_ACCUMULATE requires.
    void Process(const void *input, void *output, size_t frameCount, unsigned channelCount,
                 audio_format_t format, bool accumulate);

  private:
    float ProcessSample(float x, unsigned channel);

    // |H(f)| of the signature and EQ cascade on channel 0, whose coefficients
    // every other channel shares.
    double CascadeMagnitudeAt(double sampleRateHz, double frequencyHz) const;

    // Unity, or the attenuation that keeps the true cascade peak at 0 dBFS.
    float ComputePreampGain(unsigned sampleRateHz) const;

    Biquad stages_[kMaxChannels][kBandCount];
    Biquad signature_[kMaxChannels][kSignatureCount];
    LoudnessTilt tilt_;
    LowBandMono mono_;
    StereoWidth width_;
    SoftClip clip_;
    float preampGain_ = 1.0f;
    unsigned channelCount_ = 0;
    bool configured_ = false;
};
