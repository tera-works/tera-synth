package com.hsynth.app

import android.graphics.Color
import android.text.TextUtils
import android.view.Gravity
import android.view.View
import android.widget.LinearLayout
import android.widget.TextView
import kotlin.math.roundToInt

/**
 * Obsidian Instrument Play screen:
 * Compact top control bar (preset switcher, color-coded vector arc knobs / Arp toggle,
 * keyboard controls + OLED NIGHT KEYS toggle) maximizing vertical height for the
 * multi-touch keyboard and pitch/mod ribbons on Pixel 7's 20:9 landscape OLED.
 */
class PlayPanel(private val a: MainActivity) {
    val keyboard = KeyboardView(a)
    val statusText = Ui.text(a, "", 11f, Hw.ENGRAVE, tr = false)
    private val presetLcd = TextView(a).apply {
        text = "—"
        textSize = 15f
        typeface = Hw.BOLD
        setTextColor(Hw.SILK)
        maxLines = 1
        ellipsize = TextUtils.TruncateAt.END
    }
    private val presetSub = TextView(a).apply {
        text = ""
        textSize = 10f
        typeface = Hw.MONO
        setTextColor(Hw.MOD_OSC)
        maxLines = 1
    }
    private val quick = mutableListOf<ParamControl>()
    private var sustainOn = false
    private var keyValues: List<() -> Unit> = emptyList()
    private var recButton: LedButton? = null
    private var rhythm: RhythmBar? = null
    val view: LinearLayout

    /**
     * B3: once the current part's zone is routed EXT / BOTH (Mix id 10, `Mix::ZoneMode != 0`) the engine itself sends the
     * on-screen keyboard to the external port, so the legacy single-port `a.midiOut` send would double every event.
     */
    private fun legacyOutAllowed(): Boolean = NativeEngine.nativeGetPartMix(NativeEngine.nativeEditPart(), 10) < 0.5f

    /** Keys whose note-on went out through the legacy port: their note-off must follow even if the route changes meanwhile. */
    private val legacyHeld = BooleanArray(128)

    init {
        val d = { v: Int -> Ui.dp(a, v) }
        keyboard.firstNote = a.prefs.getInt("firstNote", 48)
        keyboard.whiteKeyCount = a.prefs.getInt(keysPref, if (Ui.portrait(a)) 8 else 15)
        keyboard.transpose = a.prefs.getInt("transpose", 0)
        keyboard.slideMode = if (a.prefs.getBoolean("slideBend", false)) KeyboardView.SlideMode.BEND else KeyboardView.SlideMode.GLISS
        keyboard.nightKeys = a.prefs.getBoolean("nightKeys", false)

        // 1. Preset mini-card
        val presetCard = LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            background = Hw.panelBg(a)
            setPadding(d(10), d(7), d(10), d(7))
            addView(presetSub)
            addView(presetLcd, LinearLayout.LayoutParams(-1, -2).apply { topMargin = d(1); bottomMargin = d(6) })
            addView(LinearLayout(a).apply {
                orientation = LinearLayout.HORIZONTAL
                addView(LedButton(a, "◀", tr = false, led = false).apply {
                    minHeight = d(32); textSize = 12f
                    setOnClickListener { a.stepPreset(-1) }
                }, LinearLayout.LayoutParams(0, -2, 1f).apply { marginEnd = d(4) })
                addView(LedButton(a, "▶", tr = false, led = false).apply {
                    minHeight = d(32); textSize = 12f
                    setOnClickListener { a.stepPreset(1) }
                }, LinearLayout.LayoutParams(0, -2, 1f).apply { marginEnd = d(4) })
                addView(LedButton(a, "PANIC", tr = false, led = false, accentColor = Hw.MOD_RED).apply {
                    minHeight = d(32); textSize = 10.5f; setTextColor(Hw.MOD_RED)
                    setOnClickListener {
                        keyboard.allNotesOff()
                        NativeEngine.control(NativeEngine.Control.ALL_SOUND_OFF, 0f)
                    }
                }, LinearLayout.LayoutParams(0, -2, 1.35f))
            }, LinearLayout.LayoutParams(-1, -2))
        }

