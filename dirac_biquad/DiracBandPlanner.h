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
#include <stdint.h>

#include "Biquad.h"
#include "DiracBandPlan.h"

// Builds the bend plan for one stream rate and one set of band gains from the
// chain's designed response: the worst point still over the magnitude limit
// takes a section sized to carry the run of frequencies around it that are
// within a few dB of it - its core - and the next worst takes the next section,
// until the section budget runs out or nothing is left over the limit. A core
// that reaches an end of the scanned band is carried by a fourth-order
// Butterworth shelf cornered at the edge it does not violate, and any other core
// by a peaking cut at its own worst point. Sizing a section on its core, and not
// on the violation it lies in, is what keeps the shape that carries a violation's
// worst point from over-cutting that violation's tails.
//
// The design depth is the shallowest one that still brings its own core to the
// limit, found by regula falsi on the core's excess, because a deeper cut than
// the violation needs is exactly what leaks into the bands beyond it. Where the
// whole scanned band is over the limit, the part of the excess common to all of
// it is taken as a flat gain, and the sections are planned against what is left.
//
// The planner is stateless apart from its own scratch, which is fixed-size
// members sized by compile-time constants: it allocates nothing and never keeps a
// previous plan of its own, so the plan it builds is a function of the sample
// rate and the band gains alone and one curve always plans the same way. It is
// meant to run off the audio thread.
class DiracBandPlanner {
  public:
    static constexpr size_t kMaxGridPoints = 512;
    static constexpr size_t kMaxCheckPoints = 2048;

    // Builds the plan for `sampleRateHz` and `gainsHalfDb`, writing `out`
    // completely: a plan is a function of those two alone. Allocates nothing;
    // every loop has a fixed iteration cap.
    void Build(unsigned sampleRateHz, const int32_t gainsHalfDb[DiracBandPlan::kBandCount],
               DiracBandPlan *out);

  private:
    // One bend section while the plan is being built: the run of grid points it
    // carries, the cut it applies, the depth that cut was designed at, and the
    // coefficients of each of its stages. Per-stage coefficients, not a Biquad
    // per channel: a plan never needs state, and the peel's locals then stay
    // small.
    struct Shape {
        BiquadType type = BiquadType::kPeaking;
        double cornerHz = 0.0;
        double q = 1.0;
        unsigned stageCount = 0;
        double capGain = 1.0;  // the linear gain of the designed cut
        size_t coreFirst = 0;  // the grid points this section carries
        size_t coreLast = 0;
        size_t peelOrder = 0;  // the placement order, the sort's last key
        BiquadCoefficients stage[DiracBandPlan::kMaxStages];
    };

    // The kinds of shape a core can be carried by. A peaking cut is offered for
    // every core; a shelf only where the core reaches an end of the scanned band.
    enum class BendKind {
        kPeaking,
        kLowShelf,
        kHighShelf,
    };

    // One shape weighed for a core: what it is, and what it costs.
    struct Candidate {
        BiquadType type = BiquadType::kPeaking;
        double cornerHz = 0.0;
        double q = 1.0;
        unsigned stageCount = 0;
        double depthDb = 0.0;
        bool reached = false;    // whether any depth the law can ask for carries the core
        double overcutDb = 0.0;  // the worst cut beyond what a point's own excess asks for
    };

    // Fills one grid's points and magnitudes from the designed response, and
    // returns its point count clamped to `capacity`.
    size_t FillGrid(BiquadPoint *points, double *magnitude, double *hz, size_t capacity,
                    double pointsPerOctave, double lowOmega, double highOmega, double rateOverPi,
                    const Biquad *signature, const Biquad *bands);

    // The residual excess of the running composite at one plan grid point, and at
    // one check grid point, in dB above the target.
    double ResidualDb(size_t index, double target) const;
    double CheckResidualDb(size_t index, double target) const;

    // The contiguous run of plan grid points around `peak` whose residual is at
    // least half the peak's own excess: the violation the next section must
    // carry. An RBJ section's bandwidth is defined between its midpoint-gain
    // points, so a core that is half the excess is the width the shape itself
    // asks for, and it widens as the hump deepens.
    void CoreAround(size_t peak, double target, size_t *first, size_t *last) const;

