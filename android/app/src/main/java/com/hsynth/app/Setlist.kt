package com.hsynth.app

import android.annotation.SuppressLint
import android.content.Context
import android.content.Intent
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.net.Uri
import android.text.InputType
import android.text.TextUtils
import android.util.Log
import android.util.TypedValue
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.FileOutputStream
import java.nio.ByteBuffer
import java.nio.charset.CharacterCodingException
import java.nio.charset.Charset
import java.nio.charset.CodingErrorAction
import java.util.Locale
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference
import kotlin.math.abs
import kotlin.math.roundToInt

/*
 * S4 — Multi-bank set list (Phase 2 Wave 1B).
 *
 * Standalone: nothing here touches the engine. It is wired into LivePanel / MainActivity in Wave 2:
 *   - LivePanel owns one [SetlistStore], one [BankStrip], one [SlotEditSheet] and one [MemoSheet];
 *   - the recall pipeline (SST sweep, GlobalParams perf.transpose / perf.trim, fx.tempo, chain stepping) stays in
 *     LivePanel — these classes only hold data and draw / edit it;
 *   - MainActivity must route `ActivityResults.REQ_MEMO_TXT` (already dispatched through ActivityResults).
 */

// ============================================================================================== data

/**
 * One set-list slot. [kind] is `"preset"`, `"set"` or `"scene"`; [ref] is an absolute file path (preset / set) or the
 * scene index (0-based, as a string). [title] overrides the natural name when not blank. [bpm] 0 = keep the current
 * tempo; [trn] = performance transpose in semitones; [trimDb] = post-master trim.
 */
data class SetlistSlot(
    var kind: String = "",
    var ref: String = "",
    var title: String = "",
    var bpm: Int = 0,
    var trn: Int = 0,
    var trimDb: Float = 0f,
    var memo: String = "",
    var memoShow: Boolean = false,
    var color: Int = -1,
) {
    val isEmpty: Boolean get() = ref.isEmpty()

    companion object {
        const val PRESET = "preset"
        const val SET = "set"
        const val SCENE = "scene"
    }
}

// ============================================================================================== store

/**
 * 8 banks (`A`..`H`) × 8 slots = 64 slots, index `i = bank * 8 + n` (A1 = 0 … H8 = 63).
 *
 * Persistence: `files/setlist.json`, written asynchronously on the process-wide loader (tmp file + rename; writes are
 * coalesced, the newest snapshot always wins). The file is read synchronously on construction (a few KB). When it does
 * not exist yet the legacy `prefs["liveSlots"]` (8 paths) is migrated into bank A and the file is created; the prefs
 * key itself is left untouched. A corrupt file is moved aside to `setlist.json.bad`.
 *
 * UI-thread only (like the rest of the app's state). [rev] increases on every data change so views can cheaply detect it.
 */
class SetlistStore(private val a: MainActivity) {
    companion object {
        const val BANKS = 8
        const val PER_BANK = 8
        const val SLOTS = BANKS * PER_BANK
        const val FILE_NAME = "setlist.json"
        private const val LEGACY_PREF = "liveSlots"
        private const val SAVE_DELAY_MS = 400L

        private val CODES = Array(SLOTS) { "${'A' + it / PER_BANK}${it % PER_BANK + 1}" }
        private val LETTERS = Array(BANKS) { ('A' + it).toString() }

        /** `"A1"`..`"H8"` (preformatted, allocation-free); `"--"` for an invalid index. */
        fun codeOf(i: Int): String = if (i in 0 until SLOTS) CODES[i] else "--"

        /** `"A"`..`"H"`; `"-"` for an invalid bank. */
        fun letterOf(b: Int): String = if (b in 0 until BANKS) LETTERS[b] else "-"
    }

    val slots = Array(SLOTS) { SetlistSlot() }

    /** Bumped on every data / structure change. */
    var rev = 0
        private set

    private var viewed = 0
    private var active = -1
    private var chain = false
    private var stopAtEnd = false

    /** Bank shown by the slot pads (0..7). Changing it never changes the sound. */
    var viewedBank: Int
        get() = viewed
        set(v) {
            val n = v.coerceIn(0, BANKS - 1)
            if (n != viewed) { viewed = n; saveSoon() }
        }

    /** Slot that was recalled last (-1 = none, else 0..63). */
    var activeSlot: Int
        get() = active
        set(v) {
            val n = v.coerceIn(-1, SLOTS - 1)
            if (n != active) { active = n; saveSoon() }
        }

    /** CHAIN pill: PREV / NEXT / footswitch step through [chainOrder]. */
    var chainMode: Boolean
        get() = chain
        set(v) { if (v != chain) { chain = v; saveSoon() } }

    /** Chain does not wrap from the last to the first slot (and vice versa) when true. */
    var chainStopAtEnd: Boolean
        get() = stopAtEnd
        set(v) { if (v != stopAtEnd) { stopAtEnd = v; saveSoon() } }

    private val file = File(a.filesDir, FILE_NAME)
    private val tmp = File(a.filesDir, "$FILE_NAME.tmp")
    private var order: IntArray? = null
    private var savePending = false
    private val saveRunnable = Runnable { save() }
    private val writing = AtomicBoolean(false)
    private val latest = AtomicReference<String?>(null)

    init { load() }

    // ------------------------------------------------------------------ queries

    fun slot(i: Int): SetlistSlot = slots[i]

    fun slotCode(i: Int): String = codeOf(i)

    fun bankLetter(b: Int): String = letterOf(b)

    fun bankHasContent(b: Int): Boolean {
        if (b !in 0 until BANKS) return false
        val base = b * PER_BANK
        for (n in 0 until PER_BANK) if (!slots[base + n].isEmpty) return true
        return false
    }

