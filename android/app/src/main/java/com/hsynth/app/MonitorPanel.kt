package com.hsynth.app

import android.annotation.SuppressLint
import android.app.AlertDialog
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import org.json.JSONArray
import org.json.JSONObject
import java.io.BufferedOutputStream
import java.io.File
import java.io.IOException
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.math.max
import kotlin.math.pow
import kotlin.math.roundToInt

/**
 * SYS (tab 6): a [SubTabBar] `STATUS | ENGINE | AUDIO | MIDI | POWER | SYSTEM` over six pages. Every page keeps the
 * clean two-column (weight 1 : 1) landscape layout (one column in portrait), so nothing scrolls on pages that fit.
 *
 *  - STATUS : audio / DSP status (left), message log with flexible height (right)
 *  - ENGINE : polyphony, DSP quality, S1 SST toggle + transition time (left); S3 Master EQ & Comp… (right)
 *  - AUDIO  : output settings (left); U4 audio input row with live level (right)
 *  - MIDI   : MIDI input / output + Bluetooth MIDI (left); B4 "Pedals & Controllers" (right)
 *  - POWER  : screen-off / standby / deep-sleep / auto power-off (left); explanation (right)
 *  - SYSTEM : language (left); about (right)
 *
 * The stats string is only built while STATUS is showing (it is rebuilt every 250 ms), the MIDI device list only
 * while MIDI is showing. The chosen page is persisted in prefs `sysPage`.
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
    // Phase 5: NPU / Edge TPU readout (SYS > STATUS)
    private val npuText = Ui.text(a, "", 13f, Hw.LCD_FG, tr = false).apply {
        typeface = Hw.MONO
        maxLines = 2
        background = Hw.lcdBg(a)
        setPadding(Ui.dp(a, 10), Ui.dp(a, 6), Ui.dp(a, 10), Ui.dp(a, 6))
    }
    private var npuBusy = false
    private var npuTick = 0
    private var outputs: List<AudioDeviceInfo> = emptyList()
    private var bleScan: android.widget.Button? = null
    private var blePeripheral: ToggleSwitch? = null
    private val bleList by lazy { Ui.column(a) }
    private val bleStatus by lazy { Ui.text(a, "", 12f, Ui.DIM, tr = false) }

    private val pages = ArrayList<View>()
    private lateinit var tabs: SubTabBar
    private var page = a.prefs.getInt("sysPage", PAGE_STATUS).coerceIn(0, PAGE_COUNT - 1)
    private var lastStats: FloatArray? = null

    // B3: external MIDI output slot spinners (rebuilt when the device list changes)
    private val extBox by lazy { Ui.column(a) }
    private var extSig = ""

    // B6: SYS > SYSTEM backup / restore + Wi-Fi web editor
    private var backupIncludeSamples = false
    private var backupBusy = false
    private var webTick = 0
    private lateinit var sizeA: LedButton
    private lateinit var sizeB: LedButton
    private lateinit var backupNowBtn: LedButton
    private lateinit var folderBtn: LedButton
    private lateinit var restoreBtn: LedButton
    private lateinit var backupLcd: TextView
    private lateinit var backupMsg: TextView
    private lateinit var backupBar: BarView
    private lateinit var webToggle: ToggleSwitch
    private lateinit var webLcd: TextView
    private lateinit var pinLcd: TextView
    private lateinit var clientsTv: TextView
    private lateinit var newPinBtn: LedButton
    private lateinit var stopWebBtn: LedButton

    init {
        val d = { v: Int -> Ui.dp(a, v) }
        val portrait = Ui.portrait(a)
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

        /** A scrolling page: two equal columns in landscape, one stacked column in portrait. */
        fun twoCol(left: View?, right: View?): View {
            val body: LinearLayout = if (portrait) {
                Ui.column(a).apply {
                    left?.let { addView(it, Hw.sectionLp(a)) }
                    right?.let { addView(it, LinearLayout.LayoutParams(-1, -2)) }
                }
            } else {
                LinearLayout(a).apply {
                    orientation = LinearLayout.HORIZONTAL
                    left?.let { addView(it, LinearLayout.LayoutParams(0, -2, 1f).apply { marginEnd = d(10) }) }
                    right?.let { addView(it, LinearLayout.LayoutParams(0, -2, 1f)) }
                }
            }
            return ScrollView(a).apply { addView(body, FrameLayout.LayoutParams(-1, -2)) }
        }

        // ------------------------------------------------------------ STATUS
        val status = Hw.Section(a, "Audio / DSP status")
        status.body.addView(statsText)
        status.body.addView(Hw.engraved(a, "NPU / EDGE TPU", 10.5f, tr = false, color = Hw.MOD_FX),
            LinearLayout.LayoutParams(-2, -2).apply { topMargin = d(8); bottomMargin = d(3) })
        status.body.addView(npuText, LinearLayout.LayoutParams(-1, -2))
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
        val clearBtn = LedButton(a, "Clear view", led = false).apply { setOnClickListener { logView.text = "" } }
        a.addLogListener { logView.text = a.logText(); logScroll.post { logScroll.fullScroll(View.FOCUS_DOWN) } }
        val statusPage: View = if (portrait) {
            messages.body.addView(logScroll, LinearLayout.LayoutParams(-1, d(260)))
            messages.body.addView(clearBtn, LinearLayout.LayoutParams(-2, -2).apply { topMargin = d(6) })
            twoCol(status, messages)
        } else {
            // flexible log height: the message section fills the right column
            messages.body.layoutParams = LinearLayout.LayoutParams(-1, 0, 1f)
            messages.body.addView(logScroll, LinearLayout.LayoutParams(-1, 0, 1f))
            messages.body.addView(clearBtn, LinearLayout.LayoutParams(-2, -2).apply { topMargin = d(6) })
            LinearLayout(a).apply {
                orientation = LinearLayout.HORIZONTAL
                addView(ScrollView(a).apply { addView(status, FrameLayout.LayoutParams(-1, -2)) },
                    LinearLayout.LayoutParams(0, -1, 1f).apply { marginEnd = d(10) })
                addView(messages, LinearLayout.LayoutParams(0, -1, 1f))
            }
        }

        // ------------------------------------------------------------ ENGINE
        val engine = Hw.Section(a, "Engine settings")
        val voices = listOf(32, 64, 128, 256)
        engine.body.addView(labeled("Polyphony", Ui.spinner(a, voices.map { L10n.f("%d voices", it) }, voices.indexOf(a.prefs.getInt("voices", 256)).coerceAtLeast(0)) {
            NativeEngine.nativeSetMaxVoices(voices[it]); a.prefs.edit().putInt("voices", voices[it]).apply()
        }))
        engine.body.addView(labeled("DSP quality", Ui.spinner(a, listOf("Low (SBC / power saving)", "Normal", "High").map { L10n.t(it) }, a.prefs.getInt("quality", 1)) {
            NativeEngine.nativeSetQuality(it); a.prefs.edit().putInt("quality", it).apply()
        }))
        // S1: seamless sound transition (applies to preset, set and scene changes)
        val sstTimes = listOf(120, 300, 800)
        val sstNames = listOf("Fast 120 ms", "Normal 300 ms", "Slow 800 ms")
        fun applySst(on: Boolean, ms: Int) {
            a.prefs.edit().putBoolean("sstOn", on).putInt("sstMs", ms).apply()
            SstSweep.applySettings(on, ms)
        }
        val curMs = a.prefs.getInt("sstMs", 300)
        val sstSel = sstTimes.indices.minByOrNull { kotlin.math.abs(sstTimes[it] - curMs) } ?: 1
        engine.body.addView(toggle("SST — keep notes and tails ringing across sound changes", a.prefs.getBoolean("sstOn", true)) { on ->
            applySst(on, a.prefs.getInt("sstMs", 300))
        }, LinearLayout.LayoutParams(-1, -2).apply { topMargin = d(6) })
        engine.body.addView(labeled("Transition", Ui.spinner(a, sstNames.map { L10n.t(it) }, sstSel) {
            applySst(a.prefs.getBoolean("sstOn", true), sstTimes[it])
        }))

        // S3: master stage
        val stage = Hw.Section(a, "Master stage")
        stage.body.addView(Ui.text(a, "Global 5-band EQ and compressor on the main output. It stays the same across presets, sets and scenes.", 12f, Ui.DIM))
        stage.body.addView(LedButton(a, "Master EQ & Comp…", led = true, accentColor = Hw.MOD_FX).apply {
            setOnClickListener { a.openSheet(a.masterEqSheet) }
        }, LinearLayout.LayoutParams(-2, -2).apply { topMargin = d(8) })

        // ------------------------------------------------------------ AUDIO
        val audio = Hw.Section(a, "Audio output (applies on restart)")
        val buffers = listOf(0, 1, 2, 3, 4, 6, 8)
        audio.body.addView(labeled("Buffer", Ui.spinner(a, buffers.map { if (it == 0) L10n.t("Auto (latency tuner)") else L10n.f("%d × burst", it) },
            buffers.indexOf(a.prefs.getInt("bufferBursts", 0)).coerceAtLeast(0)) {
            if (a.prefs.getInt("bufferBursts", 0) != buffers[it]) {
                a.prefs.edit().putInt("bufferBursts", buffers[it]).apply(); a.startAudio()
            }
        }))
        val rates = listOf(48000, 96000)
        audio.body.addView(labeled("Sample rate", Ui.spinner(a, rates.map { "${it / 1000} kHz" },
            rates.indexOf(a.prefs.getInt("sampleRate", 48000)).coerceAtLeast(0)) {
            if (a.prefs.getInt("sampleRate", 48000) != rates[it]) {
                a.prefs.edit().putInt("sampleRate", rates[it]).apply(); a.startAudio()
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
        audio.body.addView(toggle("Direct ALSA (USB DAC / root)", a.prefs.getBoolean("alsaDirect", false)) { c ->
            a.prefs.edit().putBoolean("alsaDirect", c).apply(); a.startAudio()
        }, LinearLayout.LayoutParams(-1, -2).apply { topMargin = d(4) })

        // U4: one shared input row (live state + level, 1-tap enable)
        val input = Hw.Section(a, "Audio input")
        input.body.addView(Ui.text(a, "Audio Input (mic / USB) for vocoder, looper, tuner and sampling", 12f, Ui.DIM),
            LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = d(6) })
        input.body.addView(InputStatusRow(a), LinearLayout.LayoutParams(-1, d(30)))

        // ------------------------------------------------------------ MIDI
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
            "MIDI 2.0 UMP devices are opened in UMP mode on Android 13+ (per-note pitch bend, per-note controllers).", 12f, Ui.DIM),
            LinearLayout.LayoutParams(-1, -2).apply { topMargin = d(6) })

        // B3: external MIDI outputs 1-4 — the engine's zone routing (MULTI > ROUTE EXT / BOTH) writes to these slots
        val extSec = Hw.Section(a, "External MIDI outputs", accent = Hw.MOD_FILTER)
        extSec.body.addView(Ui.text(a, "Zones routed EXT / BOTH on the MULTI screen play through these ports (slots 1-4).", 12f, Ui.DIM))
        extSec.body.addView(extBox, LinearLayout.LayoutParams(-1, -2).apply { topMargin = d(4) })
        rebuildExtOuts()

        // Bluetooth LE MIDI
        val ble = Hw.Section(a, "Bluetooth MIDI")
        if (!a.ble.available) {
            ble.body.addView(Ui.text(a, "Bluetooth LE is not available on this device", 12f, Ui.DIM))
        } else {
            bleScan = Ui.button(a, "Scan") {
                a.withBlePermission { if (a.ble.scanning) a.ble.stopScan() else a.ble.scan() }
            }
            ble.body.addView(Ui.row(a, bleScan!!), LinearLayout.LayoutParams(-1, -2))
            ble.body.addView(bleList, LinearLayout.LayoutParams(-1, -2))
            blePeripheral = toggle("Peripheral: a PC / tablet can connect to this phone", a.prefs.getBoolean("blePeripheral", false)) { c ->
                a.prefs.edit().putBoolean("blePeripheral", c).apply()
                if (c) a.withBlePermission { a.ble.startPeripheral() } else a.ble.stopPeripheral()
            }
            ble.body.addView(blePeripheral, LinearLayout.LayoutParams(-1, -2).apply { topMargin = d(6) })
            ble.body.addView(bleStatus, LinearLayout.LayoutParams(-1, -2))
            ble.body.addView(Ui.text(a, "Scan finds BLE MIDI keyboards and controllers; tap one to connect (it is reconnected next time). " +
                "Peripheral mode lets a PC or tablet connect to this phone for playing and SysEx remote control.", 12f, Ui.DIM))
        }

        // B4: sustain polarity, velocity / aftertouch curves, expression range and 4 assignable footswitches
        val pedals = Hw.Section(a, "Pedals & Controllers", accent = Hw.MOD_FILTER)
        pedals.body.addView(PedalsPanel(a), LinearLayout.LayoutParams(-1, -2))
        val midiColumn = Ui.column(a).apply {
            addView(midi, Hw.sectionLp(a))
            addView(extSec, Hw.sectionLp(a))
            addView(ble, LinearLayout.LayoutParams(-1, -2))
        }

        // ------------------------------------------------------------ POWER
        val power = Hw.Section(a, "Power saving")
        fun minLabel(m: Int) = if (m == 0) L10n.t("Off") else L10n.f("%d min", m)
        val screenOpts = listOf(0, 1, 2, 3, 5, 10, 15, 30)
        power.body.addView(labeled("Screen off", Ui.spinner(a, screenOpts.map(::minLabel),
            screenOpts.indexOf(a.prefs.getInt("powerScreenOffMin", 3)).coerceAtLeast(0)) {
            a.prefs.edit().putInt("powerScreenOffMin", screenOpts[it]).apply(); a.onPowerSettingsChanged()
        }))
        val standbyOpts = listOf(0, 3, 5, 10, 15, 30)
        power.body.addView(labeled("Audio standby", Ui.spinner(a, standbyOpts.map(::minLabel),
            standbyOpts.indexOf(a.prefs.getInt("powerStandbyMin", 5)).coerceAtLeast(0)) {
            a.prefs.edit().putInt("powerStandbyMin", standbyOpts[it]).apply(); a.onPowerSettingsChanged()
        }))
        val sleepOpts = listOf(0, 10, 15, 30, 60)
        power.body.addView(labeled("Deep sleep", Ui.spinner(a, sleepOpts.map(::minLabel),
            sleepOpts.indexOf(a.prefs.getInt("powerDeepSleepMin", 15)).coerceAtLeast(0)) {
            a.prefs.edit().putInt("powerDeepSleepMin", sleepOpts[it]).apply(); a.onPowerSettingsChanged()
        }))
        val offOpts = listOf(0, 15, 30, 60, 120)
        power.body.addView(labeled("Auto power off", Ui.spinner(a, offOpts.map(::minLabel),
            offOpts.indexOf(a.prefs.getInt("powerShutdownMin", 60)).coerceAtLeast(0)) {
            a.prefs.edit().putInt("powerShutdownMin", offOpts[it]).apply(); a.onPowerSettingsChanged()
        }))
        power.body.addView(toggle("Auto power off on battery only (root)", a.prefs.getBoolean("powerShutdownBattOnly", true)) { c ->
            a.prefs.edit().putBoolean("powerShutdownBattOnly", c).apply(); a.onPowerSettingsChanged()
        }, LinearLayout.LayoutParams(-1, -2).apply { topMargin = d(4) })
        val powerInfo = Hw.Section(a, "How it works")
        powerInfo.body.addView(Ui.text(a, "Screen turns off first, then audio pauses while BLE MIDI stays ready to wake on any note. Tap the screen or press the power button to wake.", 12f, Ui.DIM))

        // ------------------------------------------------------------ SYSTEM
        val lang = Hw.Section(a, "Language")
        val langs = listOf("auto", "ja", "en")
        lang.body.addView(Ui.spinner(a, listOf(L10n.t("Auto (device)"), "日本語", "English"),
            langs.indexOf(a.prefs.getString("lang", "auto")).coerceAtLeast(0)) {
            if (a.prefs.getString("lang", "auto") != langs[it]) {
                a.prefs.edit().putString("lang", langs[it]).apply()
                a.recreate()  // rebuild the UI in the new language
            }
        })
        val about = Hw.Section(a, "About")
        val ver = runCatching { a.packageManager.getPackageInfo(a.packageName, 0).versionName }.getOrNull() ?: "?"
        about.body.addView(Ui.text(a, "HSynth $ver", 13f, Hw.SILK, tr = false))
        // Phase 6: authors / licenses of the downloadable sound packs
        about.body.addView(LedButton(a, L10n.t("SOUND LICENSES"), tr = false, led = false, accentColor = Hw.MOD_FX).apply {
            textSize = 12.5f
            setOnClickListener { a.openSheet(a.soundLicensesSheet) }
        }, LinearLayout.LayoutParams(-2, -2).apply { topMargin = d(8) })

        // ------------------------------------------------------------ assemble
        pages += statusPage
        pages += twoCol(engine, stage)
        pages += twoCol(audio, input)
        pages += twoCol(midiColumn, pedals)
        pages += twoCol(power, powerInfo)
        // B6: backup / restore (left, above language + about) and the Wi-Fi web editor (right)
        val systemLeft = Ui.column(a).apply {
            addView(buildBackupSection(), Hw.sectionLp(a))
            addView(lang, Hw.sectionLp(a))
            addView(about, LinearLayout.LayoutParams(-1, -2))
        }
        pages += twoCol(systemLeft, buildWebSection())

        tabs = SubTabBar(a, arrayOf("STATUS", "ENGINE", "AUDIO", "MIDI", "POWER", "SYSTEM"), accent = Hw.MOD_OSC).apply {
            selected = page
            onSelect = { showPage(it) }
        }
        val frame = FrameLayout(a)
        pages.forEach { frame.addView(it, FrameLayout.LayoutParams(-1, -1)) }

        view = LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(d(10), d(8), d(10), d(8))
            addView(tabs, LinearLayout.LayoutParams(-1, d(26)))
            addView(frame, LinearLayout.LayoutParams(-1, 0, 1f).apply { topMargin = d(8) })
        }
        showPage(page)
        refreshBle()
    }

    private fun showPage(i: Int) {
        page = i.coerceIn(0, PAGE_COUNT - 1)
        pages.forEachIndexed { k, v -> v.visibility = if (k == page) View.VISIBLE else View.GONE }
        a.prefs.edit().putInt("sysPage", page).apply()
        when (page) {
            PAGE_STATUS -> { lastStats?.let { renderStats(it) }; refreshNpu() }
            PAGE_MIDI -> refreshMidi()
            PAGE_SYSTEM -> { refreshWeb(); refreshBackupSizes(); updateBackupLcd() }
        }
        FrameTicker.poke()
    }

    private fun typeName(t: Int) = when (t) {
        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> L10n.t("speaker")
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES, AudioDeviceInfo.TYPE_WIRED_HEADSET -> L10n.t("wired")
        AudioDeviceInfo.TYPE_USB_DEVICE, AudioDeviceInfo.TYPE_USB_HEADSET -> "USB"
        AudioDeviceInfo.TYPE_BLUETOOTH_A2DP -> L10n.t("Bluetooth (high latency)")
        else -> L10n.f("type %d", t)
    }

    /** Bluetooth MIDI: found / connected devices, peripheral clients. */
    fun refreshBle() {
        if (!a.ble.available) return
        bleScan?.text = L10n.t(if (a.ble.scanning) "Stop scan" else "Scan")
        bleList.removeAllViews()
        val conn = a.ble.connectedDevices() + a.ble.connectingDevices()
        for ((addr, name) in conn)
            bleList.addView(Ui.button(a, L10n.f("Disconnect %s", name)) { a.ble.disconnect(addr) },
                LinearLayout.LayoutParams(-1, -2).apply { topMargin = Ui.dp(a, 4) })
        for (f in a.ble.found.values) {
            if (conn.any { it.first == f.address }) continue
            bleList.addView(Ui.button(a, L10n.f("Connect %s", f.name)) { a.withBlePermission { a.ble.connect(f.address, f.name) } },
                LinearLayout.LayoutParams(-1, -2).apply { topMargin = Ui.dp(a, 4) })
        }
        if (a.ble.scanning && a.ble.found.isEmpty())
            bleList.addView(Ui.text(a, "Searching…", 12f, Ui.DIM))
        blePeripheral?.on = a.ble.peripheralOn || a.prefs.getBoolean("blePeripheral", false)
        val clients = a.ble.peripheralClients()
        bleStatus.text = if (!a.ble.peripheralOn) "" else if (clients.isEmpty()) L10n.t("Advertising, waiting for a connection")
            else L10n.f("Connected: %s", clients.joinToString(", "))
        refreshMidi()
    }

    fun refreshMidi() {
        val devs = a.midi.deviceNames() + a.ble.connectedDevices().map { "${it.second} (MIDI 1.0)" }
        midiText.text = if (!a.midi.available && devs.isEmpty()) L10n.t("MIDI service not available")
        else if (devs.isEmpty()) L10n.t("No MIDI devices connected (USB MIDI hot-plug is detected automatically)")
        else devs.joinToString("\n") { "• $it" } + "\n" + L10n.f("events received: %d", NativeEngine.nativeMidiEventCount())
        rebuildExtOuts()
    }

    /** B3: four slot spinners. The chosen device *name* is persisted (`extPort1..4`); the engine only knows the slot number. */
    private fun rebuildExtOuts() {
        val dests = a.midiOut.destinations()
        val sig = dests.joinToString("|") { it.second } + "#" + (1..MidiOutput.SLOTS).joinToString("|") { a.prefs.getString("extPort$it", "") ?: "" }
        if (sig == extSig) return
        extSig = sig
        extBox.removeAllViews()
        val d = { v: Int -> Ui.dp(a, v) }
        for (slot in 1..MidiOutput.SLOTS) {
            val saved = a.prefs.getString("extPort$slot", "") ?: ""
            val names = dests.map { it.second }
            // a saved device that is not connected right now stays visible (and selected) so the choice is not lost
            val absent = saved.isNotEmpty() && saved !in names
            val items = ArrayList<String>()
            items += L10n.t("(none)")
            items += names
            if (absent) items += "$saved (" + L10n.t("not connected") + ")"
            val sel = if (saved.isEmpty()) 0 else if (absent) items.size - 1 else names.indexOf(saved) + 1
            val spinner = Ui.spinner(a, items, sel) { i ->
                if (i == 0) {
                    a.midiOut.closeSlot(slot)
                    a.prefs.edit().putString("extPort$slot", "").apply()
                } else if (i <= names.size) {
                    val dev = dests[i - 1]
                    a.midiOut.openSlot(slot, dev.first)
                    a.prefs.edit().putString("extPort$slot", dev.second).apply()
                }
            }
            extBox.addView(LinearLayout(a).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                setPadding(0, d(3), 0, d(3))
                addView(Hw.engraved(a, "Out $slot", 11f, tr = false), LinearLayout.LayoutParams(d(120), -2))
                addView(spinner, LinearLayout.LayoutParams(0, -2, 1f))
            })
        }
    }

    /** Called every stats tick while the SYS tab is shown; only the visible page does any work. */
    fun updateStats(s: FloatArray) {
        lastStats = s
        if (page == PAGE_STATUS) { renderStats(s); if (++npuTick % 4 == 0) refreshNpu() }   // NPU readout ~1 Hz
        if (page == PAGE_MIDI && System.currentTimeMillis() / 1000 % 2 == 0L) refreshMidi()
        if (page == PAGE_SYSTEM && ++webTick % 4 == 0) refreshWeb()     // client count / idle stop, ~1 Hz
    }

    /** `nativeNpuStatus()` on the loader thread (it can wait behind an NNAPI compile), rendered on the UI thread. */
    private fun refreshNpu() {
        if (npuBusy) return
        npuBusy = true
        a.loader.execute {
            val raw = try { NativeEngine.nativeNpuStatus() } catch (_: Throwable) { "" }
            a.ui.post {
                npuBusy = false
                if (!a.isDestroyed) renderNpu(raw)
            }
        }
    }

    private fun renderNpu(raw: String) {
        val o = runCatching { org.json.JSONObject(raw) }.getOrNull()
        if (o == null) { npuText.text = L10n.t("NPU status unavailable"); return }
        val backend = o.optString("backend", "cpu")
        val dev = when (backend) { "edgetpu" -> "google-edgetpu"; "nnapi" -> "nnapi"; else -> "cpu" }
        val dtype = o.optString("dtype", "fp32").ifEmpty { "fp32" }
        val us = o.optDouble("lastUs", -1.0)
        val last = if (us >= 0.0) "${us.roundToInt()} \u00b5s" else "\u2014"
        val err = o.optString("error", "")
        val sb = StringBuilder()
        sb.append(dev).append("  \u00b7  ").append(dtype).append("  \u00b7  ").append(L10n.t("last")).append(' ').append(last)
        val name = o.optString("device", "")
        if (name.isNotEmpty() && name != backend && name != dev) sb.append("  (").append(name).append(')')
        if (o.optBoolean("compiled", false)) {
            sb.append("\n").append(L10n.f("%d anchors, %d params, compiled", o.optInt("anchors"), o.optInt("params")))
        } else if (err.isNotEmpty()) {
            sb.append("\n").append(err.take(90))
        } else if (!o.optBoolean("available", false)) {
            sb.append("\n").append(L10n.t("NPU not available - CPU fallback"))
        }
        npuText.text = sb.toString()
        npuText.setTextColor(if (backend == "cpu") Hw.INK_DIM else Hw.MOD_FX)
    }

    private fun renderStats(s: FloatArray) {
        if (s.size < 25) return
        val api = when (s[14].toInt()) { 3 -> "Direct ALSA"; 2 -> "AAudio"; 1 -> "OpenSL ES"; else -> "?" }
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
    }

    // ============================================================ B6 backup / restore (SYS > SYSTEM)

    private fun sizeText(n: Long): String = if (n >= (1L shl 30)) "%.1f GB".format(n / 1073741824.0) else "%.1f MB".format(n / 1048576.0)

    private fun backupName() = "HSynth_Backup_" + SimpleDateFormat("yyyyMMdd_HHmm", Locale.US).format(Date()) + ".zip"

    private fun buildBackupSection(): View {
        val d = { v: Int -> Ui.dp(a, v) }
        val sec = Hw.Section(a, "Backup / Restore", accent = Hw.MOD_FX)
        fun radio(label: String, samples: Boolean) = LedButton(a, label, tr = false, led = true, accentColor = Hw.MOD_FX).apply {
            textSize = 11.5f
            minHeight = 0
            minimumHeight = 0
            setPadding(d(16), d(6), d(6), d(6))
            gravity = Gravity.CENTER_VERTICAL or Gravity.START
            setOnClickListener { backupIncludeSamples = samples; syncBackupUi() }
        }
        sizeA = radio("Presets & settings", false)
        sizeB = radio("+ user samples", true)
        fun action(label: String, accent: Int, onClick: () -> Unit) = LedButton(a, label, tr = false, led = false, accentColor = accent).apply {
            textSize = 12.5f
            minHeight = 0
            minimumHeight = 0
            setPadding(d(6), 0, d(6), 0)
            setOnClickListener { onClick() }
        }
        backupNowBtn = action("BACKUP NOW", Hw.MOD_FX) { backupNow() }
        folderBtn = action("CHOOSE FOLDER…", Hw.MOD_OSC) { chooseBackupFolder() }
        restoreBtn = action("RESTORE…", Hw.MOD_FILTER) { chooseRestoreFile() }
        backupLcd = Hw.lcd(a, "", 11.5f).apply { maxLines = 2 }
        backupMsg = Ui.text(a, "", 11.5f, Hw.ENGRAVE, tr = false).apply { maxLines = 2 }
        backupBar = BarView(a)

        sec.body.addView(sizeA, LinearLayout.LayoutParams(-1, d(32)))
        sec.body.addView(sizeB, LinearLayout.LayoutParams(-1, d(32)).apply { topMargin = d(4) })
        sec.body.addView(LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(backupNowBtn, LinearLayout.LayoutParams(0, d(40), 1.3f).apply { marginEnd = d(6) })
            addView(folderBtn, LinearLayout.LayoutParams(0, d(40), 1f).apply { marginEnd = d(6) })
            addView(restoreBtn, LinearLayout.LayoutParams(0, d(40), 1f))
        }, LinearLayout.LayoutParams(-1, d(40)).apply { topMargin = d(8) })
        sec.body.addView(backupLcd, LinearLayout.LayoutParams(-1, -2).apply { topMargin = d(8) })
        sec.body.addView(backupBar, LinearLayout.LayoutParams(-1, d(8)).apply { topMargin = d(6) })
        sec.body.addView(backupMsg, LinearLayout.LayoutParams(-1, -2).apply { topMargin = d(4) })

        ActivityResults.register(ActivityResults.REQ_BACKUP_EXPORT) { data -> data?.data?.let { u -> runBackup { u } } }
        ActivityResults.register(ActivityResults.REQ_BACKUP_TREE) { data -> onBackupTree(data) }
        ActivityResults.register(ActivityResults.REQ_BACKUP_IMPORT) { data -> data?.data?.let { askRestore(it) } }
        syncBackupUi()
        updateBackupLcd()
        return sec
    }

    private fun syncBackupUi() {
        sizeA.lit = !backupIncludeSamples
        sizeB.lit = backupIncludeSamples
        val hasTree = a.prefs.getString("backupTreeUri", null) != null
        folderBtn.lit = hasTree
        folderBtn.text = if (hasTree) "FOLDER ✓" else "CHOOSE FOLDER…"
        for (b in arrayOf(backupNowBtn, folderBtn, restoreBtn, sizeA, sizeB)) { b.isEnabled = !backupBusy; b.alpha = if (backupBusy) 0.5f else 1f }
    }

    private fun refreshBackupSizes() {
        a.backupManager.estimateSizesAsync(false) { (bytes, _) -> sizeA.text = "Presets & settings ≈ ${sizeText(bytes)}" }
        a.backupManager.estimateSizesAsync(true) { (bytes, _) ->
            sizeB.text = "+ user samples ≈ ${sizeText(bytes)}" + if (bytes > (512L shl 20)) "  ⚠" else ""
            sizeB.setTextColor(if (bytes > (512L shl 20)) Hw.MOD_FILTER else Hw.SILK)
        }
    }

    private fun updateBackupLcd() {
        val ms = a.prefs.getLong("lastBackupMs", 0L)
        backupLcd.text = if (ms <= 0L) L10n.t("No backup yet")
        else "Last backup %s · %s · %,d presets".format(SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.US).format(Date(ms)),
            sizeText(a.prefs.getLong("lastBackupBytes", 0L)), a.prefs.getInt("lastBackupPresets", 0))
    }

    private fun backupNow() {
        if (backupBusy) return
        val tree = a.prefs.getString("backupTreeUri", null)
        if (tree != null) {
            // one press: the zip is created in the chosen folder
            runBackup {
                val t = Uri.parse(tree)
                val doc = DocumentsContract.buildDocumentUriUsingTree(t, DocumentsContract.getTreeDocumentId(t))
                DocumentsContract.createDocument(a.contentResolver, doc, "application/zip", backupName())
            }
            return
        }
        val i = Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "application/zip"
            putExtra(Intent.EXTRA_TITLE, backupName())
        }
        @Suppress("DEPRECATION")
        a.startActivityForResult(i, ActivityResults.REQ_BACKUP_EXPORT)
    }

    private fun chooseBackupFolder() {
        val i = Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).apply {
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION or
                Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION or Intent.FLAG_GRANT_PREFIX_URI_PERMISSION)
        }
        @Suppress("DEPRECATION")
        a.startActivityForResult(i, ActivityResults.REQ_BACKUP_TREE)
    }

    private fun onBackupTree(data: Intent?) {
        val u = data?.data ?: return
        try {
            a.contentResolver.takePersistableUriPermission(u, Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
        } catch (_: Exception) {}
        a.prefs.edit().putString("backupTreeUri", u.toString()).apply()
        backupMsg.text = L10n.t("Backup folder set — BACKUP NOW saves there in one tap.")
        syncBackupUi()
    }

    /** Streams the ZIP to the uri [target] returns (called on the loader thread). A partial file is removed on failure. */
    private fun runBackup(target: () -> Uri?) {
        if (backupBusy) return
        backupBusy = true
        syncBackupUi()
        backupBar.progress = 0f
        backupMsg.text = L10n.t("Backing up…")
        val samples = backupIncludeSamples
        a.loader.execute {
            var uri: Uri? = null
            try {
                uri = target() ?: throw IOException("could not create the backup file")
                val os = a.contentResolver.openOutputStream(uri) ?: throw IOException("cannot write the backup file")
                val sum = BufferedOutputStream(os, 64 * 1024).use { out ->
                    a.backupManager.createBackup(out, samples, { p, _ -> a.ui.post { backupBar.progress = p } })
                }
                a.ui.post {
                    backupBusy = false
                    backupBar.progress = 1f
                    a.prefs.edit().putLong("lastBackupMs", sum.createdMs).putLong("lastBackupBytes", sum.zipBytes)
                        .putInt("lastBackupPresets", sum.presets).apply()
                    backupMsg.text = L10n.t("Backup saved.")
                    a.log("backup saved (${sizeText(sum.zipBytes)}, ${sum.presets} presets)")
                    syncBackupUi()
                    updateBackupLcd()
                }
            } catch (e: Exception) {
                try { uri?.let { DocumentsContract.deleteDocument(a.contentResolver, it) } } catch (_: Exception) {}
                a.ui.post {
                    backupBusy = false
                    backupBar.progress = 0f
                    backupMsg.text = "Backup failed: ${e.message ?: e}"
                    a.log("backup failed: ${e.message ?: e}", Ui.ERR)
                    syncBackupUi()
                }
            }
        }
    }

    private fun chooseRestoreFile() {
        if (backupBusy) return
        val i = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
            putExtra(Intent.EXTRA_MIME_TYPES, arrayOf("application/zip", "application/x-zip-compressed", "application/octet-stream"))
        }
        @Suppress("DEPRECATION")
        a.startActivityForResult(i, ActivityResults.REQ_BACKUP_IMPORT)
    }

    private fun askRestore(uri: Uri) {
        if (backupBusy) return
        AlertDialog.Builder(a)
            .setTitle(L10n.t("Restore backup"))
            .setMessage(L10n.t("Replace everything (presets, scenes, set list, pads, settings) with this backup, or merge and keep newer files? " +
                "The backup is verified first and nothing changes if it is damaged. The app restarts afterwards."))
            .setPositiveButton(L10n.t("Replace")) { _, _ -> runRestore(uri, merge = false) }
            .setNeutralButton(L10n.t("Merge (keep newer)")) { _, _ -> runRestore(uri, merge = true) }
            .setNegativeButton(L10n.t("Cancel"), null)
            .show()
    }

    private fun runRestore(uri: Uri, merge: Boolean) {
        if (backupBusy) return
        backupBusy = true
        syncBackupUi()
        backupBar.progress = 0f
        backupMsg.text = L10n.t("Verifying and restoring…")
        val size = try {
            a.contentResolver.query(uri, arrayOf(OpenableColumns.SIZE), null, null, null)?.use { c -> if (c.moveToFirst() && !c.isNull(0)) c.getLong(0) else -1L } ?: -1L
        } catch (_: Exception) { -1L }
        a.loader.execute {
            val res = try {
                a.contentResolver.openInputStream(uri)?.use { input ->
                    a.backupManager.restoreBackup(input, merge, { p, _ -> a.ui.post { backupBar.progress = p } }, size)
                } ?: RestoreResult(false, "cannot read the file")
            } catch (e: Exception) {
                RestoreResult(false, e.message ?: e.toString())
            }
            a.ui.post { onRestoreDone(res) }
        }
    }

    private fun onRestoreDone(res: RestoreResult) {
        backupBusy = false
        syncBackupUi()
        if (!res.ok) {
            backupBar.progress = 0f
            backupMsg.text = "Restore failed: ${res.error}"
            a.log("restore failed: ${res.error}", Ui.ERR)
            return
        }
        backupBar.progress = 1f
        backupMsg.text = "Restored ${res.files} files, ${res.presets} presets."
        Toast.makeText(a, "Backup restored (${res.files} files) — restarting", Toast.LENGTH_LONG).show()
        // engine-side leftovers of the old state (pad clips) and pending in-memory writes must not survive the restart
        for (p in 0 until 16) NativeEngine.nativePadClear(p)
        if (a.isPlayerReady) a.player.padsPage.discardPending()
        a.recreate()
    }

    // ============================================================ B6 Wi-Fi web editor (SYS > SYSTEM)

    private fun buildWebSection(): View {
        val d = { v: Int -> Ui.dp(a, v) }
        val sec = Hw.Section(a, "Wi-Fi Web Editor", accent = Hw.MOD_FX)
        webToggle = ToggleSwitch(a, "Enable Web Editor (LAN only)", vertical = false, accent = Hw.MOD_FX).apply {
            onToggle = { on -> setWeb(on) }
        }
        webLcd = Hw.lcd(a, "", 12f).apply {
            isClickable = true
            setOnClickListener { copyWebUrl() }
        }
        pinLcd = Hw.lcd(a, "", 15f).apply { gravity = Gravity.CENTER_VERTICAL }
        newPinBtn = LedButton(a, "NEW PIN", tr = false, led = false, accentColor = Hw.MOD_FX).apply {
            textSize = 12f; minHeight = 0; minimumHeight = 0; setPadding(d(6), 0, d(6), 0)
            setOnClickListener { a.webEditorIfCreated()?.regeneratePin(); refreshWeb() }
        }
        clientsTv = Ui.text(a, "", 12f, Hw.ENGRAVE, tr = false).apply { typeface = Hw.MONO }
        stopWebBtn = LedButton(a, "STOP", tr = false, led = false, accentColor = Hw.MOD_RED).apply {
            textSize = 12f; minHeight = 0; minimumHeight = 0; setPadding(d(6), 0, d(6), 0)
            setOnClickListener { setWeb(false) }
        }
        sec.body.addView(webToggle, LinearLayout.LayoutParams(-1, d(36)))
        sec.body.addView(webLcd, LinearLayout.LayoutParams(-1, d(36)).apply { topMargin = d(6) })
        sec.body.addView(LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(Hw.engraved(a, "PIN", 10.5f, tr = false), LinearLayout.LayoutParams(d(36), -2))
            addView(pinLcd, LinearLayout.LayoutParams(0, d(36), 1f).apply { marginEnd = d(6) })
            addView(newPinBtn, LinearLayout.LayoutParams(d(84), d(36)))
        }, LinearLayout.LayoutParams(-1, d(36)).apply { topMargin = d(6) })
        sec.body.addView(LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(clientsTv, LinearLayout.LayoutParams(0, -2, 1f))
            addView(stopWebBtn, LinearLayout.LayoutParams(d(84), d(34)))
        }, LinearLayout.LayoutParams(-1, d(34)).apply { topMargin = d(6) })
        sec.body.addView(Ui.text(a, "HTTP, not encrypted. Anyone on this Wi-Fi who knows the PIN can edit sounds. Use a network you trust. " +
            "Stops automatically when the screen turns off, the app goes to the background or after 30 min idle. It is always off after a restart.",
            11.5f, Ui.WARN), LinearLayout.LayoutParams(-1, -2).apply { topMargin = d(8) })
        a.webEditorIfCreated()?.onStateChanged = { refreshWeb() }
        refreshWeb()
        return sec
    }

    private fun setWeb(on: Boolean) {
        if (on) {
            val s = a.webEditorServer
            s.onStateChanged = { refreshWeb() }
            if (!s.start()) webToggle.on = false
        } else {
            a.webEditorIfCreated()?.stop()
        }
        refreshWeb()
    }

    private fun copyWebUrl() {
        val url = a.webEditorIfCreated()?.takeIf { it.running }?.url ?: return
        (a.getSystemService(Context.CLIPBOARD_SERVICE) as? ClipboardManager)?.setPrimaryClip(ClipData.newPlainText("HSynth", url))
        Hw.onParamTouched?.invoke("URL", L10n.t("copied"))
        Toast.makeText(a, url, Toast.LENGTH_SHORT).show()
    }

    fun refreshWeb() {
        if (!::webToggle.isInitialized) return
        val s = a.webEditorIfCreated()
        val running = s?.running == true
        webToggle.on = running
        if (running && s != null) {
            webLcd.text = "● RUNNING  ${s.url}  (tap = copy)"
            webLcd.setTextColor(Hw.MOD_GOOD)
            pinLcd.text = s.pin.toCharArray().joinToString(" ")
            clientsTv.text = "clients: ${s.clientCount}"
        } else {
            webLcd.text = s?.error ?: s?.stopReason?.let { "OFF — stopped: $it" } ?: "OFF"
            webLcd.setTextColor(if (s?.error != null) Hw.MOD_FILTER else Hw.LCD_DIM)
            pinLcd.text = "—"
            clientsTv.text = ""
        }
        for (b in arrayOf(newPinBtn, stopWebBtn)) { b.isEnabled = running; b.alpha = if (running) 1f else 0.4f }
    }

    private companion object {
        const val PAGE_STATUS = 0
        const val PAGE_MIDI = 3
        const val PAGE_SYSTEM = 5
        const val PAGE_COUNT = 6
    }
}

