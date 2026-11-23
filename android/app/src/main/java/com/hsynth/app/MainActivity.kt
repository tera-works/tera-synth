package com.hsynth.app

import android.app.Activity
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.SharedPreferences
import android.graphics.Color
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.os.SystemClock
import android.view.KeyEvent
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicInteger
import kotlin.math.pow

/** Process-wide services that must outlive an Activity instance. */
object AppServices {
    // background priority: loading presets / samples / songs must not take CPU
    // time from the audio callback and its render helpers
    val loader = Executors.newSingleThreadExecutor { r ->
        Thread({
            android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_BACKGROUND)
            r.run()
        }, "hsynth-loader")
    }
    // Serialized audio stream start/stop off the UI thread so slow USB DAC / AAudio
    // open/close calls never trigger ANR.
    val audioControl = Executors.newSingleThreadExecutor { r ->
        Thread(r, "hsynth-audio-ctl")
    }
    val audioGen = AtomicInteger(0)
}

class MainActivity : Activity() {
    companion object {
        private const val SCREEN_HOLD_MS = 5 * 60_000L
        private const val LIVE_RETURN_MS = 30_000L
        private const val ACTION_TICK_MS = 60L
        /** The running activity (PlaybackService controls the player through it). */
        @Volatile var instance: MainActivity? = null
            private set
    }
    val loader get() = AppServices.loader   // file I/O, sample decoding (process-wide)
    val ui = Handler(Looper.getMainLooper())
    lateinit var library: Library
    lateinit var prefs: SharedPreferences
    lateinit var midi: MidiInput

    lateinit var play: PlayPanel
    lateinit var live: LivePanel
    private var liveShown = false
    private var lastTouchMs = 0L
    lateinit var edit: EditPanel
    lateinit var libraryPanel: LibraryPanel
    lateinit var monitor: MonitorPanel
    lateinit var player: PlayerPanel
    lateinit var multi: MultiPanel
    lateinit var midiOut: MidiOutput
    lateinit var remote: RemoteLink
    lateinit var ble: BleMidi
    /** Overlay host for bottom / right / centre sheets (Master EQ, ...); lives in `contentHost`, above the panels. */
    lateinit var sheets: SheetHost
    lateinit var masterEqSheet: MasterEqSheet
    /** S5 TOOLS sheet: metronome, tuner, phrase recorder, scope. */
    lateinit var toolsSheet: ToolsSheet
    /** A3 MORPH drawer and A4 PIANO sheet: built lazily on first use, cached for the activity's lifetime. */
    val morphSheet: MorphSheet by lazy { MorphSheet(this) }
    val pianoSheet: PianoSheet by lazy { PianoSheet(this) }
    /** B2 live sampling wizard (CENTER sheet), built lazily; open it with [openSampleWizard]. */
    val sampleWizardSheet: SampleWizardSheet by lazy { SampleWizardSheet(this) }
    /** B6 ZIP backup / restore. */
    val backupManager: BackupManager by lazy { BackupManager(this) }
    /** Phase 5/6 integration. Everything is lazy, so none of it costs start-up time. */
    /** Engine side of the SF2 / SF3 / DLS / GIG / Decent Sampler / DX7 / wavetable importers. */
    val formatBridge: FormatBridge by lazy { NativeFormatBridge(NativeEngine) }
    /** Picker + router for the sound formats (also used by LIB, the EDIT wavetable bar and the sound packs). */
    val formatImporter: FormatImporter by lazy { FormatImporter(this, formatBridge) }
    /** Edge TPU 2D patch-morph pad (open with `openSheet(aiMorphSheet)`). */
    val aiMorphSheet: AiMorphSheet by lazy { AiMorphSheet(this, NativeNpuMorphBridge(NativeEngine)) }
    /** JSON sound-pack store. OPEN on an installed pack opens its first loadable file; finished downloads refresh LIB. */
    val soundPackSheet: SoundPackSheet by lazy {
        SoundPackSheet(this) { pack, dir -> libraryPanel.openInstalledPack(pack, dir) }.also { sheet ->
            sheet.downloader.addListener(object : SoundPackDownloader.Listener {
                override fun onResult(pack: SoundPack, ok: Boolean, message: String) {
                    if (ok && !isDestroyed) runCatching { libraryPanel.refresh() }
                }
            })
        }
    }
    /** Authors and licenses of the sound packs (SYS > SYSTEM > SOUND LICENSES); shares the store's catalog. */
    val soundLicensesSheet: SoundLicensesSheet by lazy { SoundLicensesSheet(this, soundPackSheet.catalog) }
    /** B6 Wi-Fi web editor: created on first use, always OFF at boot, stopped in onStop / outside the ACTIVE power state. */
    private val webServerLazy = lazy {
        WebEditorServer(
            this,
            remoteHandle = NativeEngine::nativeRemoteHandle,
            remoteChanges = NativeEngine::nativeRemoteChanges,
            setlistJsonGet = { setlistJsonGet() },
            setlistJsonSet = { setlistJsonSet(it) },
        )
    }
    val webEditorServer: WebEditorServer get() = webServerLazy.value
    /** B3: drains the engine's external-MIDI queue into the output slots (started in onCreate, stopped in onDestroy). */
    val extMidiPump = ExtMidiPump(this)
    private var eqChip: ChipButton? = null
    private var toolsChip: ChipButton? = null
    private val inputListeners = mutableListOf<() -> Unit>()
    private var backCallback: Any? = null
    /** Name of the sound loaded into each part (for the Multi page and the header). */
    val partNames = Array(16) { "" }
    private var learnArmed = -1

    private val railButtons = mutableListOf<RailNavButton>()
    private var sysBtn: LedButton? = null
    private lateinit var headerLcd: TextView
    private var topMidiDot: View? = null
    private var topMidiLabel: TextView? = null
    private var topAudioPill: TextView? = null
    private var topStereoMeter: MiniStereoMeter? = null
    private var topClockBat: TextView? = null
    private var topLastMidi = -1
    private var topMidiFlashUntil = 0L
    private var topLastSlowTick = 0L
    private val clearParamDisplay = Runnable { showPresetInHeader() }
    private val panels = mutableListOf<View>()
    private var lastNonStageTab = 1
    private val logLines = ArrayDeque<String>()
    private val logListeners = mutableListOf<() -> Unit>()

