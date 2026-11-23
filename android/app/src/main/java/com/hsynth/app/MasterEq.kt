package com.hsynth.app

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.os.Handler
import android.os.Looper
import android.view.GestureDetector
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.ln
import kotlin.math.log10
import kotlin.math.pow
import kotlin.math.roundToInt
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * S3 — Global master stage (5-band EQ + compressor) model, persisted in `files/master_stage.json`.
 *
 * The file uses exactly the engine's MasterStage JSON (`bypass`, `eqEnabled`, `compEnabled`, `bands[5]`, `comp`), is
 * applied on startup ([loadAndApply], called from `MainActivity.onCreate`) and is written (debounced, tmp + rename,
 * on the loader thread) after every edit. Presets, scenes and `History` never touch it.
 *
 * Semantics of the three switches: the stage is audible when `!bypass && (eqOn || compOn)`. Editing implies
 * "use it": turning EQ / COMP on, applying a venue preset or dragging a node while bypassed also clears the bypass,
 * so the user never drags a node that makes no sound.
 * UI thread only (the file write itself runs on the loader).
 */
object MasterEqStore {
    class Band(var freq: Float, var gainDb: Float = 0f, var q: Float = 0.70710678f, var enabled: Boolean = false)

    /** Venue preset: per-band (freq, gain, q), optional compressor (thresh, ratio, makeup) and the limiter. */
    class Venue(val name: String, val bands: Array<FloatArray>, val comp: FloatArray?, val limiter: Boolean = false)

    val BAND_NAMES = arrayOf("LOW SHELF", "BELL 1", "BELL 2", "BELL 3", "HIGH SHELF")
    private const val SHELF_Q = 0.70710678f

    val VENUES = listOf(
        Venue("FLAT", arrayOf(f(80f, 0f), f(250f, 0f), f(1000f, 0f), f(4000f, 0f), f(12000f, 0f)), null),
        Venue("BRIGHT PA", arrayOf(f(80f, -2f), f(250f, -2f, 1f), f(3000f, 1.5f, 1f), f(8000f, 2f, 0.9f), f(12000f, 2f)),
            floatArrayOf(-12f, 2f, 0f)),
        Venue("WARM ROOM", arrayOf(f(120f, 2.5f), f(350f, 1f, 1f), f(3000f, -1.5f, 0.8f), f(6000f, -2f, 0.9f), f(10000f, -3f)),
            floatArrayOf(-14f, 2f, 0f)),
        Venue("IEM", arrayOf(f(60f, -3f), f(250f, 0f), f(3000f, 1f, 1f), f(7000f, 0f), f(10000f, -1.5f)),
            floatArrayOf(-10f, 3f, 2f), limiter = true),
    )

    private fun f(freq: Float, gain: Float, q: Float = SHELF_Q) = floatArrayOf(freq, gain, q)

    val bands = arrayOf(Band(80f), Band(250f), Band(1000f), Band(4000f), Band(12000f))
    var bypass = true
    var eqOn = false
    var compOn = false
    var thresh = -18f
    var ratio = 2f
    var attack = 10f
    var release = 150f
    var knee = 6f
    var makeup = 0f

    /** Index of the venue preset last applied and not edited since (−1 = none). */
    var venue = -1

    /** Audible right now. */
    val active: Boolean get() = !bypass && (eqOn || compOn)

    /** Something is configured but bypassed (amber LED). */
    val bypassedConfigured: Boolean get() = bypass && (eqOn || compOn)

    /** Telemetry chip refresh hook (set by MainActivity.buildUi). */
    var onChanged: (() -> Unit)? = null

    private var file: File? = null
    private var loaded = false
    private val handler = Handler(Looper.getMainLooper())
    private val saveRun = Runnable { writeNow() }

    fun changed() { onChanged?.invoke() }

    // ------------------------------------------------------------ JSON

    fun toJson(): String = JSONObject().apply {
        put("bypass", bypass)
        put("eqEnabled", eqOn)
        put("compEnabled", compOn)
        put("bands", JSONArray().also { arr ->
            for (b in bands) arr.put(JSONObject().put("freq", b.freq.toDouble()).put("gainDb", b.gainDb.toDouble())
                .put("q", b.q.toDouble()).put("enabled", b.enabled))
        })
        put("comp", JSONObject().put("thresholdDb", thresh.toDouble()).put("ratio", ratio.toDouble())
            .put("attackMs", attack.toDouble()).put("releaseMs", release.toDouble())
            .put("kneeDb", knee.toDouble()).put("makeupDb", makeup.toDouble()))
    }.toString()