    /** Name for lists / cue lines: the slot title, else the preset / set file name, else `Scene N`. */
    fun displayTitle(i: Int): String {
        if (i !in 0 until SLOTS) return ""
        val s = slots[i]
        if (s.title.isNotBlank()) return s.title
        if (s.ref.isEmpty()) return ""
        return if (s.kind == SetlistSlot.SCENE) L10n.t("Scene") + " " + ((s.ref.toIntOrNull() ?: 0) + 1)
        else File(s.ref).nameWithoutExtension
    }

    /** Non-empty slot indices in set-list order (A1 → H8). Cached and rebuilt on change — do not modify the result. */
    fun chainOrder(): IntArray {
        order?.let { return it }
        var n = 0
        for (s in slots) if (!s.isEmpty) n++
        val o = IntArray(n)
        var k = 0
        for (i in 0 until SLOTS) if (!slots[i].isEmpty) o[k++] = i
        order = o
        return o
    }

    /**
     * The slot [delta] steps away from [from] in [chainOrder]. [from] need not be filled (or valid: -1 = "before the
     * first"). Wraps around unless [chainStopAtEnd]. @return the slot index, or -1 when the chain is empty / the end
     * was reached with stop-at-end.
     */
    fun nextInChain(from: Int, delta: Int): Int {
        val o = chainOrder()
        val n = o.size
        if (n == 0) return -1
        var pos = -1
        for (k in 0 until n) if (o[k] == from) { pos = k; break }
        if (delta == 0) return if (pos >= 0) from else -1
        val np: Int = if (pos >= 0) {
            pos + delta
        } else if (delta > 0) {
            var p = n
            for (k in 0 until n) if (o[k] > from) { p = k; break }
            p + delta - 1
        } else {
            var q = -1
            for (k in n - 1 downTo 0) if (o[k] < from) { q = k; break }
            q + delta + 1
        }
        if (np in 0 until n) return o[np]
        if (stopAtEnd) return -1
        return o[((np % n) + n) % n]
    }

    // ------------------------------------------------------------------ mutations (each one saves)

    /** Apply [block] to slot [i], then invalidate caches and save. */
    fun update(i: Int, block: SetlistSlot.() -> Unit) {
        if (i !in 0 until SLOTS) return
        slots[i].block()
        changed()
    }

    /** Point slot [i] at the current sound; performance offsets and memo of the slot are kept. [title] replaces the slot title ("" = natural name). */
    fun storeCurrent(i: Int, kind: String, ref: String, title: String) {
        update(i) { this.kind = kind; this.ref = ref; this.title = title }
    }

    fun clearSlot(i: Int) {
        if (i !in 0 until SLOTS) return
        slots[i] = SetlistSlot()
        if (active == i) active = -1
        changed()
    }

    /** Swap slot [from] with slot [to] (an empty target simply receives the slot). @return false when nothing moved. */
    fun moveSlot(from: Int, to: Int): Boolean {
        if (from !in 0 until SLOTS || to !in 0 until SLOTS || from == to) return false
        val t = slots[from]
        slots[from] = slots[to]
        slots[to] = t
        if (active == from) active = to else if (active == to) active = from
        changed()
        return true
    }

    private fun changed() {
        order = null
        rev++
        save()
    }

    // ------------------------------------------------------------------ persistence

    /** Snapshot now, write on the loader thread (coalesced). */
    fun save() {
        a.ui.removeCallbacks(saveRunnable)
        savePending = false
        val json = try { toJson() } catch (e: Exception) { Log.w("HSynth", "setlist snapshot failed: ${e.message}"); return }
        latest.set(json)
        kick()
    }

    /** Debounced save for view state (bank / active / chain flags). */
    private fun saveSoon() {
        savePending = true
        a.ui.removeCallbacks(saveRunnable)
        a.ui.postDelayed(saveRunnable, SAVE_DELAY_MS)
    }

    /** Write a pending debounced change right now (call from onPause). */
    fun flush() { if (savePending) save() }

    private fun kick() {
        if (!writing.compareAndSet(false, true)) return
        a.loader.execute {
            while (true) {
                val s = latest.getAndSet(null) ?: break
                writeAtomic(s)
            }
            writing.set(false)
            if (latest.get() != null) kick()
        }
    }

    private fun writeAtomic(s: String) {
        try {
            FileOutputStream(tmp).use {
                it.write(s.toByteArray(Charsets.UTF_8))
                it.fd.sync()
            }
            if (!tmp.renameTo(file)) {
                file.delete()
                tmp.renameTo(file)
            }
        } catch (e: Exception) {
            Log.w("HSynth", "setlist write failed: ${e.message}")
        }
    }

    private fun toJson(): String {
        val arr = JSONArray()
        for (i in 0 until SLOTS) {
            val s = slots[i]
            arr.put(JSONObject()
                .put("i", i)
                .put("kind", s.kind)
                .put("ref", s.ref)
                .put("title", s.title)
                .put("bpm", s.bpm)
                .put("trn", s.trn)
                .put("trim", s.trimDb.toDouble())
                .put("memo", s.memo)
                .put("memoShow", s.memoShow)
                .put("color", s.color))
        }
        return JSONObject()
            .put("v", 1)
            .put("bank", viewed)
            .put("active", active)
            .put("chain", chain)
            .put("stopAtEnd", stopAtEnd)
            .put("slots", arr)
            .toString()
    }

    private fun load() {
        if (file.exists()) {
            try {
                parse(file.readText(Charsets.UTF_8))
                return
            } catch (e: Exception) {
                Log.w("HSynth", "setlist.json unreadable (${e.message}) — moved to .bad")
                try { file.renameTo(File(a.filesDir, "$FILE_NAME.bad")) } catch (_: Exception) {}
            }
        }
        migrateLegacy()
    }

