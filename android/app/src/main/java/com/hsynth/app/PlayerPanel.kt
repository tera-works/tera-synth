package com.hsynth.app

import android.content.Intent
import android.graphics.Color
import android.graphics.Typeface
import android.net.Uri
import android.provider.DocumentsContract
import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.SeekBar
import android.widget.TextView
import org.json.JSONObject
import java.io.File

/**
 * MIDI file player (like the desktop "kitplayer"): LCD with title / BPM /
 * time, transport with repeat, seek, master volume, a 16-channel table (mute,
 * solo, volume, meters, controllers, keyboards) and a playlist that advances
 * by itself. Songs are played by HSynth's own engine (GM sounds: gm_bank.h).
 */
class PlayerPanel(private val a: MainActivity) {
    companion object {
        const val REQ_FOLDER = 201
        const val REQ_FILES = 202
    }

    enum class Repeat { OFF, ALL, ONE }

    val view: LinearLayout
    private val table = ChannelTableView(a)
    private val playlistCol = Ui.column(a)
    private val playlistScroll = ScrollView(a)
    private val content = FrameLayout(a)
    private val tableScroll = ScrollView(a)
    private val lcdMsg = Ui.text(a, "", 13f, Color.rgb(255, 112, 67))
    private val lcdTitle = Ui.text(a, "—", 20f, Hw.LCD_FG, bold = true, tr = false).apply { typeface = Hw.MONO }
    private val lcdInfo = Ui.text(a, "", 12f, Hw.LCD_FG, tr = false).apply { typeface = Hw.MONO }
    private val playBtn: LedButton
    private val repeatBtn: LedButton
    private val seek: SeekBar
    private val posText = Ui.text(a, "0:00", 13f, Ui.DIM)
    private val lenText = Ui.text(a, "0:00", 13f, Ui.DIM)
    private val masterSeek: SeekBar
    private val masterText = Ui.text(a, "100%", 13f, Ui.DIM)
    private val tabChannels: LedButton
    private val tabPlaylist: LedButton

    private lateinit var topRow: LinearLayout
    private lateinit var topLcd: View
    private lateinit var topTransport: View

