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

#include <pthread.h>
#include <stdint.h>

#include <atomic>
#include <condition_variable>
#include <mutex>

#include "Biquad.h"
#include "DiracBandPlan.h"
#include "DiracBandPlanner.h"
#include "DiracBiquadConfig.h"
#include "DiracGuardedValue.h"

// A complete, consistent copy of the QEM state file's observable state. This
// is what the audio thread reads; the file itself is only ever touched by the
// watcher's own thread.
struct DiracBiquadConfigSnapshot {
    uint64_t generation = 0;
    int gainsHalfDb[DiracBiquadConfig::kBandCount] = {};
    bool diracEnabled = false;
    bool fallback = false;
    double volumeDb = DiracBiquadConfig::kUnknownVolumeDb;
    // The bend plan for exactly `gainsHalfDb` at the rate the watcher was last
    // told. It is built before the snapshot is published, so a snapshot always
    // carries a plan for its own gains; the audio side adopts it only when its
    // key matches the state it has applied.
    DiracBandPlan plan;
    // The seven user-EQ sections for `gainsHalfDb` at `bandsRateHz`, designed by
    // the helper so a live gain change on the audio path only loads them. A
    // snapshot whose bands were designed at another rate must not be applied to
    // this stream.
    uint32_t bandsRateHz = 0;
    BiquadCoefficients userBands[DiracBiquadConfig::kBandCount] = {};
};

// Owns the module's only filesystem access.
//
// The effect used to stat() /data/vendor/audio/dirac_qem.conf every 4096
// frames (about 85 ms at 48 kHz) and to re-parse it when the mtime had moved,
// with a conditional fopen/fgets/fclose -- all from the effects HAL's
// process() call. That call runs on the effects-HAL process thread (a
// binderized HIDL service) with the AudioFlinger mixer thread synchronously
// waiting on it, so a filesystem call there is an unbounded tail-latency event
// under f2fs writeback or page-cache pressure. This class moves the whole
// check (stat and re-parse) onto one helper thread and publishes the parsed
// state as a snapshot.
//
// Synchronisation: one writer (the helper) and any number of lock-free readers
// (the effect's process path and its command handlers). The published snapshot
// is guarded by an atomic_flag that a reader only ever tries to take, so it
// never waits and never enters the kernel; the writer holds the flag only for
// the struct copy, never across stat()/fopen()/fgets()/fclose(). A reader that
// loses the race skips that read and keeps the state it already applied, which
// is at worst one poll interval stale -- the same guarantee the in-process
// throttle gave.
//
// This class also owns the bend planner and the last plan. It turns the conf
// state and the stream rate into a complete published snapshot, with the plan
// built on this helper thread before the snapshot is published and outside the
// publish guard, so the audio path never plans. The helper touches only these
// members and file-local data, never the effect, the filter or the shaper.
class DiracBiquadConfigWatcher {
  public:
    // The old in-process throttle accumulated frames and checked the file when
    // the count crossed this many, so it fired only at Process() entry and its
    // window was buffer-quantised. Dividing the count by the stream rate keeps
    // a wall-clock interval of the same order at every rate: 4096 * 1000 /
    // 48000 = 85 ms at 48 kHz by integer division, not 85.33.
    static constexpr unsigned kReloadCheckFrames = 4096;

    // A forced reload (configure, enable or device change) must land well
    // inside one polling interval. The helper looks at the request flag at this
    // granularity instead of sleeping a whole interval, so no caller ever has
    // to wake it and the audio path never takes a futex.
    static constexpr unsigned kReloadRequestPollMs = 2;

    DiracBiquadConfigWatcher();
    ~DiracBiquadConfigWatcher();

    DiracBiquadConfigWatcher(const DiracBiquadConfigWatcher &) = delete;
    DiracBiquadConfigWatcher &operator=(const DiracBiquadConfigWatcher &) = delete;

    // Spawns the helper and does the first read. Idempotent.
    void Start();
    // Signals the helper and joins it. A no-op when it was never started.
    void Stop();

    // Ties the poll interval to the stream rate. The helper is a wall-clock
    // timer, not a frame counter: the old in-process throttle accumulated
    // frames and checked only at Process() entry, so its window was
    // buffer-quantised, while this is 4096 * 1000 / rate ms -- 85 ms at 48 kHz
    // by integer division, not 85.33 -- and is not frame-aligned. Safe from any
    // thread; one relaxed store.
    void SetSampleRate(unsigned sampleRateHz);

    // Asks for a reload even when the file's mtime did not move: the old code
    // reloaded unconditionally on configure, enable and device change. One
    // relaxed store; no syscall and no wakeup.
    void RequestReload();

    // Copies the latest snapshot. Returns false only if a publication was being
    // copied at the same instant, in which case the caller keeps the state it
    // already applied. Never blocks.
    bool Read(DiracBiquadConfigSnapshot *out);

    // The generation of the latest published snapshot. A reader compares this
    // against the generation it has applied and skips the copy entirely when
    // nothing has moved: the snapshot carries the plan, and the audio path reads
    // one on every process() call. The value is stored after the snapshot it
    // belongs to, so a reader that sees a new generation always finds the new
    // state behind it.
    uint64_t SnapshotGeneration() const {
        return publishedGeneration_.load(std::memory_order_acquire);
    }

  private:
    // pthread_create entry point; `self` is the watcher.
    static void *ThreadMain(void *self);
    void Loop();
    void PublishOnce(bool force);
    void Publish(const DiracBiquadConfigSnapshot &snapshot);
    // Writer thread only.
    DiracBiquadConfigSnapshot MakeSnapshot(unsigned sampleRateHz, const Biquad *designed,
                                           const DiracBandPlan &plan);

    pthread_t thread_{};
    // True from a successful spawn until Stop() has joined the helper.
    bool threadStarted_ = false;
    std::mutex stopMutex_;
    std::condition_variable stopCondition_;
    bool stop_ = false;
    // Set once if the helper could not be spawned (EAGAIN and friends); the
    // watcher then keeps serving the last-good or seeded state and never
    // retries.
    bool threadStartFailed_ = false;

    std::atomic<bool> reloadRequested_{false};
    std::atomic<unsigned> sampleRateHz_{48000};

    // One writer (the helper), try-readers on the audio path.
    DiracGuardedValue<DiracBiquadConfigSnapshot> published_;

    // The plan builder and the plan last built for it. `lastPlan_` has exactly
    // one owner, this helper thread, and is rebuilt whenever the rate or the gains
    // it covers move; `plannedRate_` is the rate the last build actually covered,
    // so a rate that moved while a build ran is seen and replanned.
    DiracBandPlanner planner_;
    DiracBandPlan lastPlan_;
    unsigned plannedRate_ = 0;

    // Writer-only state, touched by the helper thread alone.
    int lastGoodGainsHalfDb_[DiracBiquadConfig::kBandCount];
    bool lastGoodEnabled_;
    bool lastGoodFallback_;
    double lastGoodVolumeDb_;
    int lastLoadError_;
    int64_t mtimeSec_;
    long mtimeNsec_;
    bool mtimeValid_;
    // The writer's own counter, and the copy of it readers gate on. The counter
    // moves before the snapshot is published, the published copy after, so the
    // gate can never lead the state.
    uint64_t generation_;
    std::atomic<uint64_t> publishedGeneration_{0};
};