    private fun parse(text: String) {
        val o = JSONObject(text)
        val arr = o.optJSONArray("slots") ?: JSONArray()
        val fresh = Array(SLOTS) { SetlistSlot() }
        for (k in 0 until arr.length()) {
            val e = arr.optJSONObject(k) ?: continue
            val i = e.optInt("i", k)
            if (i !in 0 until SLOTS) continue
            fresh[i] = SetlistSlot(
                kind = e.optString("kind", ""),
                ref = e.optString("ref", ""),
                title = e.optString("title", ""),
                bpm = e.optInt("bpm", 0).let { if (it in 40..240) it else 0 },
                trn = e.optInt("trn", 0).coerceIn(-12, 12),
                trimDb = e.optDouble("trim", 0.0).toFloat().coerceIn(-12f, 6f),
                memo = e.optString("memo", ""),
                memoShow = e.optBoolean("memoShow", false),
                color = e.optInt("color", -1),
            )
        }
        for (i in 0 until SLOTS) slots[i] = fresh[i]
        viewed = o.optInt("bank", 0).coerceIn(0, BANKS - 1)
        active = o.optInt("active", -1).coerceIn(-1, SLOTS - 1)
        chain = o.optBoolean("chain", false)
        stopAtEnd = o.optBoolean("stopAtEnd", false)
        order = null
    }

    /** First run: the 8 legacy LivePanel paths become bank A. The prefs key is left as it was. */
    private fun migrateLegacy() {
        try {
            val arr = JSONArray(a.prefs.getString(LEGACY_PREF, "[]"))
            val liveDir = File(a.filesDir, "setups/live")
            for (i in 0 until minOf(PER_BANK, arr.length())) {
                val p = arr.optString(i, "")
                if (p.isEmpty()) continue
                val s = slots[i]
                s.ref = p
                s.kind = if (File(p).parentFile == liveDir) SetlistSlot.SET else SetlistSlot.PRESET
            }
        } catch (e: Exception) {
            Log.w("HSynth", "setlist migration skipped: ${e.message}")
        }
        order = null
        save()   // creates setlist.json, so the migration runs once
    }
}

// ============================================================================================== BankStrip

/**
 * 26 dp strip: bank tiles `A..H`, a `CHAIN` pill and a position readout (`"A3 · 19/64"`), all in one view.
 *
 * Tile states: viewed = cyan fill + underline, contains the playing slot = emerald dot, has content = dim dot.
 * Rects are computed in [onSizeChanged]; [onDraw] allocates nothing and formats nothing — the readout string is
 * rebuilt in [sync] only when the active slot / viewed bank changed.
 *
 * The strip is a pure view: tapping only fires [onSelectBank] / [onToggleChain]; the owner updates the
 * [SetlistStore] (`viewedBank`, `chainMode`) and calls [sync].
 */
@SuppressLint("ClickableViewAccessibility")
class BankStrip(ctx: Context) : View(ctx) {
    var onSelectBank: ((Int) -> Unit)? = null
    var onToggleChain: (() -> Unit)? = null

    private val d = resources.displayMetrics.density
    private var store: SetlistStore? = null

    private var viewed = 0
    private var playing = -1
    private var chain = false
    private val has = BooleanArray(SetlistStore.BANKS)
    private var posText = "--"
    private var lastActive = Int.MIN_VALUE
    private var lastViewed = -1
    private var pressIdx = -1

    private val letters = Array(SetlistStore.BANKS) { SetlistStore.letterOf(it) }
    private val chainLabel = "CHAIN"
    private val tiles = Array(SetlistStore.BANKS) { RectF() }
    private val pill = RectF()
    private var letterBase = 0f
    private var chainBase = 0f
    private var posBase = 0f

