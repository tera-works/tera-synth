package com.hsynth.app

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.TypedValue
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import org.json.JSONObject
import java.io.File
import kotlin.math.abs
import kotlin.math.min
import kotlin.math.roundToInt

/*
 * S5 — TOOLS sheet: METRO | TUNER | PHRASE | SCOPE (BOTTOM, 862 × 232 dp).
 * The metronome click and the tuner run in the engine; this file is only UI + the persisted click settings.
 */

/** Allocation-free digit writer shared by the drawn readouts. */
internal object Digits {
    /** Writes [value] (with '-' when negative) at [at]; returns the next free index. */
    fun putInt(buf: CharArray, at: Int, value: Int): Int {
        var p = at
        var v = value
        if (v < 0) { buf[p++] = '-'; v = -v }
        var div = 1
        while (v / div >= 10) div *= 10
        while (div > 0) { buf[p++] = '0' + (v / div) % 10; div /= 10 }
        return p
    }
}

// ============================================================================================== click settings

/**
 * Metronome settings (everything but the tempo, which is the shared `fx.tempo`) in `files/metronome.json`.
 * `enabled` is never persisted as on: the click always boots OFF ([loadAndApply]). Settings changes are pushed
 * WITHOUT an `enabled` key so a running click keeps running.
 */
object MetronomeStore {
    var numerator = 4
    var denominator = 4
    var subdivision = 1
    var countIn = 0
    var route = 0
    var levelDb = -18f
    var accentDb = 4f

    private var file: File? = null
    private val handler = Handler(Looper.getMainLooper())
    private val saveRun = Runnable { writeNow() }

    /** Click running right now (engine truth; also changes through a footswitch). */
    val running: Boolean get() = NativeEngine.nativeClickBeat() >= 0

    private fun json(enabled: Boolean?): String = JSONObject().apply {
        if (enabled != null) put("enabled", enabled)
        put("numerator", numerator)
        put("denominator", denominator)
        put("subdivision", subdivision)
        put("countInBars", countIn)
        put("route", route)
        put("levelDb", levelDb.toDouble())
        put("accentDb", accentDb.toDouble())
    }.toString()

    private fun parse(text: String): Boolean = try {
        val j = JSONObject(text)
        numerator = j.optInt("numerator", numerator).coerceIn(1, 16)
        denominator = j.optInt("denominator", denominator).let { if (it == 2 || it == 4 || it == 8 || it == 16) it else 4 }
        subdivision = j.optInt("subdivision", subdivision).coerceIn(1, 4)
        countIn = j.optInt("countInBars", countIn).coerceIn(0, 4)
        route = j.optInt("route", route).coerceIn(0, 2)
        levelDb = j.optDouble("levelDb", levelDb.toDouble()).toFloat().coerceIn(-60f, 0f)
        accentDb = j.optDouble("accentDb", accentDb.toDouble()).toFloat().coerceIn(0f, 9f)
        true
    } catch (_: Exception) {
        false
    }

    /** Startup: engine defaults, then `files/metronome.json`; the click itself is forced OFF. */
    fun loadAndApply(a: MainActivity) {
        val f = File(a.filesDir, "metronome.json")
        file = f
        parse(NativeEngine.nativeGetMetronomeJson())
        if (f.isFile) {
            val ok = runCatching { parse(f.readText()) }.getOrDefault(false)
            if (!ok) a.log("metronome.json is invalid — using the defaults", Ui.WARN)
        }
        NativeEngine.nativeSetMetronomeJson(json(false))
        NativeEngine.nativeSetMetronomeEnabled(false)
    }

    /** After the user changed a setting: send it (the run state is untouched) and save, debounced. */
    fun push() {
        NativeEngine.nativeSetMetronomeJson(json(null))
        handler.removeCallbacks(saveRun)
        handler.postDelayed(saveRun, 500L)
    }

    fun setEnabled(on: Boolean) { NativeEngine.nativeSetMetronomeEnabled(on) }

    private fun writeNow() {
        val f = file ?: return
        val text = json(false)
        AppServices.loader.execute {
            runCatching {
                val tmp = File(f.parentFile, f.name + ".tmp")
                tmp.writeText(text)
                if (!tmp.renameTo(f)) { f.writeText(text); tmp.delete() }
            }
        }
    }
}

// ============================================================================================== BeatDotsView

/**
 * Up to 16 beat dots (accent on beat 1, current beat lit cyan). A [FrameTicker] client at 30 fps that exists only
 * while the click runs ([setRunning]) and the view is shown; it reads `nativeClickBeat()` and redraws on change only.
 */
