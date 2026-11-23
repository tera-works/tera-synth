package com.hsynth.app

import android.annotation.SuppressLint
import android.app.AlertDialog
import android.content.Context
import android.content.Intent
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.net.Uri
import android.view.GestureDetector
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.widget.LinearLayout
import android.widget.TextView
import org.json.JSONObject
import java.io.File
import kotlin.math.abs
import kotlin.math.ln
import kotlin.math.roundToInt

/**
 * U3 — built-in historical temperaments.
 *
 * Every temperament is a table of 12 absolute cents (scale degree 0 = the root, so degree k sits at `cents[k]`); the
 * stock `tune.c … tune.b` parameters (±100 ct per pitch class, per part) hold the offset to 12-TET, i.e.
 * `offsets[(root + k) % 12] = cents[k] − 100·k` — the same convention as the engine's Scala import.
 * The temperament name shown in the UI is always *derived* ([identify]); only `tune.*` is stored in presets.
 */
object Temperaments {
    class Temperament(val name: String, val note: String, val cents: FloatArray)

    val PC_PARAM = arrayOf("c", "c#", "d", "d#", "e", "f", "f#", "g", "g#", "a", "a#", "b")
    val PC_LABEL = arrayOf("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")

    private fun ratio(n: Int, d: Int): Float = (1200.0 * ln(n.toDouble() / d) / ln(2.0)).toFloat()
    private fun ratios(vararg nd: Int) = FloatArray(nd.size / 2) { ratio(nd[it * 2], nd[it * 2 + 1]) }

    /** Quarter-comma meantone: fifth = 5^¼ (696.578 ct), chain Eb … G# (11 fifths), wolf between G# and Eb. */
    private fun meantone(): FloatArray {
        val fifth = 1200.0 * ln(5.0) / ln(2.0) / 4.0
        return FloatArray(12) { k ->
            var f = (7 * k) % 12
            if (f > 8) f -= 12
            val c = (f * fifth) % 1200.0
            (if (c < 0) c + 1200.0 else c).toFloat()
        }
    }

    val EQUAL = Temperament("Equal", "12-TET", FloatArray(12) { it * 100f })
    val all: List<Temperament> = listOf(
        EQUAL,
        Temperament("Just (5-limit)", "Ptolemy", ratios(1, 1, 16, 15, 9, 8, 6, 5, 5, 4, 4, 3, 45, 32, 3, 2, 8, 5, 5, 3, 16, 9, 15, 8)),
        Temperament("Pythagorean", "3-limit", ratios(1, 1, 256, 243, 9, 8, 32, 27, 81, 64, 4, 3, 729, 512, 3, 2, 128, 81, 27, 16, 16, 9, 243, 128)),
        Temperament("Meantone (1/4 comma)", "wolf G#–Eb", meantone()),
        Temperament("Werckmeister III", "1691",
            floatArrayOf(0f, 90.225f, 192.180f, 294.135f, 390.225f, 498.045f, 588.270f, 696.090f, 792.180f, 888.270f, 996.090f, 1092.180f)),
        Temperament("Kirnberger III", "1779",
            floatArrayOf(0f, 90.225f, 193.157f, 294.135f, 386.314f, 498.045f, 590.224f, 696.578f, 792.180f, 889.735f, 996.090f, 1088.269f)),
        // a 12-offset system cannot hold 24-TET: neutral 3rd / 7th only (approx.)
        Temperament("Arabic Rast (approx.)", "neutral 3rd / 7th",
            floatArrayOf(0f, 100f, 200f, 300f, 350f, 500f, 600f, 700f, 800f, 900f, 1000f, 1050f)),
    )

    /** `tune.*` offsets (index = absolute pitch class) of [t] with scale degree 0 on [root]. */
    fun offsets(t: Temperament, root: Int, out: FloatArray = FloatArray(12)): FloatArray {
        for (k in 0 until 12) out[(root + k) % 12] = t.cents[k] - 100f * k
        return out
    }