    private val fill = Paint(Paint.ANTI_ALIAS_FLAG)
    private val line = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = d }
    private val letterP = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Hw.BOLD; textAlign = Paint.Align.CENTER; textSize = 13f * d }
    private val chainP = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Hw.BOLD; textAlign = Paint.Align.CENTER; textSize = 10.5f * d; letterSpacing = 0.08f }
    private val posP = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Hw.MONO; textAlign = Paint.Align.RIGHT; textSize = 11f * d; color = Hw.ENGRAVE }

    private val cViewedFill = Hw.withAlpha(Hw.MOD_OSC, 92)       // cyan 36 %
    private val cViewedLine = Hw.withAlpha(Hw.MOD_OSC, 190)
    private val cChainFill = Hw.withAlpha(Hw.MOD_FX, 52)
    private val cDotDim = Hw.withAlpha(Hw.ENGRAVE, 150)

    init {
        isClickable = true
        contentDescription = "Set list banks"
    }

    /** Attach the store the strip mirrors; call [sync] after every change. */
    fun bind(s: SetlistStore) {
        store = s
        sync()
    }

    /** Re-read viewed bank / playing bank / chain / content flags from the store. Redraws only on change. */
    fun sync() {
        val s = store ?: return
        var ch = false
        val v = s.viewedBank
        if (v != viewed) { viewed = v; ch = true }
        val act = s.activeSlot
        val pb = if (act >= 0) act / SetlistStore.PER_BANK else -1
        if (pb != playing) { playing = pb; ch = true }
        if (s.chainMode != chain) { chain = s.chainMode; ch = true }
        for (b in 0 until SetlistStore.BANKS) {
            val h = s.bankHasContent(b)
            if (h != has[b]) { has[b] = h; ch = true }
        }
        if (act != lastActive || v != lastViewed) {
            lastActive = act
            lastViewed = v
            posText = if (act >= 0) "${SetlistStore.codeOf(act)} · ${act + 1}/${SetlistStore.SLOTS}"
            else "${SetlistStore.letterOf(v)} · --/${SetlistStore.SLOTS}"
            ch = true
        }
        if (ch) invalidate()
    }

    override fun onMeasure(widthSpec: Int, heightSpec: Int) {
        setMeasuredDimension(resolveSize((463f * d).toInt(), widthSpec), (26f * d).toInt())
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        val gap = 3f * d
        val chainW = 64f * d
        val chainH = 22f * d
        val readW = 76f * d
        val g2 = 10f * d
        val half = d / 2f
        val hf = h.toFloat()
        val tw = ((w - chainW - readW - 2f * g2 - 7f * gap) / 8f).coerceIn(20f * d, 34f * d)
        for (i in 0 until SetlistStore.BANKS) {
            val l = i * (tw + gap)
            tiles[i].set(l + half, half, l + tw - half, hf - half)
        }
        val tilesEnd = 8f * tw + 7f * gap
        val py = (hf - chainH) / 2f
        pill.set(tilesEnd + g2 + half, py + half, tilesEnd + g2 + chainW - half, py + chainH - half)
        letterBase = hf / 2f - (letterP.ascent() + letterP.descent()) / 2f
        chainBase = hf / 2f - (chainP.ascent() + chainP.descent()) / 2f
        posBase = hf / 2f - (posP.ascent() + posP.descent()) / 2f
    }

    override fun onDraw(c: Canvas) {
        val r = 5f * d
        for (i in 0 until SetlistStore.BANKS) {
            val t = tiles[i]
            val isViewed = i == viewed
            if (isViewed) {
                fill.color = cViewedFill
                c.drawRoundRect(t, r, r, fill)
            } else if (pressIdx == i) {
                fill.color = Hw.PANEL_RAISED
                c.drawRoundRect(t, r, r, fill)
            }
            line.color = if (isViewed) cViewedLine else Hw.PANEL_EDGE
            c.drawRoundRect(t, r, r, line)

            letterP.color = if (isViewed) Hw.SILK else if (has[i]) Hw.ENGRAVE else Hw.INK_DIM
            c.drawText(letters[i], t.centerX(), letterBase, letterP)

            if (isViewed) {
                fill.color = Hw.MOD_OSC
                c.drawRect(t.left + 5f * d, t.bottom - 3f * d, t.right - 5f * d, t.bottom - d, fill)
            }
            if (i == playing) {
                fill.color = Hw.MOD_FX
                c.drawCircle(t.right - 6f * d, t.top + 6f * d, 2.5f * d, fill)
            } else if (has[i]) {
                fill.color = cDotDim
                c.drawCircle(t.right - 6f * d, t.top + 6f * d, 2f * d, fill)
            }
        }

        val pr = pill.height() / 2f
        if (chain) {
            fill.color = cChainFill
            c.drawRoundRect(pill, pr, pr, fill)
        } else if (pressIdx == SetlistStore.BANKS) {
            fill.color = Hw.PANEL_RAISED
            c.drawRoundRect(pill, pr, pr, fill)
        }
        line.color = if (chain) Hw.MOD_FX else Hw.PANEL_EDGE
        c.drawRoundRect(pill, pr, pr, line)
        chainP.color = if (chain) Hw.SILK else Hw.ENGRAVE
        c.drawText(chainLabel, pill.centerX(), chainBase, chainP)

        c.drawText(posText, width - 2f * d, posBase, posP)
    }

    /** 0..7 = bank tile, 8 = CHAIN pill, -1 = none. Full strip height, x only (the strip is just 26 dp tall). */
    private fun hit(x: Float): Int {
        val slop = 1.5f * d
        for (i in 0 until SetlistStore.BANKS) {
            val t = tiles[i]
            if (x >= t.left - slop && x <= t.right + slop) return i
        }
        if (x >= pill.left - 4f * d && x <= pill.right + 4f * d) return SetlistStore.BANKS
        return -1
    }

    override fun onTouchEvent(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                val h = hit(e.x)
                if (h < 0) return false
                pressIdx = h
                invalidate()
            }
            MotionEvent.ACTION_MOVE -> if (pressIdx >= 0 && hit(e.x) != pressIdx) {
                pressIdx = -1
                invalidate()
            }
            MotionEvent.ACTION_UP -> {
                val p = pressIdx
                pressIdx = -1
                invalidate()
                if (p >= 0 && hit(e.x) == p) {
                    performClick()
                    if (p < SetlistStore.BANKS) onSelectBank?.invoke(p) else onToggleChain?.invoke()
                }
            }
            MotionEvent.ACTION_CANCEL -> {
                pressIdx = -1
                invalidate()
            }
        }
        return true
    }

    override fun performClick(): Boolean {
        super.performClick()
        return true
    }
}

// ============================================================================================== MemoSheet

/** ScrollView that reports a horizontal swipe (−1 = right swipe / previous, +1 = left swipe / next) without eating vertical scrolling. */
@SuppressLint("ViewConstructor")
private class SwipeScrollView(ctx: Context, private val body: TextView, private val onSwipe: (Int) -> Unit) : ScrollView(ctx) {
    private val d = resources.displayMetrics.density
    private var x0 = 0f
    private var y0 = 0f
    private var t0 = 0L
    private var armed = false

    override fun dispatchTouchEvent(e: MotionEvent): Boolean {
        var swipe = 0
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> { x0 = e.x; y0 = e.y; t0 = e.eventTime; armed = true }
            MotionEvent.ACTION_POINTER_DOWN, MotionEvent.ACTION_CANCEL -> armed = false
            MotionEvent.ACTION_UP -> {
                if (armed && !body.hasSelection()) {
                    val dx = e.x - x0
                    val dy = e.y - y0
                    if (abs(dx) > 72f * d && abs(dx) > 2.2f * abs(dy) && e.eventTime - t0 < 700L) swipe = if (dx < 0f) 1 else -1
                }
                armed = false
            }
        }
        val r = super.dispatchTouchEvent(e)
        if (swipe != 0) onSwipe(swipe)
        return r
    }
}

/**
 * Pure-black stage reading sheet for a slot's memo (lyrics, chords, notes).
 *
 * Header: `"A3 · Title · ♩120 · -2st"`, `A-` / `A+` (14–40 sp, persisted), `MONO` (chord-chart font, persisted),
 * `◀ PREV` / `NEXT ▶`, `✕`. Body: scrollable + selectable text; swipe left / right = next / previous song.
 * Bottom cue: `NEXT ▸ A4 · Title` (next slot in the chain order).
 *
 * [onStep] (±1) is called for PREV / NEXT / swipe; the owner steps its chain relative to the active slot and recalls it.
 * The sheet then follows `store.activeSlot` automatically; the owner may also call [showSlot] / [refresh].
 * Never opens by itself — the owner decides (`memoShow` / MEMO chip) and calls [open].
 */