class BeatDotsView(ctx: Context) : View(ctx), FrameTicker.Client {
    private val d = resources.displayMetrics.density
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)
    private val r = RectF()
    private var beats = 4
    private var beat = -1
    private var running = false

    override val intervalMs: Int get() = 33
    override val tickView: View? get() = this

    fun setBeats(n: Int) {
        val c = n.coerceIn(1, 16)
        if (c != beats) { beats = c; invalidate() }
    }

    fun setRunning(on: Boolean) {
        if (on == running) return
        running = on
        if (on && isAttachedToWindow) FrameTicker.register(this)
        if (!on) {
            FrameTicker.unregister(this)
            if (beat != -1) { beat = -1; invalidate() }
        }
    }

    override fun onFrame(nowMs: Long) {
        val b = NativeEngine.nativeClickBeat()
        if (b != beat) { beat = b; invalidate() }
    }

    override fun onAttachedToWindow() { super.onAttachedToWindow(); if (running) FrameTicker.register(this) }
    override fun onDetachedFromWindow() { FrameTicker.unregister(this); super.onDetachedFromWindow() }
    override fun onVisibilityAggregated(isVisible: Boolean) { super.onVisibilityAggregated(isVisible); FrameTicker.poke() }
    override fun onWindowFocusChanged(hasWindowFocus: Boolean) { super.onWindowFocusChanged(hasWindowFocus); FrameTicker.poke() }

    override fun onDraw(c: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        r.set(0f, 0f, w, h)
        p.style = Paint.Style.FILL
        p.color = Hw.WELL
        c.drawRoundRect(r, 8f * d, 8f * d, p)
        p.style = Paint.Style.STROKE
        p.strokeWidth = d
        p.color = Hw.PANEL_EDGE
        r.inset(d / 2f, d / 2f)
        c.drawRoundRect(r, 8f * d, 8f * d, p)
        p.style = Paint.Style.FILL
        val n = beats
        val cell = (w - 12f * d) / n
        val rr = min(cell * 0.34f, h * 0.30f)
        val cy = h / 2f
        for (i in 0 until n) {
            val cx = 6f * d + cell * (i + 0.5f)
            val cur = i == beat
            val acc = i == 0
            if (cur) {
                p.color = Hw.withAlpha(Hw.MOD_OSC, 70)
                c.drawCircle(cx, cy, rr * 1.55f, p)
            }
            p.color = if (cur) (if (acc) Color.WHITE else Hw.MOD_OSC) else if (acc) Hw.withAlpha(Hw.MOD_OSC, 110) else Hw.LED_OFF
            c.drawCircle(cx, cy, if (acc) rr * 1.15f else rr, p)
        }
    }
}

// ============================================================================================== TunerView

/**
 * Chromatic tuner display: 72 sp note + octave, cents read-out, frequency and a −50..+50 ¢ bar with a smoothed needle
 * (green < 3 ¢, amber < 15 ¢, red beyond). Reads `nativeTunerRead` at 20 fps through [FrameTicker] (only while [active],
 * shown and not [hold]); no signal for 400 ms / confidence < 0.6 fades the needle and shows "—". Zero allocations per frame.
 */
class TunerView(ctx: Context) : View(ctx), FrameTicker.Client {
    /** Tuner enabled in the engine (set by the sheet); resets the smoothing when it changes. */
    var active = false
        set(v) { if (field != v) { field = v; resetState(); invalidate() } }

    /** Freeze the reading. */
    var hold = false
        set(v) { if (field != v) { field = v; invalidate() } }

    override val intervalMs: Int get() = 50
    override val tickView: View? get() = this

    private val d = resources.displayMetrics.density
    private val buf = FloatArray(4)
    private var signal = false
    private var lastGood = 0L
    private var target = 0f
    private var needle = 0f
    private var fade = 0f
    private var noteIdx = 9
    private var octIdx = 4
    private var centsInt = Int.MIN_VALUE
    private var tenthsHz = -1

    private val centsTxt = CharArray(10)
    private var centsLen = 0
    private val freqTxt = CharArray(16)
    private var freqLen = 0

