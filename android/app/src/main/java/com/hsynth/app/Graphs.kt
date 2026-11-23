package com.hsynth.app

import android.annotation.SuppressLint
import android.app.AlertDialog
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.widget.LinearLayout
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.ln
import kotlin.math.log10
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow
import kotlin.math.roundToInt
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * Graphic editors drawn like a synth's LCD: envelope, filter response, LFO
 * shape, FM algorithm, step sequencer, MSEG and the modulation matrix. They
 * read and write engine parameters by name; [onEdited] tells the page to
 * refresh its knobs after a drag.
 */
abstract class GraphView(ctx: Context, private val heightDp: Float, accentColor: Int = Hw.MOD_OSC) : View(ctx) {
    var onEdited: (() -> Unit)? = null
    protected val d = resources.displayMetrics.density
    protected val line = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = accentColor; style = Paint.Style.STROKE; strokeWidth = 2.2f * d; strokeJoin = Paint.Join.ROUND; strokeCap = Paint.Cap.ROUND }
    protected val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Hw.withAlpha(accentColor, 46) }
    protected val grid = Paint().apply { color = Color.rgb(22, 30, 44); strokeWidth = 1f * d }
    protected val dot = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.WHITE }
    protected val label = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Hw.ENGRAVE; textSize = 10.5f * d; typeface = Hw.MONO }
    protected val bg = Paint().apply { color = Hw.WELL }
    protected val frame = Paint().apply { color = Hw.PANEL_EDGE; style = Paint.Style.STROKE; strokeWidth = 1f * d }
    private val cornerR = 6f * d
    private val frameRect = RectF()

    var accent: Int = accentColor
        set(v) {
            field = v
            line.color = v
            fill.color = Hw.withAlpha(v, 46)
            invalidate()
        }

    override fun onMeasure(widthSpec: Int, heightSpec: Int) {
        setMeasuredDimension(MeasureSpec.getSize(widthSpec), (heightDp * d).toInt())
    }

    protected fun meta(name: String) = Params.metaOf(name)
    protected fun get(name: String) = Params.get(name)
    protected fun set(name: String, v: Float) {
        val m = meta(name) ?: return
        NativeEngine.setParam(m.id, v)
        Hw.onParamTouched?.invoke(m.label, m.format(NativeEngine.nativeGetParam(m.id)))
        invalidate()
    }

    /** Pure OLED black vector scope with hairline grid. */
    protected fun screen(c: Canvas, cols: Int = 8, rows: Int = 4) {
        frameRect.set(0.5f * d, 0.5f * d, width - 0.5f * d, height - 0.5f * d)
        c.drawRoundRect(frameRect, cornerR, cornerR, bg)
        for (i in 1 until cols) { val x = width * i / cols.toFloat(); c.drawLine(x, d, x, height - d, grid) }
        for (i in 1 until rows) { val y = height * i / rows.toFloat(); c.drawLine(d, y, width - d, y, grid) }
        c.drawRoundRect(frameRect, cornerR, cornerR, frame)
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        if (e.actionMasked == MotionEvent.ACTION_DOWN) {
            parent?.requestDisallowInterceptTouchEvent(true)
            History.checkpoint()
        }
        val r = touch(e)
        if (e.actionMasked == MotionEvent.ACTION_UP || e.actionMasked == MotionEvent.ACTION_CANCEL) onEdited?.invoke()
        return r
    }

    protected open fun touch(e: MotionEvent): Boolean = false
}

// ---------------------------------------------------------------- envelope

/** ADSR (prefix "amp", "env2", "env3" or "a.fm.op1"); drag the three corners. */
class EnvGraphView(ctx: Context, private val prefix: String) : GraphView(ctx, 120f) {
    private var drag = -1
    private val nAtk = "$prefix.attack"
    private val nDec = "$prefix.decay"
    private val nSus = "$prefix.sustain"
    private val nRel = "$prefix.release"
    private val pad get() = 10 * d
    private val q get() = (width - 2 * pad) / 3.7f
    private val xs = FloatArray(5)      // x of: start, attack end, decay end, sustain end, release end
    private val path = Path()

    private fun layoutX() {
        val a = sqrt(get(nAtk) / 10f).coerceIn(0f, 1f) * q
        val dd = sqrt(get(nDec) / 20f).coerceIn(0f, 1f) * q
        val r = sqrt(get(nRel) / 20f).coerceIn(0f, 1f) * q
        xs[0] = pad
        xs[1] = xs[0] + a
        xs[2] = xs[1] + dd
        xs[3] = xs[2] + 0.7f * q
        xs[4] = xs[3] + r
    }

    override fun onDraw(c: Canvas) {
        screen(c)
        layoutX()
        val yTop = 12 * d
        val yBot = height - 12 * d
        val sus = yBot - (yBot - yTop) * get(nSus).coerceIn(0f, 1f)
        path.rewind()
        path.moveTo(xs[0], yBot); path.lineTo(xs[1], yTop); path.lineTo(xs[2], sus); path.lineTo(xs[3], sus); path.lineTo(xs[4], yBot)
        c.drawPath(path, fill)      // a FILL paint closes the contour implicitly
        c.drawPath(path, line)
        c.drawCircle(xs[1], yTop, 5 * d, dot)
        c.drawCircle(xs[2], sus, 5 * d, dot)
        c.drawCircle(xs[4], yBot, 5 * d, dot)
        c.drawText("A", xs[1] - 4 * d, yTop - 2 * d + 10 * d, label)
        c.drawText("D/S", xs[2] + 6 * d, sus - 6 * d, label)
        c.drawText("R", xs[4] + 6 * d, yBot - 6 * d, label)
    }

    override fun touch(e: MotionEvent): Boolean {
        layoutX()
        val yTop = 12 * d
        val yBot = height - 12 * d
        val sus = yBot - (yBot - yTop) * get(nSus)
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                val d0 = hypot(xs[1] - e.x, yTop - e.y)
                val d1 = hypot(xs[2] - e.x, sus - e.y)
                val d2 = hypot(xs[4] - e.x, yBot - e.y)
                drag = if (d0 <= d1 && d0 <= d2) 0 else if (d1 <= d2) 1 else 2
            }
            MotionEvent.ACTION_MOVE -> when (drag) {
                0 -> set(nAtk, (((e.x - xs[0]) / q).coerceIn(0f, 1f)).pow(2) * 10f)
                1 -> {
                    set(nDec, (((e.x - xs[1]) / q).coerceIn(0f, 1f)).pow(2) * 20f)
                    set(nSus, ((yBot - e.y) / (yBot - yTop)).coerceIn(0f, 1f))
                }
                2 -> set(nRel, (((e.x - xs[3]) / q).coerceIn(0f, 1f)).pow(2) * 20f)
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> drag = -1
        }
        return true
    }
}

// ---------------------------------------------------------------- filter

/** Approximate magnitude response; drag left / right for cutoff, up / down for resonance. */
class FilterCurveView(ctx: Context) : GraphView(ctx, 130f) {
    private val minDb = -36f
    private val maxDb = 18f
    private val steps = 160
    private val freq = FloatArray(steps + 1) { 20f * 1000f.pow(it / steps.toFloat()) }
    private val path = Path()
    private val area = Path()
    private val markF = floatArrayOf(100f, 1000f, 10000f)
    private val markT = arrayOf("100", "1k", "10k")
    private val f1 = floatArrayOf(730f, 530f, 270f, 570f, 300f)     // vowel formants a-e-i-o-u
    private val f2 = floatArrayOf(1090f, 1840f, 2290f, 840f, 870f)
    private var hdrType = ""
    private var hdrFc = -1f
    private var hdr = ""

    private fun fx(f: Float) = width * (log10(f / 20f) / 3f)
    private fun fy(db: Float) = height * (1f - (db - minDb) / (maxDb - minDb))

    private fun peak(f: Float, c: Float): Double {
        val ww = f / c
        val qq = 6.0
        val re = 1.0 - ww * ww
        val im = ww / qq
        return (ww / qq) / sqrt(re * re + im * im)
    }