    /** Reads a MasterStage JSON into the model; missing fields keep their value. @return false on a malformed text. */
    fun parse(text: String): Boolean = try {
        val j = JSONObject(text)
        bypass = j.optBoolean("bypass", bypass)
        eqOn = j.optBoolean("eqEnabled", eqOn)
        compOn = j.optBoolean("compEnabled", compOn)
        j.optJSONArray("bands")?.let { arr ->
            for (i in 0 until minOf(5, arr.length())) {
                val o = arr.optJSONObject(i) ?: continue
                val b = bands[i]
                b.freq = o.optDouble("freq", b.freq.toDouble()).toFloat().coerceIn(20f, 20000f)
                b.gainDb = o.optDouble("gainDb", b.gainDb.toDouble()).toFloat().coerceIn(-18f, 18f)
                b.q = o.optDouble("q", b.q.toDouble()).toFloat().coerceIn(0.25f, 12f)
                b.enabled = o.optBoolean("enabled", b.enabled)
            }
        }
        j.optJSONObject("comp")?.let { o ->
            thresh = o.optDouble("thresholdDb", thresh.toDouble()).toFloat().coerceIn(-48f, 0f)
            ratio = o.optDouble("ratio", ratio.toDouble()).toFloat().coerceIn(1f, 20f)
            attack = o.optDouble("attackMs", attack.toDouble()).toFloat().coerceIn(0.2f, 100f)
            release = o.optDouble("releaseMs", release.toDouble()).toFloat().coerceIn(20f, 1000f)
            knee = o.optDouble("kneeDb", knee.toDouble()).toFloat().coerceIn(0f, 18f)
            makeup = o.optDouble("makeupDb", makeup.toDouble()).toFloat().coerceIn(-12f, 18f)
        }
        true
    } catch (_: Exception) {
        false
    }

    // ------------------------------------------------------------ engine

    /** Whole stage in one call (discrete changes: switches, venue presets, startup). */
    fun pushAll() { NativeEngine.nativeSetMasterStageJson(toJson()) }

    /** Fast path while dragging an EQ node. */
    fun pushBand(i: Int) {
        val b = bands[i]
        NativeEngine.nativeSetMasterEqBand(i, b.freq, b.gainDb, b.q, b.enabled)
    }

    /** Fast path while turning a compressor control. */
    fun pushComp() {
        NativeEngine.nativeSetMasterComp(compOn, thresh, ratio, attack, release, makeup)
    }

    fun pushBypass() { NativeEngine.nativeSetMasterStageBypass(bypass) }

    /** Adopt the engine's current stage (first use, no file). */
    fun syncFromEngine() { parse(NativeEngine.nativeGetMasterStageJson()) }

    /** Startup: read `files/master_stage.json` (if any) and send it to the engine. */
    fun loadAndApply(a: MainActivity) {
        val f = File(a.filesDir, "master_stage.json")
        file = f
        syncFromEngine()
        if (f.isFile) {
            val ok = runCatching { parse(f.readText()) }.getOrDefault(false)
            if (ok) pushAll() else a.log("master_stage.json is invalid — using the engine defaults", Ui.WARN)
        }
        loaded = true
        changed()
    }

    fun ensureLoaded() { if (!loaded) { syncFromEngine(); loaded = true } }

    // ------------------------------------------------------------ persistence

    /** Debounced (500 ms) save; call after every user edit. */
    fun markDirty() {
        handler.removeCallbacks(saveRun)
        handler.postDelayed(saveRun, 500L)
    }

    private fun writeNow() {
        val f = file ?: return
        val json = toJson()
        AppServices.loader.execute {
            runCatching {
                val tmp = File(f.parentFile, f.name + ".tmp")
                tmp.writeText(json)
                if (!tmp.renameTo(f)) { f.writeText(json); tmp.delete() }
            }
        }
    }

    // ------------------------------------------------------------ venue presets

    fun applyVenue(index: Int) {
        val v = VENUES.getOrNull(index) ?: return
        for (i in 0 until 5) {
            val src = v.bands[i]
            val b = bands[i]
            b.freq = src[0]; b.gainDb = src[1]; b.q = src[2]; b.enabled = true
        }
        eqOn = index != 0
        if (v.comp != null) {
            thresh = v.comp[0]; ratio = v.comp[1]; makeup = v.comp[2]
            compOn = true
        } else compOn = false
        bypass = false
        venue = index
        pushAll()
        if (v.limiter) { History.checkpoint(); Params.set("master.limiter", 1f) }
        markDirty()
        changed()
    }
}

/**
 * S3 — editable 5-node EQ curve on a pure-black well: LS / Bell ×3 / HS nodes, log 20 Hz–20 kHz, ±12 dB.
 *
 * Drag a node (X = frequency, Y = gain, relative to where the finger landed); a second finger moving vertically
 * changes Q. The response (128 log-spaced points × 5 analytic RBJ biquads at 48 kHz) is evaluated into preallocated
 * arrays in [recompute] — only when something changed — and drawn with one reused [Path]; onDraw() allocates nothing.
 */
@SuppressLint("ViewConstructor")
class MasterEqView(ctx: Context) : View(ctx) {
    /** Selected band 0..4. */
    var selected = 0
        set(v) { if (field != v) { field = v; invalidate() } }

    /** Draw the live [ScopeData] spectrum (40 % alpha) behind the curve; the sheet owns the acquire / release. */
    var backdrop = false
        set(v) { if (field != v) { field = v; invalidate() } }