    private val p = Paint(Paint.ANTI_ALIAS_FLAG)
    private val r = RectF()
    private val tri = Path()
    private val tickPts = FloatArray(4 * 11)
    private val noteP = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        typeface = Hw.BOLD; textAlign = Paint.Align.CENTER
        textSize = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, 72f, resources.displayMetrics)
    }
    private val octP = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        typeface = Hw.BOLD; textAlign = Paint.Align.LEFT
        textSize = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, 24f, resources.displayMetrics)
    }
    private val centsP = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        typeface = Hw.MONO; textAlign = Paint.Align.CENTER
        textSize = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, 28f, resources.displayMetrics)
    }
    private val freqP = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        typeface = Hw.MONO; textAlign = Paint.Align.CENTER; color = Hw.ENGRAVE
        textSize = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, 14f, resources.displayMetrics)
    }
    private val smallP = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        typeface = Hw.MONO; textAlign = Paint.Align.CENTER; color = Hw.INK_DIM; textSize = 9f * d
    }

    companion object {
        private val NOTE = arrayOf("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
        private val OCT = Array(12) { (it - 1).toString() }   // octave -1 .. 10
    }

    private fun resetState() {
        signal = false
        target = 0f
        needle = 0f
        fade = 0f
        centsInt = Int.MIN_VALUE
        tenthsHz = -1
    }

    override fun onFrame(nowMs: Long) {
        if (!active || hold) return
        var dirty = false
        val ok = NativeEngine.nativeTunerRead(buf)
        if (ok && buf[3] >= 0.6f && buf[0] > 0f) {
            lastGood = nowMs
            val cents = buf[1].coerceIn(-50f, 50f)
            val n = Math.round(buf[2] - buf[1] / 100f)
            noteIdx = ((n % 12) + 12) % 12
            octIdx = (Math.floorDiv(n, 12)).coerceIn(0, 11)   // MIDI octave + 1 == OCT index (n/12 - 1 + 1)
            target = cents
            if (!signal) { signal = true; dirty = true }
            val ci = cents.roundToInt()
            if (ci != centsInt) {
                centsInt = ci
                var q = 0
                centsTxt[q++] = if (ci < 0) '−' else '+'
                q = Digits.putInt(centsTxt, q, abs(ci))
                centsTxt[q++] = ' '
                centsTxt[q++] = '¢'
                centsLen = q
                dirty = true
            }
            val t = (buf[0] * 10f).roundToInt()
            if (t != tenthsHz) {
                tenthsHz = t
                var q = Digits.putInt(freqTxt, 0, t / 10)
                freqTxt[q++] = '.'
                freqTxt[q++] = '0' + t % 10
                freqTxt[q++] = ' '
                freqTxt[q++] = 'H'
                freqTxt[q++] = 'z'
                freqLen = q
                dirty = true
            }
        } else if (signal && nowMs - lastGood > 400L) {
            signal = false
            target = 0f
            dirty = true
        }
        val dn = (target - needle) * 0.4f
        if (abs(dn) > 0.02f) { needle += dn; dirty = true }
        val tf = if (signal) 1f else 0f
        if (fade != tf) {
            fade = if (tf > fade) min(tf, fade + 0.25f) else Math.max(tf, fade - 0.12f)
            dirty = true
        }
        if (dirty) invalidate()
    }

    override fun onAttachedToWindow() { super.onAttachedToWindow(); FrameTicker.register(this) }
    override fun onDetachedFromWindow() { FrameTicker.unregister(this); super.onDetachedFromWindow() }
    override fun onVisibilityAggregated(isVisible: Boolean) { super.onVisibilityAggregated(isVisible); FrameTicker.poke() }
    override fun onWindowFocusChanged(hasWindowFocus: Boolean) { super.onWindowFocusChanged(hasWindowFocus); FrameTicker.poke() }

    private fun zoneColor(c: Float): Int = if (c < 3f) Hw.MOD_GOOD else if (c < 15f) Hw.MOD_FILTER else Hw.MOD_RED

    override fun onDraw(c: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        if (w <= 0f) return
        // well
        r.set(0f, 0f, w, h)
        p.style = Paint.Style.FILL
        p.color = Hw.WELL
        c.drawRoundRect(r, 8f * d, 8f * d, p)
        p.style = Paint.Style.STROKE
        p.strokeWidth = d
        p.color = Hw.PANEL_EDGE
        r.inset(d / 2f, d / 2f)
        c.drawRoundRect(r, 8f * d, 8f * d, p)
        p.style = Paint.Style.FILL

        val hasSig = signal || fade > 0.05f
        val col = if (!hasSig) Hw.INK_DIM else zoneColor(abs(needle))

        // note + octave
        val nx = w * 0.27f
        val nb = h * 0.56f
        noteP.color = col
        if (hasSig) {
            val name = NOTE[noteIdx]
            c.drawText(name, nx, nb, noteP)
            octP.color = Hw.withAlpha(col, 200)
            c.drawText(OCT[octIdx], nx + noteP.measureText(name) / 2f + 3f * d, nb, octP)
        } else {
            c.drawText("—", nx, nb, noteP)
        }

        // cents + frequency
        val cx = w * 0.72f
        if (hasSig) {
            centsP.color = col
            c.drawText(centsTxt, 0, centsLen, cx, h * 0.36f, centsP)
            c.drawText(freqTxt, 0, freqLen, cx, h * 0.36f + 22f * d, freqP)
        } else {
            freqP.color = Hw.INK_DIM
            c.drawText(L10n.t(if (active) "play a note" else "tuner off"), cx, h * 0.40f, freqP)
            freqP.color = Hw.ENGRAVE
        }
        if (hold) {
            freqP.color = Hw.MOD_FILTER
            c.drawText("HOLD", cx, h * 0.36f + 44f * d, freqP)
            freqP.color = Hw.ENGRAVE
        }

        // cents bar −50 .. +50
        val bl = 18f * d
        val br = w - 18f * d
        val bt = h * 0.70f
        val bb = bt + 24f * d
        val mid = (bl + br) / 2f
        r.set(bl, bt, br, bb)
        p.color = Color.rgb(10, 13, 19)
        c.drawRoundRect(r, 4f * d, 4f * d, p)
        // in-tune zone (±3 ¢)
        val zw = (br - bl) * 3f / 100f
        p.color = Hw.withAlpha(Hw.MOD_GOOD, 46)
        c.drawRect(mid - zw, bt, mid + zw, bb, p)
        // ticks every 10 ¢
        for (i in 0..10) {
            val x = bl + (br - bl) * i / 10f
            val tall = i == 5
            tickPts[i * 4] = x
            tickPts[i * 4 + 1] = if (tall) bt else bt + 6f * d
            tickPts[i * 4 + 2] = x
            tickPts[i * 4 + 3] = if (tall) bb else bb - 6f * d
        }
        p.style = Paint.Style.STROKE
        p.strokeWidth = d
        p.color = Hw.PANEL_HI
        c.drawLines(tickPts, p)
        p.style = Paint.Style.FILL
        // fill from the centre to the needle + needle
        if (fade > 0.02f) {
            val nxp = mid + (br - bl) * needle / 100f
            val ncol = Hw.withAlpha(zoneColor(abs(needle)), (255 * fade).toInt())
            p.color = Hw.withAlpha(zoneColor(abs(needle)), (70 * fade).toInt())
            if (nxp >= mid) c.drawRect(mid, bt + 3f * d, nxp, bb - 3f * d, p) else c.drawRect(nxp, bt + 3f * d, mid, bb - 3f * d, p)
            p.color = ncol
            c.drawRect(nxp - 1.5f * d, bt - 2f * d, nxp + 1.5f * d, bb + 2f * d, p)
            tri.rewind()
            tri.moveTo(nxp, bt - 3f * d)
            tri.lineTo(nxp - 6f * d, bt - 12f * d)
            tri.lineTo(nxp + 6f * d, bt - 12f * d)
            tri.close()
            c.drawPath(tri, p)
        }
        // scale labels
        val ly = bb + 12f * d
        c.drawText("-50", bl, ly, smallP)
        c.drawText("0", mid, ly, smallP)
        c.drawText("+50", br, ly, smallP)
    }
}

