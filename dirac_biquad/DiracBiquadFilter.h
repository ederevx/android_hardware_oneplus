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

#include <hardware/audio.h>

#include "Biquad.h"
#include "DiracBiquadTable.h"
#include "DiracLimiter.h"

// Host-side biquad curve with Dirac-QEM parity.
//
// The module exposes both a fixed low-level filter bank and a high-level
// seven-float band curve, and the QEM app drives only the curve; this
// calibration zeroes the bank, so the audible user EQ is the seven-band curve:
// seven peaking bands at 68/165/400/972/2000/6000/14000 Hz.
//
// The band shape is the Dirac equalizer's own model: the app's EqCurveView
// reduces each band to the small-signal limit of the RBJ peaking response, a
// rational Lorentzian in (cos w - cos w0), and builds it with
// alpha = sin(w0)/2. In RBJ terms alpha = sin(w0)/(2Q), so the Dirac bands are
// Q = 1.0, which is exactly what the chain below uses. It is therefore the
// same second-order peaking bank, at the same centres and the same Q, in the
// same low-to-high order.
//
// Not reproduced: the fixed filter bank the module also carries, whose data is
// not in any readable source here and which this calibration zeroes, and the
// module's input and output rumble high-passes, also zeroed, so this leg adds
// no high-pass either: a corner would be an invention, and faithfulness here
// means doing what this calibration does.
//
// The host chain ends in two limiter stages of the same kind, both owned by
// DiracLimiter: a level stage that clamps the low-frequency energy the curve
// boosts, and a peak stage after it that bounds what the level stage is too slow
// to see. Neither stage adds gain, and the peak stage keeps its output at or
// below its own threshold - with a 2x true-peak refinement of the input its
// ceiling bounds, so the reconstruction between samples is covered and not only
// the samples - so the chain needs no static headroom preamp, no clipper and no
// margin: whatever the signature and the curve ask for, nothing leaves above
// that stage's threshold.
//
// Before the user EQ the chain applies a fixed approximation of one Dirac
// signature: the FIR response of the hdsound slot 8 filter
// usecase/eheadset/hdsound-filters/09-Oneplus-Earphone_General_Bluetooth_
// 170928v02 in dirac_resource.dar, the OEM "Earphone General Bluetooth"
// earphone voicing. The design parameters live in DiracBiquadTable, one set
// per covered rate; that unit owns rate selection and its comment records the
// target, the objective and the measured per-rate accuracy. Only the filter's
// FIR magnitude is modelled; its IIR sections and the module's dynamics are
// not.
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

    // The two limiter stages the chain ends with, so a diagnostic can name what
    // bounds the output without restating the configuration.
    static const DiracLimiter::Params &LevelStage();
    static const DiracLimiter::Params &PeakStage();

    // Builds the curve for the stream rate and channel count from the half-dB
    // band gains. Returns false for an unsupported combination, in which case
    // the caller must pass the stream through untouched. Rates below 44100 Hz
    // are refused: the seven user EQ bands are designed with w0 = 2*pi*f/fs and
    // no clamp, so a band centre past Nyquist turns sin(w0) negative and the
    // peaking section's poles leave the unit circle (measured: divergence to
    // NaN within 37-145 ms at 8000-24000 Hz). 44100 itself is stable and the
    // lowest rate served, so the predicate is `sampleRateHz < 44100`.
    //
    // A successful call leaves the chain fresh: the coefficients are rebuilt and
    // the memories behind them are cleared, so a caller that reconfigures
    // mid-stream does not have the previous configuration ring into the new one.
    bool Configure(unsigned sampleRateHz, unsigned channelCount,
                   const int gainsHalfDb[kBandCount]);

    void Reset();

    // Applies the curve to an interleaved PCM buffer. `input` and `output` may
    // alias; `accumulate` adds the result to the existing output content, as
    // EFFECT_BUFFER_ACCESS_ACCUMULATE requires.
    void Process(const void *input, void *output, size_t frameCount, unsigned channelCount,
                 audio_format_t format, bool accumulate);

  private:
    float ProcessSample(float x, unsigned channel);

    Biquad stages_[kMaxChannels][kBandCount];
    Biquad signature_[kMaxChannels][kSignatureCount];
    DiracLimiter levelLimiter_;
    DiracLimiter peakLimiter_;
    unsigned channelCount_ = 0;
    bool configured_ = false;
};
