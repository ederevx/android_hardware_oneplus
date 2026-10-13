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

#include "DiracBandPlanner.h"

#include <algorithm>
#include <cmath>

#include "DiracChainStages.h"
#include "DiracCurveDesign.h"

namespace {

// Two runs of violated grid points closer than this many points are one
// violation seen through a rounded grid rather than two bands, and a core this
// close to an end of the grid has reached that end. At the chain's 24 points per
// octave that is a sixth of an octave.
constexpr size_t kMergePoints = 4;

// Regula falsi steps for the design depth. The bracket is the law's own cap, the
// function is monotone, and false position converges on it from one side, so a
// handful of evaluations resolves the depth to well under a thousandth of a dB.
constexpr unsigned kDepthSteps = 8;

// The Q a peaking candidate's core spans, clamped. The upper clamp is the one
// that matters: a peaking cut's own curvature grows with Q squared, so a deeper
// and narrower cut carries more structure between two plan grid points than the
// grid samples, and a plan that holds only on its own points is not a plan. Q =
// 1/(2 sinh(ln2 BW/2)) is 3.44 at 3 dB, 2.43 at 6 dB and 1.98 at the law's own
// 9 dB floor; the depth bisection moves the depth, so the clamp is that floor's
// value, and no admissible cut carries more than about 0.01 dB between plan
// points at 24 points per Omega-octave. The lower clamp is about shape, not
// safety: an RBJ peaking cut is bounded real at every Q.
constexpr double kMinPeakQ = 0.25;
constexpr double kMaxPeakQ = 2.0;

// How far over the target a point may sit and still count as held: the same
// tolerance the plan's certificate uses, so "holds the limit" and "no residual"
// cannot disagree.
constexpr double kPlanTolerance = 1.0e-6;

// How many times the coverage repair may deepen a section, rebuild the composites
// and look again. Each pass carries every point it can, so a second pass only sees
// points a deepened section could not reach, and a third sees none but a shape
// whose deepening moved the point it was carrying out of its own reach.
constexpr unsigned kCoverPasses = 3;

// The plan is built against a target this far below the limit, because the grid
// samples the response and the continuous response between two grid points can
// sit above both of them. One per cent is 0.086 dB.
constexpr double kPlanMargin = 0.01;

// How many builds the flat-gain search may spend, including the one that ships.
// The plan's worst composite is linear in the flat gain - at full engagement the
// flat gain multiplies straight into it - so the gain that brings the worst point
// to the target is flat * target / worst, and that is what each attempt asks for.
// The sections are rebuilt against the new gain, which moves the cores and so the
// worst point, so the step is repeated rather than solved once; measured over the
// whole curve space, a budget of three left 112027 of 312500 curves failing with
// room still in the flat gain, and every one of those failures is fixed by a
// deeper gain. Five builds is 5x the measured host build cost, and the last one
// always ships: a deepening that is never built would leave the sections sized
// for a gain the plan no longer carries.
constexpr unsigned kFlatAttempts = 5;

// The order of the shelf an interval that reaches an end of the band is bent
// with. Order 4 is two biquads with different damping, which is what makes the
// transition steep: the leakage d octaves past the edge falls as
// gain/(1 + 2^(4d)) against gain/(1 + 2^(2d)) for one RBJ shelf, so what is
// 0.39 dB a full octave out with order 2 is 0.03 dB with order 4, and the bands
// that carry other instruments are left alone.
constexpr unsigned kShelfOrder = 4;

}  // namespace

