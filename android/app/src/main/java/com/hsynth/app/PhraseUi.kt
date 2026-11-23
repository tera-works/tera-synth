package com.hsynth.app

import android.annotation.SuppressLint
import android.content.Context
import android.content.Intent
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.net.Uri
import android.view.Gravity
import android.view.View
import android.widget.LinearLayout
import android.widget.TextView
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.math.abs
import kotlin.math.ceil
import kotlin.math.max

/**
 * U2 — Phrase recorder transport + ruler (Rec / Dub, Play, Stop, Undo, Clear, Export `.mid`, Bars, Quantize).
 *
 * One implementation, two placements: `PLAY > PHRASE` (compact, [wide] = false, 72 dp high so the top card keeps
 * its ≈119 dp and the keyboard loses nothing) and `EDIT > Arp / Seq > Phrase Recorder` ([wide] = true, adds the
 * quantize-strength field and a "save to Songs" button).
 *
 * State mirrors `Engine::PhraseState` (0 Empty, 1 Recording, 2 Playing, 3 Overdub, 4 Stopped); the first button reads
 * REC while Empty / Recording and DUB otherwise (red while overdubbing). Quantize and export touch files / snapshots
 * and therefore always run on `a.loader`.
 */
@SuppressLint("ViewConstructor")
class PhrasePanelView(private val a: MainActivity, private val wide: Boolean) : LinearLayout(a) {
    private val d = resources.displayMetrics.density
    private val ruler = PhraseRuler(a)

    private val recBtn = pbtn("REC", Hw.MOD_RED, led = true)
    private val playBtn = pbtn("PLAY", Hw.MOD_FX, led = true)
    private val stopBtn = pbtn("STOP", Hw.ENGRAVE, led = true)
    private val undoBtn = pbtn("UNDO", Hw.MOD_OSC, led = false)
    private val clearBtn = pbtn("CLEAR", Hw.MOD_OSC, led = false)
    private val midBtn = pbtn(".MID", Hw.MOD_OSC, led = false)
    private val songsBtn = pbtn("→ SONGS", Hw.MOD_OSC, led = false)
    private val barsLcd = Hw.lcd(a, "AUTO", 11f).apply {
        gravity = Gravity.CENTER
        setPadding(0, 0, 0, 0)
    }
    private val pills = Array(GRID_LABELS.size) { pbtn(GRID_LABELS[it], Hw.MOD_FX, led = false) }

    private var barIdx = 0
    private var quantIdx = a.prefs.getInt("phraseQuant", 0).coerceIn(0, GRID_LABELS.size - 1)
    private var strength = a.prefs.getInt("phraseQStrength", 100).coerceIn(0, 100)
    private var state = 0
    private var events = 0