// ============================================================================================== small widgets

/** Row of equal-width exclusive buttons. */
@SuppressLint("ViewConstructor")
internal class SegRow(a: MainActivity, labels: Array<String>, accent: Int, private val onPick: (Int) -> Unit) : LinearLayout(a) {
    private val btns = ArrayList<LedButton>()

    var selected = -1
        set(v) {
            field = v
            for (i in btns.indices) { val on = i == v; if (btns[i].lit != on) btns[i].lit = on }
        }

    init {
        orientation = HORIZONTAL
        val gap = (3 * resources.displayMetrics.density).toInt()
        labels.forEachIndexed { i, l ->
            val b = LedButton(a, l, tr = false, led = false, accentColor = accent).apply {
                textSize = 12f
                minHeight = 0
                minimumHeight = 0
                setPadding(0, 0, 0, 0)
                setOnClickListener { onPick(i) }
            }
            btns += b
            addView(b, LayoutParams(0, -1, 1f).apply { if (i < labels.size - 1) marginEnd = gap })
        }
    }
}

/** `[−] value [+]` stepper (88 dp). */
@SuppressLint("ViewConstructor")
private class MiniStepper(a: MainActivity, private val text: () -> String, private val onStep: (Int) -> Unit) : LinearLayout(a) {
    private val lcd = Hw.lcd(a, "", 13f).apply { gravity = Gravity.CENTER; setPadding(0, 0, 0, 0) }

    init {
        orientation = HORIZONTAL
        val dd = resources.displayMetrics.density
        fun btn(label: String, delta: Int) = LedButton(a, label, tr = false, led = false).apply {
            textSize = 14f
            minHeight = 0
            minimumHeight = 0
            setPadding(0, 0, 0, 0)
            setOnClickListener { onStep(delta); refresh() }
        }
        addView(btn("−", -1), LayoutParams((24 * dd).toInt(), -1))
        addView(lcd, LayoutParams(0, -1, 1f).apply { marginStart = (2 * dd).toInt(); marginEnd = (2 * dd).toInt() })
        addView(btn("+", 1), LayoutParams((24 * dd).toInt(), -1))
        refresh()
    }

    fun refresh() { lcd.text = text() }
}