    /** The temperament (and root) whose offsets match [offs] within 0.05 ct, or null = "Custom". */
    fun identify(offs: FloatArray, preferRoot: Int = 0): Pair<Temperament, Int>? {
        val tmp = FloatArray(12)
        if (offs.all { abs(it) < 0.05f }) return EQUAL to preferRoot
        for (t in all) {
            if (t === EQUAL) continue
            for (i in 0 until 12) {
                val root = (preferRoot + i) % 12
                offsets(t, root, tmp)
                var ok = true
                for (k in 0 until 12) if (abs(tmp[k] - offs[k]) > 0.05f) { ok = false; break }
                if (ok) return t to root
            }
        }
        return null
    }
}

/**
 * Twelve vertical cents bars (C … B) on a black well. Relative vertical drag per bar (0.5 ct steps, ±100 ct),
 * double-tap = 0, one `History.checkpoint()` on touch-down. The visible window is ±60 ct; bars beyond it are
 * clipped (the value label still shows the real number). Zero allocations in onDraw(): values and their strings
 * are prepared in [setValues].
 */
@SuppressLint("ViewConstructor")
class CentsBarView(ctx: Context) : View(ctx) {
    /** (pitch class, new cents) after every drag step. */
    var onChange: ((Int, Float) -> Unit)? = null

    private val d = resources.displayMetrics.density
    private val values = FloatArray(12)
    private val texts = Array(12) { "0" }
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)
    private val t = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Hw.MONO; textAlign = Paint.Align.CENTER; textSize = 10f * d }
    private val r = RectF()
    private var active = -1
    private var downY = 0f
    private var startV = 0f
    private val gest = GestureDetector(ctx, object : GestureDetector.SimpleOnGestureListener() {
        override fun onDoubleTap(e: MotionEvent): Boolean {
            val i = slot(e.x)
            History.checkpoint()
            commit(i, 0f)
            return true
        }
    })

    init { isClickable = true }

    override fun onMeasure(widthSpec: Int, heightSpec: Int) {
        setMeasuredDimension(MeasureSpec.getSize(widthSpec), (96 * d).toInt())
    }

    fun setValues(src: FloatArray) {
        var dirty = false
        for (i in 0 until 12) if (values[i] != src[i]) {
            values[i] = src[i]
            texts[i] = fmt(src[i])
            dirty = true
        }
        if (dirty) invalidate()
    }

    private fun fmt(v: Float): String {
        val x = (v * 10f).roundToInt() / 10f
        return if (x == 0f) "0" else if (x == x.toInt().toFloat()) String.format("%+d", x.toInt()) else String.format("%+.1f", x)
    }

    private fun slot(x: Float) = (x / (width / 12f)).toInt().coerceIn(0, 11)

    private fun commit(i: Int, ct: Float) {
        val v = ((ct * 2f).roundToInt() / 2f).coerceIn(-100f, 100f)
        if (v == values[i]) return
        values[i] = v
        texts[i] = fmt(v)
        invalidate()
        onChange?.invoke(i, v)
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        gest.onTouchEvent(e)
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                parent?.requestDisallowInterceptTouchEvent(true)
                active = slot(e.x)
                downY = e.y
                startV = values[active]
                History.checkpoint()
                invalidate()
            }
            MotionEvent.ACTION_MOVE -> if (active >= 0) commit(active, startV + (downY - e.y) / (0.9f * d))
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> { active = -1; invalidate() }
        }
        return true
    }

    override fun onDraw(c: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        if (w <= 0f) return
        r.set(0f, 0f, w, h)
        p.style = Paint.Style.FILL
        p.color = Hw.WELL
        c.drawRoundRect(r, 6f * d, 6f * d, p)
        p.style = Paint.Style.STROKE
        p.strokeWidth = d
        p.color = Hw.PANEL_EDGE
        r.inset(d / 2f, d / 2f)
        c.drawRoundRect(r, 6f * d, 6f * d, p)
        p.style = Paint.Style.FILL

        val top = 14f * d          // value labels above
        val bot = h - 14f * d      // pitch-class names below
        val mid = (top + bot) / 2f
        val half = (bot - top) / 2f
        p.color = Hw.PANEL_EDGE
        c.drawRect(4f * d, mid, w - 4f * d, mid + d, p)
        val sw = w / 12f
        val bw = sw * 0.56f
        for (i in 0 until 12) {
            val cx = sw * (i + 0.5f)
            val v = values[i]
            val good = abs(v) < 2f
            val sel = i == active
            val col = if (good) Hw.MOD_GOOD else Hw.MOD_FILTER
            val hh = (v / 60f).coerceIn(-1f, 1f) * half
            if (hh != 0f) {
                p.color = if (sel) col else Hw.withAlpha(col, 190)
                if (hh > 0f) r.set(cx - bw / 2f, mid - hh, cx + bw / 2f, mid) else r.set(cx - bw / 2f, mid, cx + bw / 2f, mid - hh)
                c.drawRoundRect(r, 2f * d, 2f * d, p)
            }
            t.color = if (sel) Hw.SILK else if (good) Hw.INK_DIM else col
            c.drawText(texts[i], cx, 10.5f * d, t)
            t.color = if (sel) Hw.SILK else Hw.ENGRAVE
            c.drawText(Temperaments.PC_LABEL[i], cx, h - 4f * d, t)
        }
    }
}