    private val specPaint = Paint().apply { color = Hw.withAlpha(Hw.MOD_FX, 102) }

    /** A node was touched (also fired when it becomes selected). */
    var onSelect: ((Int) -> Unit)? = null

    /** (band, structural) after every drag step; structural = EQ / bypass / band-enable state changed. */
    var onEdit: ((Int, Boolean) -> Unit)? = null

    private val d = resources.displayMetrics.density
    private val n = 128
    private val px = FloatArray(n)
    private val curve = FloatArray(n)
    private val c1 = DoubleArray(n)
    private val c2 = DoubleArray(n)
    private val coef = DoubleArray(30)
    private val live = BooleanArray(5)
    private val gridX = FloatArray(GRID_F.size)
    private val path = Path()
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)
    private val t = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Hw.MONO; textSize = 8.5f * d }
    private val r = RectF()
    private var l = 0f
    private var rt = 0f
    private var tp = 0f
    private var bt = 0f
    private var drag = -1
    private var offX = 0f
    private var offY = 0f
    private var qMode = false
    private var qStartY = 0f
    private var qStart = 1f
    private val gest = GestureDetector(ctx, object : GestureDetector.SimpleOnGestureListener() {
        override fun onDoubleTap(e: MotionEvent): Boolean {
            val i = hit(e.x, e.y)
            if (i >= 0) {
                val b = MasterEqStore.bands[i]
                b.gainDb = 0f
                b.q = 0.70710678f
                onSelect?.invoke(i)
                onEdit?.invoke(i, false)
                recompute()
            }
            return true
        }
    })

    init {
        isClickable = true
        for (i in 0 until n) {
            val w = 2.0 * PI * freqAt(i) / FS
            c1[i] = cos(w)
            c2[i] = cos(2.0 * w)
        }
        recompute()
    }

    private fun freqAt(i: Int) = (20.0 * 10.0.pow(3.0 * i / (n - 1))).toFloat()
    private fun fx(f: Float) = l + (log10(f / 20f) / 3f) * (rt - l)
    private fun gy(db: Float) = tp + (1f - (db.coerceIn(-12f, 12f) + 12f) / 24f) * (bt - tp)
    private fun xf(x: Float) = (20f * 10f.pow(3f * ((x - l) / (rt - l)).coerceIn(0f, 1f))).coerceIn(20f, 20000f)
    private fun yg(y: Float) = ((1f - ((y - tp) / (bt - tp)).coerceIn(0f, 1f)) * 24f - 12f)

    override fun onMeasure(widthSpec: Int, heightSpec: Int) {
        setMeasuredDimension(resolveSize((520 * d).toInt(), widthSpec), resolveSize((184 * d).toInt(), heightSpec))
    }

    override fun onSizeChanged(w: Int, h: Int, ow: Int, oh: Int) {
        super.onSizeChanged(w, h, ow, oh)
        l = 24f * d
        rt = w - 8f * d
        tp = 8f * d
        bt = h - 14f * d
        for (i in 0 until n) px[i] = l + i / (n - 1f) * (rt - l)
        for (i in GRID_F.indices) gridX[i] = fx(GRID_F[i])
    }

    /** Re-evaluate the combined response (call after any band change). */
    fun recompute() {
        java.util.Arrays.fill(curve, 0f)
        for (bi in 0 until 5) {
            val b = MasterEqStore.bands[bi]
            live[bi] = b.enabled && b.gainDb != 0f
            if (!live[bi]) continue
            val a = 10.0.pow(b.gainDb / 40.0)
            val w0 = 2.0 * PI * b.freq / FS
            val cw = cos(w0)
            val alpha = sin(w0) / (2.0 * b.q)
            val o = bi * 6
            when (bi) {
                0 -> {
                    val s = 2.0 * sqrt(a) * alpha
                    coef[o] = a * ((a + 1) - (a - 1) * cw + s)
                    coef[o + 1] = 2 * a * ((a - 1) - (a + 1) * cw)
                    coef[o + 2] = a * ((a + 1) - (a - 1) * cw - s)
                    coef[o + 3] = (a + 1) + (a - 1) * cw + s
                    coef[o + 4] = -2 * ((a - 1) + (a + 1) * cw)
                    coef[o + 5] = (a + 1) + (a - 1) * cw - s
                }
                4 -> {
                    val s = 2.0 * sqrt(a) * alpha
                    coef[o] = a * ((a + 1) + (a - 1) * cw + s)
                    coef[o + 1] = -2 * a * ((a - 1) + (a + 1) * cw)
                    coef[o + 2] = a * ((a + 1) + (a - 1) * cw - s)
                    coef[o + 3] = (a + 1) - (a - 1) * cw + s
                    coef[o + 4] = 2 * ((a - 1) - (a + 1) * cw)
                    coef[o + 5] = (a + 1) - (a - 1) * cw - s
                }
                else -> {
                    coef[o] = 1 + alpha * a
                    coef[o + 1] = -2 * cw
                    coef[o + 2] = 1 - alpha * a
                    coef[o + 3] = 1 + alpha / a
                    coef[o + 4] = -2 * cw
                    coef[o + 5] = 1 - alpha / a
                }
            }
        }
        for (bi in 0 until 5) {
            if (!live[bi]) continue
            val o = bi * 6
            val b0 = coef[o]; val b1 = coef[o + 1]; val b2 = coef[o + 2]
            val a0 = coef[o + 3]; val a1 = coef[o + 4]; val a2 = coef[o + 5]
            val nc = b0 * b0 + b1 * b1 + b2 * b2
            val dc = a0 * a0 + a1 * a1 + a2 * a2
            val n1 = 2 * (b0 * b1 + b1 * b2)
            val d1 = 2 * (a0 * a1 + a1 * a2)
            val n2 = 2 * b0 * b2
            val d2 = 2 * a0 * a2
            for (i in 0 until n) {
                val num = nc + n1 * c1[i] + n2 * c2[i]
                val den = dc + d1 * c1[i] + d2 * c2[i]
                if (num > 0.0 && den > 0.0) curve[i] += (10.0 * log10(num / den)).toFloat()
            }
        }
        invalidate()
    }

    private fun hit(x: Float, y: Float): Int {
        var best = -1
        var bd = (22f * d) * (22f * d)
        for (i in 0 until 5) {
            val b = MasterEqStore.bands[i]
            val dx = x - fx(b.freq)
            val dy = y - gy(b.gainDb)
            val dd = dx * dx + dy * dy
            if (dd <= bd) { bd = dd; best = i }
        }
        return best
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        gest.onTouchEvent(e)
        val st = MasterEqStore
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                parent?.requestDisallowInterceptTouchEvent(true)
                qMode = false
                drag = hit(e.x, e.y)
                if (drag >= 0) {
                    selected = drag
                    val b = st.bands[drag]
                    offX = fx(b.freq) - e.x
                    offY = gy(b.gainDb) - e.y
                    onSelect?.invoke(drag)
                }
            }
            MotionEvent.ACTION_POINTER_DOWN -> if (drag >= 0 && e.pointerCount == 2) {
                qMode = true
                qStartY = e.getY(1)
                qStart = st.bands[drag].q
            }
            MotionEvent.ACTION_POINTER_UP -> if (qMode && e.pointerCount <= 2) {
                qMode = false
                drag = -1   // finger 1 lifted: the gesture ends, the next DOWN starts a new one
            }
            MotionEvent.ACTION_MOVE -> if (drag >= 0) {
                val b = st.bands[drag]
                var structural = false
                if (!b.enabled) { b.enabled = true; structural = true }
                if (!st.eqOn) { st.eqOn = true; structural = true }
                if (st.bypass) { st.bypass = false; structural = true }
                if (qMode && e.pointerCount >= 2) {
                    b.q = (qStart * 2f.pow((qStartY - e.getY(1)) / (80f * d))).coerceIn(0.25f, 12f)
                } else if (!qMode) {
                    b.freq = xf(e.x + offX)
                    b.gainDb = (yg(e.y + offY) * 10f).roundToInt() / 10f
                }
                st.venue = -1
                recompute()
                onEdit?.invoke(drag, structural)
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> { drag = -1; qMode = false }
        }
        return true
    }

    override fun onDraw(c: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        if (w <= 0f) return
        val st = MasterEqStore
        val on = st.active && st.eqOn
        val col = if (on) Hw.MOD_FX else Hw.INK_DIM

        r.set(0f, 0f, w, h)
        p.style = Paint.Style.FILL
        p.color = Hw.WELL
        c.drawRoundRect(r, 6f * d, 6f * d, p)

        // live spectrum backdrop: 64 log bins over 20 Hz .. 20 kHz == the plot's own x range
        if (backdrop) {
            val bw = (rt - l) / 64f
            val ph = bt - tp
            val sp = ScopeData.spec
            for (i in 0 until 64) {
                val v = sp[i]
                if (v <= 0.01f) continue
                val x = l + i * bw
                c.drawRect(x, bt - v.coerceAtMost(1f) * ph, x + bw - 0.5f * d, bt, specPaint)
            }
        }

        // grid
        p.strokeWidth = d
        p.color = Hw.LED_OFF
        t.textAlign = Paint.Align.CENTER
        t.color = Hw.INK_DIM
        for (i in GRID_F.indices) {
            c.drawRect(gridX[i], tp, gridX[i] + d, bt, p)
            c.drawText(GRID_L[i], gridX[i], h - 3f * d, t)
        }
        t.textAlign = Paint.Align.RIGHT
        for (db in intArrayOf(-12, -6, 0, 6, 12)) {
            val y = gy(db.toFloat())
            p.color = if (db == 0) Hw.PANEL_HI else Hw.LED_OFF
            c.drawRect(l, y, rt, y + d, p)
            c.drawText(DB_L[db / 6 + 2], l - 3f * d, y + 3f * d, t)
        }

        // response curve + fill toward 0 dB
        val zero = gy(0f)
        path.rewind()
        path.moveTo(px[0], gy(curve[0]))
        for (i in 1 until n) path.lineTo(px[i], gy(curve[i]))
        p.style = Paint.Style.STROKE
        p.strokeWidth = 2f * d
        p.strokeJoin = Paint.Join.ROUND
        p.color = col
        c.drawPath(path, p)
        path.lineTo(px[n - 1], zero)
        path.lineTo(px[0], zero)
        path.close()
        p.style = Paint.Style.FILL
        p.color = Hw.withAlpha(col, 34)
        c.drawPath(path, p)

        // nodes
        t.textAlign = Paint.Align.CENTER
        for (i in 0 until 5) {
            val b = st.bands[i]
            val x = fx(b.freq)
            val y = gy(b.gainDb)
            val sel = i == selected
            val nc = if (b.enabled && on) col else Hw.INK_DIM
            if (sel) {
                p.color = Hw.withAlpha(nc, 60)
                c.drawCircle(x, y, 11f * d, p)
            }
            p.color = if (sel) Hw.SILK else nc
            c.drawCircle(x, y, (if (sel) 6f else 5f) * d, p)
            p.color = Hw.WELL
            c.drawCircle(x, y, 2.2f * d, p)
            t.color = if (sel) Hw.SILK else Hw.ENGRAVE
            c.drawText(NODE_L[i], x, y - 9f * d, t)
        }
        p.style = Paint.Style.STROKE
        p.strokeWidth = d
        p.color = Hw.PANEL_EDGE
        r.inset(d / 2f, d / 2f)
        c.drawRoundRect(r, 6f * d, 6f * d, p)
        p.style = Paint.Style.FILL
    }

    companion object {
        private const val FS = 48000.0
        private val GRID_F = floatArrayOf(50f, 100f, 200f, 500f, 1000f, 2000f, 5000f, 10000f)
        private val GRID_L = arrayOf("50", "100", "200", "500", "1k", "2k", "5k", "10k")
        private val DB_L = arrayOf("-12", "-6", "0", "+6", "+12")
        private val NODE_L = arrayOf("1", "2", "3", "4", "5")
    }
}

