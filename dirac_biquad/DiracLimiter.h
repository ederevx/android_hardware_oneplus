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

#include <vector>

// One limiter stage of the host chain; the class is instantiated twice.
//
// The fixed signature and the user curve together can ask for more than full
// scale, so the chain ends in two stages of the same kind: a level stage that
// holds the mean level down without tracking the waveform, and a peak stage that
// bounds the transients the level stage is too slow to see. Neither adds gain;
// each applies at most the attenuation its configuration caps. The per-channel
// law is detect, hold, smooth, convert, shape, smooth, apply.
//
// A stage configured to bound its output adds a ceiling on the gain it applies,
// read from the larger of the current and the previous detector blocks, so a
// sample cannot leave the delay line without its block being counted; the law is
// untouched wherever it already holds the output, so the ceiling only takes over
// on the transients the law lets through. The ceiling is a gain computed from
// that maximum, so the applied multiply can land one float rounding above the
// exact ceiling; the bound is measured against the built stage.
//
// A true-peak detector widens what the ceiling sees to the largest of |x| and
// every fractional-phase interpolated value; what it still holds exactly is the
// output sample-peak cap, and the true (inter-sample) peak stays kernel-
// dependent with no guarantee.
//
// The phases are evaluated folded (c_{P-p}[k] = c_p[L-1-k]): a phase and its
// mirror are two dot products of half the taps against the sample sums and
// differences, and max(|u+v|, |u-v|) = |u| + |v|; the middle phase of an even
// count needs the sums alone. Same values as the unfolded form, up to the
// reassociation of the sums, at half the multiply-accumulates.
//
// The look-ahead belongs to the stage that configures it, and the legacy effect
// API reports no latency, so the framework cannot compensate it.
class DiracLimiter {
  public:
    static constexpr unsigned kMaxChannels = 8;
    static constexpr unsigned kMaxTruePeakTaps = 32;
    static constexpr unsigned kMaxTruePeakPhases = 8;
    // The mirrored pairs of the largest kernel: phases 1..(P-1)/2, each covering
    // itself and its mirror P-p.
    static constexpr unsigned kMaxTruePeakPairs = (kMaxTruePeakPhases - 1) / 2;

    // One stage's configuration. Every field is a property of the stage, so a
    // caller names its stages by what they do rather than by position.
    struct Params {
        float thresholdDb;
        float maxAttenuationDb;
        float slope;
        float kneeDb;
        unsigned window;      // detector window, in samples
        bool energyDetector;  // true: mean square; false: per-sample peak
        float levelMemoryMs;  // one-pole on the detected level; 0 = the tau->0 limit
        float peakHoldMs;     // detector hold; 0 = none
        float releaseMs;
        float attackMs;
        bool instantAttack;
        float lookaheadMs;
        bool hardBound;       // the look-ahead ceiling stage (sample-peak bound)
        unsigned truePeakTaps;    // true-peak detector half-length (taps per phase); 0 = off
        unsigned truePeakPhases;  // fractional phases between samples; >=2 enables the detector
    };

    // Builds the detector, the smoothing coefficients and the look-ahead ring
    // for the stream rate. An unsupported combination leaves the stage bypassed.
    void Configure(unsigned sampleRateHz, unsigned channelCount, const Params &params);

    // Replaces the attenuation cap, never past the configured one, and keeps
    // every detector and smoothing state, so a live cap change does not reset
    // the law. A cap the law is already past is reached by its own release, so
    // the change never steps the gain.
    void SetMaxAttenuationDb(float maxAttenuationDb);

    void Reset();

    // One sample of one channel; `channel` selects the per-channel detector,
    // hold, smoothing and delay state of an interleaved stream.
    float ProcessSample(float x, unsigned channel);

    // The linear gain the stage is applying on this channel after the last
    // processed sample, so an owner can use the law's attenuation without the
    // sample it was applied to. For a bounding stage the applied gain is the
    // smaller of this and the ceiling it reads; a stage without a ceiling, which
    // is what a band shaper drives, applies exactly this.
    float Gain(unsigned channel) const;