void DiracBandPlanner::Build(unsigned sampleRateHz,
                             const int32_t gainsHalfDb[DiracBandPlan::kBandCount],
                             DiracBandPlan *out) {
    if (out == nullptr) {
        return;
    }

    *out = DiracBandPlan{};
    if (sampleRateHz == 0 || gainsHalfDb == nullptr) {
        return;
    }
    sampleRateHz_ = sampleRateHz;

    // Both grids are read from the chain's designed response - the same design
    // the filter builds its sections from - so the plan is read from what the
    // chain will actually do, at the plan's own resolution and at the finer one
    // it is verified and repaired on.
    const double lowOmega =
            std::tan(M_PI * dirac_chain::kBendGridLowHz / static_cast<double>(sampleRateHz));
    const double highOmega = std::tan(M_PI * dirac_chain::kBendGridHighFactor);
    const double rateOverPi = static_cast<double>(sampleRateHz) / M_PI;

    Biquad signature[DiracCurveDesign::kSignatureCount];
    Biquad bands[DiracCurveDesign::kBandCount];
    DiracCurveDesign::Signature(sampleRateHz, signature);
    DiracCurveDesign::UserBands(sampleRateHz, gainsHalfDb, bands);

    gridPoints_ = FillGrid(points_, gridMagnitude_, gridHz_, kMaxGridPoints,
                           dirac_chain::kBendGridPerOctave, lowOmega, highOmega, rateOverPi,
                           signature, bands);
    checkPointCount_ = FillGrid(checkPoints_, checkMagnitude_, nullptr, kMaxCheckPoints,
                                dirac_chain::kBendCheckPerOctave, lowOmega, highOmega,
                                rateOverPi, signature, bands);

    // The plan is built against a target a little below the limit: the grid
    // samples the response, and the continuous response between two grid points
    // can sit above both of them by more than the verification tolerance.
    const double target = dirac_chain::kMagnitudeLimit * (1.0 - kPlanMargin);

    // The law is what applies the flat gain, so its own floor bounds how much of
    // the shared excess the flat gain may take.
    lawFloorGain_ = static_cast<float>(std::pow(
            10.0, -static_cast<double>(dirac_chain::LevelStage().maxAttenuationDb) / 20.0));

    // The part of the excess every grid point shares. While the whole scanned
    // band is over the limit no shaped section can hold it, because a shape
    // always passes below the limit somewhere; a flat gain can, and it preserves
    // the relative shape exactly.
    double lowest = gridMagnitude_[0];
    for (size_t index = 1; index < gridPoints_; ++index) {
        lowest = std::min(lowest, gridMagnitude_[index]);
    }
    flatGain_ = lowest > target ? static_cast<float>(target / lowest) : 1.0f;
    if (flatGain_ < lawFloorGain_) {
        flatGain_ = lawFloorGain_;
    }

    // The flat gain is what the shapes are planned against. When a curve lifts
    // the whole band past what the law may attenuate, the shapes cannot carry
    // what is left, so the flat gain deepens and the plan is rebuilt; it settles
    // in one step for a smooth response and is bounded by the law's own floor.
    // The first attempt weighs the caller's previous plan; after that the source
    // is what this build has just produced.
    for (unsigned attempt = 0; attempt < kFlatAttempts; ++attempt) {
        // A section is designed no deeper than the law's own floor: that is the
        // deepest attenuation the law can ask for, so a deeper section would never
        // be fully engaged. The law's 9 dB cap bounds each actuator and not their
        // sum - the flat gain is gated by the law's own gain and a section's cut
        // rides on top of it - so a band the plan cuts can reach dB(flat)+dB(cap),
        // and between idle and the flat gain a section's centre gets the law's ask
        // twice in dB. That is a design position, not an accident: capping the sum
        // leaves every curve whose designed response exceeds +9 dB anywhere
        // unholdable by any plan at all, and such curves exist in the user curve
        // space. The cap therefore governs the law's ask, and the plan's depth is
        // its own.
        deepestDepthDb_ = 20.0 * std::log10(static_cast<double>(lawFloorGain_));
        BuildPlan(target);
        const double worst = CheckWorstComposite();
        if (worst <= target * (1.0 + kPlanTolerance) || flatGain_ <= lawFloorGain_) {
            // This build is the one that ships: it holds, or the flat gain has no
            // room left to deepen into.
            break;
        }
        const double needed = static_cast<double>(flatGain_) * target / worst;
        const float next =
                static_cast<float>(std::max(static_cast<double>(lawFloorGain_), needed));
        if (attempt + 1 == kFlatAttempts) {
            // The last build is calibrated, not re-planned - the break above is
            // deliberately not taken on the last attempt, so this branch owns it.
            // The sections stay as
            // this build made them and only the flat gain moves to the value this
            // plan's own worst point asks for. At full engagement the composite is
            // the flat gain multiplied into the sections' response, so that value
            // brings the worst point to the target exactly. Re-planning instead
            // moves the cores, which moves the worst point again, and measured over
            // the curve space the search does not converge inside any budget that
            // fits the poll window: five re-planning builds left 24814 curves
            // failing with room still in the flat gain, and every one of them is
            // held by this calibration. The price is a plan that over-cuts what the
            // deeper flat gain already covered, which is the direction that holds
            // the limit.
            const double ratio =
                    static_cast<double>(next) / static_cast<double>(flatGain_);
            flatGain_ = next;
            // Both composites carry the flat gain as a factor, so the calibration
            // is one multiply per point. They have to describe the plan that
            // ships: the residual count is read from them, and a count taken
            // against the plan from before the calibration would say the plan
            // leaves violations it no longer leaves.
            for (size_t index = 0; index < gridPoints_; ++index) {
                composite_[index] *= ratio;
            }
            for (size_t index = 0; index < checkPointCount_; ++index) {
                checkComposite_[index] *= ratio;
            }
            break;
        }
        flatGain_ = next;
    }
    planHoldsLimit_ = CheckWorstComposite() <= target * (1.0 + kPlanTolerance);

    // The residual is the coverage of the plan that is actually shipped, counted
    // once, on the grid the certificate is read from and at the certificate's own
    // tolerance, so "holds the limit" and "no residual" cannot disagree.
    residualIntervals_ = UncoveredRuns(target);

    // The output is written once, after the last build has finished.
    out->sampleRateHz = sampleRateHz;
    for (size_t band = 0; band < DiracBandPlan::kBandCount; ++band) {
        out->gainsHalfDb[band] = gainsHalfDb[band];
    }
    out->valid = true;
    out->holdsLimit = planHoldsLimit_;
    out->sectionCount = static_cast<uint32_t>(builtCount_);
    out->residualIntervals = static_cast<uint32_t>(residualIntervals_);
    out->flatGain = flatGain_;
    for (size_t slot = 0; slot < builtCount_; ++slot) {
        DiracBandPlan::Section &section = out->sections[slot];
        section.type = built_[slot].type;
        section.stageCount = built_[slot].stageCount;
        section.cornerHz = built_[slot].cornerHz;
        section.q = built_[slot].q;
        section.capGain = built_[slot].capGain;
        for (unsigned stage = 0; stage < built_[slot].stageCount; ++stage) {
            section.stage[stage] = built_[slot].stage[stage];
        }
    }
}