// ============================================================================================== B4 pedals & footswitches

/**
 * Pedal / controller configuration in `files/pedal_config.json` (the engine's PerformanceInput JSON: sustain polarity,
 * velocity / aftertouch curves, expression range, 4 footswitch bindings). Applied at startup ([loadAndApply]) and after
 * every edit ([push], saved debounced, tmp + rename, on the loader). Never part of a preset.
 */
object PedalStore {
    var sustainInverted = false
    var velocityCurve = 0f        // −1 (soft) .. +1 (hard)
    var velocityFixed = 0         // 0 = off, else 1..127
    var aftertouchCurve = 0f
    var expMin = 0f
    var expMax = 1f
    val fsCc = intArrayOf(-1, -1, -1, -1)
    val fsAction = IntArray(4)
    val fsInvert = BooleanArray(4)

    private var file: File? = null
    private val handler = Handler(Looper.getMainLooper())
    private val saveRun = Runnable { writeNow() }

    fun toJson(): String = JSONObject().apply {
        put("sustainInverted", sustainInverted)
        put("velocityCurve", velocityCurve.toDouble())
        put("velocityFixed", velocityFixed)
        put("aftertouchCurve", aftertouchCurve.toDouble())
        put("expMin", expMin.toDouble())
        put("expMax", expMax.toDouble())
        put("footswitches", JSONArray().also { arr ->
            for (i in 0 until 4) arr.put(JSONObject().put("cc", fsCc[i]).put("action", fsAction[i]).put("invert", fsInvert[i]))
        })
    }.toString()