class MemoSheet(
    private val a: MainActivity,
    private val store: SetlistStore,
    private val onStep: (Int) -> Unit,
) : Sheet(Edge.CENTER, 846, 370) {
    companion object {
        const val PREF_FONT = "memoFontSp"
        const val PREF_MONO = "memoMono"
        const val FONT_MIN = 14
        const val FONT_MAX = 40
    }

    override val surface: Int get() = Hw.WELL
    override val view: View by lazy { build() }

    private val d = a.resources.displayMetrics.density
    private fun dp(v: Int) = (v * d).toInt()

    private var built = false
    private var cur = -1
    private var shown = -2
    private var fontSp = 22
    private var mono = false

    private lateinit var titleTv: TextView
    private lateinit var bodyTv: TextView
    private lateinit var scroll: ScrollView
    private lateinit var cueTv: TextView
    private lateinit var monoBtn: Button

    /** Which slot the sheet shows (does not open it). */
    fun showSlot(i: Int) {
        cur = i
        if (built) refresh()
    }

    /** Show slot [i] and open the sheet. */
    fun open(i: Int) {
        showSlot(i)
        a.sheets.open(this)
    }

    override fun onShow() {
        view
        refresh()
    }

    /** Re-read the current slot (title / memo edited elsewhere). */
    fun refresh() {
        if (!built) return
        val i = cur
        if (i !in 0 until SetlistStore.SLOTS) {
            titleTv.text = "—"
            bodyTv.text = ""
            cueTv.text = ""
            shown = -1
            return
        }
        val s = store.slot(i)
        val sb = StringBuilder(48)
        sb.append(SetlistStore.codeOf(i))
        val name = store.displayTitle(i)
        if (name.isNotEmpty()) sb.append(" · ").append(name)
        if (s.bpm > 0) sb.append(" · ♩").append(s.bpm)
        if (s.trn != 0) sb.append(" · ").append(if (s.trn > 0) "+" else "").append(s.trn).append("st")
        titleTv.text = sb.toString()

        val blank = s.memo.isBlank()
        bodyTv.setTextColor(if (blank) Hw.INK_DIM else Hw.SILK)
        bodyTv.text = if (blank) L10n.t("no memo for this slot") else s.memo
        if (shown != i) scroll.scrollTo(0, 0)
        shown = i

        val nx = store.nextInChain(i, 1)
        cueTv.text = if (nx >= 0 && nx != i) "NEXT ▸ " + SetlistStore.codeOf(nx) + store.displayTitle(nx).let { if (it.isEmpty()) "" else " · $it" }
        else L10n.t("END OF SET")
    }

    private fun step(delta: Int) {
        onStep(delta)
        val act = store.activeSlot
        if (act >= 0 && act != cur) showSlot(act)
    }

    private fun setFont(v: Int) {
        fontSp = v.coerceIn(FONT_MIN, FONT_MAX)
        bodyTv.setTextSize(TypedValue.COMPLEX_UNIT_SP, fontSp.toFloat())
        a.prefs.edit().putInt(PREF_FONT, fontSp).apply()
    }

    private fun applyMono() {
        bodyTv.typeface = if (mono) Hw.MONO else Hw.LABEL
        monoBtn.background = Hw.keyBg(a, accent = if (mono) Hw.MOD_OSC else 0)
    }

    private fun hdrBtn(label: String, onClick: () -> Unit): Button =
        Ui.button(a, label) { onClick() }.apply {
            setPadding(dp(4), 0, dp(4), 0)
            isSingleLine = true
        }

    private fun build(): View {
        fontSp = a.prefs.getInt(PREF_FONT, 22).coerceIn(FONT_MIN, FONT_MAX)
        mono = a.prefs.getBoolean(PREF_MONO, false)

        val root = LinearLayout(a).apply { orientation = LinearLayout.VERTICAL }

        // ---- header (36 dp)
        val header = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(12), 0, dp(8), 0)
        }
        titleTv = TextView(a).apply {
            textSize = 16f
            setTextColor(Hw.SILK)
            typeface = Hw.BOLD
            maxLines = 1
            ellipsize = TextUtils.TruncateAt.END
        }
        header.addView(titleTv, LinearLayout.LayoutParams(0, -2, 1f))
        fun add(b: View, w: Int) = header.addView(b, LinearLayout.LayoutParams(dp(w), dp(36)).apply { marginStart = dp(5) })
        add(hdrBtn("A-") { setFont(fontSp - 2) }, 40)
        add(hdrBtn("A+") { setFont(fontSp + 2) }, 40)
        monoBtn = hdrBtn("MONO") {
            mono = !mono
            a.prefs.edit().putBoolean(PREF_MONO, mono).apply()
            applyMono()
        }
        add(monoBtn, 60)
        add(hdrBtn("◀ PREV") { step(-1) }, 90)
        add(hdrBtn("NEXT ▶") { step(1) }, 90)
        add(hdrBtn("✕") { dismiss() }, 40)
        root.addView(header, LinearLayout.LayoutParams(-1, dp(36)))

        // ---- body
        bodyTv = TextView(a).apply {
            setTextColor(Hw.SILK)
            setLineSpacing(0f, 1.15f)
            setPadding(dp(16), dp(8), dp(16), dp(12))
            setTextIsSelectable(true)
        }
        bodyTv.setTextSize(TypedValue.COMPLEX_UNIT_SP, fontSp.toFloat())
        scroll = SwipeScrollView(a, bodyTv) { step(it) }.apply {
            overScrollMode = View.OVER_SCROLL_NEVER
            isVerticalScrollBarEnabled = true
            addView(bodyTv, android.view.ViewGroup.LayoutParams(-1, -2))
        }
        root.addView(scroll, LinearLayout.LayoutParams(-1, 0, 1f))

        // ---- cue bar (26 dp)
        cueTv = TextView(a).apply {
            textSize = 12.5f
            setTextColor(Hw.ENGRAVE)
            typeface = Hw.MONO
            maxLines = 1
            ellipsize = TextUtils.TruncateAt.END
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(16), 0, dp(16), 0)
        }
        root.addView(cueTv, LinearLayout.LayoutParams(-1, dp(26)))

        applyMono()
        built = true
        return root
    }
}

