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

#include "DiracBandShaper.h"

#include <cmath>

#include "DiracChainStages.h"

void DiracBandShaper::Configure(unsigned sampleRateHz, unsigned channelCount,
                                const DiracLimiter::Params &law) {
    if (sampleRateHz == 0 || channelCount == 0 || channelCount > kMaxChannels) {
        configured_ = false;
        return;
    }

    sampleRateHz_ = sampleRateHz;
    channelCount_ = channelCount;
    // A section whose depth moved is moved to its new coefficients over this many
    // samples: 50 ms, the same fade the engagement uses.
    rampSamples_ = dirac_chain::FadeSamples(sampleRateHz);
    law_.Configure(sampleRateHz, channelCount, law);
    slewCoef_ = static_cast<float>(
            1.0 - std::exp(-1000.0 / (static_cast<double>(sampleRateHz) * dirac_chain::kFadeMs)));

    for (Section &section : sections_) {
        section.active = false;
        section.inPlan = false;
        section.reach = 0.0f;
        section.reachTarget = 0.0f;
        for (unsigned ch = 0; ch < kMaxChannels; ++ch) {
            section.running[ch] = false;
        }
    }
    adopted_ = DiracBandPlan{};
    flatCurrent_ = 1.0f;
    flatTarget_ = 1.0f;
    planWeight_ = 0.0f;
    planTarget_ = 0.0f;
    sectionCount_ = 0;
    configured_ = true;
}

bool DiracBandShaper::SameShape(const Section &section, const DiracBandPlan::Section &next,
                                unsigned stages) {
    return section.active && section.type == next.type && section.cornerHz == next.cornerHz &&
           section.q == next.q && section.stageCount == stages;
}

void DiracBandShaper::LoadDepth(Section &section, const DiracBandPlan::Section &next,
                                unsigned stages) {
    // The shape did not move, so only how deep it cuts did: the coefficients move
    // under the state, which is what a section continuing from the samples it has
    // already seen wants. They move over the ramp rather than in one step, because
    // a coefficient set changed under live state steps the output by the size of
    // the change - the same mechanism the curve path has - and the whole point of
    // matching by shape is that this path keeps cutting while it changes.
    for (unsigned ch = 0; ch < channelCount_; ++ch) {
        for (unsigned stage = 0; stage < stages; ++stage) {
            section.stage[ch][stage].LoadRamped(next.stage[stage], rampSamples_);
        }
    }
    section.stageCount = stages;
    section.capGain = next.capGain;
    section.depth = 1.0 - next.capGain;
    section.depthReciprocal = section.depth > 0.0 ? 1.0 / section.depth : 0.0;
}

void DiracBandShaper::LoadShape(Section &section, const DiracBandPlan::Section &next,
                                unsigned stages) {
    // A new shape starts transparent with cleared filters, so it fades in through
    // the slew instead of stepping the output or replaying the state left behind
    // by the previous one.
    for (unsigned ch = 0; ch < channelCount_; ++ch) {
        for (unsigned stage = 0; stage < stages; ++stage) {
            section.stage[ch][stage].Load(next.stage[stage]);
            section.stage[ch][stage].Reset();
        }
        section.running[ch] = false;
    }
    section.type = next.type;
    section.cornerHz = next.cornerHz;
    section.q = next.q;
    section.stageCount = stages;
    section.capGain = next.capGain;
    // Both are invariant for the adopted plan, so the per-sample engagement needs
    // neither the subtraction nor the division.
    section.depth = 1.0 - next.capGain;
    section.depthReciprocal = section.depth > 0.0 ? 1.0 / section.depth : 0.0;
    section.reach = 0.0f;
    section.active = true;
}

