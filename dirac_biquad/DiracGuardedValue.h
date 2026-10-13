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

#include <atomic>
#include <type_traits>

// One writer, try-read many. A reader never waits and never enters the kernel;
// the writer holds the guard only for the value's copy. A reader that loses the
// race keeps the state it already has.
template <typename T>
class DiracGuardedValue {
  public:
    static_assert(std::is_trivially_copyable<T>::value,
                  "a guarded value crosses threads by copy");

    // Returns false when a writer held the guard, in which case `out` is left
    // untouched. Never spins.
    bool TryRead(T *out) const {
        if (guard_.test_and_set(std::memory_order_acquire)) {
            return false;
        }
        *out = value_;
        guard_.clear(std::memory_order_release);
        return true;
    }

    // Bounded spin: the guard is only ever held for the copy above, never across
    // a syscall or a build.
    void Write(const T &value) {
        while (guard_.test_and_set(std::memory_order_acquire)) {
        }
        value_ = value;
        guard_.clear(std::memory_order_release);
    }

  private:
    mutable std::atomic_flag guard_ = ATOMIC_FLAG_INIT;
    T value_{};
};