/** Slim gain-reduction meter (14 dp): amber bar growing from the left, "GR −3.2 dB" read-out. Zero-alloc onDraw. */
@SuppressLint("ViewConstructor")
private class GrMeter(ctx: Context) : View(ctx) {
    private val d = resources.displayMetrics.density
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)
    private val t = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Hw.MONO; textSize = 9.5f * d }
    private val r = RectF()
    private var gr = 0f
    private var label = "GR  0.0 dB"

    /** [db] = gain reduction in dB (sign ignored). */
    fun set(db: Float) {
        val v = (abs(db) * 10f).roundToInt() / 10f
        if (v == gr) return
        gr = v
        label = "GR -" + String.format("%.1f", v) + " dB"
        invalidate()
    }

    override fun onDraw(c: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        r.set(0f, 0f, w, h)
        p.style = Paint.Style.FILL
        p.color = Hw.WELL
        c.drawRoundRect(r, 4f * d, 4f * d, p)
        val k = (gr / 18f).coerceIn(0f, 1f)
        if (k > 0f) {
            p.color = if (gr > 12f) Hw.MOD_RED else Hw.MOD_FILTER
            r.set(2f * d, 2f * d, 2f * d + (w - 4f * d) * k, h - 2f * d)
            c.drawRoundRect(r, 2f * d, 2f * d, p)
        }
        t.color = Hw.SILK
        t.textAlign = Paint.Align.RIGHT
        c.drawText(label, w - 5f * d, h / 2f + 3.3f * d, t)
    }
}