/** Big BPM read-out: vertical drag changes the shared tempo (+1 per 6 dp), double-tap is not used (tap = nothing). */
@SuppressLint("ViewConstructor", "ClickableViewAccessibility")
private class BpmLcd(private val a: MainActivity) : TextView(a) {
    private val d = resources.displayMetrics.density
    private var downY = 0f
    private var downV = 0f
    private var shown = Int.MIN_VALUE
    private val tag = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Hw.BOLD; color = Hw.INK_DIM; textSize = 9.5f * d; letterSpacing = 0.1f }

    init {
        textSize = 40f
        setTextColor(Hw.LCD_FG)
        typeface = Hw.MONO
        background = Hw.lcdBg(a)
        gravity = Gravity.CENTER
        setPadding(0, (6 * d).toInt(), 0, 0)
        maxLines = 1
        refresh()
    }

    fun bpm(): Int = Params.get("fx.tempo").roundToInt()

    fun refresh() {
        val v = bpm()
        if (v != shown) { shown = v; text = v.toString() }
    }

    override fun onDraw(c: Canvas) {
        super.onDraw(c)
        c.drawText("BPM", 8f * d, 14f * d, tag)
    }

    override fun onTouchEvent(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                parent?.requestDisallowInterceptTouchEvent(true)
                downY = e.y
                downV = Params.get("fx.tempo")
                History.checkpoint()
            }
            MotionEvent.ACTION_MOVE -> {
                val steps = ((downY - e.y) / (6f * d)).toInt()
                val v = (downV.roundToInt() + steps).coerceIn(30, 300)
                if (v != bpm()) {
                    Params.set("fx.tempo", v.toFloat())
                    refresh()
                    Hw.onParamTouched?.invoke("BPM", v.toString())
                }
            }
        }
        return true
    }
}

// ============================================================================================== ToolsSheet

/**
 * TOOLS sheet (BOTTOM, 862 × 232): `SubTabBar [METRO | TUNER | PHRASE | SCOPE]` (tab persisted in prefs `toolsTab`) + ✕.
 *
 *  - METRO : BPM (shared `fx.tempo`), −5/−1/+1/+5, 5-tap TAP, START / STOP, beat dots, time signature, subdivision,
 *            count-in, level, route. Settings live in [MetronomeStore].
 *  - TUNER : [InputStatusRow] while the audio input is off, otherwise [TunerView] + A4 reference, MUTE OUT, HOLD.
 *            The engine tuner is enabled only while this tab is showing with the input on; muting never outlives it.
 *  - PHRASE: [PhrasePanelView] (wide). SCOPE: full-size [AudioScopeView].
 *
 * [poll] is called from the 250 ms stats tick while the sheet is open (external changes: footswitch click / tap tempo).
 */
class ToolsSheet(private val a: MainActivity) : Sheet(Edge.BOTTOM, 232) {
    companion object {
        const val TAB_METRO = 0
        const val TAB_TUNER = 1
        const val TAB_PHRASE = 2
        const val TAB_SCOPE = 3
        private const val PREF_TAB = "toolsTab"
        private const val PREF_A4 = "tunerA4"
        private const val PREF_MUTE = "tunerMute"
        private val SUBS = intArrayOf(1, 2, 4, 3)
        private val COUNT_IN = intArrayOf(0, 1, 2, 4)
        private val DENOMS = intArrayOf(2, 4, 8, 16)
    }

    override val view: View by lazy { build() }

    private val d = a.resources.displayMetrics.density
    private fun dp(v: Int) = (v * d).toInt()

    var tab = a.prefs.getInt(PREF_TAB, TAB_METRO).coerceIn(0, 3)
        private set

    private var a4 = a.prefs.getInt(PREF_A4, 440).coerceIn(415, 466)
    private var muteOut = a.prefs.getBoolean(PREF_MUTE, false)
    private var tunerOn = false
    private var running = false

    private lateinit var tabs: SubTabBar
    private val pages = arrayOfNulls<View>(4)
    private lateinit var bpmLcd: BpmLcd
    private lateinit var startBtn: LedButton
    private lateinit var beatDots: BeatDotsView
    private lateinit var stepNum: MiniStepper
    private lateinit var stepDen: MiniStepper
    private lateinit var subRow: SegRow
    private lateinit var countRow: SegRow
    private lateinit var routeRow: SegRow
    private lateinit var level: HSegBar
    private lateinit var tunerView: TunerView
    private lateinit var tunerBody: View
    private lateinit var inputBox: View
    private lateinit var muteSw: ToggleSwitch
    private lateinit var holdSw: ToggleSwitch
    private lateinit var a4Num: DragNumber
    private val tapTimes = LongArray(5)
    private var tapCount = 0
    private val inputListener: () -> Unit = { syncTunerMode() }