    init {
        orientation = VERTICAL
        val gap = (4 * d).toInt()
        val rowH = (34 * d).toInt()

        // row 1: transport
        val row1 = LinearLayout(a).apply { orientation = HORIZONTAL }
        val tr = listOf(recBtn, playBtn, stopBtn, undoBtn, clearBtn, midBtn) + if (wide) listOf(songsBtn) else emptyList()
        tr.forEachIndexed { i, b -> row1.addView(b, LayoutParams(0, -1, 1f).apply { if (i < tr.size - 1) marginEnd = gap }) }
        addView(row1, LayoutParams(-1, rowH))

        // row 2: bars stepper, quantize pills, [strength], ruler
        val row2 = LinearLayout(a).apply { orientation = HORIZONTAL }
        val stepper = LinearLayout(a).apply {
            orientation = HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(pbtn("◂", Hw.ENGRAVE, led = false).also { it.setOnClickListener { stepBars(-1) } }, LayoutParams((22 * d).toInt(), -1))
            addView(barsLcd, LayoutParams(0, -1, 1f).apply { marginStart = (2 * d).toInt(); marginEnd = (2 * d).toInt() })
            addView(pbtn("▸", Hw.ENGRAVE, led = false).also { it.setOnClickListener { stepBars(1) } }, LayoutParams((22 * d).toInt(), -1))
        }
        row2.addView(stepper, LayoutParams(0, -1, if (wide) 0.9f else 1.3f).apply { marginEnd = gap })
        val pillBox = LinearLayout(a).apply { orientation = HORIZONTAL }
        pills.forEachIndexed { i, b ->
            b.setOnClickListener { selectQuant(i, apply = true) }
            pillBox.addView(b, LayoutParams(0, -1, 1f).apply { if (i < pills.size - 1) marginEnd = (2 * d).toInt() })
        }
        row2.addView(pillBox, LayoutParams(0, -1, if (wide) 2.2f else 2.6f).apply { marginEnd = gap })
        if (wide) {
            val strengthBox = LinearLayout(a).apply {
                orientation = HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                addView(Hw.engraved(a, "STRENGTH", 9f), LayoutParams(-2, -2).apply { marginEnd = (4 * d).toInt() })
                addView(DragNumber(a, 0f, 100f, 5f, 100f, { "${it.toInt()}%" }, { strength.toFloat() }) {
                    strength = it.toInt()
                    a.prefs.edit().putInt("phraseQStrength", strength).apply()
                }.apply { textSize = 11.5f; setPadding(0, 0, 0, 0) }, LayoutParams(0, -1, 1f))
            }
            row2.addView(strengthBox, LayoutParams(0, -1, 1.2f).apply { marginEnd = gap })
        }
        row2.addView(ruler, LayoutParams(0, -1, if (wide) 2.4f else 1.5f))
        addView(row2, LayoutParams(-1, rowH).apply { topMargin = gap })

        wire()
        selectQuant(quantIdx, apply = false)
        ruler.onStatus = { st, ev, bars ->
            state = st
            events = ev
            barIdx = BAR_OPTIONS.indexOf(bars).coerceAtLeast(0)
            updateButtons()
        }
        ruler.pollNow()
        updateButtons()
        // identical handler in both placements: the result does not depend on which panel started the export
        ActivityResults.register(ActivityResults.REQ_PHRASE_EXPORT) { data -> exportToUri(data?.data) }
    }

    private fun pbtn(text: String, accent: Int, led: Boolean) =
        LedButton(a, text, tr = false, led = led, accentColor = accent).apply {
            textSize = 10.5f
            minHeight = 0
            minimumHeight = 0
            setPadding(if (led) (15 * d).toInt() else (3 * d).toInt(), 0, (3 * d).toInt(), 0)
        }

    private fun wire() {
        recBtn.setOnClickListener { cmd(if (state == 1) CMD_PLAY else CMD_RECORD) }
        playBtn.setOnClickListener { cmd(CMD_PLAY) }
        stopBtn.setOnClickListener { cmd(CMD_STOP) }
        undoBtn.setOnClickListener { cmd(CMD_UNDO) }
        clearBtn.setOnClickListener { cmd(CMD_CLEAR) }
        midBtn.setOnClickListener { exportSaf() }
        midBtn.setOnLongClickListener { saveToSongs(); true }
        songsBtn.setOnClickListener { saveToSongs() }
    }

    private fun cmd(c: Int) {
        NativeEngine.nativePhraseCommand(c)
        ruler.pollNow()
    }

    private fun stepBars(dir: Int) {
        barIdx = (barIdx + dir).coerceIn(0, BAR_OPTIONS.size - 1)
        NativeEngine.nativePhraseSetBars(BAR_OPTIONS[barIdx])
        ruler.pollNow()
        updateButtons()
    }

    private fun selectQuant(i: Int, apply: Boolean) {
        quantIdx = i
        a.prefs.edit().putInt("phraseQuant", i).apply()
        pills.forEachIndexed { k, b -> if (b.lit != (k == i)) b.lit = k == i }
        if (apply && GRIDS[i] > 0f && events > 0) {
            val grid = GRIDS[i]
            val str = strength / 100f
            a.loader.execute {
                val ok = NativeEngine.nativePhraseQuantize(grid, str)
                a.ui.post {
                    a.log(if (ok) "phrase quantized to ${GRID_LABELS[i]}" else "phrase quantize failed", if (ok) Ui.TEXT else Ui.WARN)
                    ruler.pollNow()
                }
            }
        }
    }

