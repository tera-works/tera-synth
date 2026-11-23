package com.hsynth.app

import android.annotation.SuppressLint
import android.app.AlertDialog
import android.content.Context
import android.content.Intent
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.net.Uri
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.util.concurrent.CancellationException
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

/*
 * B1 — SONG > PADS: 4 × 4 RGB velocity-sensitive one-shot sampler.
 *
 *  - [PadGridView]  one custom View (not 16 children), multi-touch, zero-alloc onDraw. Touch-down calls
 *                   NativeEngine.nativePadTrigger *directly on the UI thread* (no handler hop = lowest latency).
 *  - [PadsPage]     header (velocity mode, STOP ALL) + selected-pad editor; persists to files/pads/pads.json and
 *                   re-loads every pad's sample on startup.
 *
 * Pad numbering is 0-based in the engine (MIDI notes 36..51 trigger pads 0..15, bottom row = pads 1-4 like a hardware pad bank).
 */

/** How the touch is turned into a velocity. */
enum class PadVelocity { AREA, POSITION, FIXED }

@SuppressLint("ViewConstructor")
class PadGridView(ctx: Context) : View(ctx), FrameTicker.Client {
    companion object {
        const val PADS = 16
        private const val GLOW_MS = 300f
        private val NUMBERS = Array(PADS) { "${it + 1}" }
    }

    var velocityMode = PadVelocity.POSITION
    var selected = 0
        set(v) { if (field != v) { field = v; invalidate() } }
    var onSelect: ((Int) -> Unit)? = null

    val colors = IntArray(PADS) { Hw.MOD_OSC }
    val hasSample = BooleanArray(PADS)
    private val names = Array(PADS) { "" }
    private val shown = Array(PADS) { "" }

