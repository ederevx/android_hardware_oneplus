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
    sampleRateHz_.store(sampleRateHz == 0 ? 48000u : sampleRateHz, std::memory_order_relaxed);
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
            if (reloadRequested_.load(std::memory_order_acquire)) {
                break;
            }
        }
        if (stop_) {
            break;
        }

        lock.unlock();
        if (reloadRequested_.exchange(false, std::memory_order_acq_rel)) {
            PublishOnce(/*force=*/true);
        } else {
            PublishOnce(/*force=*/false);
        }
        lock.lock();
    }
}

void DiracBiquadConfigWatcher::PublishOnce(bool force) {
    struct stat st;
    if (force) {
        // Record the mtime without gating on it: a forced reload must re-parse
        // even when the file's timestamp did not move.
        if (stat(DiracBiquadConfig::ConfigPath(), &st) == 0) {
            mtimeSec_ = static_cast<int64_t>(st.st_mtim.tv_sec);
            mtimeNsec_ = st.st_mtim.tv_nsec;
            mtimeValid_ = true;
        }
    } else {
        if (stat(DiracBiquadConfig::ConfigPath(), &st) != 0) {
            return;
        }
        if (mtimeValid_ && static_cast<int64_t>(st.st_mtim.tv_sec) == mtimeSec_ &&
            st.st_mtim.tv_nsec == mtimeNsec_) {
            return;
        }
        mtimeSec_ = static_cast<int64_t>(st.st_mtim.tv_sec);
        mtimeNsec_ = st.st_mtim.tv_nsec;
        mtimeValid_ = true;
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
    } else if (error != lastLoadError_) {
        // Keep the last good state: an unreadable or torn file must never
        // silently convert the effect to a full pass-through. Log each distinct
        // failure once, so an EACCES is visible without flooding logcat.
        lastLoadError_ = error;
        ALOGW("%s: state file %s unusable (%s); keeping last good state", __func__,
              DiracBiquadConfig::ConfigPath(), strerror(error));
    }

    DiracBiquadConfigSnapshot snapshot;
    snapshot.generation = ++generation_;
    memcpy(snapshot.gainsHalfDb, lastGoodGainsHalfDb_, sizeof(snapshot.gainsHalfDb));
    snapshot.diracEnabled = lastGoodEnabled_;
    snapshot.fallback = lastGoodFallback_;
    snapshot.volumeDb = lastGoodVolumeDb_;
    Publish(snapshot);
}

void DiracBiquadConfigWatcher::Publish(const DiracBiquadConfigSnapshot &snapshot) {
    // The helper is the only writer, and a reader holds the guard only for the
    // struct copy, so this cannot spin for long.
    while (publishGuard_.test_and_set(std::memory_order_acquire)) {
    }
    published_ = snapshot;
    publishGuard_.clear(std::memory_order_release);
}
