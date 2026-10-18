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
#include "DiracBandPlan.h"
#include "DiracLimiter.h"

// The chain's level stage, made band-selective: the recovered law runs
// unchanged, and the attenuation it asks for is applied only where the
// configured response violates the chain's magnitude limit.
//
// The law is the shipped level stage, untouched: the same detector on the same
// signal, the same constants, and so the same per-channel gain sequence it has
// always produced. What the stage owns is the actuator. The plan it applies is
// built elsewhere (DiracBandPlanner) from the chain's designed response; this
// class only turns that plan into samples.
//
// A section is applied as a crossfade between the signal and its own cut:
//
//     c = C(y);  alpha = min(1, (1 - g) / (1 - cap));  y += alpha * (c - y)
//
// with `g` the law's gain and C designed at the depth `cap` the band it bends
// needs. The plan's flat gain sits in front of the sections and the law's gain
// drives their engagement, so what a section reaches in band is the law's ask
// scaled by the section's own design - and the law's 9 dB cap bounds each
// actuator rather than their sum. `g` here is the frame's linked gain, the
// deepest attenuation any channel's law asks for, because a section blends the
// signal with its own cut and the blend's phase depends on that weight: engaged
// per channel, two channels of one source would run different filters and a
// correlated centre would lose its phase between them. The broadband gain in
// front of the sections remains each channel's own law gain, which moves no
// phase. Both consequences belong in the review: a band the plan cuts can be
// attenuated by dB(flat) + dB(cap), and while the law sits
// between idle and the flat gain a section's centre gets the law's ask twice in
// dB. That is the design position this chain declares, not an accident, because
// capping the sum leaves every curve whose designed response exceeds +9 dB
// anywhere unholdable by any plan at all. At gain 1 the section is transparent,
// and the expression is bit-exact identity there because the difference is
// multiplied by zero. The form needs no coefficient rebuild per sample, holds for
// any order and needs no factorisation of the cut, and never adds gain: every
// shape the planner designs is bounded real (|C| <= 1 at every frequency), so
// |1 - alpha + alpha C| <= 1.
//
// The flat gain the plan carries in front of the sections is the part of the
// excess the whole scanned band shares, and it is faded into place twice over:
// the plan as a whole has a weight that moves by one slew step per frame, so
// entering a plan from fallback never steps the output, and the flat gain itself
// is slewed towards the value the adopted plan asks for, so a plan change that
// moves only the flat gain does not step either. The plan's contribution is
// `gain + w * (flat - gain)`, which is the law's own gain at weight 0, so the
// stage is bit-identical to the law whenever no plan is in force. At an idle law
// the broadband gain is exactly 1 whatever the weight is.
//
// A section's engagement is slewed, and one whose shape changed restarts
// transparent with its filters cleared, so a live curve change neither steps the
// output nor replays a stale filter state. A section whose shape is unchanged
// and only whose depth moved keeps its filters, its state and its engagement:
// a curve change that moves a section's depth is a change of how much it cuts,
// not of what it is. A replaced shape keeps running in its own slot until its
// engagement reaches zero, so the cut it was applying is faded rather than
// dropped; the stage therefore holds twice as many slots as a plan can name.
// The law and every section keep their state across a plan change, because a
// new plan is a property of the response, not of the stream's past.
//
// The plan is a property of the designed response, not a copy of the law: where
// nothing in the scanned band violates the limit the plan names no section and
// the stage applies no attenuation at all, and where something violates the
// attenuation appears only around it. With no plan adopted at all the stage is
// the recovered level stage applied broadband, bit for bit the law's own output.
class DiracBandShaper {
  public:
    static constexpr unsigned kMaxChannels = 8;
    // A plan names at most this many sections; the stage holds two slots per
    // planned section, because a slot that is replaced keeps cutting while it
    // fades out.
    static constexpr size_t kMaxPlanSections = DiracBandPlan::kMaxSections;
    static constexpr size_t kMaxSections = 2 * kMaxPlanSections;
    static constexpr unsigned kMaxStages = DiracBandPlan::kMaxStages;

    // Builds the law and the engagement slew and leaves the stage with no plan:
    // the recovered level stage, applied broadband. Allocates nothing.
    void Configure(unsigned sampleRateHz, unsigned channelCount, const DiracLimiter::Params &law);

    // Adopts a plan built for this stage's rate. Returns false and changes
    // nothing on a rate mismatch, an invalid plan, a non-finite coefficient, or a
    // flat gain or designed depth outside (0, 1]. Every plan slot is matched to
    // the slot already carrying its shape, which keeps that slot's filters, state
    // and engagement; a slot left over fades out through its own slew. Sets the
    // flat gain and plan weight targets, which then fade in over the slew. No
    // allocation, no transcendental, no syscall.
    bool AdoptPlan(const DiracBandPlan &plan);

    // Drops the adopted plan and returns the stage to the recovered level stage
    // applied broadband: the plan's weight fades to zero, the sections fade out
    // through their own slew and the flat gain returns to unity. The law's and
    // the sections' state is kept, so an adoption that follows resumes from where
    // this left off.
    void ClearPlan();