size_t DiracBandPlanner::FillGrid(BiquadPoint *points, double *magnitude, double *hz,
                                  size_t capacity, double pointsPerOctave, double lowOmega,
                                  double highOmega, double rateOverPi, const Biquad *signature,
                                  const Biquad *bands) {
    // The grid is uniform in Omega and reported in Hz, so its own resolution is
    // the same at every frequency and at every rate.
    size_t pointsCount =
            static_cast<size_t>(std::log2(highOmega / lowOmega) * pointsPerOctave) + 1;
    if (pointsCount > capacity) {
        pointsCount = capacity;
    }
    if (pointsCount < 2) {
        pointsCount = 2;
    }

    // The response is the product of every section's own response at the point,
    // evaluated on the coefficients the chain is designed with, so the plan is
    // read from what the chain will actually do rather than from a curve it hopes
    // for. The unit-circle point is computed once per frequency and shared by all
    // 23 sections.
    for (size_t index = 0; index < pointsCount; ++index) {
        const double fraction =
                static_cast<double>(index) / static_cast<double>(pointsCount - 1);
        const double omega = lowOmega * std::pow(highOmega / lowOmega, fraction);
        const double frequency = rateOverPi * std::atan(omega);
        const BiquadPoint point = Biquad::PointAt(sampleRateHz_, frequency);
        double value = 1.0;
        for (size_t section = 0; section < DiracCurveDesign::kSignatureCount; ++section) {
            const BiquadResponse response = signature[section].ResponseAt(point);
            value *= std::hypot(response.real, response.imaginary);
        }
        for (size_t band = 0; band < DiracCurveDesign::kBandCount; ++band) {
            const BiquadResponse response = bands[band].ResponseAt(point);
            value *= std::hypot(response.real, response.imaginary);
        }
        if (hz != nullptr) {
            hz[index] = frequency;
        }
        points[index] = point;
        magnitude[index] = value;
    }
    return pointsCount;
}

