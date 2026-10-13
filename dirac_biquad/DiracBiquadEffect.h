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

#include <stdint.h>

#include <hardware/audio.h>
#include <hardware/audio_effect.h>

#include "DiracBiquadConfig.h"
#include "DiracBiquadConfigWatcher.h"
#include "DiracBiquadFilter.h"

// The effect's semantics: the stream configuration, the route gate, the
// published snapshot's adoption and the engage crossfade. The exported C ABI is
// a thin adapter in EffectDiracBiquad.cpp; this class has no virtual function,
// so the adapter's handle can keep the interface pointer as its first word.
class DiracBiquadEffect {
  public:
    // Idempotent.
    int Init();

    // An unchanged rate, channel count and format is a no-op: AudioFlinger
    // re-sends a configuration to a live effect.
    int Configure(const effect_config_t &config);

    // A chain mid-fade is left alone; its gate handling owns the fade.
    void Reset();

    // Returns -ENODATA once a disabled effect has handed back to the dry stream.
    int32_t Process(const audio_buffer_t *inBuffer, audio_buffer_t *outBuffer);

    void SetEnabled(bool enabled);
    void SetDevice(audio_devices_t device);

    // Marks the effect uninitialised and stops the watcher thread.
    void Shutdown();

    bool Initialized() const { return state_ != kUninitialized; }
    const effect_config_t &Config() const { return config_; }
    void Dump(int fd) const;

  private:
    enum State { kUninitialized = 0, kInitialized, kActive };

    // Applies whatever the watcher has published. `fromSetConfig` is true only
    // on the SET_CONFIG path, the one place a stream configuration may be built.
    void ApplySnapshot(bool force, bool fromSetConfig);

    // Drops a plan that cannot be adopted, so the shaper never bends a stale
    // response.
    void AdoptPlanOrClear(const DiracBandPlan &plan);

    void PassThrough(const audio_buffer_t *inBuffer, audio_buffer_t *outBuffer) const;

    uint32_t state_ = kUninitialized;
    // True once any SET_CONFIG has been received.
    bool configured_ = false;
    bool enabled_ = false;

    audio_devices_t device_ = AUDIO_DEVICE_NONE;

    // Dirac-QEM parity state, reloaded on configure and on every device change.
    int gainsHalfDb_[DiracBiquadFilter::kBandCount] = {};
    bool diracEnabled_ = false;
    // The software-fallback switch: the effect is a fallback for routes the
    // HAL/DSP Dirac topology cannot reach, so it stays bypassed unless the user
    // extends Dirac to them.
    bool fallback_ = false;
    // Stream attenuation from `volume_db` in the QEM state file, kept for the
    // dump and the additive conf contract; nothing consumes it.
    double volumeDb_ = DiracBiquadConfig::kUnknownVolumeDb;
    // The snapshot generation the chain was last rebuilt from.
    uint64_t appliedGeneration_ = 0;
    // False while the stream goes out unprocessed: the framework's STARTING
    // bypass before the first ENABLE, and every frame this effect's gate closes.
    bool chainRunning_ = false;

    uint32_t planRefusals_ = 0;
    uint32_t buffersBeforeEngage_ = 0;

    // Owns the only thread that touches the QEM state file. The mixer thread
    // reads a published snapshot and takes no filesystem call.
    DiracBiquadConfigWatcher configWatcher_;

    DiracBiquadFilter filter_;
    effect_config_t config_ = {};
};