    private fun lp2(w: Float, q: Float): Double {
        val re = 1.0 - w * w
        val im = w / q.toDouble()
        return 1.0 / sqrt(re * re + im * im)
    }

    private fun magDb(type: String, f: Float, fc: Float, reso: Float, morph: Float): Float {
        val w = f / fc
        val q = 0.55f + reso * 9f
        val m: Double = when (type) {
            "off" -> 1.0
            "lp12" -> lp2(w, q)
            "lp24", "ladder" -> {
                val a = lp2(w, q)
                val q2 = 0.71
                val re = 1.0 - w * w
                val im = w / q2
                a / sqrt(re * re + im * im)
            }
            "hp" -> lp2(w, q) * w * w
            "bp" -> lp2(w, q) * (w / q) * q
            "notch" -> lp2(w, q) * abs(1.0 - w * w)
            "comb+", "comb-" -> {
                val g = reso * 0.95
                val ph = 2.0 * PI * f / fc
                val s = if (type == "comb+") 1.0 else -1.0
                sqrt((1 + s * g * cos(ph)).pow(2) + (s * g * sin(ph)).pow(2)) / (1 + g)
            }
            "formant" -> {
                // interpolated by morph
                val t = morph * 4f
                val i = t.toInt().coerceIn(0, 3)
                val k = t - i
                val a = f1[i] + (f1[i + 1] - f1[i]) * k
                val b = f2[i] + (f2[i + 1] - f2[i]) * k
                (peak(f, a) + 0.7 * peak(f, b)) * 1.2
            }
            else -> lp2(w, q)
        }
        return (20 * log10(max(m, 1e-5))).toFloat()
    }

    override fun onDraw(c: Canvas) {
        screen(c, 3, 6)
        val type = meta("filter.type")?.let { it.options.getOrNull(get("filter.type").roundToInt()) } ?: "lp24"
        val fc = get("filter.cutoff")
        val reso = get("filter.reso")
        val morph = get("filter.morph")
        path.rewind()
        for (i in 0..steps) {
            val f = freq[i]
            val y = fy(magDb(type, f, fc, reso, morph).coerceIn(minDb - 6, maxDb))
            if (i == 0) path.moveTo(fx(f), y) else path.lineTo(fx(f), y)
        }
        area.set(path)
        area.lineTo(width.toFloat(), height.toFloat())
        area.lineTo(0f, height.toFloat())
        area.close()
        c.drawPath(area, fill)
        c.drawPath(path, line)
        c.drawLine(0f, fy(0f), width.toFloat(), fy(0f), grid)
        for (i in markF.indices) c.drawText(markT[i], fx(markF[i]) + 3 * d, height - 4 * d, label)
        if (type != hdrType || fc != hdrFc) {
            hdrType = type
            hdrFc = fc
            hdr = "${type.uppercase()}  ${meta("filter.cutoff")?.format(fc) ?: ""}"
        }
        c.drawText(hdr, 6 * d, 14 * d, label)
        c.drawCircle(fx(fc), fy(magDb(type, fc, fc, reso, morph).coerceIn(minDb, maxDb)), 5 * d, dot)
    }

    override fun touch(e: MotionEvent): Boolean {
        if (e.actionMasked == MotionEvent.ACTION_DOWN || e.actionMasked == MotionEvent.ACTION_MOVE) {
            set("filter.cutoff", (20f * 1000f.pow((e.x / width).coerceIn(0f, 1f))))
            set("filter.reso", (1f - e.y / height).coerceIn(0f, 1f))
        }
        return true
    }
}

// ---------------------------------------------------------------- LFO

/** Two cycles of the LFO shape with its rate. */
class LfoView(ctx: Context, private val n: Int) : GraphView(ctx, 80f) {
    private val nWave = "lfo$n.wave"
    private val nSync = "lfo$n.sync"
    private val nRate = "lfo$n.rate"
    private val path = Path()
    private val steps = 200
    private var seed = 7
    private var rateV = Float.NaN      // cached formatted rate (re-formatted only when the value changes)
    private var rateTxt = ""
    private var hdrWave = ""           // cached header (rebuilt only when wave / rate text change)
    private var hdrRate = ""
    private var hdr = ""

    /** Tiny deterministic LCG in [-1, 1): the S&H preview looks the same on every redraw, without a Random per draw. */
    private fun nextRnd(): Float {
        seed = seed * 1103515245 + 12345
        return ((seed ushr 8) and 0xFFFF) / 32768f - 1f
    }

    override fun onDraw(c: Canvas) {
        screen(c, 4, 2)
        val wave = meta(nWave)?.options?.getOrNull(get(nWave).roundToInt()) ?: "sine"
        val mid = height / 2f
        val amp = height * 0.38f
        path.rewind()
        seed = 7
        var sh = 0f
        for (i in 0..steps) {
            val t = 2f * i / steps
            val ph = t - t.toInt()
            if (wave == "s&h" && (i == 0 || (t * 4).toInt() != ((t - 2f / steps) * 4).toInt())) sh = nextRnd()
            val v = when (wave) {
                "triangle" -> 1f - 4f * abs(ph - 0.5f)
                "saw" -> 1f - 2f * ph
                "square" -> if (ph < 0.5f) 1f else -1f
                "s&h" -> sh
                else -> sin(2 * PI * ph).toFloat()
            }
            val x = width * i / steps.toFloat()
            if (i == 0) path.moveTo(x, mid - v * amp) else path.lineTo(x, mid - v * amp)
        }
        c.drawPath(path, line)
        val sync = meta(nSync)?.let { it.options.getOrNull(get(nSync).roundToInt()) } ?: "off"
        val rate: String
        if (sync != "off") {
            rate = sync
        } else {
            val rv = get(nRate)
            if (rv != rateV) { rateV = rv; rateTxt = meta(nRate)?.format(rv) ?: "" }
            rate = rateTxt
        }
        if (wave != hdrWave || rate != hdrRate) {
            hdrWave = wave
            hdrRate = rate
            hdr = "LFO$n  ${wave.uppercase()}  $rate"
        }
        c.drawText(hdr, 6 * d, 14 * d, label)
    }
}

// ---------------------------------------------------------------- FM algorithm

/** Operators as boxes, modulators above the operators they modulate; tap one to edit it. */
class FmAlgoView(ctx: Context, private val layer: Char) : GraphView(ctx, 150f) {
    var selected = 0
        set(v) { field = v; invalidate() }
    var onSelect: ((Int) -> Unit)? = null
    private val boxes = Array(8) { RectF() }
    private val box = Paint(Paint.ANTI_ALIAS_FLAG)
    private val txt = Paint(Paint.ANTI_ALIAS_FLAG).apply { textAlign = Paint.Align.CENTER; textSize = 14f * d; typeface = Hw.MONO }
    private val nAlgo = "$layer.fm.algo"
    private val opNum = Array(8) { "${it + 1}" }
    private val mods = IntArray(8)
    private val level = IntArray(8)
    private val fbRect = RectF()
    private var algoIdx = -1
    private var algo = IntArray(0)
    private var algoLabel = ""
    var numOps = 4
        private set