    // Clears every memory - the law's and the sections' - and restarts the
    // sections transparent. The adopted plan survives: it is a property of the
    // configured response, not of the stream's past. Every engagement restarts,
    // because the law restarts at gain 1 and there is no step to fade.
    void Reset();

    // One frame of `channels` interleaved samples, in place. The caller must call
    // BeginFrame() first: the flat gain and the sections' engagement are advanced
    // there, so without it no section ever engages. DiracBiquadFilter::Process
    // does this; a direct caller must too. Every channel's law runs before any
    // section does, because the sections' engagement is taken from the deepest
    // attenuation the whole frame asks for. No allocation, no transcendental, no
    // syscall.
    void ProcessFrame(float *frame, unsigned channels);

    // Starts one frame: advances the flat gain and every section's engagement by
    // one slew step. The caller calls it once per frame, before the frame's
    // channels, so the fade is a fixed time whatever the channel count. No
    // allocation, no transcendental, no syscall.
    void BeginFrame();

    // Diagnostics, read from the adopted plan: how many violating intervals it
    // could not carry, and whether its own verification pass found a grid point
    // it does not hold at the limit with everything engaged. Zero and false with
    // no plan. A plan that does not hold its limit is still applied, on purpose:
    // it is the best the section budget can do, and the peak stage still bounds
    // the sample peak. The watcher logs it, and the dump reports it.
    size_t ResidualIntervals() const {
        return adopted_.valid ? static_cast<size_t>(adopted_.residualIntervals) : 0;
    }
    bool PlanHoldsLimit() const { return adopted_.valid && adopted_.holdsLimit; }
    bool HasPlan() const { return adopted_.valid; }

    // Diagnostic: the flat gain the adopted plan asks for, the one actually in
    // force this frame, and how far the plan as a whole has faded in, so a test
    // can tell a plan's target from its fade.
    float FlatGain() const { return adopted_.valid ? adopted_.flatGain : 1.0f; }
    float AppliedFlatGain() const { return flatCurrent_; }
    float PlanWeight() const { return planWeight_; }

    // Diagnostic: how many slots are engaged, and how many of those carry a
    // section of the adopted plan rather than one still fading out.
    size_t EngagedSections() const;
    size_t RetiringSections() const;

    // Diagnostic: one plan slot, so a test can assert what the plan built and
    // re-derive the response it was built for.
    struct PlanSlot {
        bool active;
        BiquadType type;
        double cornerHz;
        double q;
        unsigned order;  // stages: 1 for a peaking cut, 2 for a fourth-order shelf
        double depthDb;
    };
    PlanSlot PlanAt(size_t slot) const;

  private:
    // One bend section as the stage runs it: the per-channel filters, the shape
    // the plan named, and the slew that carries its engagement.
    struct Section {
        Biquad stage[kMaxChannels][kMaxStages];
        BiquadType type = BiquadType::kPeaking;
        double cornerHz = 0.0;
        double q = 1.0;
        unsigned stageCount = 0;
        double capGain = 1.0;  // the linear gain of the designed cut
        // 1 - capGain and its reciprocal, fixed at adoption: the engagement the
        // law asks for is (1 - g) * depthReciprocal, so no per-sample
        // subtraction or division is needed.
        double depth = 0.0;
        double depthReciprocal = 0.0;
        float reach = 0.0f;    // engagement, slewed from 0 to 1
        float reachTarget = 0.0f;
        // Whether the filters ran on the previous sample. The engagement is one
        // value for the frame, so the flag turns for every channel at once; it
        // stays per channel because the IIR state itself does.
        bool running[kMaxChannels] = {};
        bool active = false;
        // Whether the adopted plan still names this slot, which is what the
        // diagnostics report as engaged rather than retiring.
        bool inPlan = false;
    };

    // Whether a slot carries this plan section's shape: same type, corner, Q and
    // stage count, so only the depth moved and the filters can stay.
    static bool SameShape(const Section &section, const DiracBandPlan::Section &next,
                          unsigned stages);

    // Loads a plan section's shape into a slot. Keeps the filters and the
    // engagement and ramps the coefficients to their new values rather than
    // swapping them under the running state; the caller has already established
    // SameShape.
    void LoadDepth(Section &section, const DiracBandPlan::Section &next, unsigned stages);

    // Loads a plan section's shape into a slot from scratch: coefficients loaded,
    // every channel's filters cleared and its running flag dropped, and the
    // engagement restarted transparent.
    void LoadShape(Section &section, const DiracBandPlan::Section &next, unsigned stages);

    Section sections_[kMaxSections];
    DiracLimiter law_;
    DiracBandPlan adopted_;
    float flatCurrent_ = 1.0f;  // the flat gain in force
    float flatTarget_ = 1.0f;   // the adopted plan's flat gain
    float planWeight_ = 0.0f;   // how far the adopted plan has faded in
    float planTarget_ = 0.0f;
    float slewCoef_ = 0.0f;
    unsigned rampSamples_ = 0;  // samples a coefficient change is ramped over
    unsigned sampleRateHz_ = 0;
    unsigned channelCount_ = 0;
    size_t sectionCount_ = 0;
    bool configured_ = false;
};