/**
 * U3 — `EDIT > Voice > Tuning` section body:
 * `[Temperament LCD ▾] [Root C … B] [Load .scl…] [Reset]`, the cents bars, an info line and the 12 stock `tune.*`
 * knobs in a collapsed drawer. Built-in temperaments only write the 12 stock parameters (no JNI); `.scl` files are
 * copied to `files/scales/` on the loader and applied with `nativeApplyScala`.
 */
@SuppressLint("ViewConstructor")
class TuningPanel(private val a: MainActivity, private val accent: Int) : LinearLayout(a) {
    private val d = resources.displayMetrics.density
    private val metas = Temperaments.PC_PARAM.map { Params.metaOf("tune.$it") }
    private val bars = CentsBarView(a)
    private val tempLcd: TextView = Hw.lcd(a, "", 12.5f).apply {
        background = Hw.lcdBg(a, stroke = Hw.withAlpha(accent, 110))
        gravity = Gravity.CENTER_VERTICAL
        isClickable = true
        setOnClickListener { chooseTemperament() }
    }
    private val rootPills = Array(12) { i ->
        LedButton(a, Temperaments.PC_LABEL[i], tr = false, led = false, accentColor = accent).apply {
            textSize = 10.5f
            minHeight = 0
            minimumHeight = 0
            setPadding(0, 0, 0, 0)
            setOnClickListener { selectRoot(i, reapply = true) }
        }
    }
    private val info: TextView = Ui.text(a, "", 11f, Hw.ENGRAVE, tr = false).apply { typeface = Hw.MONO; maxLines = 2 }
    private val knobs = ArrayList<ParamControl>(12)
    private val tmp = FloatArray(12)
    private var root = a.prefs.getInt("tuningRoot", 0).coerceIn(0, 11)
    private var current: Temperaments.Temperament? = null