void DiracBandPlanner::BuildPlan(double target) {
    FillComposites();

    Shape rebuilt[DiracBandPlan::kMaxSections];
    size_t count = 0;
    residualIntervals_ = 0;
    for (unsigned step = 0; step < DiracBandPlan::kMaxSections; ++step) {
        // The worst point still over the target, and the core it belongs to. The
        // core, and not the whole violation the point lies in, is what the next
        // section must carry: sizing a section on its core is what keeps the
        // shape that carries a violation's worst point from over-cutting that
        // violation's tails.
        size_t peak = 0;
        double peakDb = ResidualDb(0, target);
        for (size_t index = 1; index < gridPoints_; ++index) {
            const double db = ResidualDb(index, target);
            if (db > peakDb) {
                peakDb = db;
                peak = index;
            }
        }
        if (peakDb <= 0.0) {
            break;
        }
        size_t first = 0;
        size_t last = 0;
        CoreAround(peak, target, &first, &last);

        // The candidates for this core: a peaking cut at its worst point, and a
        // shelf where it reaches an end of the scanned band.
        constexpr BendKind kKinds[] = {BendKind::kPeaking, BendKind::kLowShelf,
                                       BendKind::kHighShelf};
        Shape winner;
        Candidate best;
        bool hasBest = false;
        Shape probe;
        for (const BendKind kind : kKinds) {
            if (!ShapeForKind(probe, kind, first, last)) {
                continue;
            }
            const Candidate choice = WeighCandidate(probe, first, last, target);
            if (!hasBest || choice.overcutDb < best.overcutDb) {
                winner = probe;
                best = choice;
                hasBest = true;
            }
        }
        winner.coreFirst = first;
        winner.coreLast = last;
        winner.peelOrder = step;
        rebuilt[count] = winner;
        ScaleComposite(rebuilt[count], false);
        ++count;
    }

    // The plan is read in frequency order, so a section's neighbours in the plan
    // are its neighbours in frequency however the peel found them. The order is
    // total: two cores can begin at the same point where both reach an end of the
    // scanned band, and a sort that left those tied would let the standard library
    // decide the plan.
    std::sort(rebuilt, rebuilt + count, [](const Shape &a, const Shape &b) {
        if (a.coreFirst != b.coreFirst) {
            return a.coreFirst < b.coreFirst;
        }
        if (a.coreLast != b.coreLast) {
            return a.coreLast < b.coreLast;
        }
        if (a.cornerHz != b.cornerHz) {
            return a.cornerHz < b.cornerHz;
        }
        if (a.type != b.type) {
            return static_cast<int>(a.type) < static_cast<int>(b.type);
        }
        // The peel order is the last key, so the order is total: two sections can
        // agree on everything above and still differ in depth, and a comparator
        // that stopped there would leave the order - and with it the depth the
        // repair pass assigns - to the standard library.
        return a.peelOrder < b.peelOrder;
    });
    // Every depth is re-measured once with the other sections in place, and then
    // whatever is left over the target is deepened into the section that already
    // cuts it most.
    RefineDepths(rebuilt, count, target);
    CoverResidual(rebuilt, count, target);

    // A section left at no depth at all cuts nothing: it would still run its
    // filters on the audio thread for no response, so the plan does not name it.
    size_t kept = 0;
    for (size_t slot = 0; slot < count; ++slot) {
        if (rebuilt[slot].capGain >= 1.0) {
            continue;
        }
        if (kept != slot) {
            rebuilt[kept] = rebuilt[slot];
        }
        ++kept;
    }
    count = kept;

    for (size_t slot = 0; slot < count; ++slot) {
        built_[slot] = rebuilt[slot];
    }
    builtCount_ = count;
}