bool DiracBandShaper::AdoptPlan(const DiracBandPlan &plan) {
    if (!configured_ || !plan.valid || plan.sampleRateHz != sampleRateHz_) {
        return false;
    }
    // A plan this stage did not build could carry a non-finite coefficient, which
    // would persist in the filter state; refuse it rather than load it. The flat
    // gain and every designed depth multiply into the response, so they are held
    // to the same range the runtime can honour: a gain above unity would have the
    // stage add gain, and a depth outside (0, 1] is not a cut.
    if (!std::isfinite(plan.flatGain) || plan.flatGain <= 0.0f || plan.flatGain > 1.0f) {
        return false;
    }
    const size_t count =
            plan.sectionCount < kMaxPlanSections ? plan.sectionCount : kMaxPlanSections;
    for (size_t slot = 0; slot < count; ++slot) {
        const unsigned stages = plan.sections[slot].stageCount < kMaxStages
                                        ? plan.sections[slot].stageCount
                                        : kMaxStages;
        if (!std::isfinite(plan.sections[slot].capGain) || plan.sections[slot].capGain <= 0.0 ||
            plan.sections[slot].capGain > 1.0) {
            return false;
        }
        for (unsigned stage = 0; stage < stages; ++stage) {
            const BiquadCoefficients &coefficients = plan.sections[slot].stage[stage];
            if (!std::isfinite(coefficients.b0) || !std::isfinite(coefficients.b1) ||
                !std::isfinite(coefficients.b2) || !std::isfinite(coefficients.a1) ||
                !std::isfinite(coefficients.a2)) {
                return false;
            }
        }
    }

    // Match every plan slot to the slot that should carry it, in this order: the
    // slot already running this shape, wherever the plan happens to place it, so
    // a curve change that only moves depths keeps every filter and every
    // engagement; then an idle slot; then the least engaged slot, which is the
    // one whose cut is smallest and so the one whose replacement steps least.
    // The stage holds twice as many slots as a plan names, so a replaced shape
    // keeps cutting while it fades out instead of vanishing in one sample.
    bool taken[kMaxSections] = {};
    size_t chosen[kMaxPlanSections] = {};
    for (size_t slot = 0; slot < count; ++slot) {
        const DiracBandPlan::Section &next = plan.sections[slot];
        const unsigned stages =
                next.stageCount < kMaxStages ? next.stageCount : kMaxStages;
        size_t pick = kMaxSections;
        for (size_t candidate = 0; candidate < kMaxSections; ++candidate) {
            if (!taken[candidate] && SameShape(sections_[candidate], next, stages)) {
                pick = candidate;
                break;
            }
        }
        if (pick == kMaxSections) {
            for (size_t candidate = 0; candidate < kMaxSections; ++candidate) {
                if (!taken[candidate] && !sections_[candidate].active) {
                    pick = candidate;
                    break;
                }
            }
        }
        if (pick == kMaxSections) {
            for (size_t candidate = 0; candidate < kMaxSections; ++candidate) {
                if (taken[candidate] || !sections_[candidate].active) {
                    continue;
                }
                if (pick == kMaxSections || sections_[candidate].reach < sections_[pick].reach) {
                    pick = candidate;
                }
            }
        }
        chosen[slot] = pick;
        taken[pick] = true;
    }

    // Every slot the new plan does not name fades out through its own slew. A
    // slot the new plan does name keeps fading in from wherever its engagement
    // stands, so a shape adopted twice does not restart transparent.
    for (size_t slot = 0; slot < kMaxSections; ++slot) {
        Section &section = sections_[slot];
        if (!taken[slot]) {
            section.inPlan = false;
            section.reachTarget = 0.0f;
            continue;
        }
        section.inPlan = true;
    }
    for (size_t slot = 0; slot < count; ++slot) {
        const DiracBandPlan::Section &next = plan.sections[slot];
        const unsigned stages =
                next.stageCount < kMaxStages ? next.stageCount : kMaxStages;
        Section &section = sections_[chosen[slot]];
        if (SameShape(section, next, stages)) {
            LoadDepth(section, next, stages);
        } else {
            LoadShape(section, next, stages);
        }
        section.reachTarget = 1.0f;
    }

    // The plan is copied, never retained: the caller owns the source. The counts
    // are clamped where they are stored, not only where they are loaded, so a
    // hand-built plan cannot leave an out-of-range count for a later loop to
    // trust.
    adopted_ = plan;
    adopted_.sectionCount = static_cast<uint32_t>(count);
    for (size_t slot = 0; slot < count; ++slot) {
        if (adopted_.sections[slot].stageCount > kMaxStages) {
            adopted_.sections[slot].stageCount = kMaxStages;
        }
    }
    sectionCount_ = count;
    flatTarget_ = plan.flatGain;
    planTarget_ = 1.0f;
    return true;
}