    private val d = resources.displayMetrics.density
    private val lit = FloatArray(PADS)
    private val engine = FloatArray(PADS)
    private val pl = FloatArray(PADS)
    private val pt = FloatArray(PADS)
    private val pr = FloatArray(PADS)
    private val pb = FloatArray(PADS)
    private val pointerPad = IntArray(32) { -1 }
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG)
    private val txtNum = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Hw.MONO; textSize = 11f * d }
    private val txtName = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Hw.BOLD; textSize = 11.5f * d; textAlign = Paint.Align.CENTER }
    private val r = RectF()

    // adaptive touch-size range for AREA velocity
    private var sMin = 0.1f
    private var sMax = 0.3f

    private var registered = false
    private var lastMs = 0L

    override val intervalMs: Int get() = 16
    override val tickView: View? get() = this

    init { isClickable = true }

    fun setPad(i: Int, name: String, color: Int, has: Boolean) {
        names[i] = name
        colors[i] = color
        hasSample[i] = has
        fit(i)
        invalidate()
    }

    private fun fit(i: Int) {
        val w = pr[i] - pl[i]
        var s = names[i]
        if (w > 0f && s.isNotEmpty() && txtName.measureText(s) > w - 8f * d) {
            while (s.length > 1 && txtName.measureText("$s…") > w - 8f * d) s = s.dropLast(1)
            s = "$s…"
        }
        shown[i] = s
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        val gap = 6f * d
        val pw = (w - gap * 3) / 4f
        val ph = (h - gap * 3) / 4f
        for (i in 0 until PADS) {
            val col = i % 4
            val row = 3 - i / 4              // pads 1-4 on the bottom row
            pl[i] = col * (pw + gap)
            pt[i] = row * (ph + gap)
            pr[i] = pl[i] + pw
            pb[i] = pt[i] + ph
            fit(i)
        }
    }

    private fun hit(x: Float, y: Float): Int {
        for (i in 0 until PADS) if (x >= pl[i] && x < pr[i] && y >= pt[i] && y < pb[i]) return i
        return -1
    }

    private fun velocityFor(e: MotionEvent, idx: Int, pad: Int): Float = when (velocityMode) {
        PadVelocity.FIXED -> 100f / 127f
        PadVelocity.POSITION -> {
            val rel = ((e.getY(idx) - pt[pad]) / (pb[pad] - pt[pad]).coerceAtLeast(1f)).coerceIn(0f, 1f)
            0.25f + 0.75f * rel                          // bottom of the pad = hard
        }
        PadVelocity.AREA -> {
            val s = e.getSize(idx)
            if (s < sMin) sMin = s else sMin += (s - sMin) * 0.01f
            if (s > sMax) sMax = s else sMax -= (sMax - s) * 0.01f
            if (sMax - sMin < 0.02f) 0.8f else (0.25f + 0.75f * ((s - sMin) / (sMax - sMin))).coerceIn(0.2f, 1f)
        }
    }

    private fun down(e: MotionEvent, idx: Int) {
        val pid = e.getPointerId(idx)
        if (pid !in pointerPad.indices) return
        val pad = hit(e.getX(idx), e.getY(idx))
        if (pad < 0) return
        pointerPad[pid] = pad
        NativeEngine.nativePadTrigger(pad, velocityFor(e, idx, pad))     // UI thread, no hop
        lit[pad] = 1f
        if (selected != pad) { selected = pad; onSelect?.invoke(pad) }
        wake()
        invalidate()
    }

    private fun up(pid: Int) {
        if (pid !in pointerPad.indices) return
        val pad = pointerPad[pid]
        if (pad < 0) return
        pointerPad[pid] = -1
        NativeEngine.nativePadRelease(pad)
        wake()
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                parent?.requestDisallowInterceptTouchEvent(true)
                down(e, 0)
            }
            MotionEvent.ACTION_POINTER_DOWN -> down(e, e.actionIndex)
            MotionEvent.ACTION_POINTER_UP -> up(e.getPointerId(e.actionIndex))
            MotionEvent.ACTION_UP -> up(e.getPointerId(0))
            MotionEvent.ACTION_CANCEL -> for (pid in pointerPad.indices) up(pid)
        }
        return true
    }

    /** Release every held pad (page hidden, STOP ALL, …). */
    fun releaseAll() {
        for (pid in pointerPad.indices) up(pid)
    }

    // ------------------------------------------------------------------ glow decay: 60 fps, only while something is lit

    private fun wake() {
        if (!registered) registered = FrameTicker.register(this)
    }

    override fun onFrame(nowMs: Long) {
        val dt = if (lastMs == 0L) 16f else (nowMs - lastMs).toFloat().coerceIn(1f, 100f)
        lastMs = nowMs
        NativeEngine.nativePadLevels(engine)
        var any = false
        for (i in 0 until PADS) {
            if (lit[i] > 0f) lit[i] = max(0f, lit[i] - dt / GLOW_MS)
            if (lit[i] > 0f || engine[i] > 0.01f) any = true
        }
        invalidate()
        if (!any) {
            FrameTicker.unregister(this)
            registered = false
            lastMs = 0L
        }
    }

    override fun onDetachedFromWindow() {
        releaseAll()
        if (registered) { FrameTicker.unregister(this); registered = false; lastMs = 0L }
        super.onDetachedFromWindow()
    }

    override fun onVisibilityAggregated(isVisible: Boolean) {
        super.onVisibilityAggregated(isVisible)
        if (!isVisible) releaseAll()
        FrameTicker.poke()
    }

    // ------------------------------------------------------------------ draw

    override fun onDraw(c: Canvas) {
        val rad = 8f * d
        for (i in 0 until PADS) {
            val col = colors[i]
            val g = max(lit[i], engine[i]).coerceIn(0f, 1f)
            r.set(pl[i], pt[i], pr[i], pb[i])
            fill.style = Paint.Style.FILL
            fill.color = Hw.withAlpha(col, if (g > 0.01f) (15 + g * 200).toInt() else 15)
            c.drawRoundRect(r, rad, rad, fill)
            fill.style = Paint.Style.STROKE
            fill.strokeWidth = if (g > 0.01f) 2f * d else d
            fill.color = Hw.withAlpha(col, if (g > 0.01f) (89 + g * 166).toInt() else 89)
            r.inset(d / 2f, d / 2f)
            c.drawRoundRect(r, rad, rad, fill)
            if (i == selected) {
                fill.strokeWidth = 1.5f * d
                fill.color = Hw.SILK
                r.inset(2f * d, 2f * d)
                c.drawRoundRect(r, rad - 2f * d, rad - 2f * d, fill)
            }
            val lightText = g > 0.45f
            txtNum.color = if (lightText) Color_BLACK2 else if (hasSample[i]) Hw.ENGRAVE else Hw.INK_DIM
            c.drawText(NUMBERS[i], pl[i] + 7f * d, pt[i] + 15f * d, txtNum)
            val s = shown[i]
            if (s.isNotEmpty()) {
                txtName.color = if (lightText) Color_BLACK2 else Hw.SILK
                c.drawText(s, (pl[i] + pr[i]) / 2f, pb[i] - 9f * d, txtName)
            }
        }
    }
}