    // The output delay this stage's look-ahead imposes, so an owner can hold its
    // own output dry for exactly as long as the ring needs to fill.
    unsigned LatencySamples() const { return delaySamples_; }

  private:
    // One channel's detector, hold, smoothing and delay state.
    struct Envelope {
        double accumulator = 0.0;  // window sum of squares, or window peak
        unsigned fill = 0;
        double held = 0.0;  // held detector value
        unsigned holdCount = 0;
        bool heldValid = false;
        double level = 0.0;  // level-memory state
        bool levelValid = false;
        double boundAccumulator = 0.0;  // largest input in the current block
        double boundPrevious = 0.0;  // ... and in the one before it
        unsigned boundFill = 0;
        // The folded coefficient sets are read from this mirrored ring, as the
        // two half-kernels a pair or the middle phase weights; its size is the
        // largest read span, 2*(2*kMaxTaps).
        float truePeakHistory[4 * kMaxTruePeakTaps] = {};  // mirrored ring
        unsigned truePeakWrite = 0;
        float s1 = 0.0f;  // release state, in dB of attenuation
        float s2 = 0.0f;  // attack state, in dB of attenuation
        float gain = 1.0f;
        float rampStep = 0.0f;
        unsigned write = 0;
    };

    // Attenuation in dB the given level asks for, after slope, knee and cap.
    float TargetDb(float levelDb) const;

    // The gain the hard-bound ceiling applies at the given detected level, i.e.
    // 10^(-TargetDb(levelDb)/20). The ceiling repeats the detector's law as a
    // gain, so for a slope of one the level-to-dB-to-gain round trip collapses
    // to one division; only the knee's quadratic band keeps the dB form.
    float BoundGain(double level) const;

  public:
    // Diagnostic: the built true-peak interpolation coefficients and count
    // (zero when the stage has no detector), so a test can compare the table it
    // was built with against the design it was derived from.
    const float *TruePeakCoefficients() const { return truePeakCoef_; }
    unsigned TruePeakCoefficientCount() const {
        return truePeakPhases_ > 0 ? (truePeakPhases_ - 1) * truePeakLength_ : 0;
    }

  private:
    Envelope channels_[kMaxChannels];
    std::vector<float> delay_;
    Params params_{};
    float kdbPerLog2_ = 0.0f;
    float levelMemoryCoef_ = 0.0f;
    float releaseCoef_ = 0.0f;
    float attackCoef_ = 0.0f;
    // The hard-bound ceiling's linear-domain constants, built in Configure when
    // `boundLinear_` says the ceiling's law has a slope of one.
    bool boundLinear_ = false;
    float boundThresholdLinear_ = 1.0f;  // 10^((thresholdDb + kneeDb/2)/20)
    float boundFloorGain_ = 1.0f;        // 10^(-maxAttenuationDb/20)
    float boundKneeGain_ = 1.0f;         // 10^(kneeDb/40): the ratio at the knee
    float boundKneeInverse_ = 1.0f;      // 10^(-kneeDb/40)
    unsigned holdSamples_ = 0;
    unsigned delaySamples_ = 0;
    float truePeakCoef_[kMaxTruePeakPhases * 2 * kMaxTruePeakTaps] = {};
    // The kernel folded for evaluation: one even and one odd set per mirrored
    // phase pair, and the symmetric middle phase where the count is even.
    float truePeakEven_[kMaxTruePeakPairs * kMaxTruePeakTaps] = {};
    float truePeakOdd_[kMaxTruePeakPairs * kMaxTruePeakTaps] = {};
    float truePeakMiddle_[kMaxTruePeakTaps] = {};
    unsigned truePeakPairs_ = 0;
    bool truePeakMiddlePhase_ = false;
    unsigned truePeakTaps_ = 0;
    unsigned truePeakLength_ = 0;
    unsigned truePeakPhases_ = 0;
    unsigned channelCount_ = 0;
    bool configured_ = false;
};