    var currentPreset: File? = null
        private set
    var currentPresetName = "Init"
        internal set
    private val presetListeners = mutableListOf<() -> Unit>()
    private var audioRunning = false
    /** Between onStart and onStop. */
    var visible = false
        private set
    private var pausedSince = 0L   // background, song paused: when (stops the audio after a while)
    val isPlayerReady get() = ::player.isInitialized

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // B6: a restore that was killed mid-swap is rolled back before anything reads files/
        BackupManager.recoverInterrupted(filesDir)
        instance = this
        // phones are played in landscape only; tablets (600 dp and wider) may rotate
        if (resources.configuration.smallestScreenWidthDp < 600)
            requestedOrientation = android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
        if (Build.VERSION.SDK_INT >= 28) {
            window.attributes = window.attributes.apply {
                layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
            }
        }
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        volumeControlStream = AudioManager.STREAM_MUSIC
        prefs = getSharedPreferences("hsynth", Context.MODE_PRIVATE)
        Hw.init(this)
        library = Library(this)
        L10n.init(prefs.getString("lang", "auto") ?: "auto")
        Params.clearListeners()
        // Default polyphony is 256 (Sol 6.2); move earlier 32 / 128 defaults once
        if (!prefs.getBoolean("voices256", false)) {
            val v = prefs.getInt("voices", 256)
            if (v == 32 || v == 128) prefs.edit().putInt("voices", 256).apply()
            prefs.edit().putBoolean("voices256", true).apply()
        }
        NativeEngine.nativeSetMaxVoices(prefs.getInt("voices", 256))
        NativeEngine.nativeSetQuality(prefs.getInt("quality", 1))
        NativeEngine.nativeSetMpe(prefs.getBoolean("mpe", false))
        // S1 seamless sound transition (default ON, 300 ms) and S3 master EQ / comp (global, never part of a preset)
        SstSweep.applySettings(prefs.getBoolean("sstOn", true), prefs.getInt("sstMs", 300))
        MasterEqStore.loadAndApply(this)
        // S5 click settings (the click itself always boots OFF) and B4 pedal / footswitch configuration
        MetronomeStore.loadAndApply(this)
        PedalStore.loadAndApply(this)
        // A6 rhythm groove settings (files/rhythm.json; the groove itself always boots stopped)
        RhythmStore.loadAndApply(this)
        midiOut = MidiOutput(this)
        // hot-plug: an external-MIDI slot whose saved device has just appeared is re-opened
        midi = MidiInput(this) { restoreExtPorts(); if (::monitor.isInitialized) monitor.refreshMidi() }
        midi.onMidiActivity = { onExternalMidiActivity() }
        restoreExtPorts()
        extMidiPump.start()
        remote = RemoteLink(this)
        ble = BleMidi(this)
        ble.onChange = { if (::monitor.isInitialized) monitor.refreshBle() }
        ble.onMidiActivity = { onExternalMidiActivity() }
        registerVolumeReceiver()
        syncAlsaVolume()
        markActivity()
        writePowerStateFile(force = true)
        NativeEngine.nativeLearnLoad(File(filesDir, "midi_map.json").absolutePath)

        buildUi(if (prefs.getBoolean("liveMode", true)) 0 else 4)
        hideSystemBars()