private const val Color_BLACK2 = 0xFF000000.toInt()

/** Eight colour dots (one View, zero-alloc): tap = pick. */
@SuppressLint("ViewConstructor")
private class SwatchRow(ctx: Context, private val colors: IntArray) : View(ctx) {
    var selected = 0
        set(v) { if (field != v) { field = v; invalidate() } }
    var onPick: ((Int) -> Unit)? = null
    private val d = resources.displayMetrics.density
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)

    init { isClickable = true }

    override fun onMeasure(widthSpec: Int, heightSpec: Int) {
        setMeasuredDimension(resolveSize((colors.size * 24 * d).toInt(), widthSpec), resolveSize((28 * d).toInt(), heightSpec))
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        if (e.actionMasked == MotionEvent.ACTION_UP) {
            val cell = width / colors.size.toFloat()
            val i = (e.x / cell).toInt().coerceIn(0, colors.size - 1)
            selected = i
            onPick?.invoke(i)
            performClick()
        }
        return true
    }

    override fun performClick(): Boolean = super.performClick()

    override fun onDraw(c: Canvas) {
        val cell = width / colors.size.toFloat()
        val cy = height / 2f
        for (i in colors.indices) {
            val cx = cell * (i + 0.5f)
            p.style = Paint.Style.FILL
            p.color = colors[i]
            c.drawCircle(cx, cy, 8.5f * d, p)
            if (i == selected) {
                p.style = Paint.Style.STROKE
                p.strokeWidth = 1.6f * d
                p.color = Hw.SILK
                c.drawCircle(cx, cy, 11f * d, p)
            }
        }
    }
}

// ================================================================================================== PADS page

class PadsPage(private val a: MainActivity) {
    companion object {
        const val PADS = 16
        const val FILE_NAME = "pads.json"
        val COLORS = intArrayOf(
            Hw.MOD_OSC, Hw.MOD_FX, Hw.MOD_FILTER, Hw.MOD_ENV,
            Hw.MOD_RED, android.graphics.Color.rgb(236, 72, 153), android.graphics.Color.rgb(59, 130, 246),
            android.graphics.Color.rgb(203, 213, 225),
        )
        private val ROW_HUES = intArrayOf(Hw.MOD_OSC, Hw.MOD_FX, Hw.MOD_FILTER, Hw.MOD_ENV)
        private const val PREF_VEL = "padVelMode"
    }

    class PadCfg(i: Int) {
        var name = ""
        var color = ROW_HUES[(i / 4) % ROW_HUES.size]
        var level = 1f
        var pitch = 0f
        var mode = 0          // 0 one-shot, 1 loop, 2 gate
        var choke = 0         // 0 = none, 1..8
        var vel = 1           // 0 off, 1 normal, 2 hard
        var start = 0f
        var end = 1f
        var has = false
        var info = ""         // "1.52 s · 44.1 kHz"
    }