/** Numeric LCD with fine vertical drag over a [min]..[max] range (linear or log), double-tap = [def]. */
@SuppressLint("ViewConstructor")
private class RangeField(
    ctx: Context,
    private val min: Float,
    private val max: Float,
    private val log: Boolean,
    private val def: Float,
    private val fmt: (Float) -> String,
    private val get: () -> Float,
    private val set: (Float) -> Unit,
) : TextView(ctx) {
    private val d = resources.displayMetrics.density
    private var downY = 0f
    private var startN = 0f
    private val gest = GestureDetector(ctx, object : GestureDetector.SimpleOnGestureListener() {
        override fun onDoubleTap(e: MotionEvent): Boolean { set(def); refresh(); return true }
    })

    init {
        textSize = 12f
        setTextColor(Hw.LCD_FG)
        typeface = Hw.MONO
        background = Hw.lcdBg(ctx)
        gravity = Gravity.CENTER
        setPadding((3 * d).toInt(), 0, (3 * d).toInt(), 0)
        maxLines = 1
    }

    private fun norm(v: Float) = (if (log) ln(v / min) / ln(max / min) else (v - min) / (max - min)).coerceIn(0f, 1f)
    private fun denorm(n: Float) = if (log) min * (max / min).pow(n) else min + n * (max - min)

    fun refresh() { text = fmt(get()) }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        gest.onTouchEvent(e)
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> { parent?.requestDisallowInterceptTouchEvent(true); downY = e.y; startN = norm(get()) }
            MotionEvent.ACTION_MOVE -> {
                val v = denorm((startN + (downY - e.y) / (200f * d)).coerceIn(0f, 1f))
                if (v != get()) { set(v); refresh() }
            }
        }
        return true
    }
}