double DiracBandPlanner::ResidualDb(size_t index, double target) const {
    return 20.0 * std::log10(composite_[index] / target);
}

double DiracBandPlanner::CheckResidualDb(size_t index, double target) const {
    return 20.0 * std::log10(checkComposite_[index] / target);
}

void DiracBandPlanner::CoreAround(size_t peak, double target, size_t *first,
                                  size_t *last) const {
    // The core is the run of grid points carrying at least half the peak's excess
    // in dB: the width the shape's own bandwidth definition asks for, which widens
    // as the hump deepens.
    const double threshold = 0.5 * ResidualDb(peak, target);
    size_t low = peak;
    while (low > 0 && ResidualDb(low - 1, target) >= threshold) {
        --low;
    }
    size_t high = peak;
    while (high + 1 < gridPoints_ && ResidualDb(high + 1, target) >= threshold) {
        ++high;
    }
    // A core that reaches an end of the scanned band carries that end: the shelf
    // that bends it cuts everything on its far side, so sizing it on the core
    // alone would leave the band beyond the core over the limit until another
    // section paid for it. A core the peel leaves short of an end is a shape's
    // own business to cover.
    if (low <= kMergePoints) {
        low = 0;
    }
    if (high + 1 + kMergePoints >= gridPoints_) {
        high = gridPoints_ - 1;
    }
    *first = low;
    *last = high;
}

double DiracBandPlanner::ShapeMagnitude(const Shape &shape, const BiquadPoint &point) const {
    double real = 1.0;
    double imaginary = 0.0;
    for (unsigned stage = 0; stage < shape.stageCount; ++stage) {
        Biquad section;
        section.Load(shape.stage[stage]);
        const BiquadResponse response = section.ResponseAt(point);
        const double nextReal = real * response.real - imaginary * response.imaginary;
        const double nextImaginary = real * response.imaginary + imaginary * response.real;
        real = nextReal;
        imaginary = nextImaginary;
    }
    return std::hypot(real, imaginary);
}

double DiracBandPlanner::CoreExcessDb(const Shape &shape, size_t first, size_t last,
                                      double target) const {
    double worst = -120.0;
    for (size_t index = first; index <= last; ++index) {
        const double excess = 20.0 * std::log10(composite_[index] *
                                                ShapeMagnitude(shape, points_[index]) / target);
        worst = std::max(worst, excess);
    }
    return worst;
}

double DiracBandPlanner::OverCutDb(const Shape &shape, double target) const {
    double worst = 0.0;
    for (size_t index = 0; index < checkPointCount_; ++index) {
        const double magnitude = ShapeMagnitude(shape, checkPoints_[index]);
        if (magnitude <= 0.0) {
            continue;
        }
        const double cutDb = -20.0 * std::log10(magnitude);
        const double needed = std::max(0.0, CheckResidualDb(index, target));
        worst = std::max(worst, cutDb - needed);
    }
    return worst;
}

