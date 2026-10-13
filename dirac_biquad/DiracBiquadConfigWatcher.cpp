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

#include "DiracBiquadConfigWatcher.h"

#include <errno.h>
#include <sched.h>
#include <string.h>
#include <sys/stat.h>

#include <atomic>
#include <chrono>

#include <log/log.h>

#include "DiracCurveDesign.h"

namespace {

// The effect is created per music session and released on every stopOutput, so a
// new watcher starts from the last state any watcher read from the file rather
// than from the baked preset, and reuses its plan while the key holds. Only the
// non-audio threads touch it: a helper writes it after a real read, and a
// constructor reads it.
struct SeedState {
    bool valid = false;
    DiracBiquadConfigSnapshot snapshot;
};

DiracGuardedValue<SeedState> gSeed;

void PublishSeed(const DiracBiquadConfigSnapshot &snapshot) {
    SeedState state;
    state.valid = true;
    state.snapshot = snapshot;
    gSeed.Write(state);
}

// Spins: only a constructor calls this, never the audio path, and the guard is
// held only for the struct copy.
bool ReadSeed(DiracBiquadConfigSnapshot *out) {
    SeedState state;
    while (!gSeed.TryRead(&state)) {
    }
    if (!state.valid) {
        return false;
    }
    *out = state.snapshot;
    return true;
}

}  // namespace

DiracBiquadConfigWatcher::DiracBiquadConfigWatcher() {
    DiracBiquadConfig::Fallback(lastGoodGainsHalfDb_, &lastGoodEnabled_, &lastGoodFallback_,
                                &lastGoodVolumeDb_);
    // The effect is created per session, so start from the last state a real read
    // produced rather than the baked preset. No plan is built here: the create
    // path must not plan, so an unseeded watcher publishes an invalid plan and
    // the audio side stays in fallback until the helper's first build lands.
    DiracBiquadConfigSnapshot seed;
    const bool haveSeed = ReadSeed(&seed);
    if (haveSeed) {
        memcpy(lastGoodGainsHalfDb_, seed.gainsHalfDb, sizeof(lastGoodGainsHalfDb_));
        lastGoodEnabled_ = seed.diracEnabled;
        lastGoodFallback_ = seed.fallback;
        lastGoodVolumeDb_ = seed.volumeDb;
        lastPlan_ = seed.plan;
        plannedRate_ = seed.plan.sampleRateHz;
        if (seed.plan.valid) {
            sampleRateHz_.store(seed.plan.sampleRateHz);
        }
    } else {
        lastPlan_ = DiracBandPlan{};
        plannedRate_ = 0;
    }
    // Seed the published snapshot with the state the effect applies in Init, so
    // a forced read before the helper's first publication still sees a complete,
    // valid snapshot and never the zero-initialised default.
    DiracBiquadConfigSnapshot published;
    published.generation = 0;
    memcpy(published.gainsHalfDb, lastGoodGainsHalfDb_, sizeof(published.gainsHalfDb));
    published.diracEnabled = lastGoodEnabled_;
    published.fallback = lastGoodFallback_;
    published.volumeDb = lastGoodVolumeDb_;
    published.plan = seed.plan;
    published.bandsRateHz = haveSeed ? seed.bandsRateHz : 0;
    memcpy(published.userBands, seed.userBands, sizeof(published.userBands));
    published_.Write(published);
    lastLoadError_ = 0;
    mtimeSec_ = 0;
    mtimeNsec_ = 0;
    mtimeValid_ = false;
    generation_ = 0;
}

DiracBiquadConfigWatcher::~DiracBiquadConfigWatcher() {
    Stop();
}

void DiracBiquadConfigWatcher::Start() {
    std::lock_guard<std::mutex> lock(stopMutex_);
    if (threadStarted_ || threadStartFailed_) {
        return;
    }
    stop_ = false;
    // pthread_create, not std::thread: this module builds without exceptions, so
    // a failed spawn would abort the effects HAL rather than hand back an error.
    // A background poller must never take the HAL down - this runs from
    // create_effect/INIT, and an escaping failure would kill all audio. Degrade
    // instead: keep serving the last-good or seeded state - which may leave the
    // effect a pass-through - and log the failure once.
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    // The creating thread is the effects HAL's command thread, and a default
    // spawn would inherit its scheduling. Explicit SCHED_OTHER keeps the helper
    // off the real-time path whatever the caller runs under.
    pthread_attr_setinheritsched(&attributes, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attributes, SCHED_OTHER);
    sched_param scheduling{};
    scheduling.sched_priority = 0;
    pthread_attr_setschedparam(&attributes, &scheduling);
    const int error = pthread_create(&thread_, &attributes, &DiracBiquadConfigWatcher::ThreadMain,
                                     this);
    pthread_attr_destroy(&attributes);
    if (error != 0) {
        threadStartFailed_ = true;
        ALOGW("%s: cannot start the QEM config thread (%s); keeping the baked state", __func__,
              strerror(error));
        return;
    }
    threadStarted_ = true;
}