    // A section's own magnitude at one grid point, as the cascade of its stages.
    double ShapeMagnitude(const Shape &shape, const BiquadPoint &point) const;

    // The worst excess the candidate leaves over its core, in dB above the target,
    // with the running composite (which excludes it) in place.
    double CoreExcessDb(const Shape &shape, size_t first, size_t last, double target) const;

    // The worst cut the candidate applies beyond what a check grid point's own
    // excess asks for.
    double OverCutDb(const Shape &shape, double target) const;

    // Shapes one candidate of a kind, or reports that the kind does not suit this
    // core.
    bool ShapeForKind(Shape &shape, BendKind kind, size_t first, size_t last) const;

    // Weighs one built candidate: sizes its depth to carry its core, and measures
    // what it over-cuts.
    Candidate WeighCandidate(Shape &shape, size_t first, size_t last, double target);

    // Finds the shallowest depth at which the candidate carries its core by
    // regula falsi on the core's excess, which is monotone in the depth, and
    // reports whether any depth the law can ask for reaches it.
    bool SizeForCore(Shape &shape, size_t first, size_t last, double target);

    // Builds this section's stages for the given depth.
    void BuildShape(Shape &shape, double depthDb);

    // Multiplies both composites by a placed section's response, or divides them
    // back out for the coordinate pass and the repair.
    void ScaleComposite(const Shape &shape, bool divide);

    // Replaces one section's response in both composites: it divides out the shape
    // the section had and multiplies in the shape it now has, so the repair pass
    // reads the sections as they stand after every deepening it makes.
    void ReplaceResponse(const Shape &before, const Shape &after);

    // Re-measures every adopted depth once with the other sections in place, so a
    // section another one already covers is not left deeper than it needs.
    void RefineDepths(Shape *rebuilt, size_t count, double target);

    // Deepens one section until it carries one check grid point, given the
    // composite without it, and never makes it shallower than it already is;
    // false when no depth the law can ask for does.
    bool DeepenForPoint(Shape &shape, size_t index, double without, double target);

    // Carries every check grid point still over the target with the section that
    // already cuts it most. Deepening a section only ever lowers a composite, so
    // this pass cannot uncover a point that another section held.
    void CoverResidual(Shape *rebuilt, size_t count, double target);

    // The number of contiguous runs of check grid points still over the target by
    // more than the certificate's own tolerance. The composites it reads are kept
    // current by ReplaceResponse as the repair moves sections, so the count
    // describes the plan that ships and agrees with the certificate.
    size_t UncoveredRuns(double target) const;

    // Fills both composites from the chain's response and the flat gain, as
    // BuildPlan leaves them before a section is placed.
    void FillComposites();

    // The worst composite the plan can deliver over the check grid, with the flat
    // gain applied and every section fully engaged. It is the plan's certificate,
    // and what the flat gain deepens against.
    double CheckWorstComposite() const;

    // Builds the sections by repeatedly carrying the worst violation the running
    // composite leaves, each sized on its own core, then refines and repairs them.
    void BuildPlan(double target);

    BiquadPoint points_[kMaxGridPoints] = {};
    double composite_[kMaxGridPoints] = {};
    BiquadPoint checkPoints_[kMaxCheckPoints] = {};
    double checkComposite_[kMaxCheckPoints] = {};
    double gridHz_[kMaxGridPoints] = {};
    double gridMagnitude_[kMaxGridPoints] = {};
    double checkMagnitude_[kMaxCheckPoints] = {};
    // The plan the peel last built, read by the certificate and by the next flat
    // gain attempt.
    Shape built_[DiracBandPlan::kMaxSections];
    size_t gridPoints_ = 0;
    size_t checkPointCount_ = 0;
    size_t builtCount_ = 0;
    float lawFloorGain_ = 1.0f;
    float flatGain_ = 1.0f;
    // The deepest cut a section may be designed at for the flat gain in force:
    // floor/flat, so the flat gain and a section together reach the law's floor
    // rather than stacking past it.
    double deepestDepthDb_ = 0.0;
    unsigned sampleRateHz_ = 0;
    size_t residualIntervals_ = 0;
    bool planHoldsLimit_ = true;
};