void DiracBandShaper::ClearPlan() {
    // The law takes the whole signal back. The plan's weight is what fades, not
    // its flat gain: dropping the flat gain first would step the broadband gain
    // up to unity while the sections were still cutting. The weight carries the
    // whole contribution to zero, so the flat gain's own value stops mattering as
    // it does so, and the sections fade out through their own slew.
    adopted_ = DiracBandPlan{};
    sectionCount_ = 0;
    planTarget_ = 0.0f;
    for (Section &section : sections_) {
        section.inPlan = false;
        section.reachTarget = 0.0f;
    }
}

DiracBandShaper::PlanSlot DiracBandShaper::PlanAt(size_t slot) const {
    if (!adopted_.valid || slot >= kMaxPlanSections) {
        return PlanSlot{false, BiquadType::kPeaking, 0.0, 0.0, 0, 0.0};
    }
    const DiracBandPlan::Section &section = adopted_.sections[slot];
    return PlanSlot{slot < sectionCount_, section.type, section.cornerHz, section.q,
                    section.stageCount,
                    section.capGain > 0.0 ? 20.0 * std::log10(section.capGain) : -120.0};
}

size_t DiracBandShaper::EngagedSections() const {
    size_t count = 0;
    for (const Section &section : sections_) {
        if (section.active && section.reach > 0.0f && section.inPlan) {
            ++count;
        }
    }
    return count;
}

size_t DiracBandShaper::RetiringSections() const {
    size_t count = 0;
    for (const Section &section : sections_) {
        if (section.active && section.reach > 0.0f && !section.inPlan) {
            ++count;
        }
    }
    return count;
}

void DiracBandShaper::Reset() {
    for (Section &section : sections_) {
        for (unsigned ch = 0; ch < kMaxChannels; ++ch) {
            for (unsigned stage = 0; stage < kMaxStages; ++stage) {
                section.stage[ch][stage].Reset();
            }
            section.running[ch] = false;
        }
        section.reach = 0.0f;
    }
    // The law restarts at gain 1, where the stage is transparent whatever the
    // flat gain is, so the weights are taken straight to their targets: there is
    // no step to fade, and every section's engagement restarts and fades back in
    // rather than resuming at a weight the stream no longer earned.
    planWeight_ = planTarget_;
    flatCurrent_ = flatTarget_;
    law_.Reset();
}

void DiracBandShaper::BeginFrame() {
    if (!configured_) {
        return;
    }
    // A plan's flat gain and the plan's own weight are steps in the response, so
    // both are faded like the sections' engagement; the last step lands on the
    // target exactly rather than on a geometric residual.
    flatCurrent_ += slewCoef_ * (flatTarget_ - flatCurrent_);
    if (std::fabs(flatTarget_ - flatCurrent_) <= slewCoef_) {
        flatCurrent_ = flatTarget_;
    }
    planWeight_ += slewCoef_ * (planTarget_ - planWeight_);
    if (std::fabs(planTarget_ - planWeight_) <= slewCoef_) {
        planWeight_ = planTarget_;
    }
    for (Section &section : sections_) {
        if (!section.active) {
            continue;
        }
        if (section.reachTarget <= 0.0f && section.reach <= slewCoef_) {
            // A decaying engagement shrinks by a fraction of itself each frame, so
            // it stalls on a rounding residual instead of ever reaching zero
            // (measured: 2.8e-35 under the chain's flush-to-zero, which flushes the
            // step but not the accumulator). Retire the slot once the remaining
            // engagement is within one slew coefficient, and retire it before the
            // step so the change is at most that coefficient.
            section.reach = 0.0f;
            section.active = false;
            section.inPlan = false;
            continue;
        }
        section.reach += slewCoef_ * (section.reachTarget - section.reach);
        if (std::fabs(section.reachTarget - section.reach) <= slewCoef_) {
            section.reach = section.reachTarget;
        }
    }
}