// ============================================================================================== SlotEditSheet

/**
 * Edit one slot: title, BPM (0 = keep, 40..240), transpose (−12..+12), vol trim (−12..+6 dB), memo (typed or imported
 * from a .txt), "show on recall", plus STORE CURRENT / CLEAR / MOVE ◀ ▶ / SAVE.
 *
 * Open with [edit]. Edits are applied by SAVE (✕ discards them); MOVE and STORE CURRENT first commit the typed fields
 * so nothing is lost. [onStoreCurrent] is called with the slot index and must call [SetlistStore.storeCurrent]
 * (the sheet then reloads its fields); [onChanged] fires after every change to the store (re-bind pads / strip).
 * CLEAR needs a second tap within 2.5 s. `.txt` import goes through `ActivityResults.REQ_MEMO_TXT`
 * (UTF-8, with a Shift-JIS fallback; up to 256 KB / 20 000 characters).
 */
class SlotEditSheet(
    private val a: MainActivity,
    private val store: SetlistStore,
    private val onStoreCurrent: (Int) -> Unit,
    private val onChanged: () -> Unit,
) : Sheet(Edge.CENTER, 560, 300) {
    companion object {
        private const val MAX_IMPORT_BYTES = 256 * 1024
        private const val MAX_MEMO_CHARS = 20_000
        private const val CLEAR_CONFIRM_MS = 2500L
    }

    override val view: View by lazy { build() }

    private val d = a.resources.displayMetrics.density
    private fun dp(v: Int) = (v * d).toInt()

    private var cur = -1
    private var eBpm = 0           // 0 = keep, else 40..240
    private var eTrn = 0
    private var eTrim = 0f
    private var eShow = false
    private var confirmClear = false

    private lateinit var root: LinearLayout
    private lateinit var hdr: TextView
    private lateinit var eTitle: EditText
    private lateinit var eMemo: EditText
    private lateinit var nBpm: DragNumber
    private lateinit var nTrn: DragNumber
    private lateinit var nTrim: DragNumber
    private lateinit var showBtn: Button
    private lateinit var clearBtn: Button
    private lateinit var moveL: Button
    private lateinit var moveR: Button
    private val resetClear = Runnable { setConfirmClear(false) }

    /** Load slot [i] into the fields and open the sheet. */
    fun edit(i: Int) {
        if (i !in 0 until SetlistStore.SLOTS) return
        cur = i
        if (isShowing) load() else a.sheets.open(this)
    }

    override fun onShow() {
        view
        ActivityResults.register(ActivityResults.REQ_MEMO_TXT) { onTxt(it) }
        if (cur >= 0) load()
        root.requestFocus()   // no field grabs the keyboard on open
    }

    override fun onHide() {
        a.ui.removeCallbacks(resetClear)
        setConfirmClear(false)
        hideIme()
    }

    // ------------------------------------------------------------------ state <-> fields

    private fun load() {
        val s = store.slot(cur)
        hdr.text = L10n.t("SLOT") + " " + SetlistStore.codeOf(cur)
        eTitle.setText(s.title)
        eMemo.setText(s.memo)
        eBpm = if (s.bpm in 40..240) s.bpm else 0
        eTrn = s.trn.coerceIn(-12, 12)
        eTrim = s.trimDb.coerceIn(-12f, 6f)
        eShow = s.memoShow
        nBpm.refresh(); nTrn.refresh(); nTrim.refresh()
        syncShow()
        moveL.alpha = if (cur > 0) 1f else 0.4f
        moveR.alpha = if (cur < SetlistStore.SLOTS - 1) 1f else 0.4f
        setConfirmClear(false)
    }

    /** Write the typed fields into the slot. */
    private fun commit() {
        val title = eTitle.text.toString().trim()
        val memo = eMemo.text.toString()
        store.update(cur) {
            this.title = title
            this.bpm = eBpm
            this.trn = eTrn
            this.trimDb = eTrim
            this.memo = memo
            this.memoShow = eShow
        }
    }

    private fun syncShow() {
        showBtn.text = (if (eShow) "☑ " else "☐ ") + L10n.t("Show on recall")
        showBtn.background = Hw.keyBg(a, accent = if (eShow) Hw.MOD_OSC else 0)
    }

    private fun setConfirmClear(on: Boolean) {
        if (!::clearBtn.isInitialized) return
        confirmClear = on
        clearBtn.text = L10n.t(if (on) "CONFIRM?" else "CLEAR")
        clearBtn.background = Hw.keyBg(a, accent = if (on) Hw.MOD_RED else 0)
    }

    // ------------------------------------------------------------------ actions

    private fun doSave() {
        commit()
        onChanged()
        dismiss()
    }

    private fun doStore() {
        hideIme()
        commit()
        val typed = eTitle.text.toString().trim()
        onStoreCurrent(cur)
        if (typed.isNotEmpty() && store.slot(cur).title.isBlank()) store.update(cur) { title = typed }
        load()
        onChanged()
    }

    private fun doClear() {
        if (!confirmClear) {
            setConfirmClear(true)
            a.ui.removeCallbacks(resetClear)
            a.ui.postDelayed(resetClear, CLEAR_CONFIRM_MS)
            return
        }
        a.ui.removeCallbacks(resetClear)
        store.clearSlot(cur)
        onChanged()
        dismiss()
    }

    private fun doMove(delta: Int) {
        val to = cur + delta
        if (to !in 0 until SetlistStore.SLOTS) return
        hideIme()
        commit()
        if (store.moveSlot(cur, to)) {
            cur = to
            store.viewedBank = to / SetlistStore.PER_BANK
            load()
            onChanged()
        }
    }

    private fun bumpBpm(m: Int) {
        val k = ((if (eBpm == 0) 0 else eBpm - 39) + m).coerceIn(0, 201)
        eBpm = if (k == 0) 0 else k + 39
        nBpm.refresh()
    }

    private fun bumpTrn(m: Int) {
        eTrn = (eTrn + m).coerceIn(-12, 12)
        nTrn.refresh()
    }

    private fun bumpTrim(m: Int) {
        eTrim = (((eTrim + m * 0.5f) * 2f).roundToInt() / 2f).coerceIn(-12f, 6f)
        nTrim.refresh()
    }

    // ------------------------------------------------------------------ memo .txt import

    private fun pickTxt() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
            putExtra(Intent.EXTRA_MIME_TYPES, arrayOf("text/plain", "text/*", "application/octet-stream"))
        }
        try {
            @Suppress("DEPRECATION")
            a.startActivityForResult(intent, ActivityResults.REQ_MEMO_TXT)
        } catch (e: Exception) {
            a.log("no file picker available", Ui.WARN)
        }
    }

    private fun onTxt(data: Intent?) {
        val uri = data?.data ?: return
        val target = cur
        a.loader.execute {
            val text = try {
                readText(uri)
            } catch (e: Exception) {
                a.ui.post { a.log("memo import failed: ${e.message}", Ui.ERR) }
                return@execute
            }
            a.ui.post {
                if (isShowing && cur == target) {
                    eMemo.setText(text)
                    a.log("memo imported (${text.length} chars)")
                }
            }
        }
    }

    private fun readText(uri: Uri): String {
        val buf = ByteArrayOutputStream()
        (a.contentResolver.openInputStream(uri) ?: throw java.io.IOException("cannot open file")).use { ins ->
            val chunk = ByteArray(8192)
            while (true) {
                val n = ins.read(chunk)
                if (n < 0) break
                buf.write(chunk, 0, n)
                if (buf.size() > MAX_IMPORT_BYTES) throw java.io.IOException("file too large (max ${MAX_IMPORT_BYTES / 1024} KB)")
            }
        }
        val b = buf.toByteArray()
        var s = when {
            b.size >= 2 && b[0] == 0xFF.toByte() && b[1] == 0xFE.toByte() -> String(b, 2, b.size - 2, Charsets.UTF_16LE)
            b.size >= 2 && b[0] == 0xFE.toByte() && b[1] == 0xFF.toByte() -> String(b, 2, b.size - 2, Charsets.UTF_16BE)
            else -> {
                val off = if (b.size >= 3 && b[0] == 0xEF.toByte() && b[1] == 0xBB.toByte() && b[2] == 0xBF.toByte()) 3 else 0
                try {
                    Charsets.UTF_8.newDecoder()
                        .onMalformedInput(CodingErrorAction.REPORT)
                        .onUnmappableCharacter(CodingErrorAction.REPORT)
                        .decode(ByteBuffer.wrap(b, off, b.size - off)).toString()
                } catch (e: CharacterCodingException) {
                    try { String(b, off, b.size - off, Charset.forName("windows-31j")) }   // Windows Notepad "ANSI" in Japan
                    catch (e2: Exception) { String(b, off, b.size - off, Charsets.ISO_8859_1) }
                }
            }
        }
        s = s.replace("\r\n", "\n").replace('\r', '\n')
        return if (s.length > MAX_MEMO_CHARS) s.substring(0, MAX_MEMO_CHARS) else s
    }

    // ------------------------------------------------------------------ UI

    private fun hideIme() {
        if (!::eTitle.isInitialized) return
        val token = eTitle.windowToken
        if (token != null) {
            try {
                (a.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager).hideSoftInputFromWindow(token, 0)
            } catch (_: Exception) {}
        }
        eTitle.clearFocus()
        eMemo.clearFocus()
    }

    private fun lbl(text: String) = Hw.engraved(a, text, 9.5f)

    private fun btn(label: String, onClick: () -> Unit): Button =
        Ui.button(a, label) { onClick() }.apply {
            textSize = 12f
            isSingleLine = true
            setPadding(dp(8), 0, dp(8), 0)
        }

    private fun field(multiline: Boolean, hint: String): EditText = EditText(a).apply {
        setTextColor(Hw.LCD_FG)
        setHintTextColor(Hw.INK_DIM)
        this.hint = L10n.t(hint)
        textSize = 14f
        typeface = Hw.MONO
        background = Hw.lcdBg(a)
        setPadding(dp(8), dp(5), dp(8), dp(5))
        // landscape phone IMEs otherwise switch to a full-screen extract editor
        val flags = EditorInfo.IME_FLAG_NO_EXTRACT_UI or EditorInfo.IME_FLAG_NO_FULLSCREEN
        if (multiline) {
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE
            imeOptions = flags
            gravity = Gravity.TOP or Gravity.START
            minLines = 2
            maxLines = 6
            isVerticalScrollBarEnabled = true
        } else {
            inputType = InputType.TYPE_CLASS_TEXT
            maxLines = 1
            isSingleLine = true
            imeOptions = flags or EditorInfo.IME_ACTION_DONE
            setOnEditorActionListener { _, actionId, _ ->
                if (actionId == EditorInfo.IME_ACTION_DONE) { hideIme(); true } else false
            }
        }
    }

    /** `−` / `+` button; press = one step, hold = repeat (×[fast] after about a second). The callback gets the multiplier. */
    @SuppressLint("ClickableViewAccessibility")
    private fun stepper(label: String, fast: Int, onStep: (Int) -> Unit): TextView {
        val up = Hw.keyBg(a)
        val down = Hw.keyBg(a, pressed = true)
        val tv = TextView(a).apply {
            text = label
            gravity = Gravity.CENTER
            textSize = 17f
            setTextColor(Hw.SILK)
            typeface = Hw.BOLD
            background = up
            isClickable = true
        }
        var n = 0
        val rep = object : Runnable {
            override fun run() {
                n++
                onStep(if (n > 10) fast else 1)
                tv.postDelayed(this, 70L)
            }
        }
        tv.setOnTouchListener { v, e ->
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    v.background = down
                    n = 0
                    onStep(1)
                    v.postDelayed(rep, 400L)
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    v.background = up
                    v.removeCallbacks(rep)
                }
            }
            true
        }
        return tv
    }

    private fun cell(label: String, dn: DragNumber, minus: (Int) -> Unit, plus: (Int) -> Unit, fast: Int): View {
        val col = LinearLayout(a).apply { orientation = LinearLayout.VERTICAL }
        col.addView(lbl(label), LinearLayout.LayoutParams(-2, -2))
        val row = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        row.addView(stepper("−", fast, minus), LinearLayout.LayoutParams(dp(30), dp(34)))
        row.addView(dn, LinearLayout.LayoutParams(0, dp(34), 1f).apply { marginStart = dp(3); marginEnd = dp(3) })
        row.addView(stepper("+", fast, plus), LinearLayout.LayoutParams(dp(30), dp(34)))
        col.addView(row, LinearLayout.LayoutParams(-1, -2))
        return col
    }

    private fun build(): View {
        root = LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(12), dp(2), dp(12), dp(6))
            isFocusableInTouchMode = true
        }

        // ---- header (30 dp)
        val header = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        hdr = Hw.engraved(a, "SLOT", 11f, tr = false, color = Hw.SILK)
        header.addView(hdr, LinearLayout.LayoutParams(0, -2, 1f))
        header.addView(btn("✕") { dismiss() }, LinearLayout.LayoutParams(dp(40), dp(30)))
        root.addView(header, LinearLayout.LayoutParams(-1, dp(30)))

        // ---- title (34 dp)
        val titleRow = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        titleRow.addView(lbl("TITLE"), LinearLayout.LayoutParams(dp(48), -2))
        eTitle = field(false, "blank = preset name")
        titleRow.addView(eTitle, LinearLayout.LayoutParams(0, dp(34), 1f))
        root.addView(titleRow, LinearLayout.LayoutParams(-1, dp(34)).apply { topMargin = dp(2) })

        // ---- numbers (52 dp): BPM / TRANSPOSE / VOL TRIM
        nBpm = DragNumber(a, 0f, 201f, 1f, 0f,
            { k -> if (k <= 0f) L10n.t("KEEP") else (39 + k.roundToInt()).toString() },
            { if (eBpm == 0) 0f else (eBpm - 39).toFloat() },
            { k -> eBpm = if (k <= 0f) 0 else 39 + k.roundToInt() })
        nTrn = DragNumber(a, -12f, 12f, 1f, 0f,
            { String.format(Locale.US, "%+d st", it.roundToInt()) },
            { eTrn.toFloat() },
            { eTrn = it.roundToInt().coerceIn(-12, 12) })
        nTrim = DragNumber(a, -12f, 6f, 0.5f, 0f,
            { String.format(Locale.US, "%+.1f dB", it) },
            { eTrim },
            { eTrim = ((it * 2f).roundToInt() / 2f).coerceIn(-12f, 6f) })
        val nums = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL }
        nums.addView(cell("BPM (0 = KEEP)", nBpm, { bumpBpm(-it) }, { bumpBpm(it) }, 5), LinearLayout.LayoutParams(0, -2, 1f))
        nums.addView(cell("TRANSPOSE", nTrn, { bumpTrn(-it) }, { bumpTrn(it) }, 1), LinearLayout.LayoutParams(0, -2, 1f).apply { marginStart = dp(10) })
        nums.addView(cell("VOL TRIM", nTrim, { bumpTrim(-it) }, { bumpTrim(it) }, 2), LinearLayout.LayoutParams(0, -2, 1f).apply { marginStart = dp(10) })
        root.addView(nums, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(4) })

        // ---- memo (flex): text + [Import .txt…] [☐ show on recall]
        val memoRow = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL }
        eMemo = field(true, "memo (lyrics, chords, notes)")
        memoRow.addView(eMemo, LinearLayout.LayoutParams(0, -1, 1f))
        val side = LinearLayout(a).apply { orientation = LinearLayout.VERTICAL }
        side.addView(btn("Import .txt…") { pickTxt() }, LinearLayout.LayoutParams(-1, dp(36)))
        showBtn = btn("Show on recall") { eShow = !eShow; syncShow() }
        side.addView(showBtn, LinearLayout.LayoutParams(-1, dp(36)).apply { topMargin = dp(6) })
        memoRow.addView(side, LinearLayout.LayoutParams(dp(150), -1).apply { marginStart = dp(8) })
        root.addView(memoRow, LinearLayout.LayoutParams(-1, 0, 1f).apply { topMargin = dp(4) })

        // ---- footer (36 dp)
        val footer = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        fun fb(b: Button, last: Boolean = false) =
            footer.addView(b, LinearLayout.LayoutParams(-2, dp(36)).apply { if (!last) marginEnd = dp(5) })
        fb(btn("STORE CURRENT") { doStore() })
        clearBtn = btn("CLEAR") { doClear() }
        fb(clearBtn)
        moveL = btn("◀ MOVE") { doMove(-1) }
        fb(moveL)
        moveR = btn("MOVE ▶") { doMove(1) }
        fb(moveR)
        footer.addView(View(a), LinearLayout.LayoutParams(0, 1, 1f))
        fb(btn("SAVE") { doSave() }.apply { background = Hw.keyBg(a, accent = Hw.MOD_OSC) }, last = true)
        root.addView(footer, LinearLayout.LayoutParams(-1, dp(36)).apply { topMargin = dp(4) })

        return root
    }
}
