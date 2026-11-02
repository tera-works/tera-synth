package com.hsynth.app

import android.widget.LinearLayout
import kotlin.math.roundToInt

/** Performance screen: preset selection, volume, strips, multi-touch keyboard. */
class PlayPanel(private val a: MainActivity) {
    val keyboard = KeyboardView(a)
    val statusText = Ui.text(a, "", 13f, Ui.DIM)
    private val presetLabel = Ui.text(a, "—", 16f, Ui.TEXT, bold = true)
    private val volumeId = Params.id("master.volume")
    private var volumeSeek = Ui.seek(a, 1000, 700) { p, user -> if (user) NativeEngine.setParam(volumeId, p / 1000f) }
    private var sustainOn = false
    val view: LinearLayout

    init {
        val d = { v: Int -> Ui.dp(a, v) }
        val octaveLabel = Ui.text(a, "", 14f, Ui.DIM)
        fun updateOctave() { octaveLabel.text = "C${keyboard.firstNote / 12 - 1}  ·  ${keyboard.whiteKeyCount} keys" }
        keyboard.firstNote = a.prefs.getInt("firstNote", 48)
        keyboard.whiteKeyCount = a.prefs.getInt("whiteKeys", 15)
        keyboard.slideMode = if (a.prefs.getBoolean("slideBend", false)) KeyboardView.SlideMode.BEND else KeyboardView.SlideMode.GLISS
        updateOctave()

        val slideBtn = Ui.button(a, "") {}
        fun updateSlide() { slideBtn.text = if (keyboard.slideMode == KeyboardView.SlideMode.BEND) "Slide: Bend" else "Slide: Gliss" }
        slideBtn.setOnClickListener {
            keyboard.slideMode = if (keyboard.slideMode == KeyboardView.SlideMode.BEND) KeyboardView.SlideMode.GLISS else KeyboardView.SlideMode.BEND
            a.prefs.edit().putBoolean("slideBend", keyboard.slideMode == KeyboardView.SlideMode.BEND).apply()
            updateSlide()
        }
        updateSlide()

        val sustainBtn = Ui.button(a, "Hold") {}
        sustainBtn.setOnClickListener {
            sustainOn = !sustainOn
            NativeEngine.control(NativeEngine.Control.SUSTAIN, if (sustainOn) 1f else 0f)
            sustainBtn.setTextColor(if (sustainOn) Ui.ACCENT else Ui.TEXT)
        }

        val top = Ui.row(a,
            Ui.button(a, "◀") { a.stepPreset(-1) },
            presetLabel,
            Ui.button(a, "▶") { a.stepPreset(1) },
        )
        presetLabel.setPadding(d(8), 0, d(8), 0)
        presetLabel.minWidth = d(170)
        val vol = Ui.text(a, "Vol", 13f, Ui.DIM)
        top.addView(vol)
        top.addView(volumeSeek, LinearLayout.LayoutParams(d(160), -2))
        top.addView(Ui.button(a, "Oct −") { keyboard.firstNote = (keyboard.firstNote - 12).coerceAtLeast(12); save(); updateOctave() })
        top.addView(Ui.button(a, "Oct +") { keyboard.firstNote = (keyboard.firstNote + 12).coerceAtMost(96); save(); updateOctave() })
        top.addView(Ui.button(a, "Keys −") { keyboard.whiteKeyCount -= 7; save(); updateOctave() })
        top.addView(Ui.button(a, "Keys +") { keyboard.whiteKeyCount += 7; save(); updateOctave() })
        top.addView(octaveLabel)
        listOf(slideBtn, sustainBtn, Ui.button(a, "Panic") {
            keyboard.allNotesOff()
            NativeEngine.control(NativeEngine.Control.ALL_SOUND_OFF, 0f)
        }).forEach { top.addView(it, Ui.lp(wrap = true).apply { marginStart = d(6) }) }
        top.setPadding(d(8), d(6), d(8), d(6))

        val bend = ControlStrip(a, "Bend", spring = true, bipolar = true) { NativeEngine.control(NativeEngine.Control.PITCH_BEND, it) }
        val mod = ControlStrip(a, "Mod", spring = false, bipolar = false) { NativeEngine.control(NativeEngine.Control.MOD_WHEEL, it) }
        val bottom = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(bend, LinearLayout.LayoutParams(d(64), -1))
            addView(mod, LinearLayout.LayoutParams(d(64), -1))
            addView(keyboard, LinearLayout.LayoutParams(0, -1, 1f))
        }
        view = Ui.column(a).apply {
            // the control row scrolls horizontally on narrow screens
            addView(android.widget.HorizontalScrollView(a).apply { addView(top) }, Ui.lp())
            addView(bottom, Ui.lp(weight = 1f, height = 0).apply { width = -1 })
        }
        a.addPresetListener { presetLabel.text = a.currentPresetName; syncVolume() }
        Params.addListener { syncVolume() }
    }

    private fun save() = a.prefs.edit().putInt("firstNote", keyboard.firstNote).putInt("whiteKeys", keyboard.whiteKeyCount).apply()

    private fun syncVolume() { volumeSeek.progress = (NativeEngine.nativeGetParam(volumeId) * 1000).roundToInt() }

    fun updateStats(s: FloatArray) {
        val voices = s[0].toInt()
        val maxV = s[24].toInt()
        val load = (s[2] * 100).roundToInt()
        val xr = s[13].toInt()
        val audio = if (s[8] > 0.5f) "${s[9].toInt() / 1000.0}k/${s[11].toInt()}" else "audio off"
        statusText.text = "voices $voices/$maxV   DSP $load%   xruns ${if (xr < 0) "?" else xr}   $audio  "
        statusText.setTextColor(if (s[3] > 0.8f || load > 70) Ui.WARN else Ui.DIM)
    }
}
