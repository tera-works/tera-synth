package com.hsynth.app

import android.view.Gravity
import android.view.View
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.TextView
import kotlin.math.roundToInt

/**
 * Performance screen, laid out like the top panel of a keyboard synth:
 * preset display with up / down, quick knobs (volume, filter, envelope,
 * macros), keyboard buttons, then pitch / mod wheels and the keyboard.
 */
class PlayPanel(private val a: MainActivity) {
    val keyboard = KeyboardView(a)
    val statusText = Ui.text(a, "", 13f, Hw.LCD_DIM, tr = false)
    private val presetLcd = Hw.lcd(a, "—", 20f)
    private val presetSub = Ui.text(a, "", 11f, Hw.LCD_DIM, tr = false).apply { typeface = Hw.MONO }
    private val quick = mutableListOf<ParamControl>()
    private var sustainOn = false
    private var keyValues: List<() -> Unit> = emptyList()
    private var recButton: LedButton? = null
    val view: LinearLayout

    init {
        val d = { v: Int -> Ui.dp(a, v) }
        keyboard.firstNote = a.prefs.getInt("firstNote", 48)
        keyboard.whiteKeyCount = a.prefs.getInt(keysPref, if (Ui.portrait(a)) 8 else 15)
        keyboard.transpose = a.prefs.getInt("transpose", 0)
        keyboard.slideMode = if (a.prefs.getBoolean("slideBend", false)) KeyboardView.SlideMode.BEND else KeyboardView.SlideMode.GLISS

        val panic = LedButton(a, "Panic", led = false).apply {
            textSize = 12f
            setPadding(d(4), paddingTop, d(4), paddingBottom)
            setOnClickListener {
                keyboard.allNotesOff()
                NativeEngine.control(NativeEngine.Control.ALL_SOUND_OFF, 0f)
            }
        }
        // preset section: LCD + up / down
        val preset = Hw.Section(a, "Preset")
        val lcdCol = LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            background = Hw.lcdBg(a)
            setPadding(d(10), d(4), d(10), d(4))
            presetLcd.background = null
            presetLcd.setPadding(0, 0, 0, 0)
            addView(presetLcd)
            addView(presetSub)
        }
        preset.body.addView(lcdCol, LinearLayout.LayoutParams(-1, -2))
        preset.body.addView(LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(LedButton(a, "◀", led = false).apply { setOnClickListener { a.stepPreset(-1) } }, LinearLayout.LayoutParams(0, -2, 1f).apply { marginEnd = d(6) })
            addView(LedButton(a, "▶", led = false).apply { setOnClickListener { a.stepPreset(1) } }, LinearLayout.LayoutParams(0, -2, 1f).apply { marginEnd = d(6) })
            addView(panic, LinearLayout.LayoutParams(0, -2, 1.4f))
        }, LinearLayout.LayoutParams(-1, -2).apply { topMargin = d(6) })

        // quick knobs
        val knobs = Hw.Section(a, "Controls")
        val knobRow = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL }
        for (n in listOf("master.volume", "filter.cutoff", "filter.reso", "amp.attack", "amp.release")) {
            val m = Params.meta.firstOrNull { it.name == n } ?: continue
            val c = ParamControl(a, m, knobSize = 46f)
            quick += c
            knobRow.addView(c.view, LinearLayout.LayoutParams(0, -2, 1f))
        }
        knobRow.gravity = Gravity.CENTER_HORIZONTAL
        // ARP page: arpeggiator / sequencer on, mode, rate, tempo, transport
        val arpRow = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        for (n in listOf("arp.on", "arp.mode", "arp.rate", "fx.tempo")) {
            val m = Params.meta.firstOrNull { it.name == n } ?: continue
            val c = ParamControl(a, m, knobSize = 46f, lcdWidth = 84f)
            quick += c
            arpRow.addView(c.view, LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(6) })
        }
        val rec = LedButton(a, "Rec").apply {
            textSize = 12f
            minHeight = d(30)
            setOnClickListener { lit = a.toggleRecording() }
        }
        recButton = rec
        val run = LedButton(a, "Run").apply {
            textSize = 12f
            minHeight = d(30)
            setOnClickListener { lit = !lit; NativeEngine.nativeSetTransport(lit) }
        }
        val pages = listOf(knobRow, arpRow)
        val tabs = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL }
        val tabButtons = listOf("Sound", "Arp / Tempo").mapIndexed { i, t ->
            LedButton(a, t).apply { textSize = 12f; minHeight = d(30) }.also { b ->
                tabs.addView(b, LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(6) })
            }
        }
        fun showPage(i: Int) {
            pages.forEachIndexed { k, v -> v.visibility = if (k == i) View.VISIBLE else View.GONE }
            tabButtons.forEachIndexed { k, b -> b.lit = k == i }
            run.lit = NativeEngine.nativeTransport()
        }
        tabButtons.forEachIndexed { i, b -> b.setOnClickListener { showPage(i) } }
        tabs.addView(View(a), LinearLayout.LayoutParams(0, 1, 1f))
        tabs.addView(rec, LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(6) })
        tabs.addView(run, LinearLayout.LayoutParams(-2, -2))
        knobs.body.addView(tabs, LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = d(4) })
        knobs.body.addView(knobRow, LinearLayout.LayoutParams(-1, -2))
        knobs.body.addView(arpRow, LinearLayout.LayoutParams(-1, -2))
        showPage(0)

        // keyboard buttons (3 rows); the range shows on the preset LCD
        val keys = Hw.Section(a, "Keyboard")
        val slide = ToggleSwitch(a, "Bend", vertical = false)
        fun updateSlide() { slide.on = keyboard.slideMode == KeyboardView.SlideMode.BEND }
        slide.onToggle = { on ->
            keyboard.slideMode = if (on) KeyboardView.SlideMode.BEND else KeyboardView.SlideMode.GLISS
            a.prefs.edit().putBoolean("slideBend", on).apply()
        }
        val hold = ToggleSwitch(a, "Hold", vertical = false)
        hold.onToggle = { on ->
            sustainOn = on
            NativeEngine.control(NativeEngine.Control.SUSTAIN, if (on) 1f else 0f)
            a.midiOut.cc(64, if (on) 1f else 0f)
        }
        fun row(vararg v: View) = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            v.forEachIndexed { i, x ->
                (x as? TextView)?.textSize = 12f
                if (x is LedButton) x.setPadding(d(16), x.paddingTop, d(4), x.paddingBottom)
                addView(x, LinearLayout.LayoutParams(0, -2, 1f).apply { if (i < v.size - 1) marginEnd = d(5) })
            }
        }
        // ◀ value ▶ with the function printed underneath, as on a synth panel
        val values = mutableListOf<() -> Unit>()
        fun stepper(label: String, value: () -> String, down: () -> Unit, up: () -> Unit) = LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            val lcd = Hw.lcd(a, "", 13f).apply { gravity = Gravity.CENTER; setPadding(0, paddingTop, 0, paddingBottom) }
            values += { lcd.text = value() }
            fun arrow(t: String, f: () -> Unit) = LedButton(a, t, led = false).apply {
                textSize = 11f
                setPadding(0, paddingTop, 0, paddingBottom)
                setOnClickListener { f(); save(); refresh() }
            }
            addView(LinearLayout(a).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                addView(arrow("◀", down), LinearLayout.LayoutParams(d(26), -2))
                addView(lcd, LinearLayout.LayoutParams(0, -2, 1f).apply { marginStart = d(3); marginEnd = d(3) })
                addView(arrow("▶", up), LinearLayout.LayoutParams(d(26), -2))
            }, LinearLayout.LayoutParams(-1, -2))
            addView(Hw.engraved(a, label, 10.5f).apply { gravity = Gravity.CENTER }, LinearLayout.LayoutParams(-1, -2).apply { topMargin = d(3) })
        }
        fun signed(v: Int) = if (v > 0) "+$v" else "$v"
        keys.body.addView(LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(stepper("Octave", { signed((keyboard.firstNote - 48) / 12) },
                { keyboard.firstNote = (keyboard.firstNote - 12).coerceAtLeast(12) },
                { keyboard.firstNote = (keyboard.firstNote + 12).coerceAtMost(96) }), LinearLayout.LayoutParams(0, -2, 1f).apply { marginEnd = d(6) })
            addView(stepper("Transpose", { signed(keyboard.transpose) },
                { keyboard.transpose -= 1 }, { keyboard.transpose += 1 }), LinearLayout.LayoutParams(0, -2, 1f).apply { marginEnd = d(6) })
            addView(stepper("Keys", { "${keyboard.whiteKeyCount}" },
                { keyboard.whiteKeyCount -= 7 }, { keyboard.whiteKeyCount += 7 }), LinearLayout.LayoutParams(0, -2, 1f))
        }, LinearLayout.LayoutParams(-1, -2))
        keyValues = values
        keys.body.addView(row(slide, hold), LinearLayout.LayoutParams(-1, -2).apply { topMargin = d(5) })
        updateSlide()

        val top = if (Ui.portrait(a)) LinearLayout(a).apply {
            // portrait: preset + keyboard buttons, then the controls
            orientation = LinearLayout.VERTICAL
            setPadding(d(8), d(8), d(8), d(6))
            addView(preset, LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = d(8) })
            addView(keys, LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = d(8) })
            addView(knobs, LinearLayout.LayoutParams(-1, -2))
        } else LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(d(8), d(8), d(8), d(6))
            addView(preset, LinearLayout.LayoutParams(d(150), -1).apply { marginEnd = d(8) })
            addView(knobs, LinearLayout.LayoutParams(0, -1, 1f).apply { marginEnd = d(8) })
            addView(keys, LinearLayout.LayoutParams(d(262), -1))
        }

        val bend = ControlStrip(a, "Bend", spring = true, bipolar = true) { NativeEngine.control(NativeEngine.Control.PITCH_BEND, it); a.midiOut.bend(it) }
        val mod = ControlStrip(a, "Mod", spring = false, bipolar = false) { NativeEngine.control(NativeEngine.Control.MOD_WHEEL, it); a.midiOut.cc(1, it) }
        keyboard.onNote = { on, key, vel -> if (on) a.midiOut.noteOn(key, vel) else a.midiOut.noteOff(key) }
        val bottom = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            setBackgroundColor(Hw.PANEL)
            setPadding(d(4), d(4), 0, 0)
            addView(bend, LinearLayout.LayoutParams(d(58), -1))
            addView(mod, LinearLayout.LayoutParams(d(58), -1).apply { marginEnd = d(4) })
            addView(keyboard, LinearLayout.LayoutParams(0, -1, 1f))
        }
        view = Ui.column(a).apply {
            // the panel scrolls sideways on narrow screens
            addView(top, Ui.lp())
            addView(bottom, Ui.lp(weight = 1f, height = 0).apply { width = -1 })
        }
        a.addPresetListener { refresh() }
        Params.addListener { refresh() }
        refresh()
    }

    private fun refresh() {
        presetLcd.text = a.currentPresetName
        val kind = a.currentPreset?.parentFile?.name?.let { if (it == "user") "User" else "Factory" } ?: ""
        presetSub.text = kind
        quick.forEach { it.refresh() }
        keyValues.forEach { it() }
    }

    private val keysPref get() = if (Ui.portrait(a)) "whiteKeysPortrait" else "whiteKeys"
    private fun save() = a.prefs.edit().putInt("firstNote", keyboard.firstNote).putInt(keysPref, keyboard.whiteKeyCount)
        .putInt("transpose", keyboard.transpose).apply()

    fun updateStats(s: FloatArray) {
        val voices = s[0].toInt()
        val maxV = s[24].toInt()
        val load = (s[2] * 100).roundToInt()
        val xr = s[13].toInt()
        val audio = if (s[8] > 0.5f) "${s[9].toInt() / 1000.0}k/${s[11].toInt()}" else L10n.t("audio off")
        val rs = NativeEngine.nativeRecordSeconds()
        recButton?.lit = rs >= 0
        if (rs >= 0) {
            statusText.text = "● REC %d:%02d".format(rs.toInt() / 60, rs.toInt() % 60)
            statusText.setTextColor(Ui.ERR)
            return
        }
        statusText.text = if (s[8] > 0.5f) "VOICE $voices/$maxV  DSP $load%  XRUN ${if (xr < 0) "?" else xr}" else audio
        statusText.setTextColor(if (s[3] > 0.8f || load > 70) Ui.WARN else Hw.LCD_DIM)
    }
}
