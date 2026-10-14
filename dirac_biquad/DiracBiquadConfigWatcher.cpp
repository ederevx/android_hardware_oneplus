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
#include <string.h>
#include <sys/stat.h>

#include <chrono>

#include <log/log.h>

DiracBiquadConfigWatcher::DiracBiquadConfigWatcher() {
    DiracBiquadConfig::Fallback(lastGoodGainsHalfDb_, &lastGoodEnabled_, &lastGoodFallback_,
                                &lastGoodVolumeDb_);
    // Seed the published snapshot with the same baked state the effect applies
    // in Init, so a forced read before the helper's first publication still
    // sees a complete, valid snapshot and never the zero-initialised default.
    published_.generation = 0;
    memcpy(published_.gainsHalfDb, lastGoodGainsHalfDb_, sizeof(published_.gainsHalfDb));
    published_.diracEnabled = lastGoodEnabled_;
    published_.fallback = lastGoodFallback_;
    published_.volumeDb = lastGoodVolumeDb_;
    // No plan is built here: the create path must not plan, so the first
    // publication seeds an invalid plan and the audio side stays in fallback
    // until the helper's first build lands.
    published_.plan = DiracBandPlan{};
    lastPlan_ = DiracBandPlan{};
    plannedRate_ = 0;
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
    // instead: keep serving the seeded baked snapshot (fallback stays off, so
    // the effect is a pass-through) and log the failure once.
    const int error = pthread_create(&thread_, nullptr, &DiracBiquadConfigWatcher::ThreadMain,
                                     this);
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
    if (publishGuard_.test_and_set(std::memory_order_acquire)) {
        return false;
    }
    *out = published_;
    publishGuard_.clear(std::memory_order_release);
    return true;
}

void DiracBiquadConfigWatcher::Loop() {
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
        if (forced || rateChanged) {
            PublishOnce(/*force=*/true);
        } else {
            PublishOnce(/*force=*/false);
        }
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
    int32_t gains32[DiracBiquadConfig::kBandCount];
    for (size_t band = 0; band < DiracBiquadConfig::kBandCount; ++band) {
        gains32[band] = static_cast<int32_t>(lastGoodGainsHalfDb_[band]);
    }
    const bool samePlanKey = lastPlan_.valid && lastPlan_.sampleRateHz == rate &&
                             memcmp(lastPlan_.gainsHalfDb, gains32, sizeof(gains32)) == 0;
    if (!samePlanKey) {
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

    DiracBiquadConfigSnapshot snapshot;
    snapshot.generation = ++generation_;
    memcpy(snapshot.gainsHalfDb, lastGoodGainsHalfDb_, sizeof(snapshot.gainsHalfDb));
    snapshot.diracEnabled = lastGoodEnabled_;
    snapshot.fallback = lastGoodFallback_;
    snapshot.volumeDb = lastGoodVolumeDb_;
    snapshot.plan = lastPlan_;
    Publish(snapshot);
    // The generation a reader gates on is published only after the snapshot it
    // belongs to, so a reader that sees a new generation always finds the new
    // state behind it.
    publishedGeneration_.store(snapshot.generation, std::memory_order_release);
}

void DiracBiquadConfigWatcher::Publish(const DiracBiquadConfigSnapshot &snapshot) {
    // The helper is the only writer, and a reader holds the guard only for the
    // struct copy, so this cannot spin for long.
    while (publishGuard_.test_and_set(std::memory_order_acquire)) {
    }
    published_ = snapshot;
    publishGuard_.clear(std::memory_order_release);
}
