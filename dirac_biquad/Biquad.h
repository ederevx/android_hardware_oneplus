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

// Biquad (second-order IIR) section for the host filter chain.
//
// Coefficients follow the Audio EQ Cookbook (Robert Bristow-Johnson, W3C
// NOTE-audio-eq-cookbook) normalized by a0, and the runtime uses the direct
// form 1 recursion, which the cookbook recommends for floating point.

enum class BiquadType {
    kHighPass,
    kLowShelf,
    kPeaking,
    kHighShelf,
};

// One point on the unit circle, at which a section's response is evaluated. The
// four values a second-order response needs are computed once per point, so a
// cascade of sections shares them instead of repeating the transcendentals.
struct BiquadPoint {
    double cosW;
    double sinW;
    double cos2W;
    double sin2W;
};

// A section's complex response at one point.
struct BiquadResponse {
    double real;
    double imaginary;
};

// One section's coefficients, normalised by a0, as the recursion applies them.
// These are the whole of a section that a plan needs to carry: the filter state
// belongs to whoever runs the filter, and a Biquad owns the coefficients it was
// designed with.
struct BiquadCoefficients {
    double b0;
    double b1;
    double b2;
    double a1;
    double a2;
};

class Biquad {
  public:
    void Reset();

    // `frequencyHz` is a digital corner: the coefficient set realises, at this
    // rate, a prototype whose corner is pre-warped to land on it.  `q` is the
    // quality factor for the peaking and high-pass types, and the shelf slope S
    // for the shelving types. `gainDb` applies to peaking and shelving only.
    void SetFilter(BiquadType type, double sampleRateHz, double frequencyHz, double q,
                   double gainDb);

    // `cornerHz` is the corner of an analog prototype.  The bilinear transform
    // is pre-warped for this rate, so the same arguments realise the same
    // analog response at every sample rate instead of a rate-specific one.
    void SetPrototype(BiquadType type, double sampleRateHz, double cornerHz, double q,
                      double gainDb);

    // Realises one second-order stage of an order-n Butterworth shelf cut: the
    // shelf whose edge is `cornerHz` and whose gain at full depth is `gainDb`.
    // Order 2 is one stage; order 4 is two, selected by `stage`, which is the
    // Butterworth section with factor c = 2 sin((2*stage+1)pi/(2n)) - the two
    // stages carry different damping, which is what makes the order-4 transition
    // steeper than any cascade of order-2 shelves.
    //
    // The shelf the stages make together is bounded real (|H| <= 1 for a cut at
    // every frequency), which is the property a bend section needs to be driven
    // per sample without ever adding gain. One stage alone is not: the
    // lower-damped one peaks about 0.7 dB, which is harmless because a section
    // always applies the whole shelf. An unsupported order or stage realises a
    // pass-through.
    void SetButterworthShelf(BiquadType type, double sampleRateHz, double cornerHz, double gainDb,
                             unsigned order, unsigned stage);

    float ProcessSample(float x);

    // The unit-circle point for a frequency, and the section's response there;
    // the chain reasons about the response it is configured with rather than
    // about a curve it hopes for. Neither is on the sample path.
    static BiquadPoint PointAt(double sampleRateHz, double frequencyHz);
    BiquadResponse ResponseAt(const BiquadPoint &point) const;

    // The coefficient set the running filter was designed with, and a plain
    // replacement for it. Load keeps the filter state: a caller that continues a
    // section from the samples it has already seen uses this, and one that wants
    // a fresh section calls Reset explicitly.
    BiquadCoefficients Coefficients() const;
    void Load(const BiquadCoefficients &coefficients);

    // Loads a new coefficient set and moves to it over `rampSamples` samples
    // instead of in one step. A coefficient set changed under live filter state
    // steps the output by the size of the change; the region of (a1, a2) that a
    // stable second-order section occupies is convex, so every intermediate set on
    // the straight line between two stable sets is stable and a ramp is not a
    // stability risk. `rampSamples` 0 loads immediately, and an immediate load
    // ends a ramp in progress. A ramp started while another runs continues from
    // the coefficients in force rather than from the ones the earlier ramp was
    // heading for, which is what a control being dragged needs. No allocation.
    void LoadRamped(const BiquadCoefficients &coefficients, unsigned rampSamples);

    // Whether a coefficient ramp is in flight, and whether the set in force is
    // the pass-through a 0 dB RBJ peaking section realises (b0 == 1, b1 == a1,
    // b2 == a2 bitwise). For that set the recursion reduces to
    // y = x + a1*(x1 - y1) + a2*(x2 - y2), so on state that already satisfies
    // x1 == y1 and x2 == y2 the section is a pure shift, which is what
    // ProcessIdentity exploits. The filter reads both when it chooses the fast
    // path; neither is a predicate on coefficients anywhere else.
    bool Ramping() const { return rampRemaining_ != 0; }
    bool IsIdentity() const { return b0_ == 1.0 && b1_ == a1_ && b2_ == a2_; }

    // The identity section as a fast path: no multiply, no ramp step. It carries
    // the x half of the state by register moves and mirrors the y half from x,
    // which is what keeps y == x exact; it is valid only while that invariant
    // holds, i.e. only where the band entered it on a consistent state.
    float ProcessIdentity(float x);

  private:
    void SetCoefficients(BiquadType type, double w0, double q, double gainDb);
    void StepRamp();

    double b0_ = 1.0;
    double b1_ = 0.0;
    double b2_ = 0.0;
    double a1_ = 0.0;
    double a2_ = 0.0;

    double x1_ = 0.0;
    double x2_ = 0.0;
    double y1_ = 0.0;
    double y2_ = 0.0;

    // The ramp in progress: the set it is heading for, its per-sample step, and
    // the samples left. Inactive while rampRemaining_ is 0, which is the state of
    // every filter that was not given a ramp.
    double target_[5] = {1.0, 0.0, 0.0, 0.0, 0.0};
    double step_[5] = {0.0, 0.0, 0.0, 0.0, 0.0};
    unsigned rampRemaining_ = 0;
};