bool DiracBandPlanner::ShapeForKind(Shape &shape, BendKind kind, size_t first,
                                    size_t last) const {
    switch (kind) {
        case BendKind::kPeaking: {
            // The corner is the core's geometric centre in Omega and the Q is the
            // one whose bandwidth is the core's, because Omega is the coordinate
            // an RBJ cut is uniform in: a corner or a width taken in Hz would be a
            // different filter near Nyquist than at the bottom of the band.
            const double lowOmega =
                    std::tan(M_PI * gridHz_[first] / static_cast<double>(sampleRateHz_));
            const double highOmega =
                    std::tan(M_PI * gridHz_[last] / static_cast<double>(sampleRateHz_));
            const double midOmega = std::sqrt(lowOmega * highOmega);
            shape.type = BiquadType::kPeaking;
            shape.cornerHz = static_cast<double>(sampleRateHz_) / M_PI * std::atan(midOmega);
            const double bandwidth = std::log2(highOmega / lowOmega);  // Omega-octaves
            const double q = bandwidth > 0.0
                                     ? 1.0 / (2.0 * std::sinh(M_LN2 * bandwidth / 2.0))
                                     : 1.0;
            shape.q = std::min(kMaxPeakQ, std::max(kMinPeakQ, q));
            shape.stageCount = 1;
            return true;
        }
        case BendKind::kLowShelf:
            if (first > kMergePoints) {
                return false;
            }
            shape.type = BiquadType::kLowShelf;
            shape.cornerHz = gridHz_[last];
            break;
        case BendKind::kHighShelf:
            if (last + 1 + kMergePoints < gridPoints_) {
                return false;
            }
            shape.type = BiquadType::kHighShelf;
            shape.cornerHz = gridHz_[first];
            break;
    }
    shape.q = 1.0;
    shape.stageCount = kShelfOrder / 2;
    return true;
}

void DiracBandPlanner::BuildShape(Shape &shape, double depthDb) {
    // A section only ever cuts. Every depth the plan computes is clamped here,
    // which is the one place a section's gain is set, so no path can admit a
    // section that adds gain: a positive depth would make the runtime's
    // engagement negative and apply the boost unconditionally.
    if (depthDb > 0.0) {
        depthDb = 0.0;
    }
    for (unsigned stage = 0; stage < shape.stageCount; ++stage) {
        Biquad section;
        if (shape.type == BiquadType::kPeaking) {
            section.SetFilter(BiquadType::kPeaking, sampleRateHz_, shape.cornerHz, shape.q,
                              depthDb);
        } else {
            section.SetButterworthShelf(shape.type, sampleRateHz_, shape.cornerHz, depthDb,
                                        kShelfOrder, stage);
        }
        shape.stage[stage] = section.Coefficients();
    }
    shape.capGain = std::pow(10.0, depthDb / 20.0);
}

bool DiracBandPlanner::SizeForCore(Shape &shape, size_t first, size_t last, double target) {
    // The shallowest cut that carries the core, found by regula falsi on the
    // core's excess, which falls monotonically as the depth grows: a deeper cut
    // than the violation needs is exactly what leaks past it into the bands that
    // carry other instruments, so the shallowest is the one wanted.
    const double deepestDb = deepestDepthDb_;
    // Transparency is the shallowest depth there is, so a core another section
    // already carries is left with no cut of its own: interpolating outside the
    // bracket would return a depth above zero, which would be a boost.
    double shallow = 0.0;
    BuildShape(shape, shallow);
    double shallowExcess = CoreExcessDb(shape, first, last, target);
    if (shallowExcess <= 0.0) {
        return true;
    }
    double deep = deepestDb;
    BuildShape(shape, deep);
    double deepExcess = CoreExcessDb(shape, first, last, target);
    if (deepExcess > 0.0) {
        return false;  // no depth the law can ask for carries this core
    }
    for (unsigned step = 0; step < kDepthSteps; ++step) {
        const double spread = shallowExcess - deepExcess;
        if (spread <= 1.0e-9) {
            break;
        }
        double middle = shallow + (deep - shallow) * shallowExcess / spread;
        if (middle > 0.0) {
            middle = 0.0;
        } else if (middle < deepestDb) {
            middle = deepestDb;
        }
        BuildShape(shape, middle);
        const double middleExcess = CoreExcessDb(shape, first, last, target);
        if (middleExcess <= 0.0) {
            deep = middle;
            deepExcess = middleExcess;
        } else {
            shallow = middle;
            shallowExcess = middleExcess;
        }
    }
    BuildShape(shape, deep);
    return true;
}