    /** Tuner open (for the telemetry chip LED). */
    val tunerOpen: Boolean get() = isShowing && tab == TAB_TUNER && tunerOn

    /** Open the sheet on [t] (any tab) — used by the chip / footswitch actions. */
    fun open(t: Int = tab) {
        showTab(t, user = false)
        a.openSheet(this)
    }

    override fun onShow() {
        view
        refreshMetro()
        showTab(tab, user = false)
        syncRunning(MetronomeStore.running)
        FrameTicker.poke()
    }

    override fun onHide() {
        updateTuner()   // isShowing is already false here -> tuner off, mute released
        beatDots.setRunning(false)
    }

    /** Called when the host (screen / standby) changes: the tuner must not keep running with the screen off. */
    fun syncHost() { if (::tunerView.isInitialized) updateTuner() }

    /** Stats tick (≈ 4 Hz) while the sheet is open. */
    fun poll() {
        if (!::bpmLcd.isInitialized) return
        val run = MetronomeStore.running
        if (run != running) syncRunning(run)
        if (tab == TAB_METRO) bpmLcd.refresh()
        tabs.setBadge(TAB_METRO, if (run) Hw.MOD_OSC else 0)
        val st = NativeEngine.nativePhraseStatus()
        val rec = st.isNotEmpty() && (st[0].toInt() == 1 || st[0].toInt() == 3)
        tabs.setBadge(TAB_PHRASE, if (rec) Hw.MOD_RED else 0)
    }

    // ------------------------------------------------------------------ tabs

    private fun showTab(t: Int, user: Boolean) {
        tab = t.coerceIn(0, 3)
        if (user) a.prefs.edit().putInt(PREF_TAB, tab).apply()
        if (!::tabs.isInitialized) return
        tabs.selected = tab
        for (i in 0 until 4) pages[i]?.visibility = if (i == tab) View.VISIBLE else View.GONE
        syncTunerMode()
        FrameTicker.poke()
    }

    // ------------------------------------------------------------------ metronome

    private fun syncRunning(run: Boolean) {
        running = run
        startBtn.text = if (run) "STOP" else "START"
        startBtn.lit = run
        beatDots.setRunning(run && isShowing)
    }

    private fun refreshMetro() {
        val m = MetronomeStore
        bpmLcd.refresh()
        beatDots.setBeats(m.numerator)
        stepNum.refresh()
        stepDen.refresh()
        subRow.selected = SUBS.indexOf(m.subdivision)
        countRow.selected = COUNT_IN.indexOf(m.countIn)
        routeRow.selected = m.route
        level.value = (m.levelDb + 60f) / 60f
        level.valueText = m.levelDb.roundToInt().toString() + " dB"
    }

    private fun setTempo(v: Int) {
        History.checkpoint()
        Params.set("fx.tempo", v.coerceIn(30, 300).toFloat())
        bpmLcd.refresh()
    }

    private fun tap() {
        val now = SystemClock.elapsedRealtimeNanos()
        if (tapCount > 0 && now - tapTimes[tapCount - 1] > 2_000_000_000L) tapCount = 0
        if (tapCount == tapTimes.size) {
            for (i in 1 until tapTimes.size) tapTimes[i - 1] = tapTimes[i]
            tapCount--
        }
        tapTimes[tapCount++] = now
        if (tapCount >= 2) {
            val intervals = min(4, tapCount - 1)
            val avg = (tapTimes[tapCount - 1] - tapTimes[tapCount - 1 - intervals]).toDouble() / intervals
            if (avg > 0.0) {
                val bpm = (60e9 / avg).toFloat().coerceIn(30f, 300f)
                Params.set("fx.tempo", bpm)
                bpmLcd.refresh()
            }
        }
    }

    // ------------------------------------------------------------------ tuner

    private fun syncTunerMode() {
        if (!::tunerView.isInitialized) return
        val inputOn = NativeEngine.nativeInputEnabled()
        inputBox.visibility = if (inputOn) View.GONE else View.VISIBLE
        tunerBody.visibility = if (inputOn) View.VISIBLE else View.GONE
        updateTuner()
    }

    private fun updateTuner() {
        if (!::tunerView.isInitialized) return
        val want = isShowing && tab == TAB_TUNER && FrameTicker.hostActive && NativeEngine.nativeInputEnabled()
        if (want != tunerOn) {
            tunerOn = want
            NativeEngine.nativeTunerEnable(want)
        }
        NativeEngine.nativeTunerConfig(a4.toFloat(), muteOut && want)   // the mute never outlives the tuner
        tunerView.active = want
        FrameTicker.poke()
    }

    // ------------------------------------------------------------------ UI

    private fun lbl(t: String) = Hw.engraved(a, t, 9.5f, tr = false)