        // 2. Quick Performance Knobs (color-coded: Cyan Vol, Amber Cutoff/Reso, Violet Env) + Arp page
        val knobsCard = LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            background = Hw.panelBg(a)
            setPadding(d(8), d(6), d(8), d(4))
        }
        val knobRow = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER }
        val knobSpecs = listOf(
            "master.volume" to Hw.MOD_OSC,
            "filter.cutoff" to Hw.MOD_FILTER,
            "filter.reso" to Hw.MOD_FILTER,
            "amp.attack" to Hw.MOD_ENV,
            "amp.release" to Hw.MOD_ENV,
        )
        for ((n, col) in knobSpecs) {
            val m = Params.meta.firstOrNull { it.name == n } ?: continue
            val c = ParamControl(a, m, knobSize = 44f, accent = col)
            quick += c
            knobRow.addView(c.view, LinearLayout.LayoutParams(0, -2, 1f))
        }

        val arpRow = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        for (n in listOf("arp.on", "arp.mode", "arp.rate", "fx.tempo")) {
            val m = Params.meta.firstOrNull { it.name == n } ?: continue
            val c = ParamControl(a, m, knobSize = 42f, lcdWidth = 78f, accent = Hw.MOD_FX)
            quick += c
            arpRow.addView(c.view, LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(6) })
        }

        // audio (WAV) recorder of the output; the phrase recorder lives on the PHRASE page (REC there = MIDI phrase)
        val rec = LedButton(a, "WAV●", tr = false, led = true, accentColor = Hw.MOD_RED).apply {
            textSize = 10.5f; minHeight = d(26); setPadding(d(15), d(2), d(8), d(2))
            setOnClickListener { lit = a.toggleRecording() }
        }
        recButton = rec
        val run = LedButton(a, "RUN", tr = false, led = true, accentColor = Hw.MOD_FX).apply {
            textSize = 10.5f; minHeight = d(26); setPadding(d(15), d(2), d(8), d(2))
            setOnClickListener { lit = !lit; NativeEngine.nativeSetTransport(lit) }
        }
        // U2: PHRASE page (2 rows x 34 dp = 72 dp <= the 80 dp knob row, so the top card keeps its height)
        val phraseRow = PhrasePanelView(a, wide = false)
        // A6: RHYTHM page — groove selector / PLAY / FILL / VOL (+ strum toggle, time, order and swing); 2 x 34 dp + gap = 72 dp
        val rhythmRow = RhythmBar(a, compact = true)
        rhythm = rhythmRow
        val pages = listOf<View>(knobRow, arpRow, phraseRow, rhythmRow)
        val tabBar = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        val pageBtns = listOf("SOUND" to Hw.MOD_OSC, "ARP" to Hw.MOD_FX, "PHRASE" to Hw.MOD_RED, "RHYTHM" to Hw.MOD_FILTER).mapIndexed { i, (t, col) ->
            LedButton(a, t, tr = false, led = true, accentColor = col).apply {
                textSize = 10.5f; minHeight = d(26); setPadding(d(15), d(2), d(8), d(2))
            }.also { b -> tabBar.addView(b, LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(5) }) }
        }
        fun showPage(i: Int) {
            pages.forEachIndexed { k, v -> v.visibility = if (k == i) View.VISIBLE else View.GONE }
            pageBtns.forEachIndexed { k, b -> b.lit = k == i }
            run.lit = NativeEngine.nativeTransport()
        }
        pageBtns.forEachIndexed { i, b -> b.setOnClickListener { showPage(i) } }
        tabBar.addView(View(a), LinearLayout.LayoutParams(0, 1, 1f))
        tabBar.addView(rec, LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(4) })
        tabBar.addView(run, LinearLayout.LayoutParams(-2, -2))
        knobsCard.addView(tabBar, LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = d(3) })
        knobsCard.addView(knobRow, LinearLayout.LayoutParams(-1, -2))
        knobsCard.addView(arpRow, LinearLayout.LayoutParams(-1, -2))
        knobsCard.addView(phraseRow, LinearLayout.LayoutParams(-1, -2))
        knobsCard.addView(rhythmRow, LinearLayout.LayoutParams(-1, -2))
        showPage(0)

        // 3. Keyboard controls card + NIGHT KEYS (OLED dark keybed toggle)
        val keysCard = LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            background = Hw.panelBg(a)
            setPadding(d(8), d(6), d(8), d(6))
        }
        val values = mutableListOf<() -> Unit>()
        fun stepper(label: String, value: () -> String, down: () -> Unit, up: () -> Unit) = LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            val lcd = Hw.lcd(a, "", 11.5f).apply { gravity = Gravity.CENTER; setPadding(0, d(3), 0, d(3)) }
            values += { lcd.text = value() }
            fun arrow(t: String, f: () -> Unit) = LedButton(a, t, tr = false, led = false).apply {
                textSize = 10f; minHeight = d(26); setPadding(0, d(2), 0, d(2))
                setOnClickListener { f(); save(); refresh() }
            }
            addView(LinearLayout(a).apply {
                orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL
                addView(arrow("◀", down), LinearLayout.LayoutParams(d(22), -2))
                addView(lcd, LinearLayout.LayoutParams(0, -2, 1f).apply { marginStart = d(2); marginEnd = d(2) })
                addView(arrow("▶", up), LinearLayout.LayoutParams(d(22), -2))
            }, LinearLayout.LayoutParams(-1, -2))
            addView(Hw.engraved(a, label, 9f).apply { gravity = Gravity.CENTER }, LinearLayout.LayoutParams(-1, -2).apply { topMargin = d(2) })
        }
        fun signed(v: Int) = if (v > 0) "+$v" else "$v"
        keysCard.addView(LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(stepper("Octave", { signed((keyboard.firstNote - 48) / 12) },
                { keyboard.firstNote = (keyboard.firstNote - 12).coerceAtLeast(12) },
                { keyboard.firstNote = (keyboard.firstNote + 12).coerceAtMost(96) }), LinearLayout.LayoutParams(0, -2, 1f).apply { marginEnd = d(4) })
            addView(stepper("Trans", { signed(keyboard.transpose) },
                { keyboard.transpose -= 1 }, { keyboard.transpose += 1 }), LinearLayout.LayoutParams(0, -2, 1f).apply { marginEnd = d(4) })
            addView(stepper("Keys", { "${keyboard.whiteKeyCount}" },
                { keyboard.whiteKeyCount -= 7 }, { keyboard.whiteKeyCount += 7 }), LinearLayout.LayoutParams(0, -2, 1f))
        }, LinearLayout.LayoutParams(-1, -2))
        keyValues = values

        val bendBtn = LedButton(a, "BEND", tr = false, led = true, accentColor = Hw.MOD_FILTER).apply {
            textSize = 10f; minHeight = d(28); setPadding(d(14), d(2), d(4), d(2))
            lit = keyboard.slideMode == KeyboardView.SlideMode.BEND
            setOnClickListener {
                val on = !lit
                lit = on
                keyboard.slideMode = if (on) KeyboardView.SlideMode.BEND else KeyboardView.SlideMode.GLISS
                a.prefs.edit().putBoolean("slideBend", on).apply()
            }
        }
        val holdBtn = LedButton(a, "HOLD", tr = false, led = true, accentColor = Hw.MOD_OSC).apply {
            textSize = 10f; minHeight = d(28); setPadding(d(14), d(2), d(4), d(2))
            setOnClickListener {
                sustainOn = !sustainOn
                lit = sustainOn
                NativeEngine.control(NativeEngine.Control.SUSTAIN, if (sustainOn) 1f else 0f)
                if (legacyOutAllowed()) a.midiOut.cc(64, if (sustainOn) 1f else 0f)
            }
        }
        val nightBtn = LedButton(a, "NIGHT", tr = false, led = true, accentColor = Hw.MOD_ENV).apply {
            textSize = 10f; minHeight = d(28); setPadding(d(14), d(2), d(4), d(2))
            lit = keyboard.nightKeys
            setOnClickListener {
                keyboard.nightKeys = !keyboard.nightKeys
                lit = keyboard.nightKeys
                a.prefs.edit().putBoolean("nightKeys", keyboard.nightKeys).apply()
            }
        }
        keysCard.addView(LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(bendBtn, LinearLayout.LayoutParams(0, -2, 1f).apply { marginEnd = d(4) })
            addView(holdBtn, LinearLayout.LayoutParams(0, -2, 1f).apply { marginEnd = d(4) })
            addView(nightBtn, LinearLayout.LayoutParams(0, -2, 1f))
        }, LinearLayout.LayoutParams(-1, -2).apply { topMargin = d(6) })

        val top = if (Ui.portrait(a)) LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(d(6), d(6), d(6), d(4))
            addView(presetCard, LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = d(6) })
            addView(keysCard, LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = d(6) })
            addView(knobsCard, LinearLayout.LayoutParams(-1, -2))
        } else LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(d(6), d(6), d(6), d(5))
            addView(presetCard, LinearLayout.LayoutParams(d(168), -1).apply { marginEnd = d(6) })
            addView(knobsCard, LinearLayout.LayoutParams(0, -1, 1f).apply { marginEnd = d(6) })
            addView(keysCard, LinearLayout.LayoutParams(d(232), -1))
        }

        val bend = ControlStrip(a, "PB", spring = true, bipolar = true, accent = Hw.MOD_FILTER) {
            NativeEngine.control(NativeEngine.Control.PITCH_BEND, it); if (legacyOutAllowed()) a.midiOut.bend(it)
        }
        val mod = ControlStrip(a, "MOD", spring = false, bipolar = false, accent = Hw.MOD_OSC) {
            NativeEngine.control(NativeEngine.Control.MOD_WHEEL, it); if (legacyOutAllowed()) a.midiOut.cc(1, it)
        }
        keyboard.onNote = { on, key, vel ->
            val k = key and 127
            if (on) {
                if (legacyOutAllowed()) { legacyHeld[k] = true; a.midiOut.noteOn(key, vel) }
            } else if (legacyHeld[k]) {
                legacyHeld[k] = false
                a.midiOut.noteOff(key)
            }
        }
        val bottom = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            setBackgroundColor(Hw.CHASSIS)
            setPadding(d(4), d(2), d(4), d(4))
            addView(bend, LinearLayout.LayoutParams(d(48), -1))
            addView(mod, LinearLayout.LayoutParams(d(48), -1).apply { marginEnd = d(4) })
            addView(keyboard, LinearLayout.LayoutParams(0, -1, 1f))
        }
        view = Ui.column(a).apply {
            setBackgroundColor(Hw.CHASSIS)
            addView(top, Ui.lp())
            addView(bottom, Ui.lp(weight = 1f, height = 0).apply { width = -1 })
        }
        a.addPresetListener { refresh() }
        Params.addListener { refresh() }
        refresh()
    }

    private fun refresh() {
        presetLcd.text = a.currentPresetName
        val kind = a.currentPreset?.parentFile?.name?.let { if (it == "user") "USER" else "FACTORY" } ?: "PRESET"
        presetSub.text = if (NativeEngine.nativeIsMulti()) "PART ${NativeEngine.nativeEditPart() + 1} · $kind" else kind
        quick.forEach { it.refresh() }
        keyValues.forEach { it() }
        rhythm?.refresh()
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
        statusText.setTextColor(if (s[3] > 0.8f || load > 70) Ui.WARN else Hw.ENGRAVE)
    }
}
