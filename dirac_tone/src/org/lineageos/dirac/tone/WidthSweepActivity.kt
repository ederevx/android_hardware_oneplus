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

package org.lineageos.dirac.tone

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.text.InputType
import android.util.Log
import android.view.Gravity
import android.view.WindowManager
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.SeekBar
import android.widget.TextView

/**
 * DEV PROBE, not a shipped feature.
 *
 * A draggable Sum/Diff control for the route the user is listening to, so the
 * balance can be swept by ear instead of judged in 0.0/1.0 jumps. It emits the
 * same frame the silent probe sends and nothing else: one parameter
 * (0x12D02 by default), one setParameters call per step, `cal_devid=0` with no
 * cal_snddevid, and the value as a 4-byte float.
 *
 * The range is 0.00 to 2.00 rather than 0..1. The higher half has no meaning in
 * the module's own scale - its weights are (1-v, 1+v) - so it is there to be
 * unambiguous: if the DSP applies the parameter at all, an extreme must be
 * obvious, and if it silently clamps, refuses or misbehaves past 1.0 that is
 * visible rather than inferred. An exact value can also be typed, because a
 * drag cannot hit 1.250.
 *
 * It opens NO stream of its own. The music already provides the live output the
 * loader resolves against, and a probe-owned stream has already been shown to
 * leak the vendor HAL's primary output when its writes fail. It also means the
 * control works on the DSP-voiced routes, where the shipping app's own row is
 * inert by design - this touches nothing that ships.
 *
 * A fast drag would otherwise emit dozens of HAL writes a second, so changes are
 * coalesced: the UI records the newest value and a sender thread pushes at most
 * one frame per [SEND_INTERVAL_MS], always the latest. The final value of a drag
 * always lands.
 *
 * Trigger, for the wired route (the defaults):
 *
 *   adb shell am start -n org.lineageos.dirac.tone/.WidthSweepActivity
 *
 * Extras, all optional: topos (0x10012D01), module (0x12D01), param (0x12D02),
 * devids (0, which is what emits cal_devid=0 and no cal_snddevid), apptype
 * (69936), rate (48000), caltype (1, the raw path), persist (0). `--ez noenable
 * true` skips the one-time enable frame, which is otherwise sent first so that
 * the module is not bypassed when the balance lands.
 */
class WidthSweepActivity : Activity() {
    private val frames by lazy { QemFrames(this) }

    /** Newest value the UI produced; the sender always pushes this one. */
    @Volatile
    private var pending: Float? = null

    @Volatile
    private var running = true