    fun parse(text: String): Boolean = try {
        val j = JSONObject(text)
        sustainInverted = j.optBoolean("sustainInverted", sustainInverted)
        velocityCurve = j.optDouble("velocityCurve", velocityCurve.toDouble()).toFloat().coerceIn(-1f, 1f)
        velocityFixed = j.optInt("velocityFixed", velocityFixed).coerceIn(0, 127)
        aftertouchCurve = j.optDouble("aftertouchCurve", aftertouchCurve.toDouble()).toFloat().coerceIn(-1f, 1f)
        expMin = j.optDouble("expMin", expMin.toDouble()).toFloat().coerceIn(0f, 1f)
        expMax = j.optDouble("expMax", expMax.toDouble()).toFloat().coerceIn(0f, 1f)
        j.optJSONArray("footswitches")?.let { arr ->
            for (i in 0 until minOf(4, arr.length())) {
                val o = arr.optJSONObject(i) ?: continue
                fsCc[i] = o.optInt("cc", fsCc[i]).coerceIn(-1, 119)
                fsAction[i] = o.optInt("action", fsAction[i]).coerceIn(0, 10)
                fsInvert[i] = o.optBoolean("invert", fsInvert[i])
            }
        }
        true
    } catch (_: Exception) {
        false
    }

    /** Startup: engine defaults, then `files/pedal_config.json`, then back to the engine. */
    fun loadAndApply(a: MainActivity) {
        val f = File(a.filesDir, "pedal_config.json")
        file = f
        parse(NativeEngine.nativeGetPedalConfigJson())
        if (f.isFile) {
            val ok = runCatching { parse(f.readText()) }.getOrDefault(false)
            if (ok) NativeEngine.nativeSetPedalConfigJson(toJson())
            else a.log("pedal_config.json is invalid — using the defaults", Ui.WARN)
        }
    }