    private fun updateButtons() {
        val rec = state == 1
        val dub = state == 2 || state == 3 || state == 4
        val label = if (dub) "DUB" else "REC"
        if (recBtn.text.toString() != label) recBtn.text = label
        val recLit = rec || state == 3
        if (recBtn.lit != recLit) recBtn.lit = recLit
        val playing = state == 2 || state == 3
        if (playBtn.lit != playing) playBtn.lit = playing
        val stopLit = state == 4
        if (stopBtn.lit != stopLit) stopBtn.lit = stopLit
        val has = state != 0
        undoBtn.alpha = if (has) 1f else 0.45f
        clearBtn.alpha = if (has) 1f else 0.45f
        midBtn.alpha = if (events > 0) 1f else 0.45f
        songsBtn.alpha = midBtn.alpha
        val bars = BAR_OPTIONS[barIdx]
        val t = if (bars == 0) "AUTO" else if (bars == 1) "1 BAR" else "$bars BARS"
        if (barsLcd.text.toString() != t) barsLcd.text = t
    }

    // ------------------------------------------------------------ export

    private fun exportSaf() {
        if (events <= 0) { a.log("phrase is empty", Ui.WARN); return }
        val i = Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "audio/midi"
            putExtra(Intent.EXTRA_TITLE, "phrase.mid")
        }
        @Suppress("DEPRECATION")
        a.startActivityForResult(i, ActivityResults.REQ_PHRASE_EXPORT)
    }

    private fun exportToUri(uri: Uri?) {
        uri ?: return
        a.loader.execute {
            val tmp = File(a.cacheDir, "phrase_export.mid")
            val err = NativeEngine.nativePhraseExport(tmp.absolutePath, 1)
            if (err.isNotEmpty()) { a.ui.post { a.log("phrase export failed: $err", Ui.ERR) }; return@execute }
            try {
                a.contentResolver.openOutputStream(uri, "w")?.use { o -> tmp.inputStream().use { it.copyTo(o) } }
                a.ui.post { a.log("phrase exported") }
            } catch (e: Exception) {
                a.ui.post { a.log("phrase export failed: ${e.message}", Ui.ERR) }
            } finally {
                tmp.delete()
            }
        }
    }

    /** Long-press `.MID` / `→ SONGS`: write straight into the MIDI folder (same folder the SONG playlist lists). */
    private fun saveToSongs() {
        if (events <= 0) { a.log("phrase is empty", Ui.WARN); return }
        a.loader.execute {
            val dir = a.libraryPanel.songDir
            val f = File(dir, "Phrase_" + SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date()) + ".mid")
            val err = NativeEngine.nativePhraseExport(f.absolutePath, 1)
            a.ui.post {
                if (err.isNotEmpty()) a.log("phrase export failed: $err", Ui.ERR)
                else {
                    a.log("phrase saved: ${f.name}")
                    if (a.isPlayerReady) a.player.refreshPlaylist()
                }
            }
        }
    }

    companion object {
        const val CMD_RECORD = 1
        const val CMD_PLAY = 2
        const val CMD_STOP = 3
        const val CMD_CLEAR = 4
        const val CMD_UNDO = 5
        val BAR_OPTIONS = intArrayOf(0, 1, 2, 4, 8, 16, 32, 64)
        val GRID_LABELS = arrayOf("OFF", "1/4", "1/8", "1/16", "1/32")
        val GRIDS = floatArrayOf(0f, 1f, 0.5f, 0.25f, 0.125f)
    }
}

/**
 * 34 dp phrase ruler on a pure-black well: beat ticks (bar lines brighter), progress fill and a playhead
 * (red while recording / overdubbing, emerald while playing), with a `bar.beat / bars` read-out.
 *
 * A [FrameTicker] client: 15 fps while Recording / Playing / Overdub, 4 fps otherwise, and nothing at all while the
 * view is hidden, covered by a sheet or the screen is off. Status is polled with one `nativePhraseStatus()` per tick;
 * [onStatus] fires only when state / event count / bars changed. onDraw() allocates nothing (text goes through a
 * preallocated CharArray).
 */
@SuppressLint("ViewConstructor")
class PhraseRuler(ctx: Context) : View(ctx), FrameTicker.Client {
    /** (state, eventCount, bars) — only on change. */
    var onStatus: ((Int, Int, Int) -> Unit)? = null