    /** Re-arranges the top for the orientation (the player keeps playing). */
    fun setPortrait(portrait: Boolean) {
        val d = { v: Int -> Ui.dp(a, v) }
        topRow.removeAllViews()
        if (portrait) {
            topRow.orientation = LinearLayout.VERTICAL
            topRow.addView(topLcd, LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = d(8) })
            topRow.addView(topTransport, LinearLayout.LayoutParams(-1, -2))
        } else {
            topRow.orientation = LinearLayout.HORIZONTAL
            topRow.addView(topLcd, LinearLayout.LayoutParams(0, -1, 1f).apply { marginEnd = d(10) })
            topRow.addView(topTransport, LinearLayout.LayoutParams(0, -2, 1.05f))
        }
    }

    private var playlist: List<File> = emptyList()
    private var current = -1
    private var loaded = false
    private var loading = false
    private var repeat = Repeat.ALL
    private var lastEnd = -1
    private var seeking = false
    private var tempos: List<Pair<Double, Double>> = emptyList()
    private var dsp = 0f
    private var voices = 0

    init {
        val d = { v: Int -> Ui.dp(a, v) }
        table.onMix = { part, id, v -> NativeEngine.nativeSetPartMix(part, id, v) }

        // LCD
        val lcd = Ui.column(a).apply {
            setPadding(d(12), d(4), d(12), d(4))
            background = Hw.lcdBg(a)
            addView(lcdMsg)
            addView(lcdTitle)
            addView(lcdInfo)
        }
        // transport
        fun tbtn(s: String, size: Float = 16f, led: Boolean = false, onClick: () -> Unit) = LedButton(a, s, led = led).apply {
            textSize = size
            setOnClickListener { onClick() }
        }
        playBtn = tbtn("▶  Play", 15f, led = true) { togglePlay() }.apply { minWidth = d(if (Ui.portrait(a)) 96 else 130) }
        repeatBtn = tbtn("🔁", led = true) { cycleRepeat() }
        val transport = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            fun add(v: View) = addView(v, LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(8) })
            add(tbtn("⏮") { prev() })
            add(playBtn)
            add(tbtn("⏭") { next(manual = true) })
            add(repeatBtn)
            add(tbtn("■") { stop() })
            addView(View(a), LinearLayout.LayoutParams(0, 1, 1f))
        }
        seek = Ui.seek(a, 1000, 0) { _, _ -> }.apply {
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(sb: SeekBar, p: Int, fromUser: Boolean) {
                    if (fromUser) posText.text = fmt(p / 1000.0 * lengthSec())
                }
                override fun onStartTrackingTouch(sb: SeekBar) { seeking = true }
                override fun onStopTrackingTouch(sb: SeekBar) {
                    seeking = false
                    if (loaded) NativeEngine.nativeSongSeek(sb.progress / 1000.0 * lengthSec())
                }
            })
        }
        masterSeek = Ui.seek(a, 100, (NativeEngine.nativeGetParam(0) * 100).toInt()) { p, fromUser ->
            if (fromUser) {
                NativeEngine.nativeSetParam(0, p / 100f)  // master.volume
                masterText.text = "$p%"
            }
        }
        val seekRow = Ui.row(a, posText, seek, lenText).apply {
            (seek.layoutParams as LinearLayout.LayoutParams).apply { width = 0; weight = 1f }
        }
        val volRow = Ui.row(a, Hw.engraved(a, "Master volume", 11f), masterSeek, masterText,
            tbtn("Reset Mix", 12f) { resetChannels() }).apply {
            (masterSeek.layoutParams as LinearLayout.LayoutParams).apply { width = 0; weight = 1f }
        }
        val right = Ui.column(a).apply {
            addView(transport)
            addView(seekRow, Ui.lp().apply { width = -1 })
            addView(volRow, Ui.lp().apply { width = -1 })
        }
        val transportPanel = Hw.Section(a, "Transport").apply { body.addView(right) }
        topRow = LinearLayout(a)
        topLcd = lcd
        topTransport = transportPanel
        setPortrait(Ui.portrait(a))
        // tabs
        tabChannels = LedButton(a, "Channels").apply { setOnClickListener { showTab(0) } }
        tabPlaylist = LedButton(a, L10n.f("Playlist (%d)", 0), tr = false).apply { setOnClickListener { showTab(1) } }
        val tabs = Ui.row(a, tabChannels, tabPlaylist).apply {
            setPadding(0, d(8), 0, d(6))
            addView(View(a), LinearLayout.LayoutParams(0, 1, 1f))
            addView(tbtn("Open Folder", 13f) { pickFolder() }, LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(8) })
            addView(tbtn("Open Songs", 13f) { pickFiles() })
        }
        playlistScroll.addView(playlistCol)
        // the table keeps its landscape width and scrolls sideways in portrait
        tableScroll.addView(android.widget.HorizontalScrollView(a).apply {
            addView(table, android.view.ViewGroup.LayoutParams(maxOf(Ui.dp(a, 760), a.resources.displayMetrics.widthPixels - Ui.dp(a, 20)), -2))
        })
        content.addView(tableScroll, FrameLayout.LayoutParams(-1, -1))
        content.addView(playlistScroll, FrameLayout.LayoutParams(-1, -1))
        view = Ui.column(a).apply {
            setPadding(d(10), d(8), d(10), d(2))
            addView(topRow, Ui.lp().apply { width = -1 })
            addView(android.widget.HorizontalScrollView(a).apply { isHorizontalScrollBarEnabled = false; addView(tabs) })
            addView(content, Ui.lp(weight = 1f, height = 0).apply { width = -1 })
        }
        showTab(0)
        updateRepeat()
        refreshPlaylist()
        tick()
    }

    // ------------------------------------------------------------ state

    fun lengthSec() = NativeEngine.nativeSongState()[1]

    private fun fmt(s: Double): String {
        val t = s.toInt().coerceAtLeast(0)
        return "%d:%02d".format(t / 60, t % 60)
    }

    private fun bpmAt(t: Double): Double {
        var b = tempos.firstOrNull()?.second ?: 120.0
        for ((time, bpm) in tempos) {
            if (time > t) break
            b = bpm
        }
        return b
    }

    fun updateStats(s: FloatArray) {  // from MainActivity's stats tick
        if (s.size > 2) {
            dsp = s[2]
            voices = s[0].toInt()
        }
    }

    private fun tick() {
        val shown = view.visibility == View.VISIBLE && a.visible  // no drawing in the background
        if (shown) {
            val st = NativeEngine.nativePlayerState()
            val masks = NativeEngine.nativeNoteMasks()
            table.update(st, masks)
            val pos = st[0].toDouble()
            val len = st[1].toDouble()
            val playing = st[2] > 0.5f
            val ends = st[3].toInt()
            if (lastEnd < 0) lastEnd = ends
            if (ends != lastEnd) {
                lastEnd = ends
                onSongEnded()
            }
            playBtn.text = L10n.t(if (playing) "⏸  Pause" else "▶  Play")
            playBtn.lit = playing
            if (!seeking && len > 0) {
                seek.progress = (pos / len * 1000).toInt()
                posText.text = fmt(pos)
                lenText.text = fmt(len)
            }
            val bpm = if (loaded) "%.0f".format(bpmAt(pos)) else "--"
            val n = if (current >= 0) "${current + 1} / ${playlist.size}" else "- / ${playlist.size}"
            lcdInfo.text = L10n.f("BPM %s   TIME %s / %s   SONG %s   DSP %d%%   VOICES %d", bpm, fmt(pos), fmt(len), n, (dsp * 100).toInt(), voices)
            masterText.text = "${(st[4] * 100).toInt()}%"
        } else {
            // keep following the end of songs (playlists advance in the background)
            if (loaded) {
                val ends = NativeEngine.nativePlayerState()[3].toInt()
                if (lastEnd >= 0 && ends != lastEnd) {
                    lastEnd = ends
                    onSongEnded()
                }
            }
        }
        view.postDelayed({ tick() }, if (shown) 33L else 250L)
    }

    private fun showTab(i: Int) {
        tableScroll.visibility = if (i == 0) View.VISIBLE else View.GONE
        playlistScroll.visibility = if (i == 1) View.VISIBLE else View.GONE
        tabChannels.lit = i == 0
        tabPlaylist.lit = i == 1
    }

    // ------------------------------------------------------------ playlist

    private val songDir get() = a.libraryPanel.songDir

    fun refreshPlaylist() {
        val cur = playlist.getOrNull(current)
        playlist = songDir.listFiles { f -> f.isFile && (f.name.endsWith(".mid", true) || f.name.endsWith(".midi", true)) }
            ?.sortedBy { it.name.lowercase() } ?: emptyList()
        current = if (cur != null) playlist.indexOf(cur) else -1
        tabPlaylist.text = L10n.f("Playlist (%d)", playlist.size)
        playlistCol.removeAllViews()
        val d = { v: Int -> Ui.dp(a, v) }
        if (playlist.isEmpty()) playlistCol.addView(Ui.text(a, L10n.f("(no songs)\nAdd some with \"Open Folder\" / \"Open Songs\"\n%s", songDir.absolutePath), 13f, Ui.DIM, tr = false))
        playlist.forEachIndexed { i, f ->
            val row = Ui.row(a,
                Ui.text(a, "%3d".format(i + 1), 14f, Ui.DIM).apply { typeface = Hw.MONO },
                Ui.text(a, "  " + f.nameWithoutExtension, 15f, if (i == current) Ui.ACCENT else Ui.TEXT, bold = i == current, tr = false),
            ).apply {
                setPadding(d(10), d(8), d(10), d(8))
                background = Ui.rounded(if (i == current) Color.rgb(30, 60, 80) else Ui.PANEL, d(6))
                setOnClickListener { playIndex(i) }
                layoutParams = LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = d(3) }
            }
            playlistCol.addView(row)
        }
    }

    /** Play a file (from the Library tab, or after adding files). */
    fun playFile(f: File) {
        refreshPlaylist()
        val i = playlist.indexOf(f)
        if (i >= 0) playIndex(i)
    }

    private fun playIndex(i: Int) {
        if (i !in playlist.indices || loading) return
        val f = playlist[i]
        current = i
        refreshPlaylist()
        loading = true
        lcdTitle.text = f.nameWithoutExtension
        lcdMsg.text = L10n.t("Loading sounds…")
        NativeEngine.nativeSongPlay(false)
        val lib = a.library
        a.loader.execute {
            val dirs = listOf(lib.factoryPresets, lib.userPresets).joinToString(";") { it.absolutePath }
            val r = JSONObject(NativeEngine.nativeSongLoad(f.absolutePath, lib.libraryRoot.absolutePath, dirs))
            a.runOnUiThread {
                loading = false
                if (!r.optBoolean("ok")) {
                    lcdMsg.text = L10n.f("Cannot load: %s", r.optString("error"))
                    a.log("song failed: ${r.optString("error")}", Ui.ERR)
                    loaded = false
                    return@runOnUiThread
                }
                loaded = true
                lcdMsg.text = ""
                for (k in 0 until 16) {
                    table.preset[k] = ""
                    table.gm[k] = ""
                    table.used[k] = false
                }
                val ch = r.optJSONArray("channels")
                if (ch != null) for (j in 0 until ch.length()) {
                    val o = ch.getJSONObject(j)
                    val k = o.getInt("ch")
                    table.used[k] = true
                    table.preset[k] = o.optString("preset").replace(Regex("^SC GM2 (Capital|Rhythm Sets) [0-9]+ "), "")
                    table.gm[k] = if (o.optBoolean("drum")) L10n.t("Drums") else o.optString("gm")
                }
                val t = r.optJSONArray("tempos")
                tempos = if (t != null) (0 until t.length()).map { t.getJSONArray(it).let { p -> p.getDouble(0) to p.getDouble(1) } }
                         else listOf(0.0 to r.optDouble("tempo", 120.0))
                table.applyMutes()
                for (k in 0 until 16) a.partNames[k] = table.preset[k]
                lastEnd = NativeEngine.nativePlayerState()[3].toInt()
                NativeEngine.nativeSongSeek(0.0)
                NativeEngine.nativeSongPlay(true)
                a.log("playing ${r.optString("name")} (${fmt(r.optDouble("length"))})")
            }
        }
    }

    /** A song is loaded and playing (background playback, notification). */
    val isPlaying: Boolean get() = loaded && NativeEngine.nativeSongState()[2] > 0.5
    val title: String? get() = playlist.getOrNull(current)?.nameWithoutExtension
    fun positionSec(): Double = NativeEngine.nativeSongState()[0]

    fun togglePlay() {
        if (!loaded) {
            playIndex(if (current >= 0) current else 0)
            return
        }
        val playing = NativeEngine.nativeSongState()[2] > 0.5
        NativeEngine.nativeSongPlay(!playing)
    }

    fun stop() {
        NativeEngine.nativeSongPlay(false)
        NativeEngine.nativeSongSeek(0.0)
    }

    fun prev() {
        val pos = NativeEngine.nativeSongState()[0]
        if (loaded && pos > 3.0 || playlist.isEmpty()) {
            NativeEngine.nativeSongSeek(0.0)
            return
        }
        playIndex(if (current <= 0) playlist.size - 1 else current - 1)
    }

    fun next(manual: Boolean) {
        if (playlist.isEmpty()) return
        val n = current + 1
        if (n >= playlist.size) {
            if (manual || repeat == Repeat.ALL) playIndex(0) else stop()
        } else {
            playIndex(n)
        }
    }

    private fun onSongEnded() {
        when (repeat) {
            Repeat.ONE -> { NativeEngine.nativeSongSeek(0.0); NativeEngine.nativeSongPlay(true) }
            else -> next(manual = false)
        }
    }

    private fun cycleRepeat() {
        repeat = when (repeat) { Repeat.OFF -> Repeat.ALL; Repeat.ALL -> Repeat.ONE; Repeat.ONE -> Repeat.OFF }
        updateRepeat()
    }

    private fun updateRepeat() {
        repeatBtn.text = if (repeat == Repeat.ONE) "🔂" else "🔁"
        repeatBtn.lit = repeat != Repeat.OFF
        lcdMsg.text = L10n.t(when (repeat) { Repeat.OFF -> "Repeat: off"; Repeat.ALL -> "Repeat: all"; Repeat.ONE -> "Repeat: one" })
    }

    private fun resetChannels() {
        for (k in 0 until 16) {
            table.userMute[k] = false
            table.solo[k] = false
            NativeEngine.nativeSetPartMix(k, 0, 1f)  // Mix::Volume
        }
        table.applyMutes()
    }

    // ------------------------------------------------------------ adding songs

    private fun pickFolder() {
        @Suppress("DEPRECATION")
        a.startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQ_FOLDER)
    }

    private fun pickFiles() {
        val i = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
            putExtra(Intent.EXTRA_MIME_TYPES, arrayOf("audio/midi", "audio/x-midi", "audio/mid", "application/x-midi"))
            putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
        }
        @Suppress("DEPRECATION")
        a.startActivityForResult(i, REQ_FILES)
    }

    /** Copies the chosen MIDI files into the song folder (kept on this device only). */
    fun onActivityResult(requestCode: Int, data: Intent?) {
        if (data == null) return
        a.loader.execute {
            val added = mutableListOf<File>()
            try {
                when (requestCode) {
                    REQ_FILES -> {
                        val uris = mutableListOf<Uri>()
                        data.clipData?.let { c -> for (i in 0 until c.itemCount) uris += c.getItemAt(i).uri }
                        data.data?.let { if (uris.isEmpty()) uris += it }
                        for (u in uris) copyIn(u, displayName(u))?.let { added += it }
                    }
                    REQ_FOLDER -> {
                        val tree = data.data ?: return@execute
                        val children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree))
                        a.contentResolver.query(children, arrayOf(DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                            DocumentsContract.Document.COLUMN_DISPLAY_NAME), null, null, null)?.use { c ->
                            while (c.moveToNext()) {
                                val name = c.getString(1) ?: continue
                                if (!name.endsWith(".mid", true) && !name.endsWith(".midi", true)) continue
                                val u = DocumentsContract.buildDocumentUriUsingTree(tree, c.getString(0))
                                copyIn(u, name)?.let { added += it }
                            }
                        }
                    }
                }
            } catch (e: Exception) {
                a.runOnUiThread { a.log("adding songs failed: ${e.message}", Ui.ERR) }
            }
            a.runOnUiThread {
                a.log("added ${added.size} song(s)")
                refreshPlaylist()
                a.libraryPanel.refreshSongs()
                if (requestCode == REQ_FILES && added.isNotEmpty()) playFile(added.first())
            }
        }
    }

    private fun displayName(u: Uri): String {
        a.contentResolver.query(u, arrayOf(android.provider.OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
            if (c.moveToFirst()) return c.getString(0) ?: "song.mid"
        }
        return "song.mid"
    }

    private fun copyIn(u: Uri, name: String): File? {
        val dest = File(songDir, name.replace('/', '_'))
        a.contentResolver.openInputStream(u)?.use { input -> dest.outputStream().use { input.copyTo(it) } } ?: return null
        return dest
    }
}