    /** After an edit: apply to the engine and save (debounced). */
    fun push() {
        NativeEngine.nativeSetPedalConfigJson(toJson())
        handler.removeCallbacks(saveRun)
        handler.postDelayed(saveRun, 500L)
    }

    private fun writeNow() {
        val f = file ?: return
        val text = toJson()
        AppServices.loader.execute {
            runCatching {
                val tmp = File(f.parentFile, f.name + ".tmp")
                tmp.writeText(text)
                if (!tmp.renameTo(f)) { f.writeText(text); tmp.delete() }
            }
        }
    }
}

/** Static input → output curve preview (110 × 70 dp): `out = in^(2^(1.5·curve))`, or a flat line for a fixed velocity. */
@SuppressLint("ViewConstructor")
private class CurvePreviewView(ctx: Context) : View(ctx) {
    private val d = resources.displayMetrics.density
    private val pts = FloatArray(32)
    private val path = Path()
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)
    private val r = RectF()
    private var accent = Hw.MOD_FILTER

    init { set(0f, 0) }

    /** [curve] −1..+1; [fixed] > 0 draws a flat line at fixed / 127. */
    fun set(curve: Float, fixed: Int) {
        if (fixed > 0) java.util.Arrays.fill(pts, fixed / 127f)
        else {
            val e = 2f.pow(1.5f * curve)
            for (i in pts.indices) pts[i] = (i / (pts.size - 1f)).pow(e)
        }
        invalidate()
    }

    override fun onMeasure(widthSpec: Int, heightSpec: Int) {
        setMeasuredDimension((110 * d).toInt(), (70 * d).toInt())
    }

    override fun onDraw(c: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        r.set(0f, 0f, w, h)
        p.style = Paint.Style.FILL
        p.color = Hw.WELL
        c.drawRoundRect(r, 6f * d, 6f * d, p)
        val pad = 6f * d
        val l = pad
        val rt = w - pad
        val tp = pad
        val bt = h - pad
        p.style = Paint.Style.STROKE
        p.strokeWidth = d
        p.color = Hw.LED_OFF
        c.drawLine(l, bt, rt, tp, p)   // linear reference
        path.rewind()
        for (i in pts.indices) {
            val x = l + (rt - l) * i / (pts.size - 1f)
            val y = bt - (bt - tp) * pts[i]
            if (i == 0) path.moveTo(x, y) else path.lineTo(x, y)
        }
        p.strokeWidth = 2f * d
        p.color = accent
        c.drawPath(path, p)
        p.strokeWidth = d
        p.color = Hw.PANEL_EDGE
        r.inset(d / 2f, d / 2f)
        c.drawRoundRect(r, 6f * d, 6f * d, p)
    }
}