    private var sender: Thread? = null
    private lateinit var valueLabel: TextView
    private lateinit var valueField: EditText
    private lateinit var slider: SeekBar
    private lateinit var config: Config

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        config = Config.from(intent)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        valueLabel = TextView(this).apply {
            gravity = Gravity.CENTER
            textSize = VALUE_TEXT_SP
        }
        valueField = EditText(this).apply {
            inputType = InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_DECIMAL
            hint = "exact value, 0.000 - 2.000"
            gravity = Gravity.CENTER
        }
        slider = SeekBar(this).apply {
            max = SEEK_MAX
            progress = 0
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(bar: SeekBar?, progress: Int, fromUser: Boolean) {
                    setValue(progress / STEPS_PER_UNIT.toFloat())
                }

                override fun onStartTrackingTouch(bar: SeekBar?) = Unit

                override fun onStopTrackingTouch(bar: SeekBar?) = Unit
            })
        }
        val sendExact = Button(this).apply {
            text = "Send exact"
            setOnClickListener { setValue(valueField.text.toString().toFloatOrNull()) }
        }
        val presets = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER
            PRESETS.forEach { value ->
                addView(Button(this@WidthSweepActivity).apply {
                    text = "%.2f".format(value)
                    setOnClickListener { setValue(value) }
                })
            }
        }
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(ROW_PADDING_DP, ROW_PADDING_DP, ROW_PADDING_DP, ROW_PADDING_DP)
            addView(TextView(this@WidthSweepActivity).apply {
                text = config.describe()
                textSize = INFO_TEXT_SP
            })
            addView(valueLabel)
            addView(slider)
            addView(valueField)
            addView(sendExact)
            addView(presets)
        }
        setContentView(root)

        Log.i(TAG, "sweep up: $config range=0.00..${"%.2f".format(MAX_VALUE)}")
        if (config.enableFirst) {
            frames.send(
                topo = config.topo,
                appType = config.appType,
                sndDevId = config.sndDevId,
                sampleRate = config.sampleRate,
                calType = QemFrames.CAL_TYPE_ACDB,
                persist = config.persist,
                module = config.module,
                param = QemFrames.PARAM_ENABLE,
                value = QemFrames.ENABLE,
            )
        }
        setValue(BYPASS_VALUE)
        sender = Thread { sendLoop() }.also { it.start() }
    }

    override fun onDestroy() {
        running = false
        sender?.let { runCatching { it.join(JOIN_MS) } }
        sender = null
        Log.i(TAG, "sweep down")
        super.onDestroy()
    }

    /** The one place a value from the field, a preset or the slider lands. */
    private fun setValue(raw: Float?) {
        val value = (raw ?: return).coerceIn(0f, MAX_VALUE)
        pending = value
        valueLabel.text = label(value)
        valueField.setText("%.3f".format(value))
        slider.progress = (value * STEPS_PER_UNIT).toInt()
    }

    /** Pushes at most one frame per interval, always the newest value. */
    private fun sendLoop() {
        var last: Float? = null
        try {
            while (running) {
                val value = pending
                if (value != null && value != last) {
                    last = value
                    frames.send(
                        topo = config.topo,
                        appType = config.appType,
                        sndDevId = config.sndDevId,
                        sampleRate = config.sampleRate,
                        calType = config.calType,
                        persist = config.persist,
                        module = config.module,
                        param = config.param,
                        floatValue = value,
                    )
                }
                Thread.sleep(SEND_INTERVAL_MS)
            }
        } catch (t: Throwable) {
            Log.e(TAG, "send loop stopped", t)
        }
    }

    private fun label(value: Float): String = "%.3f".format(value) +
        if (value == BYPASS_VALUE) "  (bypass)" else ""

    /** What this run pushes; every part overridable from the trigger. */
    private data class Config(
        val topo: Int,
        val module: Int,
        val param: Int,
        val sndDevId: Int,
        val appType: Int,
        val sampleRate: Int,
        val calType: Int,
        val persist: Int,
        val enableFirst: Boolean,
    ) {
        fun describe(): String =
            "topo=0x${Integer.toHexString(topo)} module=0x${Integer.toHexString(module)} " +
                "param=0x${Integer.toHexString(param)} snddev=$sndDevId apptype=$appType " +
                "rate=$sampleRate caltype=$calType"

        companion object {
            fun from(intent: Intent?): Config = Config(
                topo = intExtra(intent, "topos", default = DEFAULT_TOPO),
                module = intExtra(intent, "module", default = DEFAULT_MODULE),
                param = intExtra(intent, "param", default = DEFAULT_PARAM),
                sndDevId = intExtra(intent, "devids", default = DEFAULT_SND_DEV_ID),
                appType = intExtra(intent, "apptype", default = DEFAULT_APP_TYPE),
                sampleRate = intExtra(intent, "rate", default = DEFAULT_RATE),
                calType = intExtra(intent, "caltype", default = QemFrames.CAL_TYPE_RAW),
                persist = intExtra(intent, "persist", default = DEFAULT_PERSIST),
                enableFirst = !boolExtra(intent, "noenable"),
            )

            private fun intExtra(intent: Intent?, name: String, default: Int): Int {
                val raw = intent?.extras?.get(name) ?: return default
                return when (raw) {
                    is Int -> raw
                    is Long -> raw.toInt()
                    is String -> parseNumber(raw) ?: default
                    else -> default
                }
            }

            private fun boolExtra(intent: Intent?, name: String): Boolean =
                intent?.extras?.get(name)?.let { raw ->
                    when (raw) {
                        is Boolean -> raw
                        is Int -> raw != 0
                        is String -> raw.equals("true", ignoreCase = true) || raw == "1"
                        else -> false
                    }
                } ?: false

            private const val DEFAULT_TOPO = 0x10012D01
            private const val DEFAULT_MODULE = QemFrames.MODULE_EXTERNAL
            private const val DEFAULT_PARAM = QemFrames.PARAM_SUMDIFF

            /** 0 is the framing that emits cal_devid=0 and no cal_snddevid. */
            private const val DEFAULT_SND_DEV_ID = 0
            private const val DEFAULT_APP_TYPE = 69936
            private const val DEFAULT_RATE = 48000
            private const val DEFAULT_PERSIST = 0
        }
    }

    private companion object {
        const val TAG = "DiracToneProbe"
        const val STEPS_PER_UNIT = 1000
        const val MAX_VALUE = 2.0f
        const val SEEK_MAX = (MAX_VALUE * STEPS_PER_UNIT).toInt()
        const val SEND_INTERVAL_MS = 60L
        const val JOIN_MS = 300L
        const val BYPASS_VALUE = 0.0f
        val PRESETS = listOf(0.0f, 0.5f, 1.0f, 1.5f, 2.0f)
        const val ROW_PADDING_DP = 24
        const val VALUE_TEXT_SP = 34f
        const val INFO_TEXT_SP = 12f
    }
}