    override fun onDraw(c: Canvas) {
        screen(c, 1, 1)
        val idx = get(nAlgo).roundToInt()
        if (idx != algoIdx || algo.size < 3) {
            // the algorithm tables are static per index: fetch (JNI) and format the label only when it changes
            algo = NativeEngine.nativeFmAlgorithm(idx)
            algoIdx = idx
            algoLabel = "ALG " + (meta(nAlgo)?.options?.getOrNull(idx) ?: "")
        }
        numOps = algo[0].coerceIn(0, 8)
        val carriers = algo[1]
        val fb = algo[2]
        for (i in 0 until 8) { mods[i] = if (3 + i < algo.size) algo[3 + i] else 0; level[i] = 0 }
        // level: carriers 0, a modulator one above the highest op it modulates
        repeat(numOps) {
            for (j in 0 until numOps) for (i in 0 until numOps)
                if (i != j && (mods[i] shr j) and 1 == 1) level[j] = max(level[j], level[i] + 1)
        }
        var maxLevel = 0
        for (i in 0 until numOps) if (level[i] > maxLevel) maxLevel = level[i]
        val rows = maxLevel + 1
        val bw = 34 * d
        val bh = 24 * d
        val top = 14 * d
        val rowH = (height - top - 26 * d) / max(1, rows)
        for (r in 0 until rows) {
            var cnt = 0
            for (op in 0 until numOps) if (level[op] == r) cnt++
            var k = 0
            for (op in 0 until numOps) if (level[op] == r) {
                val cx = width * (k + 1f) / (cnt + 1f)
                val cy = top + (rows - 1 - r) * rowH + rowH / 2
                boxes[op].set(cx - bw / 2, cy - bh / 2, cx + bw / 2, cy + bh / 2)
                k++
            }
        }
        // connections
        for (i in 0 until numOps) for (j in 0 until numOps) {
            if (i == j || (mods[i] shr j) and 1 == 0) continue
            c.drawLine(boxes[j].centerX(), boxes[j].bottom, boxes[i].centerX(), boxes[i].top, line)
        }
        val busY = height - 12 * d
        var minX = Float.MAX_VALUE
        var maxX = 0f
        for (i in 0 until numOps) if ((carriers shr i) and 1 == 1) {
            c.drawLine(boxes[i].centerX(), boxes[i].bottom, boxes[i].centerX(), busY, line)
            minX = min(minX, boxes[i].centerX()); maxX = max(maxX, boxes[i].centerX())
        }
        if (maxX >= minX) c.drawLine(minX, busY, maxX, busY, line)
        if (fb in 0 until numOps) {
            val b = boxes[fb]
            fbRect.set(b.right - 4 * d, b.top - 10 * d, b.right + 12 * d, b.centerY())
            c.drawArc(fbRect, 180f, 270f, false, line)
        }
        for (i in 0 until numOps) {
            val carrier = (carriers shr i) and 1 == 1
            box.color = if (i == selected) Hw.LED else if (carrier) Color.rgb(60, 90, 20) else Color.rgb(30, 44, 16)
            c.drawRoundRect(boxes[i], 3 * d, 3 * d, box)
            txt.color = if (i == selected) Color.BLACK else Hw.LCD_FG
            c.drawText(opNum[i], boxes[i].centerX(), boxes[i].centerY() + 5 * d, txt)
        }
        c.drawText(algoLabel, 6 * d, 12 * d, label)
    }

    override fun touch(e: MotionEvent): Boolean {
        if (e.actionMasked == MotionEvent.ACTION_UP) {
            for (i in 0 until numOps) if (boxes[i].contains(e.x, e.y)) { selected = i; onSelect?.invoke(i) }
        }
        return true
    }
}

// ---------------------------------------------------------------- step sequencer

/**
 * Step grid: note, velocity and length lanes; drag across steps to paint
 * values. Velocity at the bottom = rest.
 *
 * The playhead follows the sequencer through the shared [FrameTicker] (25 fps): it only runs while this view is
 * actually shown with window focus and the app is active, so a hidden EDIT tab or a switched-off screen costs nothing.
 * onDraw() allocates nothing (paints, strings and parameter names are all precomputed).
 */
class StepSeqView(ctx: Context) : GraphView(ctx, 230f), FrameTicker.Client {
    private var playStep = -1

    override val intervalMs: Int get() = 40
    override val tickView: View? get() = this

    override fun onFrame(nowMs: Long) {
        val s = NativeEngine.nativeSeqPosition()
        if (s != playStep) { playStep = s; invalidate() }
    }

    override fun onAttachedToWindow() { super.onAttachedToWindow(); FrameTicker.register(this) }
    override fun onDetachedFromWindow() { FrameTicker.unregister(this); super.onDetachedFromWindow() }
    override fun onVisibilityAggregated(isVisible: Boolean) { super.onVisibilityAggregated(isVisible); FrameTicker.poke() }
    override fun onWindowFocusChanged(hasWindowFocus: Boolean) { super.onWindowFocusChanged(hasWindowFocus); FrameTicker.poke() }

    private val laneNames = arrayOf("NOTE", "VEL", "LEN")
    private val noteLabels = Array(128) { arrayOf("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")[it % 12] + (it / 12 - 1) }
    private val nNote = Array(32) { "seq.s${it + 1}.note" }
    private val nVel = Array(32) { "seq.s${it + 1}.vel" }
    private val nLen = Array(32) { "seq.s${it + 1}.len" }
    private val barP = Paint().apply { color = Hw.LCD_FG }
    private val restP = Paint().apply { color = Color.rgb(40, 54, 22) }
    private val headP = Paint().apply { color = Color.argb(70, 255, 140, 26) }
    private val lenP = Paint()
    private val laneP = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Hw.ENGRAVE; textSize = 11f * d; typeface = Hw.MONO }
    private val noteP = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Hw.ENGRAVE; textSize = 9f * d; typeface = Hw.MONO }
    private val laneNote = 0.44f
    private val laneVel = 0.34f
    private var lane = -1
    private val steps get() = get("seq.steps").roundToInt().coerceIn(1, 32)
    private val labelW get() = 34 * d

    private fun laneTop(i: Int) = height * when (i) { 0 -> 0f; 1 -> laneNote; else -> laneNote + laneVel }
    private fun laneBot(i: Int) = height * when (i) { 0 -> laneNote; 1 -> laneNote + laneVel; else -> 1f }

    override fun onDraw(c: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        c.drawRect(0f, 0f, w, h, bg)
        val n = steps
        val sw = (w - labelW) / n
        lenP.color = Hw.withAlpha(accent, 140)
        for (l in 0..2) {
            c.drawLine(0f, laneBot(l), w, laneBot(l), grid)
            c.drawText(laneNames[l], 4 * d, laneTop(l) + 14 * d, laneP)
        }
        val nt = laneTop(0) + 18 * d
        val nb = laneBot(0) - 4 * d
        val vt = laneTop(1) + 4 * d
        val vb = laneBot(1) - 2 * d
        val lt = laneTop(2) + 4 * d
        val lb = laneBot(2) - 2 * d
        for (s in 0 until n) {
            val x0 = labelW + s * sw
            if (s == playStep) c.drawRect(x0, 0f, x0 + sw, h, headP)
            if (s % 4 == 0) c.drawLine(x0, 0f, x0, h, grid)
            val vel = get(nVel[s])
            val note = get(nNote[s]).roundToInt()
            val len = get(nLen[s])
            // note: position in 24..96
            val ny = nb - (nb - nt) * ((note - 24) / 72f).coerceIn(0f, 1f)
            c.drawRect(x0 + 2 * d, ny - 4 * d, x0 + sw - 2 * d, ny + 4 * d, if (vel > 0f) barP else restP)
            if (sw > 20 * d) c.drawText(noteLabels[note.coerceIn(0, 127)], x0 + 2 * d, nb, noteP)
            // velocity
            if (vel > 0f) c.drawRect(x0 + 2 * d, vb - (vb - vt) * vel, x0 + sw - 2 * d, vb, barP)
            else c.drawRect(x0 + 2 * d, vb - 2 * d, x0 + sw - 2 * d, vb, restP)
            // length (0.1..8 steps)
            c.drawRect(x0 + 2 * d, lb - (lb - lt) * (len / 2f).coerceIn(0f, 1f), x0 + sw - 2 * d, lb, if (vel > 0f) lenP else restP)
        }
        c.drawRect(0f, 0f, w, h, frame)
    }

    override fun touch(e: MotionEvent): Boolean {
        val n = steps
        val sw = (width - labelW) / n
        if (e.actionMasked == MotionEvent.ACTION_DOWN) lane = (0..2).firstOrNull { e.y >= laneTop(it) && e.y < laneBot(it) } ?: -1
        if (e.actionMasked == MotionEvent.ACTION_DOWN || e.actionMasked == MotionEvent.ACTION_MOVE) {
            if (e.x < labelW || lane < 0) return true
            val s = ((e.x - labelW) / sw).toInt().coerceIn(0, n - 1)   // 0-based step
            val y = e.y.coerceIn(laneTop(lane), laneBot(lane))
            when (lane) {
                0 -> {
                    val nt = laneTop(0) + 18 * d
                    val nb = laneBot(0) - 4 * d
                    set(nNote[s], (24 + ((nb - y) / (nb - nt)).coerceIn(0f, 1f) * 72f).roundToInt().toFloat())
                    if (get(nVel[s]) <= 0f) set(nVel[s], 0.8f)
                }
                1 -> {
                    val vt = laneTop(1) + 4 * d
                    val vb = laneBot(1) - 2 * d
                    val v = ((vb - y) / (vb - vt)).coerceIn(0f, 1f)
                    set(nVel[s], if (v < 0.06f) 0f else v)
                }
                2 -> {
                    val lt = laneTop(2) + 4 * d
                    val lb = laneBot(2) - 2 * d
                    set(nLen[s], (((lb - y) / (lb - lt)).coerceIn(0.05f, 1f) * 2f))
                }
            }
        }
        return true
    }
}