    init {
        orientation = VERTICAL
        val gap = (6 * d).toInt()

        // row A
        val rowA = FlowLayout(a, gap, gap)
        rowA.addView(tempLcd, LayoutParams((190 * d).toInt(), (34 * d).toInt()))
        val rootBox = LinearLayout(a).apply { orientation = HORIZONTAL }
        rootPills.forEachIndexed { i, b -> rootBox.addView(b, LayoutParams(0, -1, 1f).apply { if (i < 11) marginEnd = (2 * d).toInt() }) }
        rowA.addView(rootBox, LayoutParams((360 * d).toInt(), (34 * d).toInt()))
        rowA.addView(LedButton(a, "Load .scl…", led = false, accentColor = accent).apply {
            textSize = 11f; minHeight = 0; minimumHeight = 0; setPadding((8 * d).toInt(), 0, (8 * d).toInt(), 0)
            setOnClickListener { pickScl() }
        }, LayoutParams((110 * d).toInt(), (34 * d).toInt()))
        rowA.addView(LedButton(a, "Reset", led = false, accentColor = accent).apply {
            textSize = 11f; minHeight = 0; minimumHeight = 0; setPadding((8 * d).toInt(), 0, (8 * d).toInt(), 0)
            setOnClickListener { reset() }
        }, LayoutParams((70 * d).toInt(), (34 * d).toInt()))
        addView(rowA, LayoutParams(-1, -2))

        // row B
        bars.onChange = { pc, ct -> writeParam(pc, ct) }
        addView(bars, LayoutParams(-1, (96 * d).toInt()).apply { topMargin = gap })

        // row C
        addView(info, LayoutParams(-1, -2).apply { topMargin = (4 * d).toInt() })

        // row D: stock fine-tune knobs (collapsed)
        val drawer = Hw.collapsibleSection(a, "Fine-tune knobs", accent = accent, collapsed = true)
        val flow = FlowLayout(a, gap, gap)
        for (m in metas) {
            m ?: continue
            val c = ParamControl(a, m, knobSize = 40f, accent = accent) { refreshBarsAndInfo() }
            knobs += c
            flow.addView(c.view)
        }
        drawer.body.addView(flow, LayoutParams(-1, -2))
        addView(drawer, LayoutParams(-1, -2).apply { topMargin = gap })

        ActivityResults.register(ActivityResults.REQ_SCL) { data -> importScl(data?.data) }
        refresh()
    }

    /** Re-read the engine (after Undo, preset load, part change …). */
    fun refresh() {
        knobs.forEach { it.refresh() }
        refreshBarsAndInfo()
    }

    private fun readOffsets(out: FloatArray = tmp): FloatArray {
        for (i in 0 until 12) out[i] = metas[i]?.let { NativeEngine.nativeGetParam(it.id) } ?: 0f
        return out
    }

    private fun refreshBarsAndInfo() {
        val offs = readOffsets()
        bars.setValues(offs)
        val hit = Temperaments.identify(offs, root)
        if (hit != null) { current = hit.first; selectRoot(hit.second, reapply = false) } else current = null
        tempLcd.text = (hit?.let {
            if (it.first === Temperaments.EQUAL) L10n.t("Equal temperament") else "${it.first.name} · ${L10n.t("root")} ${Temperaments.PC_LABEL[it.second]}"
        } ?: L10n.t("Custom")) + "  ▾"
        showInfo()
    }

    private fun showInfo() {
        val e = lastError
        val s = lastScl
        when {
            e != null -> { info.text = e; info.setTextColor(Hw.MOD_RED) }
            s != null -> { info.text = s; info.setTextColor(Hw.ENGRAVE) }
            else -> {
                val t = current
                info.text = if (t == null) L10n.t("Custom tuning — drag a bar or turn a knob")
                else "${t.name} · ${t.note}"
                info.setTextColor(Hw.ENGRAVE)
            }
        }
    }

    private fun selectRoot(i: Int, reapply: Boolean) {
        val changed = i != root
        root = i
        if (changed) a.prefs.edit().putInt("tuningRoot", i).apply()
        rootPills.forEachIndexed { k, b -> if (b.lit != (k == i)) b.lit = k == i }
        val t = current
        if (reapply && changed && t != null && t !== Temperaments.EQUAL) applyTemperament(t)
    }