/**
 * S3 — Master EQ & Comp sheet (BOTTOM, 862 × 262 dp). Header: venue presets · EQ · COMP · BYPASS · ✕. Body: the
 * [MasterEqView] with FREQ / GAIN / Q fields for the selected band on the left, the COMPRESSOR section
 * (THRESH / RATIO / MAKEUP knobs, GR meter, ATK / REL, Limiter) on the right. Everything persists through
 * [MasterEqStore]; the view is built on first open and cached.
 */
class MasterEqSheet(private val a: MainActivity) : Sheet(Edge.BOTTOM, sizeDp = 262), FrameTicker.Client {
    private val d = a.resources.displayMetrics.density
    private fun dp(v: Int) = (v * d).toInt()

    private lateinit var eqView: MasterEqView
    private lateinit var grMeter: GrMeter
    private lateinit var bandLabel: TextView
    private lateinit var fFreq: RangeField
    private lateinit var fGain: RangeField
    private lateinit var fQ: RangeField
    private lateinit var fAtk: RangeField
    private lateinit var fRel: RangeField
    private lateinit var kThresh: KnobView
    private lateinit var kRatio: KnobView
    private lateinit var kMakeup: KnobView
    private lateinit var limiter: ToggleSwitch
    private lateinit var chipEq: ChipButton
    private lateinit var chipComp: ChipButton
    private lateinit var chipBypass: ChipButton
    private val venueChips = ArrayList<ChipButton>()
    private var sel = 0

    override val view: View by lazy { build() }

    override val intervalMs: Int get() = 50
    override val tickView: View? get() = if (::grMeter.isInitialized) grMeter else null

    override fun onFrame(nowMs: Long) {
        grMeter.set(if (MasterEqStore.active && MasterEqStore.compOn) NativeEngine.nativeMasterCompGrDb() else 0f)
    }

    override fun onShow() {
        view   // make sure it is built
        MasterEqStore.ensureLoaded()
        refreshAll()
        FrameTicker.register(this)
        // live spectrum behind the curve: the shared scope tap runs only while this sheet is open (and visible)
        eqView.backdrop = true
        ScopeData.acquire(eqView)
    }

    override fun onHide() {
        FrameTicker.unregister(this)
        if (::eqView.isInitialized) {
            ScopeData.release(eqView)
            eqView.backdrop = false
        }
    }

    private fun fmtFreq(f: Float) = if (f >= 1000f) String.format("%.2fk", f / 1000f) else "${f.roundToInt()}"

