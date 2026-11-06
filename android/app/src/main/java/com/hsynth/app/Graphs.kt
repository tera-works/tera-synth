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
abstract class GraphView(ctx: Context, private val heightDp: Float) : View(ctx) {
    var onEdited: (() -> Unit)? = null
    protected val d = resources.displayMetrics.density
    protected val line = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Hw.LCD_FG; style = Paint.Style.STROKE; strokeWidth = 2f * d; strokeJoin = Paint.Join.ROUND }
    protected val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.argb(60, 174, 234, 0) }
    protected val grid = Paint().apply { color = Color.rgb(28, 44, 14); strokeWidth = 1f * d }
    protected val dot = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Hw.LCD_FG }
    protected val label = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Hw.LCD_DIM; textSize = 11f * d; typeface = Hw.MONO }
    protected val bg = Paint().apply { color = Hw.LCD_BG }
    protected val frame = Paint().apply { color = Color.rgb(4, 5, 6); style = Paint.Style.STROKE; strokeWidth = 2f * d }

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

    /** LCD screen with a faint grid. */
    protected fun screen(c: Canvas, cols: Int = 8, rows: Int = 4) {
        c.drawRect(0f, 0f, width.toFloat(), height.toFloat(), bg)
        for (i in 1 until cols) { val x = width * i / cols.toFloat(); c.drawLine(x, 0f, x, height.toFloat(), grid) }
        for (i in 1 until rows) { val y = height * i / rows.toFloat(); c.drawLine(0f, y, width.toFloat(), y, grid) }
        c.drawRect(0f, 0f, width.toFloat(), height.toFloat(), frame)
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        if (e.actionMasked == MotionEvent.ACTION_DOWN) parent?.requestDisallowInterceptTouchEvent(true)
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
    private val pad get() = 10 * d
    private val q get() = (width - 2 * pad) / 3.7f
    private fun xs(): FloatArray {
        val a = sqrt(get("$prefix.attack") / 10f).coerceIn(0f, 1f) * q
        val dd = sqrt(get("$prefix.decay") / 20f).coerceIn(0f, 1f) * q
        val r = sqrt(get("$prefix.release") / 20f).coerceIn(0f, 1f) * q
        val x0 = pad
        val xa = x0 + a
        val xd = xa + dd
        val xs = xd + 0.7f * q
        return floatArrayOf(x0, xa, xd, xs, xs + r)
    }
    private fun ys(): Pair<Float, Float> {
        val top = 12 * d
        val bot = height - 12 * d
        return top to bot
    }

    override fun onDraw(c: Canvas) {
        screen(c)
        val x = xs()
        val (top, bot) = ys()
        val sus = bot - (bot - top) * get("$prefix.sustain").coerceIn(0f, 1f)
        val p = Path().apply {
            moveTo(x[0], bot); lineTo(x[1], top); lineTo(x[2], sus); lineTo(x[3], sus); lineTo(x[4], bot)
        }
        c.drawPath(Path(p).apply { close() }, fill)
        c.drawPath(p, line)
        for ((px, py) in listOf(x[1] to top, x[2] to sus, x[4] to bot)) c.drawCircle(px, py, 5 * d, dot)
        c.drawText("A", x[1] - 4 * d, top - 2 * d + 10 * d, label)
        c.drawText("D/S", x[2] + 6 * d, sus - 6 * d, label)
        c.drawText("R", x[4] + 6 * d, bot - 6 * d, label)
    }

    override fun touch(e: MotionEvent): Boolean {
        val x = xs()
        val (top, bot) = ys()
        val sus = bot - (bot - top) * get("$prefix.sustain")
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                val pts = listOf(x[1] to top, x[2] to sus, x[4] to bot)
                drag = pts.indices.minByOrNull { hypot(pts[it].first - e.x, pts[it].second - e.y) } ?: -1
            }
            MotionEvent.ACTION_MOVE -> when (drag) {
                0 -> set("$prefix.attack", (((e.x - x[0]) / q).coerceIn(0f, 1f)).pow(2) * 10f)
                1 -> {
                    set("$prefix.decay", (((e.x - x[1]) / q).coerceIn(0f, 1f)).pow(2) * 20f)
                    set("$prefix.sustain", ((bot - e.y) / (bot - top)).coerceIn(0f, 1f))
                }
                2 -> set("$prefix.release", (((e.x - x[3]) / q).coerceIn(0f, 1f)).pow(2) * 20f)
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

    private fun fx(f: Float) = width * (log10(f / 20f) / 3f)
    private fun fy(db: Float) = height * (1f - (db - minDb) / (maxDb - minDb))

    private fun magDb(type: String, f: Float, fc: Float, reso: Float, morph: Float): Float {
        val w = f / fc
        val q = 0.55f + reso * 9f
        fun lp2(): Double { val re = 1.0 - w * w; val im = w / q.toDouble(); return 1.0 / sqrt(re * re + im * im) }
        fun hp2(): Double = lp2() * w * w
        fun bp2(): Double = lp2() * (w / q)
        val m: Double = when (type) {
            "off" -> 1.0
            "lp12" -> lp2()
            "lp24", "ladder" -> { val a = lp2(); val q2 = 0.71; val re = 1.0 - w * w; val im = w / q2; a / sqrt(re * re + im * im) }
            "hp" -> hp2()
            "bp" -> bp2() * q
            "notch" -> lp2() * abs(1.0 - w * w)
            "comb+", "comb-" -> {
                val g = reso * 0.95
                val ph = 2.0 * PI * f / fc
                val s = if (type == "comb+") 1.0 else -1.0
                sqrt((1 + s * g * cos(ph)).pow(2) + (s * g * sin(ph)).pow(2)) / (1 + g)
            }
            "formant" -> {
                // vowel formants a-e-i-o-u (F1, F2), interpolated by morph
                val f1 = floatArrayOf(730f, 530f, 270f, 570f, 300f)
                val f2 = floatArrayOf(1090f, 1840f, 2290f, 840f, 870f)
                val t = morph * 4f
                val i = t.toInt().coerceIn(0, 3)
                val k = t - i
                val a = f1[i] + (f1[i + 1] - f1[i]) * k
                val b = f2[i] + (f2[i + 1] - f2[i]) * k
                fun peak(c: Float): Double { val ww = f / c; val qq = 6.0; val re = 1.0 - ww * ww; val im = ww / qq; return (ww / qq) / sqrt(re * re + im * im) }
                (peak(a) + 0.7 * peak(b)) * 1.2
            }
            else -> lp2()
        }
        return (20 * log10(max(m, 1e-5))).toFloat()
    }

    override fun onDraw(c: Canvas) {
        screen(c, 3, 6)
        val type = meta("filter.type")?.let { it.options.getOrNull(get("filter.type").roundToInt()) } ?: "lp24"
        val fc = get("filter.cutoff")
        val reso = get("filter.reso")
        val morph = get("filter.morph")
        val p = Path()
        val steps = 160
        for (i in 0..steps) {
            val f = 20f * 1000f.pow(i / steps.toFloat())
            val y = fy(magDb(type, f, fc, reso, morph).coerceIn(minDb - 6, maxDb))
            if (i == 0) p.moveTo(fx(f), y) else p.lineTo(fx(f), y)
        }
        val area = Path(p).apply { lineTo(width.toFloat(), height.toFloat()); lineTo(0f, height.toFloat()); close() }
        c.drawPath(area, fill)
        c.drawPath(p, line)
        c.drawLine(0f, fy(0f), width.toFloat(), fy(0f), grid)
        for ((f, t) in listOf(100f to "100", 1000f to "1k", 10000f to "10k")) c.drawText(t, fx(f) + 3 * d, height - 4 * d, label)
        c.drawText("${type.uppercase()}  ${meta("filter.cutoff")?.format(fc) ?: ""}", 6 * d, 14 * d, label)
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
    override fun onDraw(c: Canvas) {
        screen(c, 4, 2)
        val wave = meta("lfo$n.wave")?.options?.getOrNull(get("lfo$n.wave").roundToInt()) ?: "sine"
        val mid = height / 2f
        val amp = height * 0.38f
        val p = Path()
        val rnd = java.util.Random(7)
        var sh = 0f
        val steps = 200
        for (i in 0..steps) {
            val t = 2f * i / steps
            val ph = t - t.toInt()
            if (wave == "s&h" && (i == 0 || (t * 4).toInt() != ((t - 2f / steps) * 4).toInt())) sh = rnd.nextFloat() * 2 - 1
            val v = when (wave) {
                "triangle" -> 1f - 4f * abs(ph - 0.5f)
                "saw" -> 1f - 2f * ph
                "square" -> if (ph < 0.5f) 1f else -1f
                "s&h" -> sh
                else -> sin(2 * PI * ph).toFloat()
            }
            val x = width * i / steps.toFloat()
            if (i == 0) p.moveTo(x, mid - v * amp) else p.lineTo(x, mid - v * amp)
        }
        c.drawPath(p, line)
        val sync = meta("lfo$n.sync")?.let { it.options.getOrNull(get("lfo$n.sync").roundToInt()) } ?: "off"
        val rate = if (sync != "off") sync else meta("lfo$n.rate")?.format(get("lfo$n.rate")) ?: ""
        c.drawText("LFO$n  ${wave.uppercase()}  $rate", 6 * d, 14 * d, label)
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
    var numOps = 4
        private set

    override fun onDraw(c: Canvas) {
        screen(c, 1, 1)
        val algo = NativeEngine.nativeFmAlgorithm(get("$layer.fm.algo").roundToInt())
        numOps = algo[0]
        val carriers = algo[1]
        val fb = algo[2]
        val mods = IntArray(8) { algo[3 + it] }
        // level: carriers 0, a modulator one above the highest op it modulates
        val level = IntArray(numOps)
        repeat(numOps) {
            for (j in 0 until numOps) for (i in 0 until numOps)
                if (i != j && (mods[i] shr j) and 1 == 1) level[j] = max(level[j], level[i] + 1)
        }
        val rows = (level.maxOrNull() ?: 0) + 1
        val bw = 34 * d
        val bh = 24 * d
        val top = 14 * d
        val rowH = (height - top - 26 * d) / max(1, rows)
        for (r in 0 until rows) {
            val ops = (0 until numOps).filter { level[it] == r }
            ops.forEachIndexed { k, op ->
                val cx = width * (k + 1f) / (ops.size + 1f)
                val cy = top + (rows - 1 - r) * rowH + rowH / 2
                boxes[op].set(cx - bw / 2, cy - bh / 2, cx + bw / 2, cy + bh / 2)
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
            val r = RectF(b.right - 4 * d, b.top - 10 * d, b.right + 12 * d, b.centerY())
            c.drawArc(r, 180f, 270f, false, line)
        }
        for (i in 0 until numOps) {
            val carrier = (carriers shr i) and 1 == 1
            box.color = if (i == selected) Hw.LED else if (carrier) Color.rgb(60, 90, 20) else Color.rgb(30, 44, 16)
            c.drawRoundRect(boxes[i], 3 * d, 3 * d, box)
            txt.color = if (i == selected) Color.BLACK else Hw.LCD_FG
            c.drawText("${i + 1}", boxes[i].centerX(), boxes[i].centerY() + 5 * d, txt)
        }
        val name = meta("$layer.fm.algo")?.options?.getOrNull(get("$layer.fm.algo").roundToInt()) ?: ""
        c.drawText("ALG $name", 6 * d, 12 * d, label)
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
 */
class StepSeqView(ctx: Context) : GraphView(ctx, 230f) {
    private val names = arrayOf("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
    private val laneNote = 0.44f
    private val laneVel = 0.34f
    private var lane = -1
    private val steps get() = get("seq.steps").roundToInt().coerceIn(1, 32)
    private val labelW get() = 34 * d

    private fun laneTop(i: Int) = height * when (i) { 0 -> 0f; 1 -> laneNote; else -> laneNote + laneVel }
    private fun laneBot(i: Int) = height * when (i) { 0 -> laneNote; 1 -> laneNote + laneVel; else -> 1f }

    override fun onDraw(c: Canvas) {
        c.drawRect(0f, 0f, width.toFloat(), height.toFloat(), bg)
        val n = steps
        val sw = (width - labelW) / n
        val bar = Paint().apply { color = Hw.LCD_FG }
        val rest = Paint().apply { color = Color.rgb(40, 54, 22) }
        for (l in 0..2) {
            c.drawLine(0f, laneBot(l), width.toFloat(), laneBot(l), grid)
            c.drawText(arrayOf("NOTE", "VEL", "LEN")[l], 4 * d, laneTop(l) + 14 * d, label)
        }
        for (s in 0 until n) {
            val x0 = labelW + s * sw
            if (s % 4 == 0) c.drawLine(x0, 0f, x0, height.toFloat(), grid)
            val vel = get("seq.s${s + 1}.vel")
            val note = get("seq.s${s + 1}.note").roundToInt()
            val len = get("seq.s${s + 1}.len")
            // note: position in 24..96
            val nt = laneTop(0) + 18 * d
            val nb = laneBot(0) - 4 * d
            val ny = nb - (nb - nt) * ((note - 24) / 72f).coerceIn(0f, 1f)
            val r = RectF(x0 + 2 * d, ny - 4 * d, x0 + sw - 2 * d, ny + 4 * d)
            c.drawRect(r, if (vel > 0f) bar else rest)
            if (sw > 20 * d) c.drawText(names[note % 12] + (note / 12 - 1), x0 + 2 * d, nb + 0 * d - (nb - nt) * 0f, label.apply { textSize = 9f * d })
            label.textSize = 11f * d
            // velocity
            val vt = laneTop(1) + 4 * d
            val vb = laneBot(1) - 2 * d
            if (vel > 0f) c.drawRect(x0 + 2 * d, vb - (vb - vt) * vel, x0 + sw - 2 * d, vb, bar)
            else c.drawRect(x0 + 2 * d, vb - 2 * d, x0 + sw - 2 * d, vb, rest)
            // length (0.1..8 steps)
            val lt = laneTop(2) + 4 * d
            val lb = laneBot(2) - 2 * d
            c.drawRect(x0 + 2 * d, lb - (lb - lt) * (len / 2f).coerceIn(0f, 1f), x0 + sw - 2 * d, lb, if (vel > 0f) fill.apply { alpha = 140 } else rest)
        }
        c.drawRect(0f, 0f, width.toFloat(), height.toFloat(), frame)
    }

    override fun touch(e: MotionEvent): Boolean {
        val n = steps
        val sw = (width - labelW) / n
        if (e.actionMasked == MotionEvent.ACTION_DOWN) lane = (0..2).firstOrNull { e.y >= laneTop(it) && e.y < laneBot(it) } ?: -1
        if (e.actionMasked == MotionEvent.ACTION_DOWN || e.actionMasked == MotionEvent.ACTION_MOVE) {
            if (e.x < labelW || lane < 0) return true
            val s = ((e.x - labelW) / sw).toInt().coerceIn(0, n - 1) + 1
            val y = e.y.coerceIn(laneTop(lane), laneBot(lane))
            when (lane) {
                0 -> {
                    val nt = laneTop(0) + 18 * d
                    val nb = laneBot(0) - 4 * d
                    set("seq.s$s.note", (24 + ((nb - y) / (nb - nt)).coerceIn(0f, 1f) * 72f).roundToInt().toFloat())
                    if (get("seq.s$s.vel") <= 0f) set("seq.s$s.vel", 0.8f)
                }
                1 -> {
                    val vt = laneTop(1) + 4 * d
                    val vb = laneBot(1) - 2 * d
                    val v = ((vb - y) / (vb - vt)).coerceIn(0f, 1f)
                    set("seq.s$s.vel", if (v < 0.06f) 0f else v)
                }
                2 -> {
                    val lt = laneTop(2) + 4 * d
                    val lb = laneBot(2) - 2 * d
                    set("seq.s$s.len", (((lb - y) / (lb - lt)).coerceIn(0.05f, 1f) * 2f))
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

    private fun times(): FloatArray {
        val t = FloatArray(np)
        for (i in 1 until np) t[i] = t[i - 1] + max(0f, get("mseg.p${i + 1}.time"))
        return t
    }
    private fun totalScale(t: FloatArray) = max(0.5f, t.last() * 1.15f)
    private fun px(t: Float) = pad + (width - 2 * pad) * (t / scale)
    private fun py(l: Float) = height - pad - (height - 2 * pad) * l.coerceIn(0f, 1f)

    override fun onDraw(c: Canvas) {
        screen(c)
        val t = times()
        if (drag < 0) scale = totalScale(t)
        val p = Path()
        p.moveTo(px(0f), py(get("mseg.p1.level")))
        for (i in 1 until np) {
            val a = get("mseg.p$i.level")
            val b = get("mseg.p${i + 1}.level")
            val cv = get("mseg.p${i + 1}.curve")
            for (k in 1..24) {
                var x = k / 24f
                x = if (cv > 0f) x.pow(1f + 4f * cv) else if (cv < 0f) 1f - (1f - x).pow(1f - 4f * cv) else x
                p.lineTo(px(t[i - 1] + (t[i] - t[i - 1]) * k / 24f), py(a + (b - a) * x))
            }
        }
        c.drawPath(p, line)
        val loop = get("mseg.loop").roundToInt()
        if (loop in 1 until np) {
            val lx = px(t[loop])
            c.drawLine(lx, 0f, lx, height.toFloat(), Paint().apply { color = Hw.LED; strokeWidth = 1.5f * d })
            c.drawText("LOOP", lx + 3 * d, 12 * d, label)
        }
        for (i in 0 until np) c.drawCircle(px(t[i]), py(get("mseg.p${i + 1}.level")), 5 * d, dot)
        for (i in 1 until np) {
            val mx = (px(t[i - 1]) + px(t[i])) / 2
            val my = (py(get("mseg.p$i.level")) + py(get("mseg.p${i + 1}.level"))) / 2
            c.drawCircle(mx, my, 3 * d, fill.apply { alpha = 200 })
        }
        c.drawText(if (get("mseg.beats") > 0.5f) "%.2f beats".format(t.last()) else "%.2f s".format(t.last()), width - 70 * d, height - 6 * d, label)
    }

    override fun touch(e: MotionEvent): Boolean {
        val t = times()
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                scale = totalScale(t)
                var best = -1
                var bestD = 28 * d
                for (i in 0 until np) {
                    val dd = hypot(px(t[i]) - e.x, py(get("mseg.p${i + 1}.level")) - e.y)
                    if (dd < bestD) { bestD = dd; best = i }
                }
                if (best < 0) for (i in 1 until np) {
                    val mx = (px(t[i - 1]) + px(t[i])) / 2
                    val my = (py(get("mseg.p$i.level")) + py(get("mseg.p${i + 1}.level"))) / 2
                    val dd = hypot(mx - e.x, my - e.y)
                    if (dd < bestD) { bestD = dd; best = 100 + i }
                }
                drag = best
            }
            MotionEvent.ACTION_MOVE -> {
                if (drag in 0..7) {
                    val i = drag
                    set("mseg.p${i + 1}.level", ((height - pad - e.y) / (height - 2 * pad)).coerceIn(0f, 1f))
                    if (i > 0) {
                        val tt = ((e.x - pad) / (width - 2 * pad) * scale).coerceAtLeast(t[i - 1])
                        set("mseg.p${i + 1}.time", (tt - t[i - 1]).coerceIn(0f, 10f))
                    }
                } else if (drag >= 100) {
                    val i = drag - 100
                    val a = py(get("mseg.p$i.level"))
                    val b = py(get("mseg.p${i + 1}.level"))
                    val mid = (a + b) / 2
                    // finger above the straight line = slow start (positive curve) when rising
                    val dir = if (b < a) 1f else -1f
                    set("mseg.p${i + 1}.curve", (((e.y - mid) / (60 * d)) * dir).coerceIn(-1f, 1f))
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
    override fun onMeasure(widthSpec: Int, heightSpec: Int) {
        val w = if (MeasureSpec.getMode(widthSpec) == MeasureSpec.EXACTLY) MeasureSpec.getSize(widthSpec) else (150 * d).toInt()
        setMeasuredDimension(w, (34 * d).toInt())
    }
    override fun onDraw(c: Canvas) {
        c.drawRect(0f, 0f, width.toFloat(), height.toFloat(), bg)
        val v = get(name)
        val mid = width / 2f
        val x = mid + v * (width / 2f - 4 * d)
        c.drawRect(min(mid, x), 8 * d, max(mid, x), height - 8 * d, Paint().apply { color = if (v == 0f) Hw.LCD_DIM else Hw.LCD_FG })
        c.drawLine(mid, 4 * d, mid, height - 4 * d, grid)
        c.drawText("%+.2f".format(v), 6 * d, height - 10 * d, label.apply { color = Color.rgb(230, 240, 200) })
        label.color = Hw.LCD_DIM
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
