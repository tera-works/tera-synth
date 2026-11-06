package com.hsynth.app

import android.content.Context
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.widget.LinearLayout
import android.widget.ScrollView
import android.view.Gravity
import android.view.View
import kotlin.math.roundToInt

/**
 * System page: audio / DSP status and the message log on the left, engine,
 * audio, MIDI and language settings on the right.
 */
class MonitorPanel(private val a: MainActivity) {
    val view: LinearLayout
    private val statsText = Ui.text(a, "", 10.5f, Hw.LCD_FG, tr = false).apply {
        typeface = Hw.MONO
        background = Hw.lcdBg(a)
        setPadding(Ui.dp(a, 10), Ui.dp(a, 8), Ui.dp(a, 10), Ui.dp(a, 8))
    }
    private val midiText = Ui.text(a, "", 13f, tr = false)
    private val logView = Ui.text(a, "", 12f, Hw.LCD_FG, tr = false).apply {
        typeface = Hw.MONO
        setTextIsSelectable(true)
    }
    private val logScroll = ScrollView(a)
    private var outputs: List<AudioDeviceInfo> = emptyList()
    var inputSwitch: ToggleSwitch? = null
    private val inputText = Ui.text(a, "", 12f, Ui.DIM, tr = false).apply { typeface = Hw.MONO }
    private var inPeak = 0f

    init {
        val d = { v: Int -> Ui.dp(a, v) }
        fun labeled(label: String, v: View) = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(Hw.engraved(a, label, 11f), LinearLayout.LayoutParams(d(120), -2))
            addView(v, LinearLayout.LayoutParams(0, -2, 1f))
            setPadding(0, d(3), 0, d(3))
        }
        fun toggle(label: String, isOn: Boolean, f: (Boolean) -> Unit) = ToggleSwitch(a, label, vertical = false).apply {
            on = isOn
            onToggle = f
        }