void DiracBiquadConfigWatcher::Stop() {
    {
        std::lock_guard<std::mutex> lock(stopMutex_);
        if (!threadStarted_) {
            return;
        }
        stop_ = true;
    }
    stopCondition_.notify_all();
    pthread_join(thread_, nullptr);
    std::lock_guard<std::mutex> lock(stopMutex_);
    threadStarted_ = false;
}

void *DiracBiquadConfigWatcher::ThreadMain(void *self) {
    static_cast<DiracBiquadConfigWatcher *>(self)->Loop();
    return nullptr;
}

void DiracBiquadConfigWatcher::SetSampleRate(unsigned sampleRateHz) {
    // Release: the helper reads the rate with acquire before it builds a plan,
    // so a rate set here is the one the next build sees.
    sampleRateHz_.store(sampleRateHz == 0 ? 48000u : sampleRateHz, std::memory_order_release);
}

void DiracBiquadConfigWatcher::RequestReload() {
    reloadRequested_.store(true, std::memory_order_release);
}

bool DiracBiquadConfigWatcher::Read(DiracBiquadConfigSnapshot *out) {
    return published_.TryRead(out);
}

void DiracBiquadConfigWatcher::Loop() {
    pthread_setname_np(pthread_self(), "dirac_watch");
    // Publish the baked fallback and then the first real read before the first
    // wait, so a stream that starts immediately already has a snapshot.
    PublishOnce(/*force=*/true);

    std::unique_lock<std::mutex> lock(stopMutex_);
    while (!stop_) {
        unsigned intervalMs =
                (kReloadCheckFrames * 1000u) / sampleRateHz_.load(std::memory_order_relaxed);
        if (intervalMs == 0) {
            intervalMs = 1;
        }

        // Never sleep a whole interval without looking at the request flag:
        // wait in kReloadRequestPollMs slices and break out as soon as one is
        // seen, so a forced reload lands within a slice instead of within an
        // interval. The caller only stores the flag; no caller wakes this
        // thread and no caller (including the audio-path command handlers)
        // takes a futex.
        unsigned waitedMs = 0;
        while (!stop_ && waitedMs < intervalMs) {
            unsigned sliceMs = intervalMs - waitedMs;
            if (sliceMs > kReloadRequestPollMs) {
                sliceMs = kReloadRequestPollMs;
            }
            stopCondition_.wait_for(lock, std::chrono::milliseconds(sliceMs),
                                    [this] { return stop_; });
            if (stop_) {
                break;
            }
            waitedMs += sliceMs;
            // Leave the wait early on a forced reload, or when the rate moved
            // away from the one the last build covered: a rate that changed
            // while a build ran would otherwise not be replanned until the next
            // scheduled poll.
            if (reloadRequested_.load(std::memory_order_acquire) ||
                sampleRateHz_.load(std::memory_order_acquire) != plannedRate_) {
                break;
            }
        }
        if (stop_) {
            break;
        }

        lock.unlock();
        const bool forced = reloadRequested_.exchange(false, std::memory_order_acq_rel);
        // A rate change must bypass the mtime gate: the file did not move, but
        // the plan for it is now wrong.
        const bool rateChanged = sampleRateHz_.load(std::memory_order_acquire) != plannedRate_;
        PublishOnce(forced || rateChanged);
        lock.lock();
    }
}