DiracBandPlanner::Candidate DiracBandPlanner::WeighCandidate(Shape &shape, size_t first,
                                                             size_t last, double target) {
    Candidate candidate;
    candidate.type = shape.type;
    candidate.cornerHz = shape.cornerHz;
    candidate.q = shape.q;
    candidate.stageCount = shape.stageCount;
    candidate.reached = SizeForCore(shape, first, last, target);
    candidate.depthDb = shape.capGain > 0.0 ? 20.0 * std::log10(shape.capGain) : 0.0;
    candidate.overcutDb = OverCutDb(shape, target);
    return candidate;
}

void DiracBandPlanner::ScaleComposite(const Shape &shape, bool divide) {
    for (size_t index = 0; index < gridPoints_; ++index) {
        const double magnitude = ShapeMagnitude(shape, points_[index]);
        if (divide) {
            if (magnitude > 0.0) {
                composite_[index] /= magnitude;
            }
        } else {
            composite_[index] *= magnitude;
        }
    }
    for (size_t index = 0; index < checkPointCount_; ++index) {
        const double magnitude = ShapeMagnitude(shape, checkPoints_[index]);
        if (divide) {
            if (magnitude > 0.0) {
                checkComposite_[index] /= magnitude;
            }
        } else {
            checkComposite_[index] *= magnitude;
        }
    }
}

void DiracBandPlanner::ReplaceResponse(const Shape &before, const Shape &after) {
    for (size_t index = 0; index < gridPoints_; ++index) {
        const double oldMagnitude = ShapeMagnitude(before, points_[index]);
        if (oldMagnitude > 0.0) {
            composite_[index] /= oldMagnitude;
        }
        composite_[index] *= ShapeMagnitude(after, points_[index]);
    }
    for (size_t index = 0; index < checkPointCount_; ++index) {
        const double oldMagnitude = ShapeMagnitude(before, checkPoints_[index]);
        if (oldMagnitude > 0.0) {
            checkComposite_[index] /= oldMagnitude;
        }
        checkComposite_[index] *= ShapeMagnitude(after, checkPoints_[index]);
    }
}

void DiracBandPlanner::RefineDepths(Shape *rebuilt, size_t count, double target) {
    for (size_t index = 0; index < count; ++index) {
        Shape &shape = rebuilt[index];
        ScaleComposite(shape, true);
        SizeForCore(shape, shape.coreFirst, shape.coreLast, target);
        ScaleComposite(shape, false);
    }
}

bool DiracBandPlanner::DeepenForPoint(Shape &shape, size_t index, double without,
                                      double target) {
    // Bisect the shallowest depth that carries this one point, then keep the
    // section's own depth if it is already deeper: a repair may only ever add
    // attenuation, because a shallower section would uncover what another one
    // holds. `without` is the composite with this section's own response divided
    // out, so the search needs no pass over either grid.
    const double currentDb = shape.capGain > 0.0 ? 20.0 * std::log10(shape.capGain) : 0.0;
    const double deepestDb = deepestDepthDb_;
    double deep = deepestDb;
    BuildShape(shape, deep);
    if (without * ShapeMagnitude(shape, checkPoints_[index]) > target) {
        // No depth the law can ask for carries this point through this shape. The
        // section stays at the deepest depth the law allows rather than going back
        // where the caller had it: it cannot carry this point, but it is the
        // section that cuts the region around it most, and the caller registers
        // the change, so the composites remain the ones this state produces.
        return false;
    }
    double shallow = 0.0;
    for (unsigned step = 0; step < kDepthSteps; ++step) {
        const double middle = 0.5 * (shallow + deep);
        BuildShape(shape, middle);
        if (without * ShapeMagnitude(shape, checkPoints_[index]) <= target) {
            deep = middle;
        } else {
            shallow = middle;
        }
    }
    if (deep > currentDb) {
        deep = currentDb;
    }
    if (deep > 0.0) {
        deep = 0.0;
    }
    BuildShape(shape, deep);
    return true;
}

