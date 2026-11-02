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
    val loader = Executors.newSingleThreadExecutor { r -> Thread(r, "hsynth-loader") }
}

class MainActivity : Activity() {
    val loader get() = AppServices.loader   // file I/O, sample decoding (process-wide)
    val ui = Handler(Looper.getMainLooper())
    lateinit var library: Library
    lateinit var prefs: SharedPreferences
    lateinit var midi: MidiInput

    lateinit var play: PlayPanel
    lateinit var edit: EditPanel
    lateinit var libraryPanel: LibraryPanel
    lateinit var monitor: MonitorPanel

    private val tabButtons = mutableListOf<TextView>()
    private val panels = mutableListOf<View>()
    private val logLines = ArrayDeque<String>()
    private val logListeners = mutableListOf<() -> Unit>()

    var currentPreset: File? = null
        private set
    var currentPresetName = "Init"
        private set
    private val presetListeners = mutableListOf<() -> Unit>()
    private var audioRunning = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        prefs = getSharedPreferences("hsynth", Context.MODE_PRIVATE)
        library = Library(this)
        Params.clearListeners()
        NativeEngine.nativeSetMaxVoices(prefs.getInt("voices", 32))
        NativeEngine.nativeSetQuality(prefs.getInt("quality", 1))
        NativeEngine.nativeSetMpe(prefs.getBoolean("mpe", false))
        midi = MidiInput(this) { monitor.refreshMidi() }

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Ui.BG)
        }
        val tabs = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setBackgroundColor(Color.rgb(11, 13, 17))
            setPadding(Ui.dp(context, 8), Ui.dp(context, 2), Ui.dp(context, 8), Ui.dp(context, 2))
        }
        val content = FrameLayout(this)
        play = PlayPanel(this)
        edit = EditPanel(this)
        libraryPanel = LibraryPanel(this)
        monitor = MonitorPanel(this)
        listOf("Play" to play.view, "Edit" to edit.view, "Library" to libraryPanel.view, "Monitor" to monitor.view).forEachIndexed { i, (name, v) ->
            val b = Ui.text(this, name, 16f, Ui.DIM, bold = true).apply {
                setPadding(Ui.dp(context, 18), Ui.dp(context, 8), Ui.dp(context, 18), Ui.dp(context, 8))
                setOnClickListener { selectTab(i) }
            }
            tabButtons += b
            tabs.addView(b)
            panels += v
            content.addView(v, FrameLayout.LayoutParams(-1, -1))
        }
        tabs.addView(View(this), LinearLayout.LayoutParams(0, 1, 1f))  // spacer (a bare View would expand to full height)
        tabs.addView(play.statusText)
        root.addView(tabs, Ui.lp())
        root.addView(content, Ui.lp(weight = 1f, height = 0).apply { width = -1 })
        setContentView(root)
        selectTab(0)
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
                libraryPanel.refresh()
                val last = prefs.getString("lastPreset", null)?.let { File(it) }?.takeIf { it.exists() }
                loadPreset(last ?: library.presets().firstOrNull()?.file ?: return@post)
            }
        }
        ui.post(statsTick)
    }

    private fun hideSystemBars() {
        window.insetsController?.let {
            it.hide(WindowInsets.Type.systemBars())
            it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }

    fun selectTab(i: Int) {
        panels.forEachIndexed { k, v -> v.visibility = if (k == i) View.VISIBLE else View.GONE }
        tabButtons.forEachIndexed { k, b -> b.setTextColor(if (k == i) Ui.ACCENT else Ui.DIM) }
        if (i == 1) edit.refreshValues()
        if (i == 2) libraryPanel.refresh()
    }

    override fun onStart() {
        super.onStart()
        startAudio()
        midi.start()
    }

    override fun onStop() {
        super.onStop()
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

    fun loadPreset(file: File) {
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
                Params.notifyChanged()
                presetListeners.forEach { it() }
            }
        }
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
        logLines.addLast("$ts $tag$msg")
        while (logLines.size > 300) logLines.removeFirst()
        logListeners.forEach { it() }
    }

    fun logText(): String = logLines.joinToString("\n")
    fun addLogListener(l: () -> Unit) { logListeners += l }

    // ------------------------------------------------------------ SAF results

    @Deprecated("Activity result API from the framework")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (resultCode == RESULT_OK) libraryPanel.onActivityResult(requestCode, data)
    }

    // ------------------------------------------------------------ stats

    private val statsTick = object : Runnable {
        override fun run() {
            val s = NativeEngine.nativeGetStats()
            play.updateStats(s)
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
            ui.postDelayed(this, 250)
        }
    }

    override fun onDestroy() {
        ui.removeCallbacks(statsTick)
        super.onDestroy()
    }
}
