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
#include "DiracBandPlan.h"
#include "DiracBandShaper.h"
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
// Q = 1.0. DiracCurveDesign owns that construction, for this filter and for the
// plan builder, so the response the chain runs and the response a plan is built
// from are one thing.
//
// Not reproduced: the fixed filter bank the module also carries, whose data is
// not in any readable source here and which this calibration zeroes, and the
// module's input and output rumble high-passes, also zeroed, so this leg adds
// no high-pass either: a corner would be an invention, and faithfulness here
// means doing what this calibration does.
//
// The plan the level stage applies is built by DiracBandPlanner on the watcher
// thread from that designed response; this filter only builds the curve and
// adopts a published plan when its key matches the rate and gains applied here.
//
// The host chain ends in two stages. The first is the band shaper
// (DiracBandShaper): it runs the recovered level law on the low band alone, so
// the attenuation the law asks for bends only the band that violates the
// magnitude limit instead of ducking the midrange, which this voicing already
// holds below unity. The second is a broadband peak stage (DiracLimiter) that
// bounds what the shaper is too slow to see. Neither stage adds gain, so the
// chain needs no static headroom preamp and no clipper. The peak stage's hard
// bound caps its output sample peak at the ceiling that threshold computes, for
// any input up to 50 dB above the threshold and independent of any
// reconstruction kernel, within one float rounding of that ceiling (measured:
// 0.944066346 against a ceiling of 0.9440662861); its output true
// (inter-sample) peak is a kernel-dependent property of the gain path, about
// 0.97 at 16x over the trim's own stress set and higher under longer
// reconstruction kernels, so no true-peak guarantee is claimed.
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

    static constexpr unsigned kMaxChannels = 8;

    // Fixed Dirac signature sections. The parameters come from
    // DiracBiquadTable, which selects them for the stream rate; the
    // coefficients are rebuilt for that rate in Configure.
    static constexpr size_t kSignatureCount = DiracBiquadTable::kSectionCount;

    // The law the band shaper runs and the peak stage the chain ends in, so a
    // diagnostic can name what bounds the output without restating the
    // configuration.
    static const DiracLimiter::Params &LevelStage();
    static const DiracLimiter::Params &PeakStage();

    // Diagnostics for the bend plan the shaper has adopted: how many violating
    // intervals it could not carry, and whether its own verification pass found a
    // point it does not hold at the magnitude limit. With no plan the stage is
    // the recovered level stage and nothing holds the limit.
    size_t BendResidualIntervals() const { return bandShaper_.ResidualIntervals(); }
    bool BendPlanHoldsLimit() const { return bandShaper_.PlanHoldsLimit(); }

    // Adopts a bend plan, but only when it was built for the rate and the gains
    // this filter has actually applied: the filter is the owner of that state and
    // refuses a mismatch, so an adopted plan can never disagree with it. Copies
    // the plan on success, changes nothing and returns false otherwise.
    bool AdoptBendPlan(const DiracBandPlan &plan);
    bool HasBendPlan() const { return bandShaper_.HasPlan(); }

    // Drops the adopted plan: the chain returns to the recovered level stage
    // applied broadband, and the sections fade out through their own slew. A plan
    // that cannot be adopted is dropped rather than allowed to hold the curve
    // back, so a user's gains always reach the audio path.
    void ClearBendPlan() { bandShaper_.ClearPlan(); }

    // Diagnostics: the flat gain the adopted plan asks for and the one in force
    // this frame, and how many of the shaper's slots are engaged or still fading
    // out, so a dump can tell a plan's target from its fade.
    float BendFlatGain() const { return bandShaper_.FlatGain(); }
    float BendAppliedFlatGain() const { return bandShaper_.AppliedFlatGain(); }
    size_t BendEngagedSections() const { return bandShaper_.EngagedSections(); }
    size_t BendRetiringSections() const { return bandShaper_.RetiringSections(); }

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

    // Rebuilds only the seven user EQ sections for the configured stream from
    // coefficients designed on the watcher thread, so a live curve change keeps
    // the signature and both limiter stages (and all of their state) untouched,
    // and ramps the changed bands to their new coefficients over 50 ms instead
    // of swapping them under live filter state. Returns false when the chain is
    // not configured. Allocates nothing.
    bool UpdateGains(const int gainsHalfDb[kBandCount],
                     const BiquadCoefficients bands[kBandCount]);

    void Reset();

    // The effect reads this instead of keeping its own copy of that state.
    bool Configured() const { return configured_; }

    // Fades the chain's output back to the dry input from wherever the fade
    // stands; Engage turns a fade-out around. Neither touches filter state.
    void Disengage() { fadeIn_ = false; }
    void Engage() { fadeIn_ = true; }

    // The output is the dry input exactly, so the caller may switch to its own
    // pass-through.
    bool Disengaged() const { return !fadeIn_ && engageDelay_ == 0 && fadePosition_ == 0; }

    // Diagnostics for the effect dump.
    unsigned NonFiniteFlushes() const { return nonFiniteFlushes_; }
    unsigned FadePosition() const { return fadePosition_; }
    bool FadeIn() const { return fadeIn_; }

    // Applies the curve to an interleaved PCM buffer. `input` and `output` may
    // alias; `accumulate` adds the result to the existing output content, as
    // EFFECT_BUFFER_ACCESS_ACCUMULATE requires.
    void Process(const void *input, void *output, size_t frameCount, unsigned channelCount,
                 audio_format_t format, bool accumulate);

  private:
    // Defined in the .cpp, where Process instantiates it, so no format policy is
    // visible in this header.
    template <typename F>
    void ProcessFrames(const typename F::Sample *in, typename F::Sample *out, size_t frameCount,
                       unsigned channelCount, bool accumulate);

    float ProcessSample(float x, unsigned channel);

    // The dry/wet fraction for one frame of the engage crossfade, advanced once
    // per frame. Returns 1 once the fade is over, and 0 while the look-ahead
    // still has to fill.
    float EngageWet();

    // Clears the two biquad banks of one channel, leaving the limiters alone.
    void ResetBiquads(unsigned channel);

    // Loads the seven user EQ sections from coefficients designed elsewhere,
    // moving each running section to its new coefficients over `rampSamples`
    // samples, or immediately when that is zero.
    void SetUserBands(const int gainsHalfDb[kBandCount],
                      const BiquadCoefficients bands[kBandCount], unsigned rampSamples);

    // Chooses which bands take the identity fast path. Called only from
    // Configure, immediately after SetUserBands has loaded every section from the
    // incoming gains (that load is a fresh one, rampSamples 0, so no ramp is left
    // in flight) and BEFORE the Reset at the end of Configure - the predicate
    // reads only coefficients and ramp counters, and every memory is cleared a few
    // lines later, before any sample runs. A band the incoming gains hold at 0 dB
    // therefore carries the identity set with x = y = 0 exactly. Everywhere else a
    // band may only LEAVE the path: a promotion to a full section is permanent,
    // and a band that is ramped down live keeps the full recursion, because it
    // still carries the residue of the coefficients it was running with in y,
    // which only the recursion decays.
    void LatchIdentityBands(const int gainsHalfDb[kBandCount]);

    // Every band is always in the cascade; this says which of them are the
    // identity set and take the fast path. A band is never removed, so no live
    // curve change can shrink anything.
    bool identityPath_[kBandCount] = {};

    Biquad stages_[kMaxChannels][kBandCount];
    unsigned sampleRateHz_ = 0;
    // The ramp a live curve change is applied over: 50 ms of stream, the same
    // fade the shaper's engagement uses.
    unsigned rampSamples_ = 0;
    // The engage crossfade: after a Reset the chain re-enters a stream that was
    // going out unprocessed, so the output stays dry while the peak stage's
    // look-ahead fills and then ramps to the chain's own output over
    // rampSamples_. A fade can also be reversed by Disengage, so the position is
    // kept and moved in whichever direction is in force. Both are zero before
    // Configure and stay zero on a stream that never re-engaged, so a fully wet
    // frame is bit-identical to today's output.
    unsigned engageDelay_ = 0;
    unsigned fadePosition_ = 0;
    bool fadeIn_ = true;
    // Count of non-finite input or cascade samples flushed since the last
    // Init, so the effect dump can tell whether the emergency path ran.
    unsigned nonFiniteFlushes_ = 0;
    Biquad signature_[kMaxChannels][kSignatureCount];
    // The gains this filter has actually applied, the other half of the key a
    // bend plan is adopted against.
    int appliedGainsHalfDb_[kBandCount] = {};
    DiracBandShaper bandShaper_;
    DiracLimiter peakLimiter_;
    unsigned channelCount_ = 0;
    bool configured_ = false;
};
