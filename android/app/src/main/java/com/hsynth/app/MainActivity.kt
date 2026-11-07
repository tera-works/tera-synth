package com.hsynth.app

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.graphics.Color
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import org.json.JSONObject
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.Executors

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
}

class MainActivity : Activity() {
    companion object {
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
    lateinit var edit: EditPanel
    lateinit var libraryPanel: LibraryPanel
    lateinit var monitor: MonitorPanel
    lateinit var player: PlayerPanel
    lateinit var multi: MultiPanel
    lateinit var midiOut: MidiOutput
    lateinit var remote: RemoteLink
    /** Name of the sound loaded into each part (for the Multi page and the header). */
    val partNames = Array(16) { "" }
    private var learnArmed = -1

    private val tabButtons = mutableListOf<LedButton>()
    private lateinit var headerLcd: TextView
    private val clearParamDisplay = Runnable { showPresetInHeader() }
    private val panels = mutableListOf<View>()
    private val logLines = ArrayDeque<String>()
    private val logListeners = mutableListOf<() -> Unit>()

    var currentPreset: File? = null
        private set
    var currentPresetName = "Init"
        private set
    private val presetListeners = mutableListOf<() -> Unit>()
    private var audioRunning = false
    /** Between onStart and onStop. */
    var visible = false
        private set
    private var pausedSince = 0L   // background, song paused: when (stops the audio after a while)
    val isPlayerReady get() = ::player.isInitialized

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        instance = this
        // phones are played in landscape only; tablets (600 dp and wider) may rotate
        if (resources.configuration.smallestScreenWidthDp < 600)
            requestedOrientation = android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        prefs = getSharedPreferences("hsynth", Context.MODE_PRIVATE)
        Hw.init(this)
        library = Library(this)
        L10n.init(prefs.getString("lang", "auto") ?: "auto")
        Params.clearListeners()
        // v0.5: the default polyphony became 128; move the old default (32) once
        if (!prefs.getBoolean("voices128", false)) {
            if (prefs.getInt("voices", 128) == 32) prefs.edit().putInt("voices", 128).apply()
            prefs.edit().putBoolean("voices128", true).apply()
        }
        NativeEngine.nativeSetMaxVoices(prefs.getInt("voices", 128))
        NativeEngine.nativeSetQuality(prefs.getInt("quality", 1))
        NativeEngine.nativeSetMpe(prefs.getBoolean("mpe", false))
        midi = MidiInput(this) { monitor.refreshMidi() }
        midiOut = MidiOutput(this)
        remote = RemoteLink(this)
        NativeEngine.nativeLearnLoad(File(filesDir, "midi_map.json").absolutePath)

        buildUi(0)
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
                libraryPanel.refresh()
                val last = prefs.getString("lastPreset", null)?.let { File(it) }?.takeIf { it.exists() }
                loadPreset(last ?: library.presets().firstOrNull()?.file ?: return@post)
            }
        }
        ui.post(statsTick)
    }

    private var currentTab = 0

    /** Builds every screen for the current orientation. */
    private fun buildUi(tab: Int) {
        Params.clearListeners()
        presetListeners.clear()
        logListeners.clear()
        tabButtons.clear()
        panels.clear()
        play = PlayPanel(this)
        val portrait = Ui.portrait(this)
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Ui.BG)
        }
        // header: logo, LCD (preset / touched parameter + status), mode buttons
        val tabs = ChassisBar(this).apply {
            setPadding(Ui.dp(context, 10), Ui.dp(context, 6), Ui.dp(context, 10), Ui.dp(context, 6))
        }
        val logo = TextView(this).apply {
            text = android.text.SpannableString("HSYNTH").apply {
                setSpan(android.text.style.ForegroundColorSpan(Hw.LED), 0, 1, 0)
            }
            textSize = 15f
            letterSpacing = 0.12f
            setTextColor(Color.rgb(230, 232, 238))
            typeface = Hw.BOLD
            textSize = 18f
        }
        headerLcd = TextView(this).apply {
            textSize = 17f
            setTextColor(Hw.LCD_FG)
            typeface = Hw.MONO
            maxLines = 1
            ellipsize = android.text.TextUtils.TruncateAt.END
        }
        val lcdBox = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = Hw.lcdBg(context)
            setPadding(Ui.dp(context, 10), Ui.dp(context, 3), Ui.dp(context, 10), Ui.dp(context, 3))
            addView(headerLcd)
            addView(play.statusText.apply {
                textSize = 10f
                typeface = Hw.MONO
                maxLines = 1
            })
        }
        // portrait: logo + LCD on one bar, the mode buttons on a second
        val headBar = if (portrait) ChassisBar(this).apply {
            setPadding(Ui.dp(context, 10), Ui.dp(context, 6), Ui.dp(context, 10), Ui.dp(context, 2))
        } else tabs
        headBar.addView(logo, LinearLayout.LayoutParams(-2, -2).apply { marginEnd = Ui.dp(this@MainActivity, 8) })
        headBar.addView(lcdBox, if (portrait) LinearLayout.LayoutParams(0, -2, 1f)
            else LinearLayout.LayoutParams(Ui.dp(this, 185), -2).apply { marginEnd = Ui.dp(this@MainActivity, 10) })
        Hw.onParamTouched = { label, value ->
            headerLcd.text = "$label  $value"
            ui.removeCallbacks(clearParamDisplay)
            ui.postDelayed(clearParamDisplay, 1500)
        }
        val content = FrameLayout(this)
        edit = EditPanel(this)
        libraryPanel = LibraryPanel(this)
        monitor = MonitorPanel(this)
        if (!::player.isInitialized) player = PlayerPanel(this)  // keeps playing across rotations
        (player.view.parent as? android.view.ViewGroup)?.removeView(player.view)
        player.setPortrait(portrait)
        multi = MultiPanel(this)
        listOf("Play" to play.view, "Edit" to edit.view, "Multi" to multi.view, "Library" to libraryPanel.view, "Player" to player.view, "System" to monitor.view).forEachIndexed { i, (name, v) ->
            val b = LedButton(this, name).apply {
                textSize = if (portrait) 11f else 12.5f
                letterSpacing = 0f
                setPadding(Ui.dp(context, if (portrait) 13 else 16), paddingTop, Ui.dp(context, 2), paddingBottom)
                setOnClickListener { selectTab(i) }
            }
            tabButtons += b
            tabs.addView(b, LinearLayout.LayoutParams(0, -2, 1f).apply { marginEnd = Ui.dp(this@MainActivity, if (portrait) 3 else 6) })
            panels += v
            content.addView(v, FrameLayout.LayoutParams(-1, -1))
        }
        if (portrait) root.addView(headBar, Ui.lp())
        root.addView(tabs, Ui.lp())
        root.addView(content, Ui.lp(weight = 1f, height = 0).apply { width = -1 })
        setContentView(root)
        selectTab(tab)
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
        if (currentTab == 1) edit.refreshValues()
        if (currentTab == 2) multi.refresh()
    }

    fun selectTab(i: Int) {
        currentTab = i
        panels.forEachIndexed { k, v -> v.visibility = if (k == i) View.VISIBLE else View.GONE }
        tabButtons.forEachIndexed { k, b -> b.lit = k == i }
        if (i == 1) edit.refreshValues()
        if (i == 2) multi.refresh()
        if (i == 3) libraryPanel.refresh()
        if (i == 4) player.refreshPlaylist()
    }

    override fun onStart() {
        super.onStart()
        visible = true
        if (!audioRunning) {  // still running when a song played in the background
            startAudio()
            midi.start()
        }
    }

    override fun onStop() {
        super.onStop()
        visible = false
        pausedSince = 0L
        // a song is playing: keep going in the background (PlaybackService holds the process)
        if (PlaybackService.running && player.isPlaying) {
            play.keyboard.allNotesOff()
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
            if (!PlaybackService.running && visible) runCatching { PlaybackService.start(this) }
            return
        }
        if (!PlaybackService.running) return
        if (visible) { PlaybackService.stop(this); return }
        // paused in the background: keep the notification (to resume) for 10 minutes
        val now = android.os.SystemClock.elapsedRealtime()
        if (pausedSince == 0L) pausedSince = now
        else if (now - pausedSince > 10 * 60_000L) stopBackgroundPlayback()
    }

    private fun stopAudio() {
        if (NativeEngine.nativeRecordSeconds() >= 0) {
            val sec = NativeEngine.nativeRecordStop()
            log("recording saved (%.1f s): %s".format(sec, lastRecording))
        }
        play.keyboard.allNotesOff()
        NativeEngine.control(NativeEngine.Control.ALL_NOTES_OFF, 0f)
        midi.stop()
        NativeEngine.nativeStop()
        audioRunning = false
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) hideSystemBars()
    }

    fun startAudio() {
        val ok = NativeEngine.nativeStart(prefs.getInt("deviceId", 0), prefs.getBoolean("exclusive", true), prefs.getInt("bufferBursts", 2))
        audioRunning = ok
        if (!ok) log("audio output could not be started (see Monitor tab)", Ui.ERR)
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
        monitor.inputSwitch?.on = on && ok
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == reqMic) {
            if (grantResults.firstOrNull() == android.content.pm.PackageManager.PERMISSION_GRANTED) setAudioInput(true)
            else { log("microphone permission denied", Ui.WARN); monitor.inputSwitch?.on = false }
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
        headerLcd.text = if (NativeEngine.nativeIsMulti()) "P${NativeEngine.nativeEditPart() + 1} $currentPresetName" else currentPresetName
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
        if (requestCode == PlayerPanel.REQ_FOLDER || requestCode == PlayerPanel.REQ_FILES) player.onActivityResult(requestCode, data)
        else libraryPanel.onActivityResult(requestCode, data)
    }

    // ------------------------------------------------------------ stats

    private var lastXruns = -1
    private val statsTick = object : Runnable {
        override fun run() {
            val s = NativeEngine.nativeGetStats()
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
            ui.postDelayed(this, 250)
        }
    }

    override fun onDestroy() {
        ui.removeCallbacks(statsTick)
        remote.stop()
        if (instance === this) instance = null
        PlaybackService.stop(this)
        if (audioRunning) stopAudio()
        super.onDestroy()
    }
}