        log("HSynth ${packageManager.getPackageInfo(packageName, 0).versionName} starting")
        loader.execute {
            try {
                library.installFactoryContent(packageManager.getPackageInfo(packageName, 0).longVersionCode)
            } catch (e: Exception) {
                ui.post { log("factory content install failed: ${e.message}", Ui.ERR) }
            }
            ui.post {
                if (isDestroyed) return@post
                remote.start()  // SysEx remote control (after the factory presets are in place)
                ble.reconnect()  // Bluetooth MIDI devices of the last session, peripheral mode
                libraryPanel.refresh()
                // a setup placed at files/setups/startup.json (multi, MIDI map, host settings) is
                // applied on every start: the sound-module configuration of a dedicated device
                val startup = File(filesDir, "setups/startup.json")
                if (startup.isFile) {
                    loader.execute { multi.applySetup(startup.readText()) }
                    return@post
                }
                val last = prefs.getString("lastPreset", null)?.let { File(it) }?.takeIf { it.exists() }
                loadPreset(last ?: library.presets().firstOrNull()?.file ?: return@post)
            }
        }
        ui.post(statsTick)
    }

    private var currentTab = 0

    /** Builds the unified Obsidian Instrument layout (Left 52dp Rail + 26dp Telemetry Strip). */
    private fun buildUi(tab: Int) {
        // the old sheet host is about to be replaced: close what is open (tuner off, scope tap released) and save the set list
        if (::sheets.isInitialized) sheets.close()
        if (::live.isInitialized) live.store.flush()
        Params.clearListeners()
        MorphUi.clearListeners()
        RhythmStore.clearListeners()
        // A3: re-read the morph routes on every preset / part / multi load (first listener, so knobs built or
        // refreshed by the later listeners already see the new ranges) and once now for the panels built below
        Params.addListener { MorphUi.refresh() }
        MorphUi.refresh()
        presetListeners.clear()
        logListeners.clear()
        inputListeners.clear()
        ActivityResults.clear()
        railButtons.clear()
        panels.clear()
        val d = { v: Int -> Ui.dp(this, v) }
        val portrait = Ui.portrait(this)
        sheets = SheetHost(this)
        masterEqSheet = MasterEqSheet(this)
        toolsSheet = ToolsSheet(this)
        MasterEqStore.onChanged = { updateEqChip() }

        play = PlayPanel(this)
        edit = EditPanel(this)
        libraryPanel = LibraryPanel(this)
        monitor = MonitorPanel(this)
        if (!::player.isInitialized) player = PlayerPanel(this)
        (player.view.parent as? android.view.ViewGroup)?.removeView(player.view)
        player.setPortrait(portrait)
        player.onUiRebuilt()   // AUDIO / PADS file pickers re-register after ActivityResults.clear()
        multi = MultiPanel(this)
        live = LivePanel(this)
        multi.onSceneChanged = { live.onSceneChanged() }

        // Top 26dp Obsidian Telemetry Strip
        val statusStrip = ChassisBar(this).apply {
            setPadding(d(8), d(2), d(8), d(2))
        }
        val logo = TextView(this).apply {
            text = android.text.SpannableString("HSYNTH").apply {
                setSpan(android.text.style.ForegroundColorSpan(Hw.MOD_OSC), 0, 1, 0)
            }
            textSize = 12f
            letterSpacing = 0.14f
            setTextColor(Hw.SILK)
            typeface = Hw.BOLD
        }
        headerLcd = TextView(this).apply {
            textSize = 12f
            setTextColor(Hw.SILK)
            typeface = Hw.MONO
            maxLines = 1
            ellipsize = android.text.TextUtils.TruncateAt.END
            background = Hw.lcdBg(context, radiusDp = 4f)
            setPadding(d(7), d(1), d(7), d(1))
        }
        val midiDot = View(this).apply {
            background = Ui.rounded(Color.rgb(32, 46, 38), d(4))
        }
        topMidiDot = midiDot
        val midiLbl = TextView(this).apply {
            text = "MIDI"
            textSize = 9.5f
            typeface = Hw.MONO
            setTextColor(Hw.ENGRAVE)
            maxLines = 1
            ellipsize = android.text.TextUtils.TruncateAt.END
        }
        topMidiLabel = midiLbl
        val audioPill = TextView(this).apply {
            text = "ALSA · 0/256"
            textSize = 9.5f
            typeface = Hw.MONO
            setTextColor(Hw.MOD_OSC)
            background = Hw.lcdBg(context, stroke = Hw.PANEL_EDGE, radiusDp = 4f)
            setPadding(d(6), d(1), d(6), d(1))
            maxLines = 1
            setOnClickListener { selectTab(6) }
        }
        topAudioPill = audioPill
        val stereo = MiniStereoMeter(this)
        topStereoMeter = stereo
        val clkBat = TextView(this).apply {
            text = "--:--"
            textSize = 9.5f
            typeface = Hw.MONO
            setTextColor(Hw.ENGRAVE)
            maxLines = 1
        }
        topClockBat = clkBat
        val sys = LedButton(this, "SYS", tr = false, led = true, accentColor = Hw.MOD_OSC).apply {
            textSize = 10f
            minHeight = d(22)
            setPadding(d(15), d(1), d(7), d(1))
            setOnClickListener { selectTab(6) }
        }
        sysBtn = sys
        val eq = ChipButton(this, "EQ", widthDp = 34f, accent = Hw.MOD_FX).apply {
            setOnClickListener { toggleSheet(masterEqSheet) }
        }
        eqChip = eq
        // S5: ♩ chip — tap = TOOLS sheet, long-press = click on / off right now; LED: red = phrase recording, cyan = click, green = tuner
        val tools = ChipButton(this, "♩", widthDp = 30f, accent = Hw.MOD_OSC).apply {
            setOnClickListener { toggleSheet(toolsSheet) }
            setOnLongClickListener { toggleMetronome(); true }
        }
        toolsChip = tools

        statusStrip.addView(logo, LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(8) })
        statusStrip.addView(headerLcd, LinearLayout.LayoutParams(0, -2, 1f).apply { marginEnd = d(8) })
        statusStrip.addView(midiDot, LinearLayout.LayoutParams(d(7), d(7)).apply { marginEnd = d(4) })
        statusStrip.addView(midiLbl, LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(8) })
        statusStrip.addView(audioPill, LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(6) })
        statusStrip.addView(stereo, LinearLayout.LayoutParams(d(46), d(13)).apply { marginEnd = d(8) })
        statusStrip.addView(eq, LinearLayout.LayoutParams(d(34), d(22)).apply { marginEnd = d(6) })
        statusStrip.addView(tools, LinearLayout.LayoutParams(d(30), d(22)).apply { marginEnd = d(8) })
        if (!portrait) statusStrip.addView(clkBat, LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(6) })
        statusStrip.addView(sys, LinearLayout.LayoutParams(-2, d(22)))

        Hw.onParamTouched = { label, value ->
            headerLcd.text = "$label  $value"
            headerLcd.setTextColor(Hw.MOD_OSC)
            ui.removeCallbacks(clearParamDisplay)
            ui.postDelayed(clearParamDisplay, 1500)
        }

        // Content container for all 7 integrated panels:
        // 0=Stage(Live), 1=Multi, 2=Edit, 3=Library, 4=Play, 5=Song(Player), 6=Sys(Monitor)
        val content = FrameLayout(this).apply { setBackgroundColor(Hw.CHASSIS) }
        val allPanels = listOf(live.view, multi.view, edit.view, libraryPanel.view, play.view, player.view, monitor.view)
        allPanels.forEach { v ->
            (v.parent as? android.view.ViewGroup)?.removeView(v)
            panels += v
            content.addView(v, FrameLayout.LayoutParams(-1, -1))
        }

        // panels + sheet overlay; the telemetry strip and the rail stay live while a sheet is open
        val contentHost = FrameLayout(this).apply {
            addView(content, FrameLayout.LayoutParams(-1, -1))
            addView(sheets, FrameLayout.LayoutParams(-1, -1))
        }

        data class RailSpec(val tabIdx: Int, val label: String, val glyph: String, val accent: Int)
        val topSpecs = listOf(
            RailSpec(0, "STAGE", "◈", Hw.MOD_RED),
            RailSpec(1, "MULTI", "▤", Hw.MOD_OSC),
            RailSpec(2, "EDIT",  "∿", Hw.MOD_FILTER),
        )
        val botSpecs = listOf(
            RailSpec(3, "LIB",  "⊞", Hw.MOD_ENV),
            RailSpec(4, "PLAY", "🎹", Hw.MOD_OSC),
            RailSpec(5, "SONG", "▶", Hw.MOD_FX),
        )

        if (portrait) {
            val root = LinearLayout(this).apply {
                orientation = LinearLayout.VERTICAL
                setBackgroundColor(Hw.CHASSIS)
            }
            val bar = LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                setBackgroundColor(Color.rgb(8, 10, 15))
                setPadding(d(4), d(2), d(4), d(2))
            }
            (topSpecs + botSpecs).forEach { sp ->
                val b = RailNavButton(this, sp.label, sp.glyph, sp.accent).apply {
                    setOnClickListener { selectTab(sp.tabIdx) }
                }
                railButtons += b
                bar.addView(b, LinearLayout.LayoutParams(0, d(42), 1f))
            }
            root.addView(statusStrip, LinearLayout.LayoutParams(-1, d(28)))
            root.addView(bar, LinearLayout.LayoutParams(-1, d(46)))
            root.addView(contentHost, LinearLayout.LayoutParams(-1, 0, 1f))
            setContentView(root)
        } else {
            // Landscape Pixel 7 20:9: Left 52dp Rail (3 top + punch-hole camera gap + 3 bottom)
            val shell = LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                setBackgroundColor(Hw.CHASSIS)
            }
            val rail = LinearLayout(this).apply {
                orientation = LinearLayout.VERTICAL
                setBackgroundColor(Color.rgb(7, 9, 14))
                setPadding(0, d(4), 0, d(4))
            }
            topSpecs.forEach { sp ->
                val b = RailNavButton(this, sp.label, sp.glyph, sp.accent).apply {
                    setOnClickListener { selectTab(sp.tabIdx) }
                }
                railButtons += b
                rail.addView(b, LinearLayout.LayoutParams(-1, 0, 1f))
            }
            // Dedicated gap aligned with Pixel 7's landscape left-edge punch-hole camera
            rail.addView(View(this), LinearLayout.LayoutParams(-1, d(36)))
            botSpecs.forEach { sp ->
                val b = RailNavButton(this, sp.label, sp.glyph, sp.accent).apply {
                    setOnClickListener { selectTab(sp.tabIdx) }
                }
                railButtons += b
                rail.addView(b, LinearLayout.LayoutParams(-1, 0, 1f))
            }
            val divider = View(this).apply { setBackgroundColor(Hw.PANEL_EDGE) }
            val rightCol = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
            rightCol.addView(statusStrip, LinearLayout.LayoutParams(-1, d(26)))
            rightCol.addView(contentHost, LinearLayout.LayoutParams(-1, 0, 1f))

            shell.addView(rail, LinearLayout.LayoutParams(d(52), -1))
            shell.addView(divider, LinearLayout.LayoutParams(d(1), -1))
            shell.addView(rightCol, LinearLayout.LayoutParams(0, -1, 1f))
            setContentView(shell)
        }

        selectTab(tab.coerceIn(0, panels.size - 1))
        showPresetInHeader()
        logListeners.forEach { it() }
    }

    override fun onConfigurationChanged(newConfig: android.content.res.Configuration) {
        super.onConfigurationChanged(newConfig)
        if (::play.isInitialized) play.keyboard.allNotesOff()
        buildUi(currentTab)
        Params.notifyChanged()
        presetListeners.forEach { it() }
    }

    private fun hideSystemBars() {
        window.insetsController?.let {
            it.hide(WindowInsets.Type.systemBars())
            it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }

    /** A SysEx remote request changed the sound: show the new values. */
    fun onRemoteChange() {
        if (currentTab == 0) live.refresh()
        if (currentTab == 1) multi.refresh()
        if (currentTab == 2) edit.refreshValues()
    }

    fun selectTab(i: Int) {
        if (::sheets.isInitialized) sheets.close()
        val idx = i.coerceIn(0, panels.size - 1)
        if (idx != 0) lastNonStageTab = idx
        currentTab = idx
        liveShown = (idx == 0)
        panels.forEachIndexed { k, v -> v.visibility = if (k == idx) View.VISIBLE else View.GONE }
        railButtons.forEachIndexed { k, b -> b.active = (k == idx) }
        sysBtn?.lit = (idx == 6)
        when (idx) {
            0 -> live.refresh()
            1 -> multi.refresh()
            2 -> edit.refreshValues()
            3 -> libraryPanel.refresh()
            5 -> player.refreshPlaylist()
        }
        prefs.edit().putBoolean("liveMode", liveShown).apply()
        FrameTicker.poke()   // parked clients of the tab that just became visible (looper LCD, phrase ruler, ...)
    }

    override fun onStart() {
        super.onStart()
        instance = this
        visible = true
        val prevState = powerState
        powerState = PowerState.ACTIVE
        syncTicker()
        // B4: host actions (footswitch scene / set list steps) are polled every 60 ms while visible; stale ones are dropped
        for (k in 0 until 16) if (NativeEngine.nativeTakeHostAction() == 0) break
        ui.removeCallbacks(actionTick)
        ui.postDelayed(actionTick, ACTION_TICK_MS)
        markActivity()
        restoreScreenActive()
        if (PlaybackService.running && PlaybackService.standbyMode && (!isPlayerReady || !player.isPlaying)) {
            PlaybackService.stop(this)
        }
        syncAlsaVolume()
        if (!audioRunning) {  // still running when a song played in the background
            startAudio()
            midi.start()
        }
        if (prevState == PowerState.DEEP_SLEEP) {
            ble.reconnect()
        }
        writePowerStateFile(force = true)
    }

    override fun onStop() {
        super.onStop()
        visible = false
        stopWebEditor()          // B6: the Wi-Fi editor never runs with the screen off / the app in the background
        ui.removeCallbacks(actionTick)
        FrameTicker.pauseAll()
        onHostActiveChanged()   // scope tap + tuner off with the screen
        pausedSince = 0L
        // A newer Activity instance has already replaced this one (e.g. HomeAlias relaunch):
        // release only this Activity's MIDI listener; do NOT stop the shared native AudioEngine.
        if (instance !== this) {
            play.keyboard.allNotesOff()
            midi.stop()
            audioRunning = false
            return
        }
        // a song is playing: keep going in the background (PlaybackService holds the process)
        if (PlaybackService.running && player.isPlaying) {
            play.keyboard.allNotesOff()
            return
        }
        // If the screen turned off (either auto screen-off or power button) and BLE-wake standby
        // or deep sleep is enabled, keep the process alive in standby so BLE MIDI can wake us
        // until deepSleepMin expires.
        val pm = getSystemService(Context.POWER_SERVICE) as? PowerManager
        val screenOff = pm != null && !pm.isInteractive
        val standbyEnabled = prefs.getInt("powerStandbyMin", 5) > 0 || prefs.getInt("powerDeepSleepMin", 15) > 0
        if (screenOff && standbyEnabled && powerState != PowerState.DEEP_SLEEP) {
            play.keyboard.allNotesOff()
            if (powerState == PowerState.ACTIVE) {
                powerState = PowerState.SCREEN_OFF
            }
            if (!PlaybackService.running) runCatching { PlaybackService.startStandby(this) }
            writePowerStateFile(force = true)
            return
        }
        stopAudio()
    }

    /** From the notification's Close: stop the song, the audio and the service. */
    fun stopBackgroundPlayback() {
        PlaybackService.stop(this)
        if (!visible && audioRunning) stopAudio()
    }

    /** Starts / stops PlaybackService as songs play (stats tick, every 250 ms). */
    private fun updateBackgroundPlayback() {
        if (!isPlayerReady) return
        val playing = player.isPlaying
        if (playing) {
            pausedSince = 0L
            // a foreground service may only be started while the app is visible
            if ((!PlaybackService.running || PlaybackService.standbyMode) && visible) {
                runCatching { PlaybackService.start(this) }
            }
            return
        }
        if (!PlaybackService.running || PlaybackService.standbyMode) return
        if (visible) { PlaybackService.stop(this); return }
        // paused in the background: keep the notification (to resume) for 10 minutes
        val now = SystemClock.elapsedRealtime()
        if (pausedSince == 0L) pausedSince = now
        else if (now - pausedSince > 10 * 60_000L) stopBackgroundPlayback()
    }

    private fun stopAudioKeepMidi() {
        if (NativeEngine.nativeRecordSeconds() >= 0) {
            val sec = NativeEngine.nativeRecordStop()
            log("recording saved (%.1f s): %s".format(sec, lastRecording))
        }
        play.keyboard.allNotesOff()
        NativeEngine.control(NativeEngine.Control.ALL_NOTES_OFF, 0f)
        audioRunning = false
        val gen = AppServices.audioGen.incrementAndGet()
        AppServices.audioControl.execute {
            if (gen == AppServices.audioGen.get()) {
                NativeEngine.nativeStop()
            }
        }
    }

    private fun stopAudio() {
        stopAudioKeepMidi()
        midi.stop()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) hideSystemBars()
    }

    fun startAudio() {
        syncAlsaVolume()
        val deviceId = prefs.getInt("deviceId", 0)
        val exclusive = prefs.getBoolean("exclusive", true)
        val bufferBursts = prefs.getInt("bufferBursts", 0)
        val sampleRate = prefs.getInt("sampleRate", 48000)
        val alsaDirect = prefs.getBoolean("alsaDirect", false)
        audioRunning = true
        val gen = AppServices.audioGen.incrementAndGet()
        AppServices.audioControl.execute {
            if (gen != AppServices.audioGen.get()) return@execute
            val ok = NativeEngine.nativeStart(deviceId, exclusive, bufferBursts, sampleRate, alsaDirect)
            val note = NativeEngine.nativeAudioBackendNote()
            ui.post {
                if (isDestroyed || instance !== this) return@post
                if (gen == AppServices.audioGen.get()) audioRunning = ok
                if (note.isNotEmpty()) {
                    log(note, if (note.startsWith("Direct ALSA unavailable")) Ui.WARN else Ui.TEXT)
                }
                if (!ok) log("audio output could not be started (see Monitor tab)", Ui.ERR)
            }
        }
    }

    // ------------------------------------------------------------ presets

    fun addPresetListener(l: () -> Unit) { presetListeners += l }

    /** Part the editor (and preset loads) work on. */
    fun setEditPart(k: Int) {
        NativeEngine.nativeSetEditPart(k)
        currentPresetName = partNames[NativeEngine.nativeEditPart()].ifEmpty { currentPresetName }
        History.reset()
        Params.notifyChanged()
        presetListeners.forEach { it() }
        showPresetInHeader()
    }

    fun loadPreset(file: File, onDone: () -> Unit = {}) {
        log("loading preset ${file.name}…")
        SstSweep.start()
        loader.execute {
            val result = JSONObject(NativeEngine.nativeLoadPreset(file.absolutePath, library.libraryRoot.absolutePath))
            ui.post {
                if (isDestroyed) return@post
                if (!result.optBoolean("ok")) {
                    log("preset load failed: ${result.optString("error")}", Ui.ERR)
                    return@post
                }
                currentPreset = file
                currentPresetName = result.optString("name", file.nameWithoutExtension)
                prefs.edit().putString("lastPreset", file.absolutePath).apply()
                val w = result.optJSONArray("warnings")
                for (i in 0 until (w?.length() ?: 0)) log("  ${w!!.getString(i)}", Ui.WARN)
                log("preset \"$currentPresetName\" loaded")
                partNames[NativeEngine.nativeEditPart()] = currentPresetName
                History.reset()
                showPresetInHeader()
                onDone()
                Params.notifyChanged()
                presetListeners.forEach { it() }
            }
        }
    }

    // ------------------------------------------------------------ audio input / recording

    private val reqMic = 301

    fun setAudioInput(on: Boolean) {
        if (on && checkSelfPermission(android.Manifest.permission.RECORD_AUDIO) != android.content.pm.PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(android.Manifest.permission.RECORD_AUDIO), reqMic)
            return
        }
        val ok = NativeEngine.nativeSetInput(on)
        if (on && !ok) log("audio input could not be opened", Ui.ERR)
        fireInputListeners()
    }

    /** InputStatusRow & co. follow the audio-input state (also after the permission dialog). */
    fun addInputListener(l: () -> Unit) { if (!inputListeners.contains(l)) inputListeners += l }
    fun removeInputListener(l: () -> Unit) { inputListeners.remove(l) }
    private fun fireInputListeners() { inputListeners.toList().forEach { it() } }

    // ------------------------------------------------------------ Bluetooth MIDI

    private val reqBle = 302
    private var afterBlePermission: (() -> Unit)? = null

    /** Runs f once the Bluetooth permissions are granted (asks for them if needed). */
    fun withBlePermission(f: () -> Unit) {
        if (ble.hasPermissions()) return f()
        afterBlePermission = f
        requestPermissions(ble.permissions(), reqBle)
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == reqBle) {
            val f = afterBlePermission
            afterBlePermission = null
            if (grantResults.isNotEmpty() && grantResults.all { it == android.content.pm.PackageManager.PERMISSION_GRANTED }) f?.invoke()
            else log("Bluetooth permission denied", Ui.WARN)
            return
        }
        if (requestCode == reqMic) {
            if (grantResults.firstOrNull() == android.content.pm.PackageManager.PERMISSION_GRANTED) setAudioInput(true)
            else { log("microphone permission denied", Ui.WARN); fireInputListeners() }
        }
    }

    /** Starts / stops recording the output; returns whether it is recording now. */
    fun toggleRecording(): Boolean {
        if (NativeEngine.nativeRecordSeconds() >= 0) {
            val sec = NativeEngine.nativeRecordStop()
            log("recording saved (%.1f s): %s".format(sec, lastRecording))
            return false
        }
        val dir = (getExternalFilesDir("recordings") ?: File(filesDir, "recordings")).apply { mkdirs() }
        val f = File(dir, "HSynth_" + SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date()) + ".wav")
        if (!NativeEngine.nativeRecordStart(f.absolutePath)) { log("recording failed to start", Ui.ERR); return false }
        lastRecording = f.absolutePath
        log("recording…")
        return true
    }
    private var lastRecording = ""

    // ------------------------------------------------------------ MIDI learn

    fun learn(meta: ParamMeta) {
        NativeEngine.nativeLearnArm(meta.id)
        learnArmed = meta.id
        ui.removeCallbacks(clearParamDisplay)
        headerLcd.text = "LEARN ${meta.label}"
        log("MIDI learn: move a controller for ${meta.label}")
    }

    fun unlearn(meta: ParamMeta) {
        NativeEngine.nativeLearnRemove(meta.id)
        NativeEngine.nativeLearnSave(File(filesDir, "midi_map.json").absolutePath)
        log("MIDI learn removed: ${meta.label}")
    }

    private fun pollLearn() {
        if (learnArmed < 0) return
        if (NativeEngine.nativeLearnArmed() >= 0) return
        val p = learnArmed
        learnArmed = -1
        if (NativeEngine.nativeLearnLast() == p) {
            NativeEngine.nativeLearnSave(File(filesDir, "midi_map.json").absolutePath)
            val m = Params.meta.firstOrNull { it.id == p }
            log("MIDI learn: ${m?.label ?: p} = ${NativeEngine.nativeLearnDescribe(p)}")
            Hw.onParamTouched?.invoke(m?.label ?: "", NativeEngine.nativeLearnDescribe(p))
        } else showPresetInHeader()
    }

    private fun showPresetInHeader() {
        if (!::headerLcd.isInitialized) return
        headerLcd.setTextColor(Hw.SILK)
        headerLcd.text = if (NativeEngine.nativeIsMulti()) "P${NativeEngine.nativeEditPart() + 1} · $currentPresetName" else currentPresetName
    }

    fun stepPreset(delta: Int) {
        val list = library.presets()
        if (list.isEmpty()) return
        val idx = list.indexOfFirst { it.file == currentPreset }
        val next = ((if (idx < 0) 0 else idx + delta) % list.size + list.size) % list.size
        loadPreset(list[next].file)
    }

    fun savePreset(name: String, onDone: (File?) -> Unit) {
        val file = library.userPresetFile(name)
        loader.execute {
            val err = NativeEngine.nativeSavePreset(file.absolutePath, name, "User", library.libraryRoot.absolutePath)
            ui.post {
                if (err.isNotEmpty()) {
                    log("save failed: $err", Ui.ERR)
                    onDone(null)
                } else {
                    currentPreset = file
                    currentPresetName = name
                    prefs.edit().putString("lastPreset", file.absolutePath).apply()
                    log("preset saved: ${file.name}")
                    presetListeners.forEach { it() }
                    onDone(file)
                }
            }
        }
    }

    // ------------------------------------------------------------ samples

    fun loadSamples(layer: Int, file: File, onDone: () -> Unit = {}) {
        log("loading ${file.name} into layer ${"ABC"[layer]}…")
        loader.execute {
            val t0 = System.nanoTime()
            val r = JSONObject(NativeEngine.nativeLoadSamples(layer, file.absolutePath))
            val ms = (System.nanoTime() - t0) / 1_000_000
            ui.post {
                if (r.optBoolean("ok")) {
                    Params.set("${"abc"[layer]}.type", 3f)  // pcm
                    log("layer ${"ABC"[layer]}: ${file.name} loaded in $ms ms")
                } else {
                    log("load failed: ${r.optString("error")}", Ui.ERR)
                }
                val w = r.optJSONArray("warnings")
                for (i in 0 until (w?.length() ?: 0)) log("  ${w!!.getString(i)}", Ui.WARN)
                Params.notifyChanged()
                presetListeners.forEach { it() }
                onDone()
            }
        }
    }

    // ------------------------------------------------------------ log

    fun log(msg: String, color: Int = Ui.TEXT) {
        val ts = SimpleDateFormat("HH:mm:ss", Locale.US).format(Date())
        val tag = when (color) { Ui.ERR -> "ERROR "; Ui.WARN -> "warn  "; else -> "" }
        logLines.addLast("$ts $tag${L10n.log(msg)}")
        while (logLines.size > 300) logLines.removeFirst()
        logListeners.forEach { it() }
    }

    fun logText(): String = logLines.joinToString("\n")
    fun addLogListener(l: () -> Unit) { logListeners += l }

    // ------------------------------------------------------------ SAF results

    @Deprecated("Activity result API from the framework")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (resultCode != RESULT_OK) return
        if (ActivityResults.dispatch(requestCode, data)) return
        if (requestCode == PlayerPanel.REQ_FOLDER || requestCode == PlayerPanel.REQ_FILES) player.onActivityResult(requestCode, data)
        else if (requestCode == MultiPanel.REQ_SETUP_EXPORT || requestCode == MultiPanel.REQ_SETUP_IMPORT) multi.onActivityResult(requestCode, data)
        else libraryPanel.onActivityResult(requestCode, data)
    }

    // ------------------------------------------------------------ stats

    private var lastXruns = -1
    private val statsTick = object : Runnable {
        override fun run() {
            val s = NativeEngine.nativeGetStats()
            syncTicker()
            if (visible) {
                if (s.size > 20 && s[13].toInt() != lastXruns) {
                    // diagnostics: an underrun with the slowest callback / render since the last one
                    // phases: slowest block's us in params / swaps+events / voices / inserts / rack / stats, voices, parallel
                    if (lastXruns >= 0) android.util.Log.w("HSynth", "xrun +${s[13].toInt() - lastXruns} cbMax ${s[20].toInt()} us " +
                        "renderPeak ${s[5].toInt()} us phases ${NativeEngine.nativePeakProfile().joinToString(" ") { it.toInt().toString() }}")
                    lastXruns = s[13].toInt()
                }
                play.updateStats(s)
                pollLearn()
                player.updateStats(s)
                if (monitor.view.visibility == View.VISIBLE) monitor.updateStats(s)
                InputMeter.poll()
                updateEqChip()
                updateToolsChip()
                syncBackCallback()
                updateTopTelemetry(s)
                updateLive(s)
            }
            val pc = NativeEngine.nativeTakeProgramChange()
            if (pc >= 0) {
                val list = library.presets()
                if (list.isNotEmpty()) {
                    log("MIDI program change $pc")
                    loadPreset(list[pc % list.size].file)
                }
            }
            NativeEngine.nativeCollectGarbage()
            remote.poll()
            updateBackgroundPlayback()
            updatePowerState(s)
            syncTicker()
            // Slow UI tick to 2 s when screen is off to save wakeups & CPU power
            ui.postDelayed(this, if (visible) 250L else 2_000L)
        }
    }

    // ------------------------------------------------------------ Direct ALSA hardware volume

    private var volumeReceiverRegistered = false
    private val volumeReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            syncAlsaVolume()
        }
    }

    private fun registerVolumeReceiver() {
        if (volumeReceiverRegistered) return
        val filter = IntentFilter().apply {
            addAction("android.media.VOLUME_CHANGED_ACTION")
            addAction("android.media.STREAM_MUTE_CHANGED_ACTION")
        }
        runCatching {
            if (Build.VERSION.SDK_INT >= 33) registerReceiver(volumeReceiver, filter, Context.RECEIVER_EXPORTED)
            else @Suppress("DEPRECATION") registerReceiver(volumeReceiver, filter)
            volumeReceiverRegistered = true
        }
    }

    private fun syncAlsaVolume() {
        val am = getSystemService(Context.AUDIO_SERVICE) as? AudioManager ?: return
        val maxVol = am.getStreamMaxVolume(AudioManager.STREAM_MUSIC).coerceAtLeast(1)
        val vol = am.getStreamVolume(AudioManager.STREAM_MUSIC)
        val muted = am.isStreamMute(AudioManager.STREAM_MUSIC)
        val gain = if (muted || vol <= 0) {
            0f
        } else {
            val db = runCatching {
                if (Build.VERSION.SDK_INT >= 28)
                    am.getStreamVolumeDb(AudioManager.STREAM_MUSIC, vol, AudioDeviceInfo.TYPE_USB_HEADSET)
                else Float.NaN
            }.getOrDefault(Float.NaN)
            if (db.isFinite() && db <= 0f) 10f.pow(db / 20f).coerceIn(0f, 1f)
            else (vol.toFloat() / maxVol).let { it * it }
        }
        NativeEngine.nativeSetAlsaGain(gain)
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent?): Boolean {
        if (keyCode == KeyEvent.KEYCODE_VOLUME_UP || keyCode == KeyEvent.KEYCODE_VOLUME_DOWN ||
            keyCode == KeyEvent.KEYCODE_VOLUME_MUTE) {
            markActivity()
            ui.post { syncAlsaVolume() }
        }
        return super.onKeyDown(keyCode, event)
    }

    // ------------------------------------------------------------ power saving & screen hold
    // 4-stage configurable power manager:
    //   ACTIVE -> SCREEN_OFF -> AUDIO_STANDBY (BLE MIDI wake) -> DEEP_SLEEP (+ root auto power-off)

    private enum class PowerState(val wire: String) {
        ACTIVE("active"),
        SCREEN_OFF("screen_off"),
        AUDIO_STANDBY("standby"),
        DEEP_SLEEP("deep_sleep")
    }

    private val dedicatedMode by lazy {
        packageManager.getComponentEnabledSetting(android.content.ComponentName(this, "com.hsynth.app.HomeAlias")) ==
            android.content.pm.PackageManager.COMPONENT_ENABLED_STATE_ENABLED
    }
    @Volatile private var powerState = PowerState.ACTIVE
    @Volatile private var lastActivityMs = SystemClock.elapsedRealtime()
    @Volatile private var lastActivityEpochSec = System.currentTimeMillis() / 1000L
    private var lastMidiCount = -1
    private var screenHeld = true
    private var lastPowerWriteMs = 0L

    private fun markActivity() {
        lastActivityMs = SystemClock.elapsedRealtime()
        lastActivityEpochSec = System.currentTimeMillis() / 1000L
    }

    private fun restoreScreenActive() {
        if (!screenHeld) {
            screenHeld = true
            window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        }
        val lp = window.attributes
        if (lp.screenBrightness >= 0f) {
            lp.screenBrightness = WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE
            window.attributes = lp
        }
    }

    private fun onExternalMidiActivity() {
        markActivity()
        if (powerState != PowerState.ACTIVE) {
            ui.post { wakeFromMidi() }
        }
    }

    private fun wakeFromMidi() {
        if (isDestroyed || instance !== this) return
        val prev = powerState
        if (prev == PowerState.ACTIVE && audioRunning) return
        powerState = PowerState.ACTIVE
        syncTicker()
        markActivity()
        restoreScreenActive()
        val pm = getSystemService(Context.POWER_SERVICE) as? PowerManager
        if (pm != null && !pm.isInteractive) {
            @Suppress("DEPRECATION")
            runCatching {
                pm.newWakeLock(
                    PowerManager.SCREEN_BRIGHT_WAKE_LOCK or PowerManager.ACQUIRE_CAUSES_WAKEUP,
                    "hsynth:midi-wake"
                ).acquire(3_000L)
            }
        }
        if (!audioRunning) startAudio()
        if (prev == PowerState.DEEP_SLEEP) {
            midi.start()
            ble.reconnect()
        }
        if (visible && PlaybackService.running && PlaybackService.standbyMode && (!isPlayerReady || !player.isPlaying)) {
            PlaybackService.stop(this)
        }
        writePowerStateFile(force = true)
        if (prev == PowerState.AUDIO_STANDBY || prev == PowerState.DEEP_SLEEP) {
            log("power: woke up on MIDI")
        }
    }

    fun onPowerSettingsChanged() {
        markActivity()
        if (powerState != PowerState.ACTIVE && visible) {
            powerState = PowerState.ACTIVE
            syncTicker()
            restoreScreenActive()
            if (!audioRunning) startAudio()
        }
        writePowerStateFile(force = true)
    }

    /** Animated views only run while the app is in front and not in a screen-off / standby power stage. */
    private fun syncTicker() {
        if (powerState != PowerState.ACTIVE) stopWebEditor()
        val on = visible && powerState == PowerState.ACTIVE
        val changed = FrameTicker.hostActive != on
        FrameTicker.hostActive = on
        if (changed) onHostActiveChanged()
    }

    /** Screen off / standby / back in front: the native scope tap and the tuner follow (nothing runs for a dark screen). */
    private fun onHostActiveChanged() {
        ScopeData.sync()
        if (::toolsSheet.isInitialized) toolsSheet.syncHost()
    }

    // ------------------------------------------------------------ S5 click + B4 host actions

    /** ♩ chip long-press / footswitch-free shortcut: start or stop the click immediately. */
    fun toggleMetronome() {
        val on = !MetronomeStore.running
        MetronomeStore.setEnabled(on)
        Hw.onParamTouched?.invoke("CLICK", L10n.t(if (on) "on" else "off"))
        updateToolsChip()
    }

    /** `♩` chip LED: red = phrase recording / overdub, cyan = click running, green = tuner open; outlined while the sheet is open. */
    private fun updateToolsChip() {
        val chip = toolsChip ?: return
        if (!::toolsSheet.isInitialized || !::sheets.isInitialized) return
        val st = NativeEngine.nativePhraseStatus()
        val phraseRec = st.isNotEmpty() && (st[0].toInt() == 1 || st[0].toInt() == 3)
        chip.ledColor = when {
            phraseRec -> Hw.MOD_RED
            MetronomeStore.running -> Hw.MOD_OSC
            toolsSheet.tunerOpen -> Hw.MOD_GOOD
            else -> 0
        }
        val open = sheets.current === toolsSheet
        chip.active = open
        if (open) toolsSheet.poll()
    }

    private val actionTick = object : Runnable {
        override fun run() {
            if (!visible || isDestroyed) return
            // at most a few queued actions per tick; each is one atomic read
            for (k in 0 until 4) {
                val act = NativeEngine.nativeTakeHostAction()
                if (act == 0) break
                when (act) {
                    1 -> multi.stepScene(1)
                    2 -> multi.stepScene(-1)
                    3 -> live.chainStep(1)
                    4 -> live.chainStep(-1)
                }
            }
            ui.postDelayed(this, ACTION_TICK_MS)
        }
    }

    fun toggleSheet(s: Sheet) {
        sheets.toggle(s)
        syncBackCallback()
    }

    /**
     * B2: opens the live sampling wizard (LIB `● SAMPLE`, PADS `REC…`, EDIT `Record…`). [defaultLayer] (0..2) or
     * [defaultPad] (0..15) preselects the assign target of step ④; -1 = none.
     */
    fun openSampleWizard(defaultLayer: Int = -1, defaultPad: Int = -1) {
        if (!::sheets.isInitialized || sheets.current === sampleWizardSheet) return
        sampleWizardSheet.prepare(defaultLayer, defaultPad)
        openSheet(sampleWizardSheet)
    }

    // ------------------------------------------------------------ B3 external MIDI slots / B6 web editor helpers

    /** Re-opens every external-MIDI output slot whose saved device (prefs `extPort1..4`) is present but not open yet. */
    fun restoreExtPorts() {
        if (!::midiOut.isInitialized) return
        midiOut.ensureSlots(Array(MidiOutput.SLOTS + 1) { if (it == 0) "" else prefs.getString("extPort$it", "") ?: "" })
    }

    /** The web editor if it has been created (i.e. was switched on at least once this session), else null. */
    fun webEditorIfCreated(): WebEditorServer? = if (webServerLazy.isInitialized()) webServerLazy.value else null

    /** Stops the Wi-Fi web editor if it was ever started (never creates it). Safe to call any time. */
    fun stopWebEditor() {
        if (webServerLazy.isInitialized()) webServerLazy.value.stop()
    }

    /** The set list as `files/setlist.json`-shaped JSON for the web editor (UI thread). */
    private fun setlistJsonGet(): String {
        val st = live.store
        val arr = JSONArray()
        for (i in 0 until SetlistStore.SLOTS) {
            val s = st.slots[i]
            arr.put(JSONObject().put("i", i).put("kind", s.kind).put("ref", s.ref).put("title", s.title)
                .put("bpm", s.bpm).put("trn", s.trn).put("trim", s.trimDb.toDouble()).put("memo", s.memo)
                .put("memoShow", s.memoShow).put("color", s.color))
        }
        return JSONObject().put("v", 1).put("bank", st.viewedBank).put("active", st.activeSlot).put("chain", st.chainMode)
            .put("stopAtEnd", st.chainStopAtEnd).put("slots", arr).toString()
    }

    /**
     * Applies the editable fields (title, bpm, transpose, trim, memo, memoShow, colour) of a web-edited set list.
     * `kind` / `ref` are deliberately NOT taken from the web: it can edit notes and offsets but never point a slot at a file.
     */
    private fun setlistJsonSet(text: String): Boolean {
        val arr = try { JSONObject(text).optJSONArray("slots") } catch (_: Exception) { null } ?: return false
        val st = live.store
        for (k in 0 until arr.length()) {
            val e = arr.optJSONObject(k) ?: continue
            val i = e.optInt("i", k)
            if (i !in 0 until SetlistStore.SLOTS) continue
            st.update(i) {
                title = e.optString("title", title).take(80)
                bpm = e.optInt("bpm", bpm).let { if (it in 40..240) it else 0 }
                trn = e.optInt("trn", trn).coerceIn(-12, 12)
                trimDb = e.optDouble("trim", trimDb.toDouble()).toFloat().coerceIn(-12f, 6f)
                memo = e.optString("memo", memo).take(20_000)
                memoShow = e.optBoolean("memoShow", memoShow)
                color = e.optInt("color", color)
            }
        }
        live.refresh()
        return true
    }

    fun openSheet(s: Sheet) {
        sheets.open(s)
        syncBackCallback()
    }

    /** Telemetry `EQ` chip: emerald LED = stage active, amber = configured but bypassed; outlined while its sheet is open. */
    private fun updateEqChip() {
        val chip = eqChip ?: return
        val st = MasterEqStore
        chip.ledColor = if (st.active) Hw.MOD_FX else if (st.bypassedConfigured) Hw.MOD_FILTER else 0
        chip.active = ::sheets.isInitialized && ::masterEqSheet.isInitialized && sheets.current === masterEqSheet
    }

    // ------------------------------------------------------------ Back key closes an open sheet first

    @Deprecated("Back handling (pre-API 33 / opt-out of predictive back)")
    override fun onBackPressed() {
        if (::sheets.isInitialized && sheets.onBackPressed()) return
        @Suppress("DEPRECATION")
        super.onBackPressed()
    }

    /**
     * Apps targeting API 36 get predictive back, where onBackPressed() is no longer called. While a sheet is open an
     * OnBackInvokedCallback is registered (and dropped again once it is closed) so Back closes the sheet first.
     */
    private fun syncBackCallback() {
        if (Build.VERSION.SDK_INT < 33 || !::sheets.isInitialized) return
        val open = sheets.isOpen
        val cb = backCallback
        if (open && cb == null) {
            val c = android.window.OnBackInvokedCallback {
                if (!sheets.onBackPressed()) {
                    // stale registration (sheet closed by other means): hand the key back to the system default
                    syncBackCallback()
                    @Suppress("DEPRECATION")
                    super.onBackPressed()
                } else syncBackCallback()
            }
            backCallback = c
            onBackInvokedDispatcher.registerOnBackInvokedCallback(android.window.OnBackInvokedDispatcher.PRIORITY_DEFAULT, c)
        } else if (!open && cb != null) {
            onBackInvokedDispatcher.unregisterOnBackInvokedCallback(cb as android.window.OnBackInvokedCallback)
            backCallback = null
        }
    }

    private fun writePowerStateFile(force: Boolean = false) {
        val now = SystemClock.elapsedRealtime()
        if (!force && now - lastPowerWriteMs < 60_000L) return
        lastPowerWriteMs = now
        val payload = JSONObject().apply {
            put("state", powerState.wire)
            put("lastActivityEpochSec", lastActivityEpochSec)
            put("screenOffMin", prefs.getInt("powerScreenOffMin", 3))
            put("standbyMin", prefs.getInt("powerStandbyMin", 5))
            put("deepSleepMin", prefs.getInt("powerDeepSleepMin", 15))
            put("shutdownMin", prefs.getInt("powerShutdownMin", 60))
            put("shutdownBatteryOnly", prefs.getBoolean("powerShutdownBattOnly", true))
        }.toString()
        loader.execute {
            runCatching {
                val tmp = File(filesDir, "power_state.json.tmp")
                val dst = File(filesDir, "power_state.json")
                tmp.writeText(payload)
                tmp.renameTo(dst)
            }
        }
    }

    /** Show / hide the Stage (Live) screen; integrated as Tab 0 in the unified rail. */
    fun showLive(on: Boolean, byUser: Boolean = false) {
        if (on) {
            if (currentTab != 0) selectTab(0)
            else if (::live.isInitialized) live.refresh()
        } else {
            if (currentTab == 0) selectTab(lastNonStageTab.coerceIn(1, (panels.size - 1).coerceAtLeast(1)))
        }
        if (byUser) prefs.edit().putBoolean("liveMode", on).apply()
    }

    private fun updateTopTelemetry(s: FloatArray) {
        val now = SystemClock.elapsedRealtime()
        val mc = NativeEngine.nativeMidiEventCount()
        if (topLastMidi >= 0 && mc != topLastMidi) topMidiFlashUntil = now + 150
        topLastMidi = mc
        val lit = now < topMidiFlashUntil
        topMidiDot?.background = Ui.rounded(if (lit) Hw.MOD_GOOD else Color.rgb(32, 44, 38), Ui.dp(this, 4))
        if (s.size > 24) {
            topStereoMeter?.setPeak(s[7])
            val rs = NativeEngine.nativeRecordSeconds()
            if (rs >= 0) {
                topAudioPill?.text = "● REC %d:%02d".format(rs.toInt() / 60, rs.toInt() % 60)
                topAudioPill?.setTextColor(Hw.MOD_RED)
            } else {
                val running = s[8] > 0.5f
                val backend = if (prefs.getBoolean("alsaDirect", false)) "ALSA" else "AAUD"
                val voices = s[0].toInt()
                val maxV = s[24].toInt()
                val dsp = (s[2] * 100).toInt()
                topAudioPill?.text = if (running) "$backend · $voices/$maxV · $dsp%" else "AUDIO OFF"
                topAudioPill?.setTextColor(if (!running) Hw.MOD_RED else if (dsp > 70) Hw.MOD_FILTER else Hw.MOD_OSC)
            }
        }
        if (now - topLastSlowTick > 2000) {
            topLastSlowTick = now
            val sources = (ble.connectedDevices().map { it.second } + ble.peripheralClients() + midi.deviceNames()).distinct()
            topMidiLabel?.text = if (sources.isEmpty()) "NO MIDI" else sources.first()
            topMidiLabel?.setTextColor(if (sources.isEmpty()) Hw.INK_DIM else Hw.SILK)
            val timeStr = SimpleDateFormat("HH:mm", Locale.US).format(Date())
            val b = registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
            if (b != null) {
                val pct = b.getIntExtra(android.os.BatteryManager.EXTRA_LEVEL, -1) * 100 /
                    kotlin.math.max(1, b.getIntExtra(android.os.BatteryManager.EXTRA_SCALE, 100))
                val temp = b.getIntExtra(android.os.BatteryManager.EXTRA_TEMPERATURE, 0) / 10f
                topClockBat?.text = "%d%% %.0f°C %s".format(pct, temp, timeStr)
                topClockBat?.setTextColor(if (temp >= 42f) Hw.MOD_RED else Hw.ENGRAVE)
            } else {
                topClockBat?.text = timeStr
            }
        }
    }

    override fun dispatchTouchEvent(ev: android.view.MotionEvent): Boolean {
        lastTouchMs = SystemClock.elapsedRealtime()
        markActivity()
        if (powerState != PowerState.ACTIVE) {
            wakeFromMidi()
            // Consume the wake-up tap so touching a dimmed/blanked screen doesn't hit a random control
            return true
        }
        return super.dispatchTouchEvent(ev)
    }

    private var liveMidiCount = -1

    /** Dedicated mode: notes from the keyboard bring the Live screen back once the panels sit untouched. */
    private fun updateLive(s: FloatArray) {
        val mc = NativeEngine.nativeMidiEventCount()
        val changed = liveMidiCount >= 0 && mc != liveMidiCount
        liveMidiCount = mc
        if (liveShown) { live.updateStats(s); return }
        if (dedicatedMode && changed && SystemClock.elapsedRealtime() - lastTouchMs > LIVE_RETURN_MS) showLive(true)
    }

    private fun updatePowerState(s: FloatArray) {
        val now = SystemClock.elapsedRealtime()
        val midiCount = NativeEngine.nativeMidiEventCount()
        val midiChanged = lastMidiCount >= 0 && midiCount != lastMidiCount
        lastMidiCount = midiCount
        val activeVoices = s.isNotEmpty() && s[0] > 0f
        val audibleOutput = s.size > 7 && s[7] > 1e-4f
        val songPlaying = isPlayerReady && player.isPlaying
        val recording = NativeEngine.nativeRecordSeconds() >= 0.0
        if (midiChanged || activeVoices || audibleOutput || songPlaying || recording) {
            markActivity()
            if (powerState != PowerState.ACTIVE) {
                wakeFromMidi()
                return
            }
        }

        val idleSec = ((now - lastActivityMs).coerceAtLeast(0L)) / 1000L
        val screenOffSec = prefs.getInt("powerScreenOffMin", 3) * 60L
        val standbySec = prefs.getInt("powerStandbyMin", 5) * 60L
        val deepSleepSec = prefs.getInt("powerDeepSleepMin", 15) * 60L

        // Stage 1: Screen OFF (OLED blanking)
        if (powerState == PowerState.ACTIVE && screenOffSec > 0 && idleSec >= screenOffSec) {
            if ((standbySec > 0 || deepSleepSec > 0) && !PlaybackService.running && visible) {
                runCatching { PlaybackService.startStandby(this) }
            }
            powerState = PowerState.SCREEN_OFF
            if (screenHeld) {
                screenHeld = false
                window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            }
            val lp = window.attributes
            lp.screenBrightness = 0.01f
            window.attributes = lp
            writePowerStateFile(force = true)
            return
        }

        // Stage 2: Audio standby (stop 1 kHz audio stream & USB DAC burn, keep BLE MIDI connected to wake)
        if ((powerState == PowerState.ACTIVE || powerState == PowerState.SCREEN_OFF) &&
            standbySec > 0 && idleSec >= standbySec) {
            powerState = PowerState.AUDIO_STANDBY
            if (!PlaybackService.running && visible) {
                runCatching { PlaybackService.startStandby(this) }
            }
            if (screenHeld) {
                screenHeld = false
                window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            }
            if (audioRunning) stopAudioKeepMidi()
            writePowerStateFile(force = true)
            log("power: audio standby (BLE wake ready)")
            return
        }

        // Stage 3: Deep sleep (release foreground service & idle BLE polling so Android enters Deep Doze)
        if (powerState != PowerState.DEEP_SLEEP && deepSleepSec > 0 && idleSec >= deepSleepSec && !visible) {
            powerState = PowerState.DEEP_SLEEP
            if (audioRunning) stopAudio() else midi.stop()
            ble.stop()
            if (PlaybackService.running && PlaybackService.standbyMode) PlaybackService.stop(this)
            writePowerStateFile(force = true)
            log("power: deep sleep")
            return
        }

        writePowerStateFile(force = false)
    }

    override fun onPause() {
        if (::live.isInitialized) live.store.flush()   // pending bank / chain state of the set list
        if (isPlayerReady) player.flush()               // pending pad edits (files/pads/pads.json)
        super.onPause()
    }

    override fun onDestroy() {
        ui.removeCallbacks(statsTick)
        ui.removeCallbacks(actionTick)
        if (::sheets.isInitialized) sheets.close()   // closes TOOLS (tuner off) / Master EQ (scope tap released)
        if (::live.isInitialized) live.store.flush()
        if (volumeReceiverRegistered) {
            runCatching { unregisterReceiver(volumeReceiver) }
            volumeReceiverRegistered = false
        }
        stopWebEditor()
        extMidiPump.stop()
        midiOut.closeAllSlots()
        ble.stop()
        remote.stop()
        val wasCurrent = (instance === this)
        if (wasCurrent) {
            instance = null
            PlaybackService.stop(this)
            if (audioRunning) stopAudio()
        } else {
            midi.stop()
            audioRunning = false
        }
        super.onDestroy()
    }
}