void DiracBandShaper::ProcessFrame(float *frame, unsigned channels) {
    // A non-finite value would persist in the law's memories and in the
    // sections' states, exactly as it would in the chain's own sections.
    for (unsigned ch = 0; ch < channels; ++ch) {
        if (!std::isfinite(frame[ch])) {
            frame[ch] = 0.0f;
        }
    }
    if (!configured_) {
        return;
    }
    const unsigned count = channels < channelCount_ ? channels : channelCount_;

    // Every channel's law runs before any section does, so the engagement can be
    // read from the whole frame. The law's own output is exactly what the stage
    // emits while no plan is in force.
    float x[kMaxChannels];
    float gain[kMaxChannels];
    float linked = 1.0f;
    for (unsigned ch = 0; ch < count; ++ch) {
        x[ch] = frame[ch];
        frame[ch] = law_.ProcessSample(x[ch], ch);
        gain[ch] = law_.Gain(ch);
        if (gain[ch] < linked) {
            linked = gain[ch];
        }
    }
    if (!adopted_.valid && planWeight_ <= 0.0f) {
        return;
    }

    // The broadband gain the plan's weight blends against stays each channel's
    // own: it is the recovered law, and a real gain moves no phase.
    for (unsigned ch = 0; ch < count; ++ch) {
        const float flat = gain[ch] > flatCurrent_ ? gain[ch] : flatCurrent_;
        frame[ch] = x[ch] * (gain[ch] + planWeight_ * (flat - gain[ch]));
    }

    // A section's engagement is one value per frame, from the deepest attenuation
    // any channel's law asks for. Engaged per channel, two channels of one source
    // would run different blends of the same cut - a different phase and in-band
    // level per channel, modulated by the law's envelope - and a channel whose law
    // sat idle would hold its coefficient ramp frozen. Linked, every channel runs
    // the same filter, its filters start together and its ramps advance together;
    // with one channel the link is that channel's own law, so the mono result is
    // unchanged.
    for (Section &section : sections_) {
        if (!section.active) {
            continue;
        }
        float alpha = 0.0f;
        if (section.reach > 0.0f) {
            alpha = 1.0f;
            if (section.depth > 0.0) {
                alpha = static_cast<float>((1.0 - linked) * section.depthReciprocal);
                if (alpha > 1.0f) {
                    alpha = 1.0f;
                }
                if (alpha < 0.0f) {
                    alpha = 0.0f;
                }
            }
            alpha *= section.reach;
        }
        if (alpha <= 0.0f) {
            for (unsigned ch = 0; ch < count; ++ch) {
                section.running[ch] = false;
            }
            continue;
        }

        // A section that is starting to apply runs its filters from cleared
        // state: the state left behind by an earlier engagement is stale, and
        // replaying it would step the output by whatever it had decayed to.
        for (unsigned ch = 0; ch < count; ++ch) {
            if (!section.running[ch]) {
                for (unsigned stage = 0; stage < section.stageCount; ++stage) {
                    section.stage[ch][stage].Reset();
                }
                section.running[ch] = true;
            }
            float cut = frame[ch];
            for (unsigned stage = 0; stage < section.stageCount; ++stage) {
                cut = section.stage[ch][stage].ProcessSample(cut);
            }
            frame[ch] += alpha * (cut - frame[ch]);
        }
    }
}