    val view: View
    val cfg = Array(PADS) { PadCfg(it) }
    private val dir = File(a.filesDir, "pads").apply { mkdirs() }
    private val d = { v: Int -> Ui.dp(a, v) }
    private val grid = PadGridView(a)
    private lateinit var nameTv: TextView
    private val swatches = SwatchRow(a, COLORS)
    private val wave = WaveSeekView(a, tick = false, accent = Hw.MOD_OSC)
    private val infoTv = Ui.text(a, "", 10.5f, Hw.ENGRAVE, tr = false).apply { typeface = Hw.MONO; maxLines = 1 }
    private val levelKnob = KnobView(a, "LEVEL", bipolar = false, sizeDp = 36f, accent = Hw.MOD_OSC)
    private val pitchKnob = KnobView(a, "PITCH", bipolar = true, sizeDp = 36f, accent = Hw.MOD_OSC)
    private lateinit var modeRow: SegRow
    private lateinit var velRow: SegRow
    private lateinit var chokeNum: DragNumber
    private var selected = 0
    private var loadPad = 0
    private var decodeCancel: AudioCancel? = null
    private val saveRun = Runnable { writeNow() }
    private var velSegRef: SegRow? = null
    private fun velSegSelect(i: Int) { velSegRef?.selected = i }

