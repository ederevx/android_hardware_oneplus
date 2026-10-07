/*
 * Copyright (c) 2026 The LineageOS Project
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

package org.lineageos.dirac.qem

import android.util.Log

/**
 * The one switch and sink for the per-change trace.
 *
 * [ENABLED] is a constant and [log] is inline, so a build with the flag off
 * compiles every call and its message away and pays nothing.
 *
 * The trace is one line per change, never one per frame: a frame is traced
 * only when it rebuilt the curve or overran [SLOW_FRAME_NS] on the UI thread.
 */
internal object DiracTrace {

    /**
     * Off for the publish build: every [log] call and its message is then
     * compiled away. The audio-verification lines do not go through here:
     * DiracQemRoute, the band unchanged/applied short-circuit and the worker's
     * push line are unconditional, so this switch can only ever silence the
     * frame-level timing (slider flush, curve draw, board/preview bind).
     */
    const val ENABLED = false

    /** A frame slower than this is traced even when it did not rebuild. */
    const val SLOW_FRAME_NS = 8_000_000L

    inline fun log(tag: String, message: () -> String) {
        if (ENABLED) {
            Log.d(tag, message())
        }
    }
}