    private fun build(): View {
        val st = MasterEqStore
        val portrait = Ui.portrait(a)

        // ---- header
        val header = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        header.addView(Hw.engraved(a, "MASTER EQ · COMP", 11f, tr = false, color = Hw.SILK),
            LinearLayout.LayoutParams(-2, -2).apply { marginEnd = dp(10) })
        val widths = intArrayOf(44, 78, 84, 40)
        st.VENUES.forEachIndexed { i, v ->
            val chip = ChipButton(a, v.name, tr = false, widthDp = widths[i].toFloat(), accent = Hw.MOD_FX)
            chip.setOnClickListener { st.applyVenue(i); refreshAll() }
            venueChips += chip
            header.addView(chip, LinearLayout.LayoutParams(-2, dp(22)).apply { marginEnd = dp(4) })
        }
        header.addView(View(a), LinearLayout.LayoutParams(0, 1, 1f))
        chipEq = ChipButton(a, "EQ", widthDp = 40f, accent = Hw.MOD_FX).apply {
            setOnClickListener {
                st.eqOn = !st.eqOn
                if (st.eqOn) st.bypass = false
                st.venue = -1
                edited(structural = true)
            }
        }
        chipComp = ChipButton(a, "COMP", widthDp = 54f, accent = Hw.MOD_FILTER).apply {
            setOnClickListener {
                st.compOn = !st.compOn
                if (st.compOn) st.bypass = false
                st.venue = -1
                edited(structural = true)
            }
        }
        chipBypass = ChipButton(a, "BYPASS", widthDp = 64f, accent = Hw.MOD_FILTER).apply {
            setOnClickListener {
                st.bypass = !st.bypass
                st.pushBypass()
                st.markDirty()
                st.changed()
                refreshAll()
            }
        }
        val close = ChipButton(a, "✕", widthDp = 30f).apply { setOnClickListener { dismiss() } }
        header.addView(chipEq, LinearLayout.LayoutParams(-2, dp(22)).apply { marginEnd = dp(4) })
        header.addView(chipComp, LinearLayout.LayoutParams(-2, dp(22)).apply { marginEnd = dp(4) })
        header.addView(chipBypass, LinearLayout.LayoutParams(-2, dp(22)).apply { marginEnd = dp(8) })
        header.addView(close, LinearLayout.LayoutParams(-2, dp(22)))

        // ---- EQ column
        eqView = MasterEqView(a).apply {
            onSelect = { i -> select(i) }
            onEdit = { i, structural ->
                if (structural) st.pushAll() else st.pushBand(i)
                fFreq.refresh(); fGain.refresh(); fQ.refresh()
                st.markDirty()
                if (structural || st.venue != -1) { st.venue = -1; refreshChips() }
                st.changed()
            }
        }
        bandLabel = Hw.engraved(a, "", 10f, tr = false, color = Hw.MOD_FX)
        fFreq = RangeField(a, 20f, 20000f, true, 1000f, { fmtFreq(it) + " Hz" }, { st.bands[sel].freq }) { v ->
            bandEdit { it.freq = v }
        }
        fGain = RangeField(a, -12f, 12f, false, 0f, { String.format("%+.1f dB", it) }, { st.bands[sel].gainDb }) { v ->
            bandEdit { it.gainDb = (v * 10f).roundToInt() / 10f }
        }
        fQ = RangeField(a, 0.25f, 12f, true, 0.70710678f, { String.format("%.2f", it) }, { st.bands[sel].q }) { v ->
            bandEdit { it.q = v }
        }
        val fields = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        fields.addView(bandLabel, LinearLayout.LayoutParams(dp(92), -2))
        fun lbl(s: String) = Hw.engraved(a, s, 9.5f, tr = false)
        fields.addView(lbl("FREQ"), LinearLayout.LayoutParams(-2, -2).apply { marginEnd = dp(4) })
        fields.addView(fFreq, LinearLayout.LayoutParams(0, -1, 1f).apply { marginEnd = dp(8) })
        fields.addView(lbl("GAIN"), LinearLayout.LayoutParams(-2, -2).apply { marginEnd = dp(4) })
        fields.addView(fGain, LinearLayout.LayoutParams(0, -1, 1f).apply { marginEnd = dp(8) })
        fields.addView(lbl("Q"), LinearLayout.LayoutParams(-2, -2).apply { marginEnd = dp(4) })
        fields.addView(fQ, LinearLayout.LayoutParams(0, -1, 0.8f))
        val eqCol = LinearLayout(a).apply { orientation = LinearLayout.VERTICAL }
        eqCol.addView(eqView, LinearLayout.LayoutParams(-1, if (portrait) dp(170) else 0, if (portrait) 0f else 1f))
        eqCol.addView(fields, LinearLayout.LayoutParams(-1, dp(34)).apply { topMargin = dp(4) })

        // ---- compressor section
        val comp = Hw.Section(a, "COMPRESSOR", tr = false, accent = Hw.MOD_FILTER)
        kThresh = KnobView(a, "THRESH", false, 40f, Hw.MOD_FILTER).apply {
            onChange = { n -> compEdit { st.thresh = (-48f + n * 48f).let { v -> (v * 2f).roundToInt() / 2f } } }
            onReset = { compEdit { st.thresh = -18f }; refreshComp() }
        }
        kRatio = KnobView(a, "RATIO", false, 40f, Hw.MOD_FILTER).apply {
            onChange = { n -> compEdit { st.ratio = (20f.pow(n)).let { v -> (v * 10f).roundToInt() / 10f }.coerceAtLeast(1f) } }
            onReset = { compEdit { st.ratio = 2f }; refreshComp() }
        }
        kMakeup = KnobView(a, "MAKEUP", false, 40f, Hw.MOD_FILTER).apply {
            onChange = { n -> compEdit { st.makeup = (-12f + n * 30f).let { v -> (v * 2f).roundToInt() / 2f } } }
            onReset = { compEdit { st.makeup = 0f }; refreshComp() }
        }
        val knobs = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER }
        knobs.addView(kThresh, LinearLayout.LayoutParams(0, -2, 1f))
        knobs.addView(kRatio, LinearLayout.LayoutParams(0, -2, 1f))
        knobs.addView(kMakeup, LinearLayout.LayoutParams(0, -2, 1f))
        comp.body.addView(knobs, LinearLayout.LayoutParams(-1, -2))
        grMeter = GrMeter(a)
        comp.body.addView(grMeter, LinearLayout.LayoutParams(-1, dp(14)).apply { topMargin = dp(2) })
        fAtk = RangeField(a, 0.2f, 100f, true, 10f, { String.format("%.1f ms", it) }, { st.attack }) { v -> compEdit { st.attack = v } }
        fRel = RangeField(a, 20f, 1000f, true, 150f, { "${it.roundToInt()} ms" }, { st.release }) { v -> compEdit { st.release = v } }
        limiter = ToggleSwitch(a, "Limiter", vertical = false, accent = Hw.MOD_FILTER).apply {
            onToggle = { on -> History.checkpoint(); Params.set("master.limiter", if (on) 1f else 0f) }
        }
        val ar = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        ar.addView(lbl("ATK"), LinearLayout.LayoutParams(-2, -2).apply { marginEnd = dp(3) })
        ar.addView(fAtk, LinearLayout.LayoutParams(0, -1, 1f).apply { marginEnd = dp(6) })
        ar.addView(lbl("REL"), LinearLayout.LayoutParams(-2, -2).apply { marginEnd = dp(3) })
        ar.addView(fRel, LinearLayout.LayoutParams(0, -1, 1f).apply { marginEnd = dp(4) })
        ar.addView(limiter, LinearLayout.LayoutParams(-2, dp(34)))
        comp.body.addView(ar, LinearLayout.LayoutParams(-1, dp(30)).apply { topMargin = dp(4) })
        Params.addListener { if (::limiter.isInitialized) limiter.on = Params.get("master.limiter") > 0.5f }