// ---------------------------------------------------------------- MSEG

/**
 * Multi-segment envelope: drag a point (time, level), drag the middle of a
 * segment up / down for its curve.
 */
class MsegView(ctx: Context) : GraphView(ctx, 150f) {
    private var drag = -1          // point index, or 100 + segment for a curve
    private var scale = 1f         // seconds across the width, frozen while dragging
    private val pad get() = 10 * d
    private val np get() = get("mseg.points").roundToInt().coerceIn(2, 8)
    private val tBuf = FloatArray(8)       // cumulative time of each point, refreshed by fillTimes()
    private val path = Path()
    private val loopP = Paint().apply { color = Hw.LED; strokeWidth = 1.5f * d }
    private val midP = Paint(Paint.ANTI_ALIAS_FLAG)
    // parameter names, 1-based like the engine ("mseg.p1.level")
    private val nLevel = Array(9) { "mseg.p$it.level" }
    private val nTime = Array(9) { "mseg.p$it.time" }
    private val nCurve = Array(9) { "mseg.p$it.curve" }
    private var totStr = ""
    private var totV = Float.NaN
    private var totBeats = false

    private fun fillTimes(n: Int) {
        tBuf[0] = 0f
        for (i in 1 until n) tBuf[i] = tBuf[i - 1] + max(0f, get(nTime[i + 1]))
    }
    private fun totalScale(n: Int) = max(0.5f, tBuf[n - 1] * 1.15f)
    private fun px(t: Float) = pad + (width - 2 * pad) * (t / scale)
    private fun py(l: Float) = height - pad - (height - 2 * pad) * l.coerceIn(0f, 1f)

    override fun onDraw(c: Canvas) {
        screen(c)
        val n = np
        fillTimes(n)
        val t = tBuf
        if (drag < 0) scale = totalScale(n)
        path.rewind()
        path.moveTo(px(0f), py(get(nLevel[1])))
        for (i in 1 until n) {
            val a = get(nLevel[i])
            val b = get(nLevel[i + 1])
            val cv = get(nCurve[i + 1])
            for (k in 1..24) {
                var x = k / 24f
                x = if (cv > 0f) x.pow(1f + 4f * cv) else if (cv < 0f) 1f - (1f - x).pow(1f - 4f * cv) else x
                path.lineTo(px(t[i - 1] + (t[i] - t[i - 1]) * k / 24f), py(a + (b - a) * x))
            }
        }
        c.drawPath(path, line)
        val loop = get("mseg.loop").roundToInt()
        if (loop in 1 until n) {
            val lx = px(t[loop])
            c.drawLine(lx, 0f, lx, height.toFloat(), loopP)
            c.drawText("LOOP", lx + 3 * d, 12 * d, label)
        }
        for (i in 0 until n) c.drawCircle(px(t[i]), py(get(nLevel[i + 1])), 5 * d, dot)
        midP.color = Hw.withAlpha(accent, 200)
        for (i in 1 until n) {
            val mx = (px(t[i - 1]) + px(t[i])) / 2
            val my = (py(get(nLevel[i])) + py(get(nLevel[i + 1]))) / 2
            c.drawCircle(mx, my, 3 * d, midP)
        }
        val beats = get("mseg.beats") > 0.5f
        val tot = t[n - 1]
        if (tot != totV || beats != totBeats) {
            totV = tot
            totBeats = beats
            totStr = if (beats) "%.2f beats".format(tot) else "%.2f s".format(tot)
        }
        c.drawText(totStr, width - 70 * d, height - 6 * d, label)
    }

    override fun touch(e: MotionEvent): Boolean {
        val n = np
        fillTimes(n)
        val t = tBuf
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                scale = totalScale(n)
                var best = -1
                var bestD = 28 * d
                for (i in 0 until n) {
                    val dd = hypot(px(t[i]) - e.x, py(get(nLevel[i + 1])) - e.y)
                    if (dd < bestD) { bestD = dd; best = i }
                }
                if (best < 0) for (i in 1 until n) {
                    val mx = (px(t[i - 1]) + px(t[i])) / 2
                    val my = (py(get(nLevel[i])) + py(get(nLevel[i + 1]))) / 2
                    val dd = hypot(mx - e.x, my - e.y)
                    if (dd < bestD) { bestD = dd; best = 100 + i }
                }
                drag = best
            }
            MotionEvent.ACTION_MOVE -> {
                if (drag in 0..7) {
                    val i = drag
                    set(nLevel[i + 1], ((height - pad - e.y) / (height - 2 * pad)).coerceIn(0f, 1f))
                    if (i > 0) {
                        val tt = ((e.x - pad) / (width - 2 * pad) * scale).coerceAtLeast(t[i - 1])
                        set(nTime[i + 1], (tt - t[i - 1]).coerceIn(0f, 10f))
                    }
                } else if (drag >= 100) {
                    val i = drag - 100
                    val a = py(get(nLevel[i]))
                    val b = py(get(nLevel[i + 1]))
                    val mid = (a + b) / 2
                    // finger above the straight line = slow start (positive curve) when rising
                    val dir = if (b < a) 1f else -1f
                    set(nCurve[i + 1], (((e.y - mid) / (60 * d)) * dir).coerceIn(-1f, 1f))
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> { drag = -1; invalidate() }
        }
        return true
    }
}

// ---------------------------------------------------------------- modulation matrix

/** Bipolar amount slider (-1..1) drawn as an LCD bar. */
class BipolarBar(ctx: Context, private val name: String) : GraphView(ctx, 34f) {
    private val barP = Paint()
    private val valP = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.rgb(230, 240, 200); textSize = 10.5f * d; typeface = Hw.MONO }
    private var shownV = Float.NaN
    private var shownStr = ""

    override fun onMeasure(widthSpec: Int, heightSpec: Int) {
        val w = if (MeasureSpec.getMode(widthSpec) == MeasureSpec.EXACTLY) MeasureSpec.getSize(widthSpec) else (150 * d).toInt()
        setMeasuredDimension(w, (34 * d).toInt())
    }
    override fun onDraw(c: Canvas) {
        c.drawRect(0f, 0f, width.toFloat(), height.toFloat(), bg)
        val v = get(name)
        val mid = width / 2f
        val x = mid + v * (width / 2f - 4 * d)
        barP.color = if (v == 0f) Hw.LCD_DIM else Hw.LCD_FG
        c.drawRect(min(mid, x), 8 * d, max(mid, x), height - 8 * d, barP)
        c.drawLine(mid, 4 * d, mid, height - 4 * d, grid)
        if (v != shownV) { shownV = v; shownStr = "%+.2f".format(v) }
        c.drawText(shownStr, 6 * d, height - 10 * d, valP)
        c.drawRect(0f, 0f, width.toFloat(), height.toFloat(), frame)
    }
    override fun touch(e: MotionEvent): Boolean {
        if (e.actionMasked == MotionEvent.ACTION_DOWN || e.actionMasked == MotionEvent.ACTION_MOVE) {
            var v = ((e.x - width / 2f) / (width / 2f - 4 * d)).coerceIn(-1f, 1f)
            if (abs(v) < 0.03f) v = 0f
            set(name, (v * 100).roundToInt() / 100f)
        }
        return true
    }
}