/** Live pedal lamp: "PEDAL ● DOWN / UP". */
@SuppressLint("ViewConstructor")
private class PedalLed(ctx: Context) : View(ctx) {
    private val d = resources.displayMetrics.density
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)
    private val t = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Hw.MONO; textSize = 11f * d }
    private val r = RectF()
    var down = false
        set(v) { if (field != v) { field = v; invalidate() } }

    override fun onMeasure(widthSpec: Int, heightSpec: Int) {
        setMeasuredDimension((104 * d).toInt(), (26 * d).toInt())
    }

    override fun onDraw(c: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        r.set(0f, 0f, w, h)
        p.color = Hw.LCD_BG
        p.style = Paint.Style.FILL
        c.drawRoundRect(r, 6f * d, 6f * d, p)
        p.style = Paint.Style.STROKE
        p.strokeWidth = d
        p.color = if (down) Hw.MOD_FILTER else Hw.PANEL_EDGE
        r.inset(d / 2f, d / 2f)
        c.drawRoundRect(r, 6f * d, 6f * d, p)
        p.style = Paint.Style.FILL
        if (down) {
            p.color = Hw.withAlpha(Hw.MOD_FILTER, 90)
            c.drawCircle(14f * d, h / 2f, 6f * d, p)
        }
        p.color = if (down) Hw.MOD_FILTER else Hw.LED_OFF
        c.drawCircle(14f * d, h / 2f, 3.5f * d, p)
        t.color = if (down) Hw.SILK else Hw.ENGRAVE
        c.drawText(if (down) "PEDAL DOWN" else "PEDAL UP", 26f * d, h / 2f + 4f * d, t)
    }
}