        // left: status + messages
        val status = Hw.Section(a, "Audio / DSP status")
        status.body.addView(statsText)
        status.body.addView(LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(LedButton(a, "Reset peaks", led = false).apply { setOnClickListener { NativeEngine.nativeResetPeaks() } },
                LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(6) })
            addView(LedButton(a, "Restart audio", led = false).apply { setOnClickListener { a.startAudio() } })
        }, LinearLayout.LayoutParams(-2, -2).apply { topMargin = d(6) })
        val messages = Hw.Section(a, "Messages")
        logScroll.background = Hw.lcdBg(a)
        logScroll.setPadding(d(8), d(6), d(8), d(6))
        logScroll.addView(logView)
        messages.body.addView(logScroll)
        messages.body.addView(LedButton(a, "Clear view", led = false).apply { setOnClickListener { logView.text = "" } },
            LinearLayout.LayoutParams(-2, -2).apply { topMargin = d(6) })
        logScroll.layoutParams = LinearLayout.LayoutParams(-1, d(260))
        val left = Ui.column(a).apply {
            addView(status, Hw.sectionLp(a))
            addView(messages, LinearLayout.LayoutParams(-1, -2))
        }
        a.addLogListener { logView.text = a.logText(); logScroll.post { logScroll.fullScroll(View.FOCUS_DOWN) } }

        // right: settings
        val col = Ui.column(a)
        val engine = Hw.Section(a, "Engine settings")
        val voices = listOf(32, 64, 128, 256)
        engine.body.addView(labeled("Polyphony", Ui.spinner(a, voices.map { L10n.f("%d voices", it) }, voices.indexOf(a.prefs.getInt("voices", 128)).coerceAtLeast(0)) {
            NativeEngine.nativeSetMaxVoices(voices[it]); a.prefs.edit().putInt("voices", voices[it]).apply()
        }))
        engine.body.addView(labeled("DSP quality", Ui.spinner(a, listOf("Low (SBC / power saving)", "Normal", "High").map { L10n.t(it) }, a.prefs.getInt("quality", 1)) {
            NativeEngine.nativeSetQuality(it); a.prefs.edit().putInt("quality", it).apply()
        }))
        col.addView(engine, Hw.sectionLp(a))

        val audio = Hw.Section(a, "Audio output (applies on restart)")
        val buffers = listOf(0, 1, 2, 3, 4, 6, 8)
        audio.body.addView(labeled("Buffer", Ui.spinner(a, buffers.map { if (it == 0) L10n.t("Auto (latency tuner)") else L10n.f("%d × burst", it) },
            buffers.indexOf(a.prefs.getInt("bufferBursts", 2)).coerceAtLeast(0)) {
            if (a.prefs.getInt("bufferBursts", 2) != buffers[it]) {
                a.prefs.edit().putInt("bufferBursts", buffers[it]).apply(); a.startAudio()
            }
        }))
        val am = a.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        outputs = am.getDevices(AudioManager.GET_DEVICES_OUTPUTS).filter { it.isSink }
        val names = listOf(L10n.t("System default")) + outputs.map { "${it.productName} (${typeName(it.type)})" }
        val sel = outputs.indexOfFirst { it.id == a.prefs.getInt("deviceId", 0) } + 1
        audio.body.addView(labeled("Output device", Ui.spinner(a, names, sel) {
            val id = if (it == 0) 0 else outputs[it - 1].id
            if (id != a.prefs.getInt("deviceId", 0)) { a.prefs.edit().putInt("deviceId", id).apply(); a.startAudio() }
        }))
        audio.body.addView(toggle("Exclusive mode (lower latency when supported)", a.prefs.getBoolean("exclusive", true)) { c ->
            a.prefs.edit().putBoolean("exclusive", c).apply(); a.startAudio()
        }, LinearLayout.LayoutParams(-1, -2).apply { topMargin = d(6) })
        // audio input (microphone / USB input) for the vocoder and the looper
        inputSwitch = toggle("Audio Input (mic / USB) for vocoder and looper", NativeEngine.nativeInputEnabled()) { c -> a.setAudioInput(c) }
        audio.body.addView(inputSwitch, LinearLayout.LayoutParams(-1, -2).apply { topMargin = d(4) })
        audio.body.addView(inputText, LinearLayout.LayoutParams(-1, -2))
        col.addView(audio, Hw.sectionLp(a))

        val midi = Hw.Section(a, "MIDI input")
        midi.body.addView(toggle("MPE (channel 1 = master, 2-16 = per-note)", a.prefs.getBoolean("mpe", false)) { c ->
            a.prefs.edit().putBoolean("mpe", c).apply(); NativeEngine.nativeSetMpe(c)
        }, LinearLayout.LayoutParams(-1, -2))
        midi.body.addView(midiText, LinearLayout.LayoutParams(-1, -2).apply { topMargin = d(6) })
        // MIDI out: the on-screen keyboard is sent to this device as well
        val outs = a.midiOut.destinations()
        val savedOut = a.prefs.getString("midiOutName", "") ?: ""
        val outSel = outs.indexOfFirst { it.second == savedOut } + 1
        midi.body.addView(labeled("MIDI Out", Ui.spinner(a, listOf(L10n.t("(none)")) + outs.map { it.second }, outSel) {
            if (it == 0) { a.midiOut.close(); a.prefs.edit().putString("midiOutName", "").apply() }
            else { a.midiOut.open(outs[it - 1].first); a.prefs.edit().putString("midiOutName", outs[it - 1].second).apply() }
        }), LinearLayout.LayoutParams(-1, -2).apply { topMargin = d(6) })
        midi.body.addView(labeled("Out Channel", Ui.spinner(a, (1..16).map { "$it" }, a.prefs.getInt("midiOutCh", 0)) {
            a.midiOut.channel = it; a.prefs.edit().putInt("midiOutCh", it).apply()
        }))
        a.midiOut.channel = a.prefs.getInt("midiOutCh", 0)
        if (outSel > 0) a.midiOut.open(outs[outSel - 1].first)
        midi.body.addView(Ui.text(a, "Program Change selects the n-th preset in the library list. " +
            "MIDI 2.0 UMP devices are opened in UMP mode on Android 13+ (per-note pitch bend, per-note controllers).", 12f, Ui.DIM))
        col.addView(midi, Hw.sectionLp(a))

        val lang = Hw.Section(a, "Language")
        val langs = listOf("auto", "ja", "en")
        lang.body.addView(Ui.spinner(a, listOf(L10n.t("Auto (device)"), "日本語", "English"),
            langs.indexOf(a.prefs.getString("lang", "auto")).coerceAtLeast(0)) {
            if (a.prefs.getString("lang", "auto") != langs[it]) {
                a.prefs.edit().putString("lang", langs[it]).apply()
                a.recreate()  // rebuild the UI in the new language
            }
        })
        col.addView(lang, Hw.sectionLp(a))

        view = if (Ui.portrait(a)) LinearLayout(a).apply {
            setPadding(d(10), d(8), d(10), d(8))
            addView(ScrollView(a).apply {
                addView(Ui.column(a).apply {
                    addView(left, Hw.sectionLp(a))
                    addView(col, LinearLayout.LayoutParams(-1, -2))
                })
            }, LinearLayout.LayoutParams(-1, -1))
        } else LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(d(10), d(8), d(10), d(8))
            addView(ScrollView(a).apply { addView(left) }, LinearLayout.LayoutParams(0, -1, 1f).apply { marginEnd = d(10) })
            addView(ScrollView(a).apply { addView(col) }, LinearLayout.LayoutParams(0, -1, 1f))
        }
        refreshMidi()
    }

    private fun typeName(t: Int) = when (t) {
        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> L10n.t("speaker")
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES, AudioDeviceInfo.TYPE_WIRED_HEADSET -> L10n.t("wired")
        AudioDeviceInfo.TYPE_USB_DEVICE, AudioDeviceInfo.TYPE_USB_HEADSET -> "USB"
        AudioDeviceInfo.TYPE_BLUETOOTH_A2DP -> L10n.t("Bluetooth (high latency)")
        else -> L10n.f("type %d", t)
    }

    fun refreshMidi() {
        val devs = a.midi.deviceNames()
        midiText.text = if (!a.midi.available) L10n.t("MIDI service not available")
        else if (devs.isEmpty()) L10n.t("No MIDI devices connected (USB MIDI hot-plug is detected automatically)")
        else devs.joinToString("\n") { "• $it" } + "\n" + L10n.f("events received: %d", NativeEngine.nativeMidiEventCount())
    }

    fun updateStats(s: FloatArray) {
        if (inputSwitch?.on == true) {
            inPeak = maxOf(NativeEngine.nativeInputPeak(), inPeak * 0.7f)
            val db = if (inPeak > 1e-5f) 20 * kotlin.math.log10(inPeak) else -99f
            inputText.text = "IN  %s  %.0f dB".format("█".repeat(((db + 60) / 4).toInt().coerceIn(0, 15)).padEnd(15, '·'), db)
        } else inputText.text = ""
        val api = when (s[14].toInt()) { 2 -> "AAudio"; 1 -> "OpenSL ES"; else -> "?" }
        val sr = s[9].toInt()
        val buf = s[11].toInt()
        val bufMs = if (sr > 0) buf * 1000.0 / sr else 0.0
        statsText.text = buildString {
            appendLine(L10n.f("stream      %s   %s   %s   %s", L10n.t(if (s[8] > 0.5f) "running" else "STOPPED"), api,
                L10n.t(if (s[15] > 0.5f) "exclusive" else "shared"), L10n.t(if (s[16] > 0.5f) "low-latency" else "normal")))
            appendLine(L10n.f("format      %d Hz float32 stereo, device id %d", sr, s[17].toInt()))
            appendLine(L10n.f("buffer      burst %d  size %d (%.1f ms)  capacity %d  callback %d frames", s[10].toInt(), buf, bufMs,
                s[12].toInt(), s[18].toInt()))
            appendLine(L10n.f("latency     %s", if (s[22] >= 0) L10n.f("%.1f ms (output estimate)", s[22]) else L10n.t("n/a")))
            appendLine(L10n.f("xruns       %d     stream restarts %d", s[13].toInt(), s[23].toInt()))
            appendLine(L10n.f("callback    avg %.0f µs   max %.0f µs   peak load %d %%", s[19], s[20], (s[21] * 100).roundToInt()))
            appendLine(L10n.f("engine DSP  load %d %%  peak %d %%   render avg %.0f µs  max %.0f µs", (s[2] * 100).roundToInt(),
                (s[3] * 100).roundToInt(), s[4], s[5]))
            appendLine(L10n.f("voices      %d active  peak %d  limit %d", s[0].toInt(), s[1].toInt(), s[24].toInt()))
            append(L10n.f("output      peak %.2f   dropped events %d", s[7], s[6].toInt()))
        }
        if (System.currentTimeMillis() / 1000 % 2 == 0L) refreshMidi()
    }
}