    private val d = resources.displayMetrics.density
    private var state = 0
    private var lenBeats = 0f
    private var posBeats = 0f
    private var events = 0
    private var bars = -1
    private var lastX = -1f
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)
    private val t = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Hw.MONO; textSize = 9.5f * d }
    private val r = RectF()
    private val chars = CharArray(16)

    override val intervalMs: Int get() = if (state == 1 || state == 2 || state == 3) 66 else 250
    override val tickView: View? get() = this

    override fun onFrame(nowMs: Long) = pollNow()

    /** Reads the engine status once and redraws if something visible changed. */
    fun pollNow() {
        val s = NativeEngine.nativePhraseStatus()
        if (s.size < 5) return
        val ns = s[0].toInt()
        val ne = s[3].toInt()
        val nb = s[4].toInt()
        val meta = ns != state || ne != events || nb != bars
        state = ns
        lenBeats = s[1]
        posBeats = s[2]
        events = ne
        bars = nb
        if (meta) onStatus?.invoke(ns, ne, nb)
        val x = scale().let { if (it > 0f) posBeats / it * width else 0f }
        if (meta || abs(x - lastX) >= 1f) { lastX = x; invalidate() }
    }

    /** Beats across the full width: the loop length, or (first pass of an AUTO-length take) the next 4-beat multiple. */
    private fun scale(): Float = when {
        lenBeats > 0f -> lenBeats
        state == 1 -> max(16f, ceil((posBeats + 0.01f) / 4f) * 4f)
        else -> 0f
    }

    override fun onAttachedToWindow() { super.onAttachedToWindow(); FrameTicker.register(this) }
    override fun onDetachedFromWindow() { FrameTicker.unregister(this); super.onDetachedFromWindow() }
    override fun onVisibilityAggregated(isVisible: Boolean) { super.onVisibilityAggregated(isVisible); FrameTicker.poke() }
    override fun onWindowFocusChanged(hasWindowFocus: Boolean) { super.onWindowFocusChanged(hasWindowFocus); FrameTicker.poke() }

    private fun writeInt(i0: Int, v: Int): Int {
        var i = i0
        val vv = v.coerceIn(0, 999)
        if (vv >= 100) chars[i++] = '0' + vv / 100
        if (vv >= 10) chars[i++] = '0' + (vv / 10) % 10
        chars[i++] = '0' + vv % 10
        return i
    }

    override fun onDraw(c: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        if (w <= 0f || h <= 0f) return
        val rad = 5f * d
        r.set(0f, 0f, w, h)
        p.style = Paint.Style.FILL
        p.color = Hw.WELL
        c.drawRoundRect(r, rad, rad, p)
        p.style = Paint.Style.STROKE
        p.strokeWidth = d
        p.color = Hw.PANEL_EDGE
        r.inset(d / 2f, d / 2f)
        c.drawRoundRect(r, rad, rad, p)
        p.style = Paint.Style.FILL

        val sc = scale()
        val active = state == 1 || state == 2 || state == 3
        if (sc > 0f) {
            val tickStep = if (sc <= 32f) 1 else 4
            var b = 0
            while (b <= sc) {
                val x = b / sc * w
                val bar = b % 4 == 0
                p.color = if (bar) Hw.PANEL_HI else Hw.LED_OFF
                val top = if (bar) 4f * d else 11f * d
                c.drawRect(x, top, x + d, h - 4f * d, p)
                b += tickStep
            }
            val hx = (posBeats / sc * w).coerceIn(0f, w)
            val col = if (state == 1 || state == 3) Hw.MOD_RED else Hw.MOD_FX
            if (active) {
                p.color = Hw.withAlpha(col, 40)
                c.drawRect(0f, 3f * d, hx, h - 3f * d, p)
            }
            p.color = col
            c.drawRect(hx - d, 2f * d, hx + d, h - 2f * d, p)
        }
        t.color = if (active) Hw.SILK else Hw.ENGRAVE
        val base = h / 2f + 3.4f * d
        when (state) {
            0 -> c.drawText("EMPTY", 6f * d, base, t)
            4 -> c.drawText("STOPPED", 6f * d, base, t)
            else -> {
                var n = writeInt(0, (posBeats / 4f).toInt() + 1)
                chars[n++] = '.'
                n = writeInt(n, (posBeats % 4f).toInt() + 1)
                chars[n++] = ' '
                chars[n++] = '/'
                chars[n++] = ' '
                if (lenBeats > 0f) n = writeInt(n, (lenBeats / 4f + 0.001f).toInt()) else chars[n++] = '?'
                c.drawText(chars, 0, n, 6f * d, base, t)
            }
        }
    }
}