/** CC learn field: tap = "press the pedal…" (3 s), reads `nativeLastCc()`; long-press = clear (CC −1). Does not touch MIDI-learn. */
@SuppressLint("ViewConstructor", "ClickableViewAccessibility")
private class CcCaptureField(ctx: Context, private val onCc: (Int) -> Unit) : TextView(ctx) {
    var cc = -1
        set(v) { field = v; if (!learning) showCc() }

    var learning = false
        private set

    private var baseSeq = 0
    private var deadline = 0L

    init {
        textSize = 12f
        setTextColor(Hw.LCD_FG)
        typeface = Hw.MONO
        background = Hw.lcdBg(ctx)
        gravity = Gravity.CENTER
        maxLines = 1
        isClickable = true
        setOnClickListener { if (learning) stop() else start() }
        setOnLongClickListener {
            stop()
            cc = -1
            onCc(-1)
            true
        }
        showCc()
    }

    private fun showCc() {
        setTextColor(if (cc >= 0) Hw.LCD_FG else Hw.INK_DIM)
        text = if (cc >= 0) "CC $cc" else "—"
    }

    private fun start() {
        baseSeq = NativeEngine.nativeLastCc() ushr 16
        deadline = SystemClock.uptimeMillis() + 3000L
        learning = true
        setTextColor(Hw.MOD_FILTER)
        text = L10n.t("press the pedal…")
    }