    private fun chooseTemperament() {
        val all = Temperaments.all
        AlertDialog.Builder(a).setTitle(L10n.t("Temperament"))
            .setItems(all.map { "${it.name}   (${it.note})" }.toTypedArray()) { _, w -> applyTemperament(all[w]) }
            .show()
    }

    private fun applyTemperament(t: Temperaments.Temperament) {
        History.checkpoint()
        lastScl = null
        lastError = null
        val offs = Temperaments.offsets(t, root)
        for (i in 0 until 12) metas[i]?.let { NativeEngine.setParam(it.id, offs[i].coerceIn(-100f, 100f)) }
        Hw.onParamTouched?.invoke(L10n.t("Temperament"), t.name)
        Params.notifyChanged()   // other pages / quick strip; EditPanel rebuilds the page around us
    }

    private fun writeParam(pc: Int, ct: Float) {
        metas[pc]?.let { NativeEngine.setParam(it.id, ct) }
        lastScl = null
        lastError = null
        knobs.getOrNull(pc)?.refresh()
        val offs = readOffsets()
        val hit = Temperaments.identify(offs, root)
        current = hit?.first
        tempLcd.text = (hit?.let { "${it.first.name} · ${L10n.t("root")} ${Temperaments.PC_LABEL[it.second]}" } ?: L10n.t("Custom")) + "  ▾"
        showInfo()
        Hw.onParamTouched?.invoke(Temperaments.PC_LABEL[pc], String.format("%+.1f ct", ct))
    }

    private fun reset() {
        History.checkpoint()
        lastScl = null
        lastError = null
        for (m in metas) m?.let { NativeEngine.setParam(it.id, 0f) }
        Params.notifyChanged()
    }

    // ------------------------------------------------------------ Scala

    private fun pickScl() {
        val i = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
        }
        @Suppress("DEPRECATION")
        a.startActivityForResult(i, ActivityResults.REQ_SCL)
    }

    private fun importScl(uri: Uri?) {
        uri ?: return
        val part = NativeEngine.nativeEditPart()
        val rootNow = root
        a.loader.execute {
            val dir = File(a.filesDir, "scales").apply { mkdirs() }
            var name = a.library.displayName(uri)
            if (!name.endsWith(".scl", ignoreCase = true)) name += ".scl"
            val f = File(dir, a.library.safeName(name))
            try {
                a.contentResolver.openInputStream(uri)?.use { i -> f.outputStream().use { i.copyTo(it) } }
            } catch (e: Exception) {
                a.ui.post { lastError = "scl: ${e.message}"; a.log("scl import failed: ${e.message}", Ui.ERR); showInfo() }
                return@execute
            }
            val res = runCatching { JSONObject(NativeEngine.nativeApplyScala(part, f.absolutePath, rootNow)) }.getOrNull()
            val inf = runCatching { JSONObject(NativeEngine.nativeScalaInfo(f.absolutePath)) }.getOrNull()
            a.ui.post {
                if (res?.optBoolean("ok") == true) {
                    lastError = null
                    val desc = inf?.takeIf { it.optBoolean("ok") }?.let {
                        "${f.nameWithoutExtension}: ${it.optString("desc")} (${it.optInt("notes")} notes, ${String.format("%.0f", it.optDouble("period", 1200.0))} ct)"
                    } ?: f.nameWithoutExtension
                    val warn = res.optString("warning")
                    lastScl = if (warn.isNotEmpty()) "$desc — $warn" else desc
                    a.log("scale loaded: ${f.name}")
                    Params.notifyChanged()
                } else {
                    lastError = res?.optString("error")?.takeIf { it.isNotEmpty() } ?: inf?.optString("error") ?: "scale could not be applied"
                    lastScl = null
                    a.log("scale failed: $lastError", Ui.ERR)
                    showInfo()
                }
            }
        }
    }

    companion object {
        /** Survive the page rebuild that follows [Params.notifyChanged]. */
        private var lastScl: String? = null
        private var lastError: String? = null
    }
}