    init {
        readJson()

        // ---- header
        val velSeg = SegRow(a, arrayOf("AREA", "POSITION", "FIXED"), Hw.MOD_FX) { i ->
            val m = PadVelocity.entries[i]
            grid.velocityMode = m
            a.prefs.edit().putInt(PREF_VEL, i).apply()
            velSegSelect(i)
        }
        val velIdx = a.prefs.getInt(PREF_VEL, PadVelocity.POSITION.ordinal).coerceIn(0, 2)
        grid.velocityMode = PadVelocity.entries[velIdx]
        velSeg.selected = velIdx
        velSegRef = velSeg
        val stopAll = LedButton(a, "STOP ALL", tr = false, led = false, accentColor = Hw.MOD_RED).apply {
            textSize = 11.5f; minHeight = 0; minimumHeight = 0; setPadding(0, 0, 0, 0)
            setOnClickListener { grid.releaseAll(); NativeEngine.nativePadStopAll() }
        }
        val header = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(Hw.engraved(a, "PADS", 12f, tr = false, color = Hw.MOD_FX), LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(12) })
            addView(Hw.engraved(a, "VEL", 10f, tr = false), LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(6) })
            addView(velSeg, LinearLayout.LayoutParams(d(210), d(26)))
            addView(stopAll, LinearLayout.LayoutParams(d(84), d(26)).apply { marginStart = d(10) })
            addView(View(a), LinearLayout.LayoutParams(0, 1, 1f))
            addView(Hw.engraved(a, "MIDI IN base note 36 (ch 10)", 9.5f, tr = false, color = Hw.INK_DIM))
        }

        // ---- editor
        nameTv = Hw.lcd(a, "", 13f).apply {
            gravity = Gravity.CENTER_VERTICAL
            isClickable = true
            setOnClickListener { rename() }
        }
        swatches.onPick = { i -> cfg[selected].color = COLORS[i]; pushGridPad(selected); saveSoon() }
        val nameRow = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(nameTv, LinearLayout.LayoutParams(0, d(32), 1f).apply { marginEnd = d(6) })
            addView(swatches, LinearLayout.LayoutParams(d(196), d(32)))
        }

        wave.secPerUnit = 0.0
        wave.span = 1.0
        wave.showPlayhead = false
        wave.dimOutside = true
        wave.hLabel[0] = "S"; wave.hLabel[1] = "E"
        wave.hColor[0] = Hw.MOD_GOOD; wave.hColor[1] = Hw.MOD_RED
        wave.pairColor[0] = Hw.MOD_GOOD
        wave.onHandle = { i, v, final ->
            val c = cfg[selected]
            if (i == 0) c.start = v.toFloat() else c.end = v.toFloat()
            pushSetup(selected)
            if (final) saveSoon()
        }

        fun action(label: String, accent: Int, onClick: () -> Unit) = LedButton(a, label, tr = false, led = false, accentColor = accent).apply {
            textSize = 12.5f; minHeight = 0; minimumHeight = 0; setPadding(0, 0, 0, 0)
            setOnClickListener { onClick() }
        }
        val actions = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(action("LOAD…", Hw.MOD_OSC) { pickFile() }, LinearLayout.LayoutParams(0, d(34), 1f).apply { marginEnd = d(5) })
            addView(action("REC…", Hw.MOD_RED) { a.openSampleWizard(defaultPad = selected) }, LinearLayout.LayoutParams(0, d(34), 1f).apply { marginEnd = d(5) })
            addView(action("CLEAR", Hw.MOD_FILTER) { clearPad(selected) }, LinearLayout.LayoutParams(0, d(34), 1f))
        }

        levelKnob.onChange = { v -> cfg[selected].level = v * 1.5f; levelKnob.valueText = "${(cfg[selected].level * 100).roundToInt()}%"; pushSetup(selected); saveSoon() }
        levelKnob.onReset = { levelKnob.value = 1f / 1.5f; levelKnob.onChange?.invoke(levelKnob.value) }
        pitchKnob.onChange = { v -> cfg[selected].pitch = ((v - 0.5f) * 24f).roundToInt().toFloat(); pitchKnob.valueText = pitchText(cfg[selected].pitch); pushSetup(selected); saveSoon() }
        pitchKnob.onReset = { pitchKnob.value = 0.5f; pitchKnob.onChange?.invoke(0.5f) }
        modeRow = SegRow(a, arrayOf("ONE-SHOT", "LOOP", "GATE"), Hw.MOD_OSC) { i -> cfg[selected].mode = i; modeRow.selected = i; pushSetup(selected); saveSoon() }
        velRow = SegRow(a, arrayOf("OFF", "NORM", "HARD"), Hw.MOD_OSC) { i -> cfg[selected].vel = i; velRow.selected = i; pushSetup(selected); saveSoon() }
        chokeNum = DragNumber(a, 0f, 8f, 1f, 0f, { if (it < 0.5f) "—" else "${it.roundToInt()}" }, { cfg[selected].choke.toFloat() }) { v ->
            cfg[selected].choke = v.roundToInt(); pushSetup(selected); saveSoon()
        }.apply { textSize = 12f; setPadding(0, 0, 0, 0) }

        fun labelled(l: String, v: View, w: Int) = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(Hw.engraved(a, l, 9.5f, tr = false), LinearLayout.LayoutParams(d(46), -2))
            addView(v, if (w > 0) LinearLayout.LayoutParams(d(w), d(26)) else LinearLayout.LayoutParams(0, d(26), 1f))
        }
        val right3 = LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            addView(labelled("MODE", modeRow, 0), LinearLayout.LayoutParams(-1, d(26)))
            addView(labelled("CHOKE", chokeNum, 70), LinearLayout.LayoutParams(-1, d(26)).apply { topMargin = d(4) })
            addView(labelled("VEL", velRow, 0), LinearLayout.LayoutParams(-1, d(26)).apply { topMargin = d(4) })
        }
        val knobRow = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(levelKnob, LinearLayout.LayoutParams(-2, -2))
            addView(pitchKnob, LinearLayout.LayoutParams(-2, -2))
            addView(right3, LinearLayout.LayoutParams(0, -2, 1f).apply { marginStart = d(6) })
        }

        val editor = LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            addView(nameRow, LinearLayout.LayoutParams(-1, d(32)))
            addView(wave, LinearLayout.LayoutParams(-1, d(76)).apply { topMargin = d(6) })
            addView(infoTv, LinearLayout.LayoutParams(-1, d(16)).apply { topMargin = d(2) })
            addView(actions, LinearLayout.LayoutParams(-1, d(34)).apply { topMargin = d(4) })
            addView(knobRow, LinearLayout.LayoutParams(-1, -2).apply { topMargin = d(4) })
        }

        grid.onSelect = { p -> select(p) }
        val body = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(grid, LinearLayout.LayoutParams(0, -1, 1f).apply { marginEnd = d(10) })
            addView(ScrollView(a).apply { addView(editor, FrameLayout.LayoutParams(-1, -2)) }, LinearLayout.LayoutParams(0, -1, 1f))
        }
        view = LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            addView(header, LinearLayout.LayoutParams(-1, d(28)))
            addView(body, LinearLayout.LayoutParams(-1, 0, 1f).apply { topMargin = d(6) })
        }

        for (i in 0 until PADS) pushGridPad(i)
        select(selected.coerceIn(0, PADS - 1))
        loadAllSamples()
    }

    /** `MainActivity.buildUi` clears the request router; call again after every rebuild. */
    fun registerResults() {
        ActivityResults.register(ActivityResults.REQ_PAD_FILE) { data -> onPicked(data) }
    }

    fun releaseAll() = grid.releaseAll()

    // ------------------------------------------------------------------ selection / editor

    private fun pitchText(st: Float) = (if (st > 0.4f) "+" else "") + "${st.roundToInt()}st"

    private fun select(i: Int) {
        selected = i.coerceIn(0, PADS - 1)
        grid.selected = selected
        val c = cfg[selected]
        nameTv.text = "${selected + 1}  " + c.name.ifEmpty { L10n.t("(unnamed)") }
        swatches.selected = COLORS.indexOf(c.color).coerceAtLeast(0)
        levelKnob.value = c.level / 1.5f
        levelKnob.valueText = "${(c.level * 100).roundToInt()}%"
        pitchKnob.value = c.pitch / 24f + 0.5f
        pitchKnob.valueText = pitchText(c.pitch)
        modeRow.selected = c.mode
        velRow.selected = c.vel
        chokeNum.refresh()
        wave.hv[0] = c.start.toDouble()
        wave.hv[1] = c.end.toDouble()
        wave.setPeaks(null)
        infoTv.text = if (c.has) c.info else L10n.t("empty — LOAD… or REC…")
        if (c.has) loadPeaks(selected)
        wave.invalidate()
    }

    private fun loadPeaks(pad: Int) {
        a.loader.execute {
            val p = NativeEngine.nativePadPeaks(pad, 512)
            a.ui.post { if (pad == selected) wave.setPeaks(p) }
        }
    }

    private fun pushGridPad(i: Int) {
        val c = cfg[i]
        grid.setPad(i, c.name, c.color, c.has)
        if (i == selected) swatches.selected = COLORS.indexOf(c.color).coerceAtLeast(0)
    }

    private fun pushSetup(i: Int) {
        val c = cfg[i]
        NativeEngine.nativePadSetup(i, c.level, c.pitch, c.mode, c.choke, c.vel, c.start, c.end)
    }

    private fun rename() {
        val input = EditText(a).apply { setText(cfg[selected].name); setSelectAllOnFocus(true); setSingleLine() }
        AlertDialog.Builder(a).setTitle(L10n.f("Pad %d name", selected + 1)).setView(input)
            .setPositiveButton(L10n.t("Save")) { _, _ ->
                cfg[selected].name = input.text.toString().trim().take(24)
                pushGridPad(selected)
                select(selected)
                saveSoon()
            }.setNegativeButton(L10n.t("Cancel"), null).show()
    }

    // ------------------------------------------------------------------ samples

    /** `files/pads/pad_XX.<ext>` target of pad [pad] (0-based). */
    fun sampleFile(pad: Int, ext: String = "wav") = File(dir, "pad_%02d.%s".format(pad + 1, ext))

    private fun pickFile() {
        loadPad = selected
        val i = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "audio/*"
        }
        @Suppress("DEPRECATION")
        a.startActivityForResult(i, ActivityResults.REQ_PAD_FILE)
    }

    private fun onPicked(data: Intent?) {
        val u: Uri = data?.data ?: return
        val pad = loadPad
        decodeCancel?.cancel()
        val cancel = AudioCancel()
        decodeCancel = cancel
        if (pad == selected) infoTv.text = L10n.t("DECODING 0%")
        AudioDecode.decodeAsync(a, u, cancel,
            onProgress = { p -> if (cancel === decodeCancel && pad == selected) infoTv.text = "DECODING ${(p * 100).toInt()}%" },
            onDone = { res, err ->
                if (cancel === decodeCancel) {
                    if (res != null) assignFile(pad, res.wavFile, res.title) { e ->
                        if (e != null) a.log("pad ${pad + 1}: $e", Ui.ERR)
                    } else if (err !is CancellationException) {
                        if (pad == selected) infoTv.text = err?.message ?: "decode failed"
                        a.log("pad decode failed: ${err?.message}", Ui.ERR)
                    }
                }
            })
    }

    /**
     * Gives pad [pad] the audio in [src] (copied to `files/pads/pad_XX.<ext>` unless it already is that file), loads it
     * into the engine and persists. [onDone] gets null on success or the error text. Runs the I/O on `a.loader`.
     */
    fun assignFile(pad: Int, src: File, name: String, onDone: (String?) -> Unit = {}) {
        if (pad !in 0 until PADS) return onDone("bad pad")
        a.loader.execute {
            val target = sampleFile(pad, src.extension.lowercase().ifEmpty { "wav" })
            var err: String? = null
            var info = ""
            try {
                if (src.canonicalPath != target.canonicalPath) {
                    for (e in arrayOf("wav", "flac")) if (e != target.extension) sampleFile(pad, e).delete()
                    val tmp = File(dir, target.name + ".tmp")
                    src.inputStream().use { i -> FileOutputStream(tmp).use { o -> i.copyTo(o) } }
                    if (!tmp.renameTo(target)) { target.delete(); tmp.renameTo(target) }
                }
                val r = JSONObject(NativeEngine.nativePadLoad(pad, target.absolutePath))
                if (!r.optBoolean("ok")) {
                    err = r.optString("error", "load failed")
                    target.delete()
                } else {
                    info = "%.2f s · %.1f kHz".format(r.optDouble("dur", 0.0), r.optInt("sr", 0) / 1000.0)
                }
            } catch (e: Exception) {
                err = e.message ?: "copy failed"
            }
            a.ui.post {
                val c = cfg[pad]
                if (err == null) {
                    c.has = true
                    c.info = info
                    c.start = 0f
                    c.end = 1f
                    if (c.name.isEmpty()) c.name = name.take(24)
                    pushSetup(pad)
                    saveSoon()
                } else if (pad == selected) {
                    infoTv.text = err
                }
                pushGridPad(pad)
                if (pad == selected) select(pad)
                onDone(err)
            }
        }
    }

    private fun clearPad(pad: Int) {
        grid.releaseAll()
        NativeEngine.nativePadClear(pad)
        for (e in arrayOf("wav", "flac")) sampleFile(pad, e).delete()
        val c = cfg[pad]
        c.has = false
        c.info = ""
        c.start = 0f
        c.end = 1f
        c.name = ""
        pushGridPad(pad)
        select(pad)
        saveSoon()
    }

    /** Startup: every pad that has a file is loaded into the engine (loader thread), then the UI is refreshed. */
    private fun loadAllSamples() {
        val snapshot = Array(PADS) { i -> sampleFile(i, "wav").takeIf { it.isFile } ?: sampleFile(i, "flac").takeIf { it.isFile } }
        a.loader.execute {
            val ok = BooleanArray(PADS)
            val infos = Array(PADS) { "" }
            for (i in 0 until PADS) {
                val f = snapshot[i] ?: continue
                try {
                    val r = JSONObject(NativeEngine.nativePadLoad(i, f.absolutePath))
                    if (r.optBoolean("ok")) {
                        ok[i] = true
                        infos[i] = "%.2f s · %.1f kHz".format(r.optDouble("dur", 0.0), r.optInt("sr", 0) / 1000.0)
                    }
                } catch (_: Exception) {}
            }
            a.ui.post {
                for (i in 0 until PADS) {
                    cfg[i].has = ok[i]
                    cfg[i].info = infos[i]
                    if (ok[i]) pushSetup(i)
                    pushGridPad(i)
                }
                select(selected)
            }
        }
    }

    // ------------------------------------------------------------------ persistence: files/pads/pads.json (tmp + rename)

    private fun readJson() {
        val f = File(dir, FILE_NAME)
        if (!f.isFile) return
        try {
            val o = JSONObject(f.readText(Charsets.UTF_8))
            selected = o.optInt("sel", 0).coerceIn(0, PADS - 1)
            val arr = o.optJSONArray("pads") ?: return
            for (i in 0 until min(PADS, arr.length())) {
                val e = arr.optJSONObject(i) ?: continue
                val c = cfg[i]
                c.name = e.optString("name", "")
                c.color = e.optInt("color", c.color)
                c.level = e.optDouble("level", 1.0).toFloat().coerceIn(0f, 1.5f)
                c.pitch = e.optDouble("pitch", 0.0).toFloat().coerceIn(-12f, 12f)
                c.mode = e.optInt("mode", 0).coerceIn(0, 2)
                c.choke = e.optInt("choke", 0).coerceIn(0, 8)
                c.vel = e.optInt("vel", 1).coerceIn(0, 2)
                c.start = e.optDouble("start", 0.0).toFloat().coerceIn(0f, 1f)
                c.end = e.optDouble("end", 1.0).toFloat().coerceIn(0f, 1f)
                if (c.end <= c.start) { c.start = 0f; c.end = 1f }
            }
        } catch (e: Exception) {
            a.log("pads.json is invalid — using defaults", Ui.WARN)
        }
    }

    private fun toJson(): String {
        val arr = JSONArray()
        for (c in cfg) arr.put(JSONObject()
            .put("name", c.name).put("color", c.color).put("level", c.level.toDouble()).put("pitch", c.pitch.toDouble())
            .put("mode", c.mode).put("choke", c.choke).put("vel", c.vel)
            .put("start", c.start.toDouble()).put("end", c.end.toDouble()))
        return JSONObject().put("v", 1).put("sel", selected).put("pads", arr).toString()
    }

    private var dirty = false

    private fun saveSoon() {
        dirty = true
        a.ui.removeCallbacks(saveRun)
        a.ui.postDelayed(saveRun, 500L)
    }

    /** Write a pending change now (call from onPause / before a backup). Does nothing when nothing changed. */
    fun flush() {
        if (!dirty) return
        a.ui.removeCallbacks(saveRun)
        writeNow()
    }

    /** Drop an unsaved change (a restore just replaced `files/pads/`; the old in-memory state must not overwrite it). */
    fun discardPending() {
        dirty = false
        a.ui.removeCallbacks(saveRun)
    }

    private fun writeNow() {
        dirty = false
        val json = try { toJson() } catch (_: Exception) { return }
        a.loader.execute {
            try {
                dir.mkdirs()
                val tmp = File(dir, "$FILE_NAME.tmp")
                val dst = File(dir, FILE_NAME)
                FileOutputStream(tmp).use { it.write(json.toByteArray(Charsets.UTF_8)); it.fd.sync() }
                if (!tmp.renameTo(dst)) { dst.delete(); tmp.renameTo(dst) }
            } catch (_: Exception) {}
        }
    }
}