    fun stop() {
        if (!learning) return
        learning = false
        showCc()
    }

    /** From the panel's 50 ms poll while [learning]. */
    fun poll(now: Long) {
        if (!learning) return
        val v = NativeEngine.nativeLastCc()
        if ((v ushr 16) != baseSeq) {
            val n = v and 0xFF
            if (n in 0..119) {
                learning = false
                cc = n
                onCc(n)
                return
            }
            baseSeq = v ushr 16
        }
        if (now >= deadline) stop()
    }
}

/**
 * B4 section body: sustain polarity + live lamp, expression range, velocity / aftertouch curves with previews and
 * four footswitch rows (learn CC, normal / invert, action). One 50 ms [FrameTicker] client — it runs only while the
 * MIDI page is showing (attached + shown + focused) and reads one int (`nativePedalState`) per tick.
 */
@SuppressLint("ViewConstructor")
private class PedalsPanel(private val a: MainActivity) : LinearLayout(a), FrameTicker.Client {
    private val d = resources.displayMetrics.density
    private fun dp(v: Int) = (v * d).toInt()
    private val st = PedalStore

    private val led = PedalLed(a)
    private val sustainRow: SegRow = SegRow(a, arrayOf("NORMAL", "INVERTED"), Hw.MOD_FILTER) { i -> st.sustainInverted = i == 1; sustainRow.selected = i; st.push() }
    private val velPreview = CurvePreviewView(a)
    private val atPreview = CurvePreviewView(a)
    private val fsLabel = Array(4) { Hw.engraved(a, "FS${it + 1}", 10.5f, tr = false) }
    private val fsCc = Array(4) { i -> CcCaptureField(a) { n -> st.fsCc[i] = n; st.push() } }
    private val fsInv = Array(4) { i ->
        LedButton(a, "NORMAL", tr = false, led = false, accentColor = Hw.MOD_FILTER).apply {
            textSize = 11f; minHeight = 0; minimumHeight = 0; setPadding(0, 0, 0, 0)
            setOnClickListener { st.fsInvert[i] = !st.fsInvert[i]; st.push(); syncFs(i) }
        }
    }
    private val fsAct = Array(4) { i ->
        LedButton(a, "", tr = false, led = false, accentColor = Hw.MOD_FILTER).apply {
            textSize = 11.5f; minHeight = 0; minimumHeight = 0; setPadding(dp(6), 0, dp(6), 0)
            setOnClickListener { chooseAction(i) }
        }
    }
    private var lastState = -1

    override val intervalMs: Int get() = 50
    override val tickView: View? get() = this

    companion object {
        private val CURVES = floatArrayOf(-1f, -0.5f, 0f, 0.5f, 1f)
        private val CURVE_NAMES = arrayOf("Soft 2", "Soft 1", "Normal", "Hard 1", "Hard 2")
        private val ACTIONS = arrayOf("None", "Scene Next", "Scene Prev", "SetList Next", "SetList Prev",
            "Rotary Fast/Slow", "Looper Tap", "Tap Tempo", "Metronome", "Phrase REC/DUB", "Panic")
    }

    private fun curveIndex(v: Float): Int {
        var best = 0
        for (i in CURVES.indices) if (Math.abs(CURVES[i] - v) < Math.abs(CURVES[best] - v)) best = i
        return best
    }

