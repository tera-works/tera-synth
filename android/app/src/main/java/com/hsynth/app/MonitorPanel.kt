package com.hsynth.app

import android.content.Context
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Switch
import kotlin.math.roundToInt

/** Audio / DSP diagnostics and engine settings. */
class MonitorPanel(private val a: MainActivity) {
    val view: ScrollView
    private val statsText = Ui.text(a, "", 14f).apply { typeface = android.graphics.Typeface.MONOSPACE }
    private val midiText = Ui.text(a, "", 14f)
    private var outputs: List<AudioDeviceInfo> = emptyList()

    init {
        val d = { v: Int -> Ui.dp(a, v) }
        val col = Ui.column(a).apply { setPadding(d(16), d(8), d(16), d(24)) }
        fun header(t: String) = Ui.text(a, t, 16f, Ui.ACCENT, bold = true).apply { setPadding(0, d(12), 0, d(4)) }

        col.addView(header("Audio / DSP status"))
        col.addView(statsText)
        col.addView(Ui.row(a, Ui.button(a, "Reset peaks") { NativeEngine.nativeResetPeaks() },
            Ui.button(a, "Restart audio") { a.startAudio() }))

        col.addView(header("Engine settings"))
        val voices = listOf(16, 32, 64)
        col.addView(Ui.row(a, Ui.text(a, "Polyphony", 14f), Ui.spinner(a, voices.map { "$it voices" }, voices.indexOf(a.prefs.getInt("voices", 32)).coerceAtLeast(0)) {
            NativeEngine.nativeSetMaxVoices(voices[it]); a.prefs.edit().putInt("voices", voices[it]).apply()
        }))
        col.addView(Ui.row(a, Ui.text(a, "DSP quality", 14f), Ui.spinner(a, listOf("Low (SBC / power saving)", "Normal", "High"), a.prefs.getInt("quality", 1)) {
            NativeEngine.nativeSetQuality(it); a.prefs.edit().putInt("quality", it).apply()
        }))

        col.addView(header("Audio output (applies on restart)"))
        val buffers = listOf(0, 1, 2, 3, 4, 6, 8)
        col.addView(Ui.row(a, Ui.text(a, "Buffer", 14f), Ui.spinner(a, buffers.map { if (it == 0) "Auto (latency tuner)" else "$it × burst" },
            buffers.indexOf(a.prefs.getInt("bufferBursts", 2)).coerceAtLeast(0)) {
            if (a.prefs.getInt("bufferBursts", 2) != buffers[it]) {
                a.prefs.edit().putInt("bufferBursts", buffers[it]).apply(); a.startAudio()
            }
        }))
        val excl = Switch(a).apply {
            text = "Exclusive mode (lower latency when supported)"
            setTextColor(Ui.TEXT)
            isChecked = a.prefs.getBoolean("exclusive", true)
            setOnCheckedChangeListener { _, c -> a.prefs.edit().putBoolean("exclusive", c).apply(); a.startAudio() }
        }
        col.addView(excl)
        val am = a.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        outputs = am.getDevices(AudioManager.GET_DEVICES_OUTPUTS).filter { it.isSink }
        val names = listOf("System default") + outputs.map { "${it.productName} (${typeName(it.type)})" }
        val sel = outputs.indexOfFirst { it.id == a.prefs.getInt("deviceId", 0) } + 1
        col.addView(Ui.row(a, Ui.text(a, "Output device", 14f), Ui.spinner(a, names, sel) {
            val id = if (it == 0) 0 else outputs[it - 1].id
            if (id != a.prefs.getInt("deviceId", 0)) { a.prefs.edit().putInt("deviceId", id).apply(); a.startAudio() }
        }))

        col.addView(header("MIDI input"))
        val mpe = Switch(a).apply {
            text = "MPE (channel 1 = master, 2-16 = per-note)"
            setTextColor(Ui.TEXT)
            isChecked = a.prefs.getBoolean("mpe", false)
            setOnCheckedChangeListener { _, c -> a.prefs.edit().putBoolean("mpe", c).apply(); NativeEngine.nativeSetMpe(c) }
        }
        col.addView(mpe)
        col.addView(midiText)
        col.addView(Ui.text(a, "Program Change selects the n-th preset in the library list. " +
            "MIDI 2.0 UMP devices are opened in UMP mode on Android 13+ (per-note pitch bend, per-note controllers).", 12f, Ui.DIM))

        view = ScrollView(a).apply { addView(col) }
        refreshMidi()
    }

    private fun typeName(t: Int) = when (t) {
        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> "speaker"
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES, AudioDeviceInfo.TYPE_WIRED_HEADSET -> "wired"
        AudioDeviceInfo.TYPE_USB_DEVICE, AudioDeviceInfo.TYPE_USB_HEADSET -> "USB"
        AudioDeviceInfo.TYPE_BLUETOOTH_A2DP -> "Bluetooth (high latency)"
        else -> "type $t"
    }

    fun refreshMidi() {
        val devs = a.midi.deviceNames()
        midiText.text = if (!a.midi.available) "MIDI service not available"
        else if (devs.isEmpty()) "No MIDI devices connected (USB MIDI hot-plug is detected automatically)"
        else devs.joinToString("\n") { "• $it" } + "\nevents received: ${NativeEngine.nativeMidiEventCount()}"
    }

    fun updateStats(s: FloatArray) {
        val api = when (s[14].toInt()) { 2 -> "AAudio"; 1 -> "OpenSL ES"; else -> "?" }
        val sr = s[9].toInt()
        val buf = s[11].toInt()
        val bufMs = if (sr > 0) buf * 1000.0 / sr else 0.0
        statsText.text = buildString {
            appendLine("stream      ${if (s[8] > 0.5f) "running" else "STOPPED"}   $api   ${if (s[15] > 0.5f) "exclusive" else "shared"}   ${if (s[16] > 0.5f) "low-latency" else "normal"}")
            appendLine("format      ${sr} Hz float32 stereo, device id ${s[17].toInt()}")
            appendLine("buffer      burst ${s[10].toInt()}  size $buf (${"%.1f".format(bufMs)} ms)  capacity ${s[12].toInt()}  callback ${s[18].toInt()} frames")
            appendLine("latency     ${if (s[22] >= 0) "%.1f ms (output estimate)".format(s[22]) else "n/a"}")
            appendLine("xruns       ${s[13].toInt()}     stream restarts ${s[23].toInt()}")
            appendLine("callback    avg ${"%.0f".format(s[19])} µs   max ${"%.0f".format(s[20])} µs   peak load ${(s[21] * 100).roundToInt()} %")
            appendLine("engine DSP  load ${(s[2] * 100).roundToInt()} %  peak ${(s[3] * 100).roundToInt()} %   render avg ${"%.0f".format(s[4])} µs  max ${"%.0f".format(s[5])} µs")
            appendLine("voices      ${s[0].toInt()} active  peak ${s[1].toInt()}  limit ${s[24].toInt()}")
            append("output      peak ${"%.2f".format(s[7])}   dropped events ${s[6].toInt()}")
        }
        if (System.currentTimeMillis() / 1000 % 2 == 0L) refreshMidi()
    }
}