/** Slots as rows: # · source · via · destination · amount. Unused slots are dimmed. */
class ModMatrixView(private val ctx: Context, private val onChange: () -> Unit) : LinearLayout(ctx) {
    private val rows = mutableListOf<() -> Unit>()
    init {
        orientation = VERTICAL
        val d = { v: Int -> Ui.dp(ctx, v) }
        val slots = (1..32).takeWhile { Params.metaOf("mod$it.src") != null }
        val head = LinearLayout(ctx).apply {
            orientation = HORIZONTAL
            for ((t, w) in listOf("#" to 28, "Source" to 0, "Via" to 0, "Destination" to 0, "Amount" to 150))
                addView(Hw.engraved(ctx, t, 10.5f), if (w > 0) LayoutParams(d(w), -2) else LayoutParams(0, -2, 1f).apply { marginEnd = d(6) })
        }
        addView(head, LayoutParams(-1, -2).apply { bottomMargin = d(4) })
        for (s in slots) {
            val row = LinearLayout(ctx).apply { orientation = HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
            row.addView(Hw.engraved(ctx, "$s", 12f, tr = false), LayoutParams(d(28), -2))
            val sels = listOf("src", "via", "dst").map { k ->
                val name = "mod$s.$k"
                val m = Params.metaOf(name)!!
                val lcd = Hw.lcd(ctx, "", 12f).apply {
                    setOnClickListener {
                        AlertDialog.Builder(ctx).setTitle("#$s ${m.label}")
                            .setSingleChoiceItems(m.options.toTypedArray(), Params.get(name).roundToInt()) { dlg, w ->
                                NativeEngine.setParam(m.id, w.toFloat()); dlg.dismiss(); refresh(); onChange()
                            }.show()
                    }
                }
                row.addView(lcd, LayoutParams(0, -2, 1f).apply { marginEnd = d(6) })
                Triple(lcd, m, name)
            }
            val amt = BipolarBar(ctx, "mod$s.amt").apply { onEdited = onChange }
            row.addView(amt, LayoutParams(d(150), -2))
            addView(row, LayoutParams(-1, -2).apply { bottomMargin = d(4) })
            rows += {
                var used = false
                for ((lcd, m, name) in sels) {
                    val v = Params.get(name).roundToInt()
                    lcd.text = m.options.getOrElse(v) { "?" }
                    if (name.endsWith(".src") || name.endsWith(".dst")) used = used || v != 0
                }
                row.alpha = if (used) 1f else 0.55f
                amt.invalidate()
            }
        }
        refresh()
    }
    fun refresh() = rows.forEach { it() }
}

// ---------------------------------------------------------------- A5 scope

/**
 * A5 — realtime oscilloscope + FFT spectrum fed by the shared [ScopeData] tap (WAVE | SPECTRUM | BOTH).
 *
 * Read-only: [onTouchEvent] replaces the base class (no `History.checkpoint()`); tap = freeze / unfreeze, long-press =
 * cycle the mode (persisted in prefs `scopeMode`). It registers with [ScopeData] only while it is attached, allowed
 * ([allowed], e.g. the EDIT "Scope" section is expanded) and not frozen; [ScopeData] itself parks the native tap and
 * the ticker whenever the view is hidden, unfocused, covered by a sheet or the screen is off, so nothing runs
 * off-screen. onDraw() allocates nothing (preallocated points / bars / pitch label as a CharArray).
 */
class AudioScopeView(ctx: Context, heightDp: Float) : GraphView(ctx, heightDp, Hw.MOD_OSC) {
    companion object {
        const val WAVE = 0
        const val SPECTRUM = 1
        const val BOTH = 2
        private const val PREF_MODE = "scopeMode"
        private val NOTES = arrayOf("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
        private val FREQ_MARK = floatArrayOf(100f, 1000f, 10000f)
        private val FREQ_TEXT = arrayOf("100", "1k", "10k")
    }

    private val prefs = ctx.getSharedPreferences("hsynth", Context.MODE_PRIVATE)

    /** WAVE / SPECTRUM / BOTH. */
    var mode: Int = prefs.getInt(PREF_MODE, BOTH).coerceIn(0, 2)
        set(v) {
            val c = v.coerceIn(0, 2)
            if (c == field) return
            field = c
            try { prefs.edit().putInt(PREF_MODE, c).apply() } catch (_: Exception) {}
            layoutAreas()
            invalidate()
        }

    /** The owner (e.g. a collapsible section) can switch the tap off without detaching the view. */
    var allowed: Boolean = true
        set(v) { if (field != v) { field = v; updateAcquire() } }

    val isFrozen: Boolean get() = frozen

    private var frozen = false
    private var holding = false
    private val frozenWave = FloatArray(512)
    private val frozenSpec = FloatArray(64)
    private val frozenPeak = FloatArray(64)
    private var frozenF0 = 0f
    private val pts = FloatArray(4 * 511)

    private val wavePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Hw.MOD_OSC; style = Paint.Style.STROKE; strokeWidth = 1.5f * d; strokeJoin = Paint.Join.ROUND
    }
    private val barPaint = Paint().apply { color = Hw.withAlpha(Hw.MOD_FX, 200) }
    private val capPaint = Paint().apply { color = Hw.withAlpha(Color.WHITE, 220) }
    private val tickPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Hw.INK_DIM; typeface = Hw.MONO; textSize = 8.5f * d; textAlign = Paint.Align.CENTER
    }
    private val pitchPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Hw.SILK; typeface = Hw.MONO; textSize = 11.5f * d; textAlign = Paint.Align.RIGHT
    }
    private val tagPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Hw.ENGRAVE; typeface = Hw.BOLD; textSize = 10f * d; letterSpacing = 0.08f
    }

    // layout of the two plots (px)
    private var wl = 0f
    private var wt = 0f
    private var wr = 0f
    private var wb = 0f
    private var sl = 0f
    private var st = 0f
    private var sr = 0f
    private var sb = 0f
    private val markX = FloatArray(3)

    private val lbl = CharArray(32)
    private var lblLen = 0
    private var lastLabelF = -1f
    private var lastLabelMs = 0L

    private val gest = android.view.GestureDetector(ctx, object : android.view.GestureDetector.SimpleOnGestureListener() {
        override fun onDown(e: MotionEvent) = true
        override fun onSingleTapUp(e: MotionEvent): Boolean { setFrozen(!frozen); return true }
        override fun onLongPress(e: MotionEvent) {
            performHapticFeedback(android.view.HapticFeedbackConstants.LONG_PRESS)
            mode = (mode + 1) % 3
        }
    })

    init { isClickable = true; contentDescription = "Audio scope" }

    private fun setFrozen(v: Boolean) {
        if (v == frozen) return
        if (v) {
            System.arraycopy(ScopeData.wave, 0, frozenWave, 0, 512)
            System.arraycopy(ScopeData.spec, 0, frozenSpec, 0, 64)
            System.arraycopy(ScopeData.specPeak, 0, frozenPeak, 0, 64)
            frozenF0 = ScopeData.f0
            frozen = true
            buildLabel(frozenF0)
        } else {
            frozen = false
            lastLabelF = -1f
        }
        updateAcquire()
        invalidate()
    }

    private fun updateAcquire() {
        val want = allowed && !frozen && isAttachedToWindow
        if (want && !holding) { holding = true; ScopeData.acquire(this) }
        else if (!want && holding) { holding = false; ScopeData.release(this) }
    }

    override fun onAttachedToWindow() { super.onAttachedToWindow(); updateAcquire() }
    override fun onDetachedFromWindow() {
        if (holding) { holding = false; ScopeData.release(this) }
        super.onDetachedFromWindow()
    }
    override fun onVisibilityAggregated(isVisible: Boolean) { super.onVisibilityAggregated(isVisible); FrameTicker.poke() }
    override fun onWindowFocusChanged(hasWindowFocus: Boolean) { super.onWindowFocusChanged(hasWindowFocus); FrameTicker.poke() }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        layoutAreas()
    }

    private fun layoutAreas() {
        val w = width.toFloat()
        val h = height.toFloat()
        if (w <= 0f || h <= 0f) return
        val pad = 5f * d
        val axis = 11f * d   // frequency labels under the spectrum
        when (mode) {
            WAVE -> { wl = pad; wt = pad; wr = w - pad; wb = h - pad }
            SPECTRUM -> { sl = pad; st = pad; sr = w - pad; sb = h - pad - axis }
            else -> if (w >= 1.8f * h) {
                val split = w * 0.4f
                wl = pad; wt = pad; wr = split - pad; wb = h - pad
                sl = split + pad; st = pad; sr = w - pad; sb = h - pad - axis
            } else {
                val split = h * 0.4f
                wl = pad; wt = pad; wr = w - pad; wb = split - pad / 2f
                sl = pad; st = split + pad / 2f; sr = w - pad; sb = h - pad - axis
            }
        }
        if (mode != WAVE) for (i in 0 until 3) markX[i] = sl + (sr - sl) * (Math.log10(FREQ_MARK[i] / 20.0) / 3.0).toFloat()
    }

    override fun onDraw(c: Canvas) {
        screen(c, 8, 4)
        val wv = if (frozen) frozenWave else ScopeData.wave
        val sp = if (frozen) frozenSpec else ScopeData.spec
        val pk = if (frozen) frozenPeak else ScopeData.specPeak
        if (mode != SPECTRUM) drawWave(c, wv)
        if (mode != WAVE) drawSpectrum(c, sp, pk)

        val f = if (frozen) frozenF0 else ScopeData.f0
        if (!frozen && (Math.abs(f - lastLabelF) > 0.5f || (f <= 0f) != (lastLabelF <= 0f))) {
            val now = android.os.SystemClock.uptimeMillis()
            if (now - lastLabelMs >= 250L) { lastLabelMs = now; buildLabel(f) }
        }
        if (lblLen > 0) c.drawText(lbl, 0, lblLen, width - 8f * d, 15f * d, pitchPaint)
        if (frozen) {
            tagPaint.color = Hw.MOD_FILTER
            c.drawText("HOLD", 8f * d, 15f * d, tagPaint)
        } else if (!ScopeData.tapOn) {
            tagPaint.color = Hw.INK_DIM
            c.drawText("--", 8f * d, 15f * d, tagPaint)
        }
    }

    private fun drawWave(c: Canvas, wv: FloatArray) {
        val n = 512
        val w = wr - wl
        val mid = (wt + wb) / 2f
        val amp = (wb - wt) / 2f * 0.92f
        var px = wl
        var py = mid - wv[0].coerceIn(-1f, 1f) * amp
        for (i in 1 until n) {
            val x = wl + w * i / (n - 1)
            val y = mid - wv[i].coerceIn(-1f, 1f) * amp
            val o = (i - 1) * 4
            pts[o] = px; pts[o + 1] = py; pts[o + 2] = x; pts[o + 3] = y
            px = x; py = y
        }
        c.drawLines(pts, 0, 4 * (n - 1), wavePaint)
    }

    private fun drawSpectrum(c: Canvas, sp: FloatArray, pk: FloatArray) {
        val bw = (sr - sl) / 64f
        val gap = if (bw > 3f * d) d else 0f
        val h = sb - st
        for (i in 0 until 64) {
            val l = sl + i * bw
            val r = l + bw - gap
            val top = sb - sp[i].coerceIn(0f, 1f) * h
            c.drawRect(l, top, r, sb, barPaint)
            val cap = sb - pk[i].coerceIn(0f, 1f) * h
            c.drawRect(l, cap - d, r, cap, capPaint)
        }
        val ty = sb + 9f * d
        for (i in 0 until 3) c.drawText(FREQ_TEXT[i], markX[i], ty, tickPaint)
    }

    // ---- pitch label "A4  440.2 Hz  -8 cents" without any String / format allocation

    private fun putInt(at: Int, value: Int): Int {
        var p = at
        var v = value
        if (v < 0) { lbl[p++] = '-'; v = -v }
        var div = 1
        while (v / div >= 10) div *= 10
        while (div > 0) { lbl[p++] = '0' + (v / div) % 10; div /= 10 }
        return p
    }

    private fun buildLabel(f: Float) {
        lastLabelF = f
        if (f <= 0f) { lblLen = 0; return }
        val midi = 69.0 + 12.0 * Math.log(f / 440.0) / Math.log(2.0)
        val n = Math.round(midi).toInt()
        val cents = Math.round((midi - n) * 100.0).toInt()
        var p = 0
        val name = NOTES[((n % 12) + 12) % 12]
        for (i in 0 until name.length) lbl[p++] = name[i]
        p = putInt(p, Math.floorDiv(n, 12) - 1)
        lbl[p++] = ' '
        lbl[p++] = ' '
        val tenths = Math.round(f * 10f).coerceAtMost(999_999)
        p = putInt(p, tenths / 10)
        lbl[p++] = '.'
        lbl[p++] = '0' + tenths % 10
        lbl[p++] = ' '
        lbl[p++] = 'H'
        lbl[p++] = 'z'
        lbl[p++] = ' '
        lbl[p++] = ' '
        lbl[p++] = if (cents < 0) '−' else '+'
        p = putInt(p, Math.abs(cents))
        lbl[p++] = ' '
        lbl[p++] = '¢'
        lblLen = p
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        gest.onTouchEvent(e)
        return true
    }
}