    init {
        orientation = VERTICAL
        fun row(label: String, v: View, vw: Int = 0, h: Int = 34): LinearLayout = LinearLayout(a).apply {
            orientation = HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(Hw.engraved(a, label, 10.5f), LayoutParams(dp(104), -2))
            addView(v, if (vw > 0) LayoutParams(dp(vw), dp(h)) else LayoutParams(0, dp(h), 1f))
        }

        // sustain
        sustainRow.selected = if (st.sustainInverted) 1 else 0
        val sus = LinearLayout(a).apply { orientation = HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        sus.addView(sustainRow, LayoutParams(dp(180), dp(30)))
        sus.addView(View(a), LayoutParams(0, 1, 1f))
        sus.addView(led, LayoutParams(dp(104), dp(26)))
        addView(row("Sustain pedal", sus, h = 34))
        addView(Ui.text(a, "Press the pedal: the lamp must light. If it is reversed, choose INVERTED.", 11f, Ui.DIM),
            LayoutParams(-1, -2).apply { topMargin = dp(2); bottomMargin = dp(8) })

        // expression range
        val exp = LinearLayout(a).apply { orientation = HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        exp.addView(Hw.engraved(a, "MIN", 9.5f, tr = false), LayoutParams(-2, -2).apply { marginEnd = dp(4) })
        exp.addView(DragNumber(a, 0f, 100f, 1f, 0f, { "${it.roundToInt()}%" }, { st.expMin * 100f }) { v ->
            st.expMin = v / 100f; st.push()
        }.apply { textSize = 12f }, LayoutParams(0, -1, 1f).apply { marginEnd = dp(8) })
        exp.addView(Hw.engraved(a, "MAX", 9.5f, tr = false), LayoutParams(-2, -2).apply { marginEnd = dp(4) })
        exp.addView(DragNumber(a, 0f, 100f, 1f, 100f, { "${it.roundToInt()}%" }, { st.expMax * 100f }) { v ->
            st.expMax = v / 100f; st.push()
        }.apply { textSize = 12f }, LayoutParams(0, -1, 1f))
        addView(row("Expression", exp, h = 34))

        // velocity curve + fixed velocity
        val velBox = LinearLayout(a).apply { orientation = HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        val velCol = LinearLayout(a).apply { orientation = VERTICAL }
        val velSpin = Ui.spinner(a, CURVE_NAMES.map { L10n.t(it) }, curveIndex(st.velocityCurve)) { i ->
            st.velocityCurve = CURVES[i]; velPreview.set(st.velocityCurve, st.velocityFixed); st.push()
        }
        velCol.addView(velSpin, LayoutParams(-1, dp(36)))
        val fixed = LinearLayout(a).apply { orientation = HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        fixed.addView(Hw.engraved(a, "FIXED", 9.5f, tr = false), LayoutParams(-2, -2).apply { marginEnd = dp(4) })
        fixed.addView(DragNumber(a, 0f, 127f, 1f, 0f, { if (it < 1f) L10n.t("OFF") else it.roundToInt().toString() }, { st.velocityFixed.toFloat() }) { v ->
            st.velocityFixed = v.roundToInt().coerceIn(0, 127); velPreview.set(st.velocityCurve, st.velocityFixed); st.push()
        }.apply { textSize = 12f }, LayoutParams(0, dp(32), 1f))
        velCol.addView(fixed, LayoutParams(-1, dp(32)).apply { topMargin = dp(4) })
        velBox.addView(velCol, LayoutParams(0, -2, 1f).apply { marginEnd = dp(8) })
        velBox.addView(velPreview, LayoutParams(dp(110), dp(70)))
        addView(row("Velocity curve", velBox, h = 72), LayoutParams(-1, -2).apply { topMargin = dp(6) })
        velPreview.set(st.velocityCurve, st.velocityFixed)

        // aftertouch
        val atBox = LinearLayout(a).apply { orientation = HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        val atSpin = Ui.spinner(a, CURVE_NAMES.map { L10n.t(it) }, curveIndex(st.aftertouchCurve)) { i ->
            st.aftertouchCurve = CURVES[i]; atPreview.set(st.aftertouchCurve, 0); st.push()
        }
        atBox.addView(atSpin, LayoutParams(0, dp(36), 1f).apply { marginEnd = dp(8) })
        atBox.addView(atPreview, LayoutParams(dp(110), dp(70)))
        addView(row("Aftertouch curve", atBox, h = 72), LayoutParams(-1, -2).apply { topMargin = dp(6) })
        atPreview.set(st.aftertouchCurve, 0)

        // footswitches
        addView(Hw.engraved(a, "Footswitches", 10.5f), LayoutParams(-2, -2).apply { topMargin = dp(10); bottomMargin = dp(2) })
        for (i in 0 until 4) {
            val r = LinearLayout(a).apply { orientation = HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
            r.addView(fsLabel[i], LayoutParams(dp(30), -2))
            r.addView(fsCc[i], LayoutParams(dp(96), dp(32)).apply { marginEnd = dp(5) })
            r.addView(fsInv[i], LayoutParams(dp(72), dp(32)).apply { marginEnd = dp(5) })
            r.addView(fsAct[i], LayoutParams(0, dp(32), 1f))
            addView(r, LayoutParams(-1, dp(36)))
            fsCc[i].cc = st.fsCc[i]
            syncFs(i)
        }
        addView(Ui.text(a, "Tap a CC field, then press the pedal. Long-press clears it. Scene / SetList actions step through your stored scenes and set list.", 11f, Ui.DIM),
            LayoutParams(-1, -2).apply { topMargin = dp(4) })
    }

    private fun syncFs(i: Int) {
        fsInv[i].text = L10n.t(if (st.fsInvert[i]) "INVERT" else "NORMAL")
        fsInv[i].lit = st.fsInvert[i]
        fsAct[i].text = L10n.t(ACTIONS[st.fsAction[i].coerceIn(0, ACTIONS.size - 1)])
        fsAct[i].lit = st.fsAction[i] != 0
    }

    private fun chooseAction(i: Int) {
        AlertDialog.Builder(a)
            .setTitle("FS${i + 1}")
            .setSingleChoiceItems(ACTIONS.map { L10n.t(it) }.toTypedArray(), st.fsAction[i]) { dlg, w ->
                st.fsAction[i] = w
                st.push()
                syncFs(i)
                dlg.dismiss()
            }.show()
    }

    override fun onFrame(nowMs: Long) {
        val s = NativeEngine.nativePedalState()
        if (s != lastState) {
            lastState = s
            led.down = (s and 1) != 0
            for (i in 0 until 4) fsLabel[i].setTextColor(if ((s and (2 shl i)) != 0) Hw.MOD_FILTER else Hw.ENGRAVE)
        }
        val now = SystemClock.uptimeMillis()
        for (f in fsCc) if (f.learning) f.poll(now)
    }

    override fun onAttachedToWindow() { super.onAttachedToWindow(); FrameTicker.register(this) }
    override fun onDetachedFromWindow() {
        for (f in fsCc) f.stop()
        FrameTicker.unregister(this)
        super.onDetachedFromWindow()
    }
    override fun onVisibilityAggregated(isVisible: Boolean) {
        super.onVisibilityAggregated(isVisible)
        if (!isVisible) for (f in fsCc) f.stop()
        FrameTicker.poke()
    }
    override fun onWindowFocusChanged(hasWindowFocus: Boolean) { super.onWindowFocusChanged(hasWindowFocus); FrameTicker.poke() }
}

/** Thin progress bar (8 dp) for backup / restore: 0..1; a negative value draws a dim full bar (size unknown). Zero-alloc. */
@SuppressLint("ViewConstructor")
internal class BarView(ctx: Context) : View(ctx) {
    var progress = 0f
        set(v) { if (field != v) { field = v; invalidate() } }
    private val d = resources.displayMetrics.density
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)
    private val r = RectF()

    override fun onDraw(c: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        r.set(0f, 0f, w, h)
        p.style = Paint.Style.FILL
        p.color = Hw.WELL
        c.drawRoundRect(r, h / 2f, h / 2f, p)
        val f = if (progress < 0f) 1f else progress.coerceIn(0f, 1f)
        if (f > 0f) {
            p.color = if (progress < 0f) Hw.withAlpha(Hw.MOD_FX, 70) else Hw.MOD_FX
            r.set(0f, 0f, max(h, w * f), h)
            c.drawRoundRect(r, h / 2f, h / 2f, p)
        }
        p.style = Paint.Style.STROKE
        p.strokeWidth = d
        p.color = Hw.PANEL_EDGE
        r.set(d / 2f, d / 2f, w - d / 2f, h - d / 2f)
        c.drawRoundRect(r, h / 2f, h / 2f, p)
    }
}