    private fun key(label: String, accent: Int = Hw.MOD_OSC, led: Boolean = false, size: Float = 13f, onClick: () -> Unit) =
        LedButton(a, label, tr = false, led = led, accentColor = accent).apply {
            textSize = size
            minHeight = 0
            minimumHeight = 0
            setPadding(if (led) dp(15) else 0, 0, 0, 0)
            setOnClickListener { onClick() }
        }

    private fun build(): View {
        val root = LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(8), dp(2), dp(8), dp(4))
        }

        // ---- header: sub-tabs + close
        tabs = SubTabBar(a, arrayOf("METRO", "TUNER", "PHRASE", "SCOPE"), accent = Hw.MOD_OSC).apply {
            selected = tab
            onSelect = { showTab(it, user = true) }
        }
        val header = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        header.addView(tabs, LinearLayout.LayoutParams(dp(330), dp(26)))
        header.addView(View(a), LinearLayout.LayoutParams(0, 1, 1f))
        header.addView(ChipButton(a, "✕", widthDp = 30f).apply { setOnClickListener { dismiss() } }, LinearLayout.LayoutParams(dp(30), dp(22)))
        root.addView(header, LinearLayout.LayoutParams(-1, dp(28)))

        val body = FrameLayout(a)
        pages[TAB_METRO] = buildMetro()
        pages[TAB_TUNER] = buildTuner()
        pages[TAB_PHRASE] = buildPhrase()
        pages[TAB_SCOPE] = buildScope()
        for (p in pages) body.addView(p, FrameLayout.LayoutParams(-1, -1))
        root.addView(body, LinearLayout.LayoutParams(-1, 0, 1f).apply { topMargin = dp(4) })

        a.addInputListener(inputListener)
        for (i in 0 until 4) pages[i]?.visibility = if (i == tab) View.VISIBLE else View.GONE
        return root
    }

    private fun buildMetro(): View {
        val m = MetronomeStore
        val col = LinearLayout(a).apply { orientation = LinearLayout.VERTICAL }

        // row 1 (72): BPM · −5 −1 +1 +5 · TAP · START/STOP · beat dots
        val row1 = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL }
        bpmLcd = BpmLcd(a)
        row1.addView(bpmLcd, LinearLayout.LayoutParams(dp(130), -1).apply { marginEnd = dp(6) })
        for (delta in intArrayOf(-5, -1, 1, 5)) {
            val label = if (delta > 0) "+$delta" else "−${-delta}"
            row1.addView(key(label, size = 14f) { setTempo(bpmLcd.bpm() + delta) },
                LinearLayout.LayoutParams(dp(46), dp(44)).apply { marginEnd = dp(4); gravity = Gravity.CENTER_VERTICAL })
        }
        row1.addView(key("TAP", Hw.MOD_FILTER, size = 20f) { tap() }, LinearLayout.LayoutParams(dp(110), -1).apply { marginStart = dp(2); marginEnd = dp(6) })
        startBtn = key("START", Hw.MOD_FX, led = true, size = 18f) {
            val on = !MetronomeStore.running
            MetronomeStore.setEnabled(on)
            syncRunning(on)
        }
        row1.addView(startBtn, LinearLayout.LayoutParams(dp(110), -1).apply { marginEnd = dp(6) })
        beatDots = BeatDotsView(a)
        row1.addView(beatDots, LinearLayout.LayoutParams(0, -1, 1f))
        col.addView(row1, LinearLayout.LayoutParams(-1, dp(72)))

        // row 2 (36): TIME · SUB · COUNT-IN · LEVEL · ROUTE
        val row2 = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        fun group(label: String, v: View, w: Int, last: Boolean = false) {
            row2.addView(lbl(label), LinearLayout.LayoutParams(-2, -2).apply { marginEnd = dp(4) })
            row2.addView(v, if (w > 0) LinearLayout.LayoutParams(dp(w), -1).apply { if (!last) marginEnd = dp(10) }
            else LinearLayout.LayoutParams(0, -1, 1f).apply { if (!last) marginEnd = dp(10) })
        }
        stepNum = MiniStepper(a, { m.numerator.toString() }) { dlt ->
            m.numerator = (m.numerator + dlt).coerceIn(1, 16); beatDots.setBeats(m.numerator); m.push()
        }
        stepDen = MiniStepper(a, { m.denominator.toString() }) { dlt ->
            val i = (DENOMS.indexOf(m.denominator).coerceAtLeast(0) + dlt).coerceIn(0, DENOMS.size - 1)
            m.denominator = DENOMS[i]; m.push()
        }
        val sig = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        sig.addView(stepNum, LinearLayout.LayoutParams(dp(88), -1))
        sig.addView(Hw.engraved(a, "/", 12f, tr = false), LinearLayout.LayoutParams(-2, -2).apply { marginStart = dp(3); marginEnd = dp(3) })
        sig.addView(stepDen, LinearLayout.LayoutParams(dp(88), -1))
        group("TIME", sig, 188)
        subRow = SegRow(a, arrayOf("♩", "♪", "♬", "3"), Hw.MOD_OSC) { i -> m.subdivision = SUBS[i]; subRow.selected = i; m.push() }
        group("SUB", subRow, 4 * 40)
        countRow = SegRow(a, arrayOf("OFF", "1", "2", "4"), Hw.MOD_OSC) { i -> m.countIn = COUNT_IN[i]; countRow.selected = i; m.push() }
        group("COUNT-IN", countRow, 4 * 42)
        level = HSegBar(a, Hw.MOD_OSC, 12, 44f).apply {
            onChange = { v ->
                m.levelDb = (-60f + v * 60f).roundToInt().toFloat()
                valueText = m.levelDb.roundToInt().toString() + " dB"
                m.push()
            }
            onReset = { m.levelDb = -18f; value = (m.levelDb + 60f) / 60f; valueText = "-18 dB"; m.push() }
        }
        group("LEVEL", level, 0)
        routeRow = SegRow(a, arrayOf("MAIN", "L", "R"), Hw.MOD_OSC) { i -> m.route = i; routeRow.selected = i; m.push() }
        group("ROUTE", routeRow, 3 * 46, last = true)
        col.addView(row2, LinearLayout.LayoutParams(-1, dp(36)).apply { topMargin = dp(10) })

        // row 3 (24): info
        col.addView(Ui.text(a, "Click follows the tempo of arp / delay / phrase · count-in applies to Phrase REC", 11.5f, Ui.DIM),
            LinearLayout.LayoutParams(-1, dp(24)).apply { topMargin = dp(8) })
        return col
    }

    private fun buildTuner(): View {
        val frame = FrameLayout(a)

        // input off: one big "Enable Audio Input" row
        val box = LinearLayout(a).apply { orientation = LinearLayout.VERTICAL; gravity = Gravity.CENTER }
        box.addView(Ui.text(a, "The tuner listens to the Audio Input (mic / USB).", 13f, Ui.DIM),
            LinearLayout.LayoutParams(-2, -2).apply { bottomMargin = dp(12) })
        box.addView(InputStatusRow(a), LinearLayout.LayoutParams(dp(560), dp(44)))
        inputBox = box
        frame.addView(box, FrameLayout.LayoutParams(-1, -1))

        // input on: tuner + controls
        val row = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL }
        tunerView = TunerView(a)
        row.addView(tunerView, LinearLayout.LayoutParams(0, -1, 1f).apply { marginEnd = dp(8) })
        val side = LinearLayout(a).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(8), dp(2), dp(4), 0) }
        side.addView(lbl("A4 REF"), LinearLayout.LayoutParams(-2, -2))
        a4Num = DragNumber(a, 415f, 466f, 1f, 440f, { "${it.roundToInt()} Hz" }, { a4.toFloat() }) { v ->
            a4 = v.roundToInt().coerceIn(415, 466)
            a.prefs.edit().putInt(PREF_A4, a4).apply()
            updateTuner()
        }
        side.addView(a4Num, LinearLayout.LayoutParams(-1, dp(40)).apply { topMargin = dp(2) })
        muteSw = ToggleSwitch(a, "MUTE OUT", vertical = false, accent = Hw.MOD_FILTER).apply {
            on = muteOut
            onToggle = { v -> muteOut = v; a.prefs.edit().putBoolean(PREF_MUTE, v).apply(); updateTuner() }
        }
        side.addView(muteSw, LinearLayout.LayoutParams(-1, dp(36)).apply { topMargin = dp(8) })
        holdSw = ToggleSwitch(a, "HOLD", vertical = false, accent = Hw.MOD_FILTER).apply {
            onToggle = { v -> tunerView.hold = v }
        }
        side.addView(holdSw, LinearLayout.LayoutParams(-1, dp(36)).apply { topMargin = dp(2) })
        row.addView(side, LinearLayout.LayoutParams(dp(262), -1))
        tunerBody = row
        frame.addView(row, FrameLayout.LayoutParams(-1, -1))
        return frame
    }

    private fun buildPhrase(): View {
        val col = LinearLayout(a).apply { orientation = LinearLayout.VERTICAL; setPadding(0, dp(8), 0, 0) }
        col.addView(PhrasePanelView(a, wide = true), LinearLayout.LayoutParams(-1, -2))
        col.addView(Ui.text(a, "REC / DUB, quantize and export work from any screen — handy with a footswitch.", 11.5f, Ui.DIM),
            LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(10) })
        return col
    }

    private fun buildScope(): View {
        val frame = FrameLayout(a)
        frame.addView(AudioScopeView(a, 186f), FrameLayout.LayoutParams(-1, dp(186)))
        return frame
    }
}