// ---------------------------------------------------------------- 16-step pattern arp

/**
 * A6 — 16-step Pattern Arp grid (150 dp): ruler · GATE (off / on / tie) · ACC (0–200 %) · OCT (−2 … +2) · CHD
 * (single / chord). Reads / writes the pattern of [partProvider]'s part through `nativeGetArpPatternJson` /
 * `nativeSetArpPatternJson` (`{"length":16,"steps":[{"gate":1,"accent":100,"oct":0,"chord":false},…]}`).
 *
 * Touch (same idiom as the step sequencer): the lane is chosen on DOWN. GATE cycles off → on → tie on the first cell
 * and drag-paints that value across steps; ACC / OCT set the value from the finger height per column (tap or drag);
 * CHD toggles on the first cell and drag-paints. A long-press on the ruler then dragging sets the pattern length
 * (1–16; steps beyond it are drawn at 30 %). The pattern is not part of the param snapshot, so no undo checkpoint.
 *
 * The playhead follows `nativeArpStep()` through the shared [FrameTicker] at 20 fps — only while the view is shown
 * with window focus (a hidden tab / sheet / screen-off costs nothing) — and only the previous and the new column are
 * invalidated. onDraw() allocates nothing: column / lane geometry is precomputed in onSizeChanged, strings are static.
 */