        // ---- assemble
        val body: View = if (portrait) {
            ScrollView(a).apply {
                addView(LinearLayout(a).apply {
                    orientation = LinearLayout.VERTICAL
                    addView(eqCol, LinearLayout.LayoutParams(-1, -2))
                    addView(comp, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(6) })
                })
            }
        } else {
            LinearLayout(a).apply {
                orientation = LinearLayout.HORIZONTAL
                addView(eqCol, LinearLayout.LayoutParams(0, -1, 1f).apply { marginEnd = dp(8) })
                addView(comp, LinearLayout.LayoutParams(dp(322), -1))
            }
        }
        return LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(8), dp(2), dp(8), dp(6))
            addView(header, LinearLayout.LayoutParams(-1, dp(32)))
            addView(body, LinearLayout.LayoutParams(-1, 0, 1f))
        }
    }

    // ------------------------------------------------------------ editing

    private fun select(i: Int) {
        sel = i
        eqView.selected = i
        bandLabel.text = "${i + 1} · ${MasterEqStore.BAND_NAMES[i]}"
        fFreq.refresh(); fGain.refresh(); fQ.refresh()
    }

    /** Edit from a numeric field: same implied "enable + un-bypass" as dragging a node. */
    private fun bandEdit(f: (MasterEqStore.Band) -> Unit) {
        val st = MasterEqStore
        val b = st.bands[sel]
        var structural = false
        if (!b.enabled) { b.enabled = true; structural = true }
        if (!st.eqOn) { st.eqOn = true; structural = true }
        if (st.bypass) { st.bypass = false; structural = true }
        f(b)
        eqView.recompute()
        if (structural) st.pushAll() else st.pushBand(sel)
        st.markDirty()
        if (structural || st.venue != -1) { st.venue = -1; refreshChips() }
        st.changed()
    }

    private fun compEdit(f: () -> Unit) {
        val st = MasterEqStore
        var structural = false
        if (!st.compOn) { st.compOn = true; structural = true }
        if (st.bypass) { st.bypass = false; structural = true }
        f()
        if (structural) st.pushAll() else st.pushComp()
        st.markDirty()
        if (structural || st.venue != -1) { st.venue = -1; refreshChips() }
        refreshComp()
        st.changed()
    }

    private fun edited(structural: Boolean) {
        val st = MasterEqStore
        if (structural) st.pushAll()
        st.markDirty()
        st.changed()
        refreshAll()
    }

    private fun refreshChips() {
        val st = MasterEqStore
        venueChips.forEachIndexed { i, c -> if (c.active != (st.venue == i)) c.active = st.venue == i }
        val eqLit = st.eqOn && !st.bypass
        chipEq.active = st.eqOn
        chipEq.ledColor = if (eqLit) Hw.MOD_FX else if (st.eqOn) Hw.MOD_FILTER else 0
        val cLit = st.compOn && !st.bypass
        chipComp.active = st.compOn
        chipComp.ledColor = if (cLit) Hw.MOD_FX else if (st.compOn) Hw.MOD_FILTER else 0
        chipBypass.active = st.bypass
        chipBypass.ledColor = if (st.bypass) Hw.MOD_FILTER else 0
    }

    private fun refreshComp() {
        val st = MasterEqStore
        kThresh.value = ((st.thresh + 48f) / 48f).coerceIn(0f, 1f)
        kThresh.valueText = String.format("%.1f dB", st.thresh)
        kRatio.value = (ln(st.ratio.coerceAtLeast(1f)) / ln(20f)).coerceIn(0f, 1f)
        kRatio.valueText = String.format("%.1f:1", st.ratio)
        kMakeup.value = ((st.makeup + 12f) / 30f).coerceIn(0f, 1f)
        kMakeup.valueText = String.format("%+.1f dB", st.makeup)
        fAtk.refresh()
        fRel.refresh()
    }

    private fun refreshAll() {
        eqView.recompute()
        select(sel)
        refreshComp()
        refreshChips()
        limiter.on = Params.get("master.limiter") > 0.5f
    }
}