void DiracBandPlanner::CoverResidual(Shape *rebuilt, size_t count, double target) {
    if (count == 0) {
        return;
    }
    // Deepening a section changes it everywhere, so the composites are kept
    // current as it happens: a point's search reads the composite with its helper's
    // response divided out, and that division is only correct while the composite
    // still holds the response the helper had when the point was read. A pass
    // therefore carries every point it can against the state the pass itself has
    // built, and the next pass sees it.
    for (unsigned pass = 0; pass < kCoverPasses; ++pass) {
        bool deepened = false;
        for (size_t index = 0; index < checkPointCount_; ++index) {
            if (checkComposite_[index] <= target * (1.0 + kPlanTolerance)) {
                continue;
            }
            // The section that already cuts this point most is the one that needs
            // the least extra depth to carry it, so it is the one whose deepening
            // adds the least attenuation elsewhere.
            size_t helper = count;
            double helperMagnitude = 1.0;
            for (size_t slot = 0; slot < count; ++slot) {
                const double magnitude = ShapeMagnitude(rebuilt[slot], checkPoints_[index]);
                if (magnitude < helperMagnitude) {
                    helperMagnitude = magnitude;
                    helper = slot;
                }
            }
            if (helper == count || helperMagnitude <= 0.0) {
                continue;  // no section reaches this point, so no depth can carry it
            }
            const Shape before = rebuilt[helper];
            DeepenForPoint(rebuilt[helper], index, checkComposite_[index] / helperMagnitude,
                           target);
            if (rebuilt[helper].capGain != before.capGain) {
                // Whether or not the point was carried, the section moved: a repair
                // that cannot carry its point leaves the section at the deepest
                // depth the law allows. The composites take that change before the
                // next point reads them, or the rest of this pass would search
                // against a state the sections are no longer in.
                ReplaceResponse(before, rebuilt[helper]);
                deepened = true;
            }
        }
        if (!deepened) {
            break;
        }
    }
}

size_t DiracBandPlanner::UncoveredRuns(double target) const {
    size_t runs = 0;
    bool inside = false;
    for (size_t index = 0; index < checkPointCount_; ++index) {
        const bool over = checkComposite_[index] > target * (1.0 + kPlanTolerance);
        if (over && !inside) {
            ++runs;
        }
        inside = over;
    }
    return runs;
}

void DiracBandPlanner::FillComposites() {
    // Both composites are the chain's response with the flat gain in front of it,
    // and each placed section multiplies both. The peel reads the plan grid; the
    // plan is verified and repaired on the check grid, which is what sees what a
    // shape carries between two plan points.
    for (size_t index = 0; index < gridPoints_; ++index) {
        composite_[index] = gridMagnitude_[index] * static_cast<double>(flatGain_);
    }
    for (size_t index = 0; index < checkPointCount_; ++index) {
        checkComposite_[index] = checkMagnitude_[index] * static_cast<double>(flatGain_);
    }
}

double DiracBandPlanner::CheckWorstComposite() const {
    // The worst case is the deepest engagement: the law at its floor applies the
    // flat gain and every section is exactly its own cut. It is read on the check
    // grid, which is the plan's own certificate.
    double worst = 0.0;
    for (size_t index = 0; index < checkPointCount_; ++index) {
        double magnitude = checkMagnitude_[index] * static_cast<double>(flatGain_);
        for (size_t slot = 0; slot < builtCount_; ++slot) {
            magnitude *= ShapeMagnitude(built_[slot], checkPoints_[index]);
        }
        worst = std::max(worst, magnitude);
    }
    return worst;
}