class ArpPatternView(ctx: Context, private val partProvider: () -> Int = { NativeEngine.nativeEditPart() }) :
    GraphView(ctx, 150f, Hw.MOD_OSC), FrameTicker.Client {

    companion object {
        const val STEPS = 16
        private const val LANES = 5   // 0 ruler, 1 gate, 2 acc, 3 oct, 4 chd
        private val LANE_NAMES = arrayOf("", "GATE", "ACC", "OCT", "CHD")
        private val NUM = Array(STEPS) { (it + 1).toString() }
        private val LANE_DP = floatArrayOf(14f, 40f, 28f, 36f, 28f)

        /** The 8 built-in patterns (name, steps, gate string 0/1/2, accent string a/n/s/., optional octave string u/d/.). */
        val PRESET_NAMES = arrayOf("Straight 8ths", "Off-beat", "Gallop", "Syncopated", "Trance gate", "Waltz", "16th push", "Tie-roll")
        private class Preset(val len: Int, val gates: String, val acc: String, val oct: String? = null)
        private val PRESETS = arrayOf(
            Preset(16, "1010101010101010", "a.n.n.n.a.n.n.n."),
            Preset(16, "0010001000100010", "..a...a...a...a."),
            Preset(16, "1011101110111011", "a.nna.nna.nna.nn"),
            Preset(16, "1001001010010010", "a..a..a.a..a..a."),
            Preset(16, "1111111111111111", "a.s.a.s.a.s.a.s."),
            Preset(6, "111111", "a.n.n."),
            Preset(16, "1111111111111111", "n..an..an..an..a", "...u...u...u...u"),
            Preset(16, "1222122212221222", "a...a...a...a..."),
        )

        private fun accentOf(ch: Char) = when (ch) { 'a' -> 150; 's' -> 60; else -> 100 }

        /**
         * Toolbar for the Arpeggiator section: `PATTERN` mode toggle (`arp.mode` = pattern), `PATTERNS ▾` list of the
         * 8 built-ins, `RANDOM`, `CLEAR`, stacked above [view].
         */
        fun withToolbar(a: MainActivity, view: ArpPatternView): View {
            val d = a.resources.displayMetrics.density
            val modeMeta = Params.metaOf("arp.mode")
            val patIdx = modeMeta?.options?.indexOfFirst { it.equals("pattern", ignoreCase = true) } ?: -1
            val bar = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
            fun btn(t: String, accent: Int, led: Boolean, w: Int, onClick: (LedButton) -> Unit) =
                LedButton(a, t, tr = false, led = led, accentColor = accent).apply {
                    textSize = 11.5f
                    minHeight = 0
                    minimumHeight = 0
                    setPadding(if (led) (16 * d).toInt() else 0, 0, if (led) (6 * d).toInt() else 0, 0)
                    setOnClickListener { onClick(this) }
                }.also { bar.addView(it, LinearLayout.LayoutParams((w * d).toInt(), (30 * d).toInt()).apply { marginEnd = (6 * d).toInt() }) }

            val modeBtn = btn("PATTERN", Hw.MOD_OSC, true, 92) { b ->
                val m = modeMeta ?: return@btn
                if (patIdx < 0) return@btn
                History.checkpoint()
                val on = NativeEngine.nativeGetParam(m.id).roundToInt() == patIdx
                // toggle: pattern <-> up (the engine's default mode)
                NativeEngine.setParam(m.id, if (on) 0f else patIdx.toFloat())
                b.lit = !on
                view.onEdited?.invoke()
            }
            modeBtn.lit = modeMeta != null && patIdx >= 0 && NativeEngine.nativeGetParam(modeMeta.id).roundToInt() == patIdx
            if (patIdx < 0) modeBtn.alpha = 0.4f
            btn("PATTERNS ▾", Hw.MOD_OSC, false, 100) {
                AlertDialog.Builder(a).setTitle(L10n.t("Arp patterns"))
                    .setItems(PRESET_NAMES) { _, which -> view.applyPreset(which) }
                    .show()
            }
            btn("RANDOM", Hw.MOD_OSC, false, 74) { view.randomize() }
            btn("CLEAR", Hw.MOD_OSC, false, 64) { view.clearPattern() }
            return LinearLayout(a).apply {
                orientation = LinearLayout.VERTICAL
                addView(bar, LinearLayout.LayoutParams(-1, (30 * d).toInt()).apply { bottomMargin = (6 * d).toInt() })
                addView(view, LinearLayout.LayoutParams(-1, -2))
            }
        }
    }

    // pattern state
    private var len = STEPS
    private val gate = IntArray(STEPS) { 1 }
    private val acc = IntArray(STEPS) { 100 }
    private val oct = IntArray(STEPS)
    private val chd = BooleanArray(STEPS)
    private var playStep = -1

    // geometry (px), rebuilt in onSizeChanged
    private val colX = FloatArray(STEPS + 1)
    private val laneTop = FloatArray(LANES)
    private val laneBot = FloatArray(LANES)
    private var labelW = 0f

    private val offP = Paint().apply { color = Color.rgb(18, 22, 31) }
    private val onP = Paint(Paint.ANTI_ALIAS_FLAG)
    private val accP = Paint(Paint.ANTI_ALIAS_FLAG)
    private val dotP = Paint(Paint.ANTI_ALIAS_FLAG)
    private val headP = Paint().apply { color = Color.argb(60, 255, 255, 255) }
    private val markP = Paint().apply { color = Hw.MOD_FILTER }
    private val laneP = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Hw.ENGRAVE; textSize = 9f * d; typeface = Hw.MONO }
    private val numP = Paint(Paint.ANTI_ALIAS_FLAG).apply { textSize = 9f * d; typeface = Hw.MONO; textAlign = Paint.Align.CENTER }

    // touch state
    private var lane = -1
    private var paintGate = 1
    private var paintChd = false
    private var lastCol = -1
    private var lenDrag = false
    private var downX = 0f
    private var downY = 0f
    private val slop = android.view.ViewConfiguration.get(ctx).scaledTouchSlop
    private val longPress = Runnable {
        if (lane == 0) {
            lenDrag = true
            performHapticFeedback(android.view.HapticFeedbackConstants.LONG_PRESS)
            setLenFromX(downX)
        }
    }

    init {
        isClickable = true
        contentDescription = "Arp pattern"
    }

    // ------------------------------------------------------------------ FrameTicker (playhead)

    override val intervalMs: Int get() = 50
    override val tickView: View? get() = this

    override fun onFrame(nowMs: Long) {
        val s = NativeEngine.nativeArpStep()
        if (s == playStep) return
        val old = playStep
        playStep = s
        invalidateColumn(old)
        invalidateColumn(s)
    }

    @Suppress("DEPRECATION")
    private fun invalidateColumn(s: Int) {
        if (s < 0 || s >= STEPS || width == 0) return
        invalidate(colX[s].toInt(), 0, colX[s + 1].toInt() + 1, height)
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        refresh()
        FrameTicker.register(this)
    }

    override fun onDetachedFromWindow() {
        removeCallbacks(longPress)
        FrameTicker.unregister(this)
        super.onDetachedFromWindow()
    }

    override fun onVisibilityAggregated(isVisible: Boolean) { super.onVisibilityAggregated(isVisible); FrameTicker.poke() }
    override fun onWindowFocusChanged(hasWindowFocus: Boolean) { super.onWindowFocusChanged(hasWindowFocus); FrameTicker.poke() }

    // ------------------------------------------------------------------ data

    /** Re-read the pattern of the current part from the engine. */
    fun refresh() {
        val json = try { NativeEngine.nativeGetArpPatternJson(partProvider()) } catch (_: Throwable) { return }
        try {
            val o = org.json.JSONObject(json)
            len = o.optInt("length", STEPS).coerceIn(1, STEPS)
            val st = o.optJSONArray("steps")
            if (st != null) {
                for (i in 0 until min(STEPS, st.length())) {
                    val r = st.optJSONObject(i) ?: continue
                    gate[i] = r.optInt("gate", 1).coerceIn(0, 2)
                    acc[i] = r.optInt("accent", 100).coerceIn(0, 200)
                    oct[i] = r.optInt("oct", 0).coerceIn(-2, 2)
                    chd[i] = r.optBoolean("chord", false)
                }
            }
        } catch (_: Exception) {}
        invalidate()
    }

    /** Send the pattern to the engine (touch time only; never from onDraw). */
    private fun push() {
        val sb = StringBuilder(STEPS * 48 + 24)
        sb.append("{\"length\":").append(len).append(",\"steps\":[")
        for (i in 0 until STEPS) {
            if (i > 0) sb.append(',')
            sb.append("{\"gate\":").append(gate[i]).append(",\"accent\":").append(acc[i])
                .append(",\"oct\":").append(oct[i]).append(",\"chord\":").append(chd[i]).append('}')
        }
        sb.append("]}")
        try { NativeEngine.nativeSetArpPatternJson(partProvider(), sb.toString()) } catch (_: Throwable) {}
        invalidate()
    }

    fun applyPreset(i: Int) {
        val p = PRESETS.getOrNull(i) ?: return
        len = p.len
        for (s in 0 until STEPS) {
            gate[s] = if (s < p.gates.length) (p.gates[s] - '0').coerceIn(0, 2) else 0
            acc[s] = if (s < p.acc.length) accentOf(p.acc[s]) else 100
            oct[s] = when (p.oct?.getOrNull(s)) { 'u' -> 1; 'd' -> -1; else -> 0 }
            chd[s] = false
        }
        push()
        onEdited?.invoke()
    }

    fun randomize() {
        val r = java.util.Random()
        for (s in 0 until STEPS) {
            gate[s] = if (r.nextInt(100) < 72) (if (r.nextInt(100) < 12) 2 else 1) else 0
            acc[s] = intArrayOf(60, 100, 100, 150)[r.nextInt(4)]
            oct[s] = if (r.nextInt(100) < 18) (if (r.nextBoolean()) 1 else -1) else 0
            chd[s] = r.nextInt(100) < 8
        }
        if (gate.take(len).none { it != 0 }) gate[0] = 1
        push()
        onEdited?.invoke()
    }

    fun clearPattern() {
        for (s in 0 until STEPS) { gate[s] = 0; acc[s] = 100; oct[s] = 0; chd[s] = false }
        push()
        onEdited?.invoke()
    }

    // ------------------------------------------------------------------ geometry + draw

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        labelW = 34f * d
        val cw = (w - labelW) / STEPS
        for (i in 0..STEPS) colX[i] = labelW + cw * i
        var total = 0f
        for (l in 0 until LANES) total += LANE_DP[l]
        val gap = 1f * d
        val scale = (h - gap * (LANES - 1)) / (total * d)
        var y = 0f
        for (l in 0 until LANES) {
            laneTop[l] = y
            y += LANE_DP[l] * d * scale
            laneBot[l] = y
            y += gap
        }
    }

    private fun laneAt(y: Float): Int {
        for (l in 0 until LANES) if (y >= laneTop[l] && y < laneBot[l] + d) return l
        return -1
    }

    override fun onDraw(c: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        if (w <= 0f) return
        c.drawRect(0f, 0f, w, h, bg)
        val pad = 1.5f * d
        for (l in 1 until LANES) {
            c.drawLine(0f, laneTop[l] - 0.5f * d, w, laneTop[l] - 0.5f * d, grid)
            c.drawText(LANE_NAMES[l], 4f * d, (laneTop[l] + laneBot[l]) / 2f + 3.5f * d, laneP)
        }
        val numY = (laneTop[0] + laneBot[0]) / 2f + 3.5f * d
        for (s in 0 until STEPS) {
            val inLen = s < len
            val a = if (inLen) 255 else 77
            val x0 = colX[s] + pad
            val x1 = colX[s + 1] - pad
            val cx = (x0 + x1) / 2f
            if (s == playStep && inLen) c.drawRect(colX[s], 0f, colX[s + 1], h, headP)
            if (s % 4 == 0) c.drawLine(colX[s], 0f, colX[s], h, grid)

            // ruler
            numP.color = Hw.withAlpha(if (s % 4 == 0) Hw.SILK else Hw.ENGRAVE, if (inLen) 255 else 90)
            c.drawText(NUM[s], cx, numY, numP)

            // GATE
            val g0 = laneTop[1] + 2f * d
            val g1 = laneBot[1] - 2f * d
            val gt = gate[s]
            if (gt == 0) {
                c.drawRect(x0, g0, x1, g1, offP)
            } else {
                onP.color = Hw.withAlpha(accent, if (gt == 2) (a * 140) / 255 else a)
                c.drawRect(x0, g0, x1, g1, onP)
                if (gt == 2 && s < STEPS - 1) c.drawRect(x1, g0 + 3f * d, colX[s + 1] + pad, g1 - 3f * d, onP)   // tie bridge
            }

            // ACC
            val a0 = laneTop[2] + 2f * d
            val a1 = laneBot[2] - 1f * d
            c.drawRect(x0, a1 - 1.5f * d, x1, a1, offP)
            if (gt != 0 && acc[s] > 0) {
                accP.color = Hw.withAlpha(Hw.MOD_FILTER, a)
                c.drawRect(x0, a1 - (a1 - a0) * (acc[s] / 200f), x1, a1, accP)
            }

            // OCT: 5 dots, current one lit
            val o0 = laneTop[3] + 5f * d
            val o1 = laneBot[3] - 5f * d
            for (k in 0..4) {
                val v = 2 - k
                val y = o0 + (o1 - o0) * k / 4f
                if (v == oct[s]) {
                    dotP.color = Hw.withAlpha(if (v == 0) Hw.SILK else accent, a)
                    c.drawCircle(cx, y, 3f * d, dotP)
                } else {
                    dotP.color = Hw.withAlpha(Hw.PANEL_HI, if (inLen) 200 else 70)
                    c.drawCircle(cx, y, 1.3f * d, dotP)
                }
            }

            // CHD: single dot / chord (two stacked bars)
            val cy = (laneTop[4] + laneBot[4]) / 2f
            if (chd[s]) {
                dotP.color = Hw.withAlpha(Hw.MOD_ENV, a)
                c.drawRect(cx - 5f * d, cy - 6f * d, cx + 5f * d, cy - 2f * d, dotP)
                c.drawRect(cx - 5f * d, cy + 1f * d, cx + 5f * d, cy + 5f * d, dotP)
            } else {
                dotP.color = Hw.withAlpha(Hw.ENGRAVE, if (inLen) 255 else 90)
                c.drawCircle(cx, cy, 2.6f * d, dotP)
            }
        }
        if (len < STEPS) c.drawRect(colX[len] - d, 0f, colX[len] + d * 0.5f, h, markP)
        c.drawRect(0f, 0f, w, h, frame)
    }

    // ------------------------------------------------------------------ touch

    private fun setLenFromX(x: Float) {
        val cw = (width - labelW) / STEPS
        val n = (Math.ceil(((x - labelW) / cw).toDouble()).toInt()).coerceIn(1, STEPS)
        if (n != len) { len = n; push() }
    }

    private fun paintAt(e: MotionEvent, first: Boolean) {
        if (e.x < labelW) return
        val cw = (width - labelW) / STEPS
        val col = ((e.x - labelW) / cw).toInt().coerceIn(0, STEPS - 1)
        var changed = false
        when (lane) {
            1 -> {
                if (first) paintGate = (gate[col] + 1) % 3
                if ((first || col != lastCol) && gate[col] != paintGate) { gate[col] = paintGate; changed = true }
            }
            2 -> {
                val t = laneTop[2] + 2f * d
                val b = laneBot[2] - 1f * d
                val v = (((b - e.y) / (b - t)).coerceIn(0f, 1f) * 200f / 5f).roundToInt() * 5
                if (acc[col] != v) { acc[col] = v; changed = true }
            }
            3 -> {
                val t = laneTop[3] + 5f * d
                val b = laneBot[3] - 5f * d
                val v = 2 - (((e.y - t) / (b - t)).coerceIn(0f, 1f) * 4f).roundToInt()
                if (oct[col] != v) { oct[col] = v; changed = true }
            }
            4 -> {
                if (first) paintChd = !chd[col]
                if ((first || col != lastCol) && chd[col] != paintChd) { chd[col] = paintChd; changed = true }
            }
        }
        lastCol = col
        if (changed) push()
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                parent?.requestDisallowInterceptTouchEvent(true)
                downX = e.x
                downY = e.y
                lane = laneAt(e.y)
                lenDrag = false
                lastCol = -1
                if (lane == 0) postDelayed(longPress, android.view.ViewConfiguration.getLongPressTimeout().toLong())
                else if (lane > 0) paintAt(e, true)
            }
            MotionEvent.ACTION_MOVE -> {
                if (lenDrag) setLenFromX(e.x)
                else if (lane == 0) { if (hypot(e.x - downX, e.y - downY) > slop) removeCallbacks(longPress) }
                else if (lane > 0) paintAt(e, false)
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                removeCallbacks(longPress)
                lane = -1
                lenDrag = false
                if (e.actionMasked == MotionEvent.ACTION_UP) onEdited?.invoke()
            }
        }
        return true
    }
}