void DiracBiquadConfigWatcher::PublishOnce(bool force) {
    struct stat st;
    // The timestamp is only read here, never recorded: it is committed once the
    // file has been parsed, because a writer that truncates the file and rewrites
    // it inside one clock tick would otherwise be skipped for good - the
    // timestamp taken from the empty file is the one the finished file carries.
    const bool haveStat = stat(DiracBiquadConfig::ConfigPath(), &st) == 0;
    if (!force) {
        if (!haveStat) {
            return;
        }
        if (mtimeValid_ && static_cast<int64_t>(st.st_mtim.tv_sec) == mtimeSec_ &&
            st.st_mtim.tv_nsec == mtimeNsec_) {
            return;
        }
    }

    int gains[DiracBiquadConfig::kBandCount];
    memcpy(gains, lastGoodGainsHalfDb_, sizeof(gains));
    bool enabled = lastGoodEnabled_;
    bool fallback = lastGoodFallback_;
    double volumeDb = lastGoodVolumeDb_;

    int error = 0;
    if (DiracBiquadConfig::Load(gains, &enabled, &fallback, &volumeDb, &error)) {
        memcpy(lastGoodGainsHalfDb_, gains, sizeof(gains));
        lastGoodEnabled_ = enabled;
        lastGoodFallback_ = fallback;
        lastGoodVolumeDb_ = volumeDb;
        lastLoadError_ = 0;
        // Only now is the file's timestamp the state's timestamp: a file that
        // could not be parsed keeps its previous mtime on record, so the next poll
        // reads it again instead of skipping it as unchanged.
        if (haveStat) {
            mtimeSec_ = static_cast<int64_t>(st.st_mtim.tv_sec);
            mtimeNsec_ = st.st_mtim.tv_nsec;
            mtimeValid_ = true;
        }
    } else if (error != lastLoadError_) {
        // Keep the last good state: an unreadable or torn file must never
        // silently convert the effect to a full pass-through. Log each distinct
        // failure once, so an EACCES is visible without flooding logcat.
        lastLoadError_ = error;
        ALOGW("%s: state file %s unusable (%s); keeping last good state", __func__,
              DiracBiquadConfig::ConfigPath(), strerror(error));
    }

    // Build the plan for the rate and gains this snapshot will carry. This runs
    // outside the publish guard and before it is taken -- the build costs
    // milliseconds and the guard is held only for the struct copy. A change that
    // left the rate and the gains alone reuses the last plan, so a flag-only
    // change is never delayed by a build.
    const unsigned rate = sampleRateHz_.load(std::memory_order_acquire);
    // Design the seven user-EQ sections here, on the helper, so the audio path
    // only loads them for a live gain change.
    Biquad designed[DiracBiquadConfig::kBandCount];
    DiracCurveDesign::UserBands(rate, lastGoodGainsHalfDb_, designed);

    int32_t gains32[DiracBiquadConfig::kBandCount];
    for (size_t band = 0; band < DiracBiquadConfig::kBandCount; ++band) {
        gains32[band] = static_cast<int32_t>(lastGoodGainsHalfDb_[band]);
    }
    const bool samePlanKey = lastPlan_.valid && lastPlan_.sampleRateHz == rate &&
                             memcmp(lastPlan_.gainsHalfDb, gains32, sizeof(gains32)) == 0;
    if (!samePlanKey) {
        if (generation_ == 0) {
            // The first publication hands the curve over before the plan is
            // built, so a new instance is not silent for the cold build's
            // milliseconds. The plan follows in the second publication, and the
            // audio side's own branches apply curve and plan separately.
            const DiracBiquadConfigSnapshot curveOnly =
                    MakeSnapshot(rate, designed, DiracBandPlan{});
            Publish(curveOnly);
            publishedGeneration_.store(curveOnly.generation, std::memory_order_release);
        }
        // `lastPlan_` is both the destination and the source of the key, and the
        // key was read above, before the build overwrites it.
        planner_.Build(rate, gains32, &lastPlan_);
        if (lastPlan_.valid && !lastPlan_.holdsLimit) {
            // The plan is applied anyway: it is the best the section budget can
            // do, its output is still bounded by the peak stage, and reporting the
            // shortfall is what keeps it from being silent.
            ALOGW("%s: bend plan does not hold the limit: residual %u, flat %.4f, %u sections",
                  __func__, lastPlan_.residualIntervals,
                  static_cast<double>(lastPlan_.flatGain), lastPlan_.sectionCount);
        }
    }
    // The rate the plan actually covers: a rate that moved while the build ran
    // differs from this on the next slice and forces a replan.
    plannedRate_ = lastPlan_.sampleRateHz;

    const DiracBiquadConfigSnapshot snapshot = MakeSnapshot(rate, designed, lastPlan_);
    Publish(snapshot);
    // The generation a reader gates on is published only after the snapshot it
    // belongs to, so a reader that sees a new generation always finds the new
    // state behind it.
    publishedGeneration_.store(snapshot.generation, std::memory_order_release);
    // Seed the next instance from this state, but only when it really came from
    // the file: a baked or failed read must not overwrite the last real state.
    if (lastLoadError_ == 0 && mtimeValid_) {
        PublishSeed(snapshot);
    }
}

DiracBiquadConfigSnapshot DiracBiquadConfigWatcher::MakeSnapshot(unsigned sampleRateHz,
                                                                 const Biquad *designed,
                                                                 const DiracBandPlan &plan) {
    DiracBiquadConfigSnapshot snapshot;
    snapshot.generation = ++generation_;
    memcpy(snapshot.gainsHalfDb, lastGoodGainsHalfDb_, sizeof(snapshot.gainsHalfDb));
    snapshot.diracEnabled = lastGoodEnabled_;
    snapshot.fallback = lastGoodFallback_;
    snapshot.volumeDb = lastGoodVolumeDb_;
    snapshot.plan = plan;
    snapshot.bandsRateHz = sampleRateHz;
    for (size_t band = 0; band < DiracBiquadConfig::kBandCount; ++band) {
        snapshot.userBands[band] = designed[band].Coefficients();
    }
    return snapshot;
}

void DiracBiquadConfigWatcher::Publish(const DiracBiquadConfigSnapshot &snapshot) {
    published_.Write(snapshot);
}
