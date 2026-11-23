package com.hsynth.app

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.view.MotionEvent
import android.view.View
import android.widget.LinearLayout
import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.ln
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow
import kotlin.math.roundToInt
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * Phase 3 A1 — `Physical` layer UI (EDIT > OSC): model chips + interactive resonance view + audition column.
 *
 * Parameter-name contract (engine group `"Layer A / Physical"`, same for B / C): every name lives in [PHYS_MAP] so a
 * rename is a one-line change, and every view degrades gracefully (`Params.metaOf(n) ?: skip`).
 *
 * This file has no compile dependency on the not-yet-existing `nativePhysModes` JNI call: the mode table is injected as
 * a lambda (see [PhysicalModelView.modesProvider]); without it (or when it returns 0 / throws) the resonance view
 * draws an analytic "preview" instead.
 *
 * Views are deliberately NOT registered with `Params.addListener`: `Params.notifyChanged()` iterates that list and
 * `EditPanel` rebuilds its whole page (constructing views like these) from inside such a listener, so registering from
 * a constructor would modify the list during iteration. Hosts call [PhysicalModelView.refresh] instead; the views also
 * re-read the engine whenever they are attached to a window.
 */
object PHYS_MAP {
    const val MAX_MODES = 16

    /** Engine group of a layer prefix ("a" → "Layer A / Physical"). */
    fun group(p: String) = "Layer ${p.uppercase()} / Physical"

    fun model(p: String) = "$p.phys.model"          // enum: string, modal, tine, reed
    fun exciter(p: String) = "$p.phys.exciter"      // enum: pluck, mallet, hammer, bow
    fun decay(p: String) = "$p.phys.decay"
    fun damp(p: String) = "$p.phys.damp"
    fun bright(p: String) = "$p.phys.bright"
    fun pos(p: String) = "$p.phys.pos"
    fun hardness(p: String) = "$p.phys.hardness"
    fun inharm(p: String) = "$p.phys.inharm"
    fun dispersion(p: String) = "$p.phys.dispersion"
    fun pickup(p: String) = "$p.phys.pickup"
    fun drive(p: String) = "$p.phys.drive"
    fun body(p: String) = "$p.phys.body"
    fun bowpress(p: String) = "$p.phys.bowpress"

    val MODELS = listOf("string", "modal", "tine", "reed")
    val EXCITERS = listOf("pluck", "mallet", "hammer", "bow")

    /** One quick chip = a (model, exciter) pair. */
    class Quick(val label: String, val model: String, val exciter: String)

    val QUICK = listOf(
        Quick("PLUCK", "string", "pluck"),
        Quick("BOW", "string", "bow"),
        Quick("MODAL", "modal", "mallet"),
        Quick("TINE EP", "tine", "hammer"),
        Quick("REED EP", "reed", "hammer"),
    )

    /** Index of option [name] in the engine's enum (falls back to the canonical order when the meta is missing). */
    fun optionIndex(m: ParamMeta?, name: String, canon: List<String>): Int {
        val i = m?.options?.indexOfFirst { it.equals(name, ignoreCase = true) } ?: -1
        return if (i >= 0) i else canon.indexOf(name)
    }

    fun optionName(m: ParamMeta?, idx: Int, canon: List<String>): String =
        m?.options?.getOrNull(idx) ?: canon.getOrElse(idx) { "?" }
}

// ====================================================================================================== chips

/** Five model chips drawn by ONE view: 22 × 22 dp vector glyph + label each; selected = accent 36 % fill. */
@SuppressLint("ViewConstructor", "ClickableViewAccessibility")
private class ModelChipRow(ctx: Context, private val onPick: (Int) -> Unit) : View(ctx) {
    var selected = -1
        set(v) { if (field != v) { field = v; invalidate() } }

    private val d = resources.displayMetrics.density
    private val n = PHYS_MAP.QUICK.size
    private val gap = 6f * d
    private val glyph = 22f * d
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)
    private val gp = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 1.5f * d
        strokeCap = Paint.Cap.ROUND
        strokeJoin = Paint.Join.ROUND
    }
    private val t = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Hw.BOLD; textSize = 12f * d; letterSpacing = 0.05f }
    private val r = RectF()
    private val glyphs = Array(n) { Path() }   // built once, never in onDraw
    private var chipW = 0f
    private var baseline = 0f
    private var down = -1

    init {
        isClickable = true
        val s = d
        // 0 PLUCK: string pulled aside, pick dot at the apex
        glyphs[0].apply {
            moveTo(2 * s, 16 * s); lineTo(11 * s, 5 * s); lineTo(20 * s, 16 * s)
            addCircle(11 * s, 5 * s, 1.6f * s, Path.Direction.CW)
        }
        // 1 BOW: horizontal string crossed by a slanted bow
        glyphs[1].apply {
            moveTo(2 * s, 15 * s); lineTo(20 * s, 15 * s)
            moveTo(5 * s, 3 * s); lineTo(17 * s, 19 * s)
        }
        // 2 MODAL: free bar on two nodal supports
        glyphs[2].apply {
            addRoundRect(RectF(2 * s, 7 * s, 20 * s, 12 * s), 1.5f * s, 1.5f * s, Path.Direction.CW)
            moveTo(6 * s, 12 * s); lineTo(6 * s, 17 * s)
            moveTo(16 * s, 12 * s); lineTo(16 * s, 17 * s)
        }
        // 3 TINE: vertical tine + coil pickup either side
        glyphs[3].apply {
            moveTo(11 * s, 2 * s); lineTo(11 * s, 14 * s)
            addCircle(11 * s, 17 * s, 3f * s, Path.Direction.CW)
            moveTo(5 * s, 6 * s); lineTo(5 * s, 12 * s)
            moveTo(17 * s, 6 * s); lineTo(17 * s, 12 * s)
        }
        // 4 REED: clamped tapered blade
        glyphs[4].apply {
            moveTo(3 * s, 13 * s); lineTo(19 * s, 9 * s); lineTo(19 * s, 12 * s); lineTo(3 * s, 16 * s); close()
            addRect(RectF(2 * s, 11 * s, 5 * s, 18 * s), Path.Direction.CW)
        }
    }

    override fun onMeasure(widthSpec: Int, heightSpec: Int) {
        setMeasuredDimension(resolveSize((600 * d).toInt(), widthSpec), resolveSize((38 * d).toInt(), heightSpec))
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        chipW = (w - gap * (n - 1)) / n
        // shrink the label font if the widest label does not fit next to the glyph
        t.textSize = 12f * d
        var widest = 1f
        for (q in PHYS_MAP.QUICK) widest = max(widest, t.measureText(q.label))
        val room = chipW - glyph - 16f * d
        if (widest > room && room > 0f) t.textSize = 12f * d * (room / widest)
        baseline = h / 2f - (t.ascent() + t.descent()) / 2f
    }

    override fun onDraw(c: Canvas) {
        val h = height.toFloat()
        for (i in 0 until n) {
            val l = i * (chipW + gap)
            r.set(l + 0.5f * d, 0.5f * d, l + chipW - 0.5f * d, h - 0.5f * d)
            val sel = i == selected
            p.style = Paint.Style.FILL
            p.color = if (sel) Hw.withAlpha(Hw.MOD_OSC, 92) else if (i == down) Hw.PANEL_RAISED else Hw.KEY
            c.drawRoundRect(r, 8f * d, 8f * d, p)
            p.style = Paint.Style.STROKE
            p.strokeWidth = d
            p.color = if (sel) Hw.withAlpha(Hw.MOD_OSC, 200) else Hw.PANEL_EDGE
            c.drawRoundRect(r, 8f * d, 8f * d, p)

            gp.color = if (sel) Hw.SILK else Hw.ENGRAVE
            c.save()
            c.translate(l + 10f * d, (h - glyph) / 2f)
            c.drawPath(glyphs[i], gp)
            c.restore()

            t.color = if (sel) Hw.SILK else Hw.ENGRAVE
            c.drawText(PHYS_MAP.QUICK[i].label, l + 10f * d + glyph + 6f * d, baseline, t)
        }
    }

    private fun chipAt(x: Float): Int {
        if (chipW <= 0f) return -1
        val i = (x / (chipW + gap)).toInt()
        return if (i in 0 until n) i else -1
    }

    override fun onTouchEvent(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> { down = chipAt(e.x); invalidate() }
            MotionEvent.ACTION_MOVE -> {}
            MotionEvent.ACTION_UP -> {
                val hit = chipAt(e.x)
                val was = down
                down = -1
                invalidate()
                if (hit >= 0 && hit == was) { performClick(); onPick(hit) }
            }
            MotionEvent.ACTION_CANCEL -> { down = -1; invalidate() }
        }
        return true
    }

    override fun performClick(): Boolean { super.performClick(); return true }
}

// ====================================================================================================== resonance

/**
 * Interactive modal / resonance visualiser (zero allocations in onDraw, static unless dragged).
 *
 *  - left vertical track  **DAMP**    (`phys.damp`)
 *  - bottom horizontal track **BRIGHT** (`phys.bright`)
 *  - top "string" strip with the exciter **POSITION** marker (`phys.pos`)
 *  - centre: up to 16 modal bars (x = partial ratio, log; height = gain) each with a decay tail hanging below the
 *    baseline whose length ∝ T60 and which fades to 15 % alpha.
 *
 * Mode data: when [modesProvider] returns n > 0 the first n triples `[ratio, gain, t60]` of [modesBuf] are drawn
 * (the engine's truth); otherwise an analytic model of the current parameters is drawn and labelled "preview".
 * Dragging writes the parameters directly and takes one `History.checkpoint()` on DOWN.
 */
@SuppressLint("ViewConstructor", "ClickableViewAccessibility")
class ResonanceView(
    ctx: Context,
    private val layerPrefix: String,
    private val modesProvider: (() -> Int)? = null,
    private val modesBuf: FloatArray = FloatArray(PHYS_MAP.MAX_MODES * 3),
) : View(ctx) {
    /** Called once when a drag ends (hosts refresh the knobs below). */
    var onEdited: (() -> Unit)? = null

    var accent: Int = Hw.MOD_OSC
        set(v) { if (field != v) { field = v; invalidate() } }

    private val d = resources.displayMetrics.density
    private val mModel = Params.metaOf(PHYS_MAP.model(layerPrefix))
    private val mDecay = Params.metaOf(PHYS_MAP.decay(layerPrefix))
    private val mDamp = Params.metaOf(PHYS_MAP.damp(layerPrefix))
    private val mBright = Params.metaOf(PHYS_MAP.bright(layerPrefix))
    private val mPos = Params.metaOf(PHYS_MAP.pos(layerPrefix))
    private val mInharm = Params.metaOf(PHYS_MAP.inharm(layerPrefix))

    // cached normalised values (0..1)
    private var nDecay = 0.5f
    private var nDamp = 0.3f
    private var nBright = 0.6f
    private var nPos = 0.25f
    private var nInharm = 0f
    private var model = 0

    // mode table currently displayed
    private val maxLive = min(PHYS_MAP.MAX_MODES, modesBuf.size / 3).coerceAtLeast(1)
    private val fb = FloatArray(PHYS_MAP.MAX_MODES * 3)
    private var src: FloatArray = fb
    private var count = 0
    private var live = false
    private var lnMax = ln(8f)
    private var gMax = 1f
    private var tMax = 1f

    // paints / geometry (all preallocated)
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)
    private val txt = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Hw.BOLD; textSize = 8.5f * d; letterSpacing = 0.06f }
    private val r = RectF()
    private val gutter = 34f * d
    private val stripH = 20f * d
    private val bottomH = 26f * d
    private val hitR = 22f * d
    private val handleR = 5f * d
    private var cL = 0f; private var cR = 0f; private var cT = 0f; private var cB = 0f; private var base = 0f
    private var dampX = 0f; private var dampT = 0f; private var dampB = 0f
    private var posL = 0f; private var posR = 0f; private var posY = 0f
    private var brX0 = 0f; private var brX1 = 0f; private var brY = 0f

    // drag state
    private var drag = -1     // 0 pos, 1 damp, 2 bright
    private var grab = 0f

    init {
        isClickable = true
        readParams()
        refreshModes()
    }

    /** Re-read the engine parameters + mode table and redraw. */
    fun refresh() {
        readParams()
        refreshModes()
        invalidate()
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        refresh()
    }

    private fun norm(m: ParamMeta?, fallback: Float): Float =
        if (m == null) fallback else m.toNormalized(NativeEngine.nativeGetParam(m.id)).coerceIn(0f, 1f)

    private fun readParams() {
        nDecay = norm(mDecay, nDecay)
        nDamp = norm(mDamp, nDamp)
        nBright = norm(mBright, nBright)
        nPos = norm(mPos, nPos)
        nInharm = norm(mInharm, nInharm)
        model = mModel?.let { NativeEngine.nativeGetParam(it.id).roundToInt() } ?: 0
    }

    // ---------------------------------------------------------------- mode data

    private fun refreshModes() {
        val n = try { modesProvider?.invoke() ?: 0 } catch (_: Throwable) { 0 }   // JNI not there yet → preview
        if (n > 0) {
            src = modesBuf
            count = min(n, maxLive)
            live = true
        } else {
            buildPreview()
            src = fb
            count = PHYS_MAP.MAX_MODES
            live = false
        }
        var rMax = 4f
        var g = 1e-6f
        var t = 1e-3f
        for (i in 0 until count) {
            rMax = max(rMax, src[i * 3])
            g = max(g, src[i * 3 + 1])
            t = max(t, src[i * 3 + 2])
        }
        lnMax = ln(max(rMax, 4f))
        gMax = g
        tMax = t
    }

    /** Analytic stand-in: gain ∝ |sin(kπ·pos)|·b^(k−1), T60 shrinks with damping and partial number. */
    private fun buildPreview() {
        val name = PHYS_MAP.optionName(mModel, model, PHYS_MAP.MODELS).lowercase()
        val posN = 0.02f + 0.96f * nPos
        val b = 0.35f + 0.6f * nBright
        val inh = nInharm * 0.004f
        for (k in 1..PHYS_MAP.MAX_MODES) {
            val ratio = when (name) {
                "modal" -> if (k == 1) 1f else ((k + 0.5f) * 3.1415927f / 4.730f).pow(2)        // free-free bar
                "tine", "reed" -> when (k) {                                                      // cantilever
                    1 -> 1f
                    2 -> 6.267f
                    else -> ((k - 0.5f) * 3.1415927f / 1.875f).pow(2)
                }
                else -> k * sqrt(1f + inh * k * k)                                                // stiff string
            }
            val gain = max(0.04f, abs(sin(k * 3.1415927f * posN))) * b.pow(k - 1)
            val tail = (0.25f + 0.75f * nDecay) * (1f - 0.75f * nDamp) / (1f + 0.55f * (k - 1) * (0.25f + nDamp))
            fb[(k - 1) * 3] = ratio
            fb[(k - 1) * 3 + 1] = gain
            fb[(k - 1) * 3 + 2] = tail
        }
    }

    // ---------------------------------------------------------------- layout

    override fun onMeasure(widthSpec: Int, heightSpec: Int) {
        setMeasuredDimension(resolveSize((690 * d).toInt(), widthSpec), resolveSize((130 * d).toInt(), heightSpec))
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        val pad = 6f * d
        cL = gutter + pad
        cR = w - pad
        cT = stripH + 4f * d
        cB = h - bottomH
        base = cT + 0.62f * (cB - cT)
        dampX = gutter / 2f
        dampT = cT + 4f * d
        dampB = cB - 2f * d
        posL = cL + 4f * d
        posR = cR - 4f * d
        posY = stripH / 2f
        brX0 = gutter + 50f * d
        brX1 = w - 14f * d
        brY = h - bottomH / 2f + 1f * d
    }

    private fun posX() = posL + nPos * (posR - posL)
    private fun dampY() = dampB - nDamp * (dampB - dampT)
    private fun brightX() = brX0 + nBright * (brX1 - brX0)

    // ---------------------------------------------------------------- draw

    override fun onDraw(c: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        if (w <= 0f || h <= 0f) return

        // well + frame
        r.set(0f, 0f, w, h)
        p.style = Paint.Style.FILL
        p.color = Hw.WELL
        c.drawRoundRect(r, 6f * d, 6f * d, p)
        p.style = Paint.Style.STROKE
        p.strokeWidth = d
        p.color = if (drag >= 0) Hw.withAlpha(accent, 160) else Hw.PANEL_EDGE
        r.inset(d / 2f, d / 2f)
        c.drawRoundRect(r, 6f * d, 6f * d, p)

        // baseline
        p.color = Hw.PANEL_EDGE
        p.strokeWidth = d
        c.drawLine(cL, base, cR, base, p)

        // modal bars + decay tails
        val span = cR - cL - 8f * d
        val barMax = base - cT - 3f * d
        val tailMax = cB - base - 2f * d
        p.strokeCap = Paint.Cap.ROUND
        for (i in 0 until count) {
            val ratio = max(src[i * 3], 1f)
            val x = cL + 4f * d + (ln(ratio) / lnMax).coerceIn(0f, 1f) * span
            val bh = (src[i * 3 + 1] / gMax).coerceIn(0f, 1f) * barMax
            p.strokeWidth = 3f * d
            p.color = Hw.withAlpha(accent, 225)
            c.drawLine(x, base, x, base - max(bh, 1.5f * d), p)

            val tl = (src[i * 3 + 2] / tMax).coerceIn(0f, 1f) * tailMax
            if (tl > 1f) {
                p.strokeWidth = d
                p.strokeCap = Paint.Cap.BUTT
                val y1 = base + tl * 0.34f
                val y2 = base + tl * 0.67f
                val y3 = base + tl
                p.color = Hw.withAlpha(accent, 150); c.drawLine(x, base + 1f * d, x, y1, p)
                p.color = Hw.withAlpha(accent, 85); c.drawLine(x, y1, x, y2, p)
                p.color = Hw.withAlpha(accent, 38); c.drawLine(x, y2, x, y3, p)
                p.strokeCap = Paint.Cap.ROUND
            }
        }
        p.strokeCap = Paint.Cap.BUTT

        if (!live) {
            txt.color = Hw.INK_DIM
            txt.textAlign = Paint.Align.RIGHT
            c.drawText("preview", cR - 6f * d, cT + 10f * d, txt)
        }

        // POSITION strip
        p.strokeWidth = d
        p.color = if (mPos != null) Hw.PANEL_HI else Hw.PANEL_EDGE
        c.drawLine(posL, posY, posR, posY, p)
        for (i in 0..4) {
            val tx = posL + (posR - posL) * i / 4f
            c.drawLine(tx, posY - 2.5f * d, tx, posY + 2.5f * d, p)
        }
        txt.color = Hw.INK_DIM
        txt.textAlign = Paint.Align.CENTER
        c.drawText("POS", gutter / 2f, posY + 3f * d, txt)
        if (mPos != null) drawHandle(c, posX(), posY, drag == 0)

        // DAMP track
        p.color = Hw.PANEL_HI
        p.strokeWidth = 2f * d
        c.drawLine(dampX, dampT, dampX, dampB, p)
        if (mDamp != null) {
            p.color = Hw.withAlpha(accent, 150)
            c.drawLine(dampX, dampY(), dampX, dampB, p)
        }
        txt.color = Hw.INK_DIM
        txt.textAlign = Paint.Align.CENTER
        c.drawText("DAMP", gutter / 2f, h - 7f * d, txt)
        if (mDamp != null) drawHandle(c, dampX, dampY(), drag == 1)

        // BRIGHT track
        p.color = Hw.PANEL_HI
        p.strokeWidth = 2f * d
        c.drawLine(brX0, brY, brX1, brY, p)
        if (mBright != null) {
            p.color = Hw.withAlpha(accent, 150)
            c.drawLine(brX0, brY, brightX(), brY, p)
        }
        txt.color = Hw.INK_DIM
        txt.textAlign = Paint.Align.LEFT
        c.drawText("BRIGHT", gutter + 4f * d, brY + 3f * d, txt)
        if (mBright != null) drawHandle(c, brightX(), brY, drag == 2)
    }

    private fun drawHandle(c: Canvas, x: Float, y: Float, active: Boolean) {
        p.style = Paint.Style.FILL
        p.color = if (active) Color.WHITE else Hw.SILK
        c.drawCircle(x, y, handleR, p)
        p.style = Paint.Style.STROKE
        p.strokeWidth = 1.5f * d
        p.color = accent
        c.drawCircle(x, y, handleR + 1.5f * d, p)
    }

    // ---------------------------------------------------------------- touch

    private fun pick(x: Float, y: Float): Int {
        var best = -1
        var bestD = hitR
        if (mPos != null) { val dd = hypot(x - posX(), y - posY); if (dd < bestD) { bestD = dd; best = 0; grab = posX() - x } }
        if (mDamp != null) { val dd = hypot(x - dampX, y - dampY()); if (dd < bestD) { bestD = dd; best = 1; grab = dampY() - y } }
        if (mBright != null) { val dd = hypot(x - brightX(), y - brY); if (dd < bestD) { bestD = dd; best = 2; grab = brightX() - x } }
        if (best >= 0) return best
        // not on a handle: touching a track jumps the handle to the finger
        grab = 0f
        return when {
            mPos != null && y < stripH + 2f * d && x >= cL -> 0
            mDamp != null && x < gutter && y >= dampT - 8f * d && y <= dampB + 8f * d -> 1
            mBright != null && y > cB && x >= brX0 - 8f * d -> 2
            else -> -1
        }
    }

    private fun write(m: ParamMeta, n: Float) {
        val v = m.fromNormalized(n.coerceIn(0f, 1f))
        NativeEngine.setParam(m.id, v)
        Hw.onParamTouched?.invoke(m.label, m.format(v))
    }

    private fun applyDrag(x: Float, y: Float) {
        when (drag) {
            0 -> mPos?.let { nPos = ((x + grab - posL) / (posR - posL)).coerceIn(0f, 1f); write(it, nPos) }
            1 -> mDamp?.let { nDamp = ((dampB - (y + grab)) / (dampB - dampT)).coerceIn(0f, 1f); write(it, nDamp) }
            2 -> mBright?.let { nBright = ((x + grab - brX0) / (brX1 - brX0)).coerceIn(0f, 1f); write(it, nBright) }
        }
        refreshModes()
        invalidate()
    }

    override fun onTouchEvent(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                drag = pick(e.x, e.y)
                if (drag < 0) return false
                parent?.requestDisallowInterceptTouchEvent(true)
                History.checkpoint()
                applyDrag(e.x, e.y)
                return true
            }
            MotionEvent.ACTION_MOVE -> if (drag >= 0) { applyDrag(e.x, e.y); return true }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> if (drag >= 0) {
                drag = -1
                invalidate()
                onEdited?.invoke()
                return true
            }
        }
        return drag >= 0
    }
}

// ====================================================================================================== panel

/**
 * Physical-model layer block: Row 1 model chips (38 dp) · Row 2 [ResonanceView | audition column 126 dp].
 * The stock knob flow (exciter position / hardness, decay, body …) is built by `EditPanel` from the metadata.
 *
 * [modesProvider] `(part, layer, out) -> count` is injected by the host (Wave 2: `NativeEngine::nativePhysModes`);
 * `null` = analytic preview. [onModelChanged] fires after a chip changed model / exciter (the host rebuilds its page
 * because exciter-specific knobs may appear or disappear).
 */
@SuppressLint("ViewConstructor", "ClickableViewAccessibility")
class PhysicalModelView(
    private val a: MainActivity,
    private val layerIndex: Int,
    modesProvider: ((Int, Int, FloatArray) -> Int)? = null,
) : LinearLayout(a) {
    var onModelChanged: (() -> Unit)? = null

    private val prefix = "abc"[layerIndex.coerceIn(0, 2)].toString()
    private val mModel = Params.metaOf(PHYS_MAP.model(prefix))
    private val mExc = Params.metaOf(PHYS_MAP.exciter(prefix))
    private val modes = FloatArray(PHYS_MAP.MAX_MODES * 3)
    private val chips = ModelChipRow(a) { onChip(it) }
    val resonance = ResonanceView(
        a, prefix,
        modesProvider?.let { prov -> { prov(NativeEngine.nativeEditPart(), layerIndex, modes) } },
        modes,
    )
    private val testBtn = LedButton(a, "▶ TEST NOTE", tr = false, led = false, accentColor = Hw.MOD_OSC)
    private val holdSw = ToggleSwitch(a, "HOLD", tr = false, vertical = false, accent = Hw.MOD_OSC)
    private val modeLcd = Hw.lcd(a, "", 11f)
    private var sounding = false

    init {
        orientation = VERTICAL
        val d = resources.displayMetrics.density
        fun dp(v: Int) = (v * d).toInt()

        addView(chips, LayoutParams(-1, dp(38)))
        chips.visibility = if (mModel != null) VISIBLE else GONE

        val col = LinearLayout(a).apply { orientation = VERTICAL }
        testBtn.apply {
            minHeight = 0
            minimumHeight = 0
            setOnTouchListener { _, e -> onTestTouch(e); true }
        }
        holdSw.onToggle = { on -> if (!on && sounding) stopNote() }
        col.addView(testBtn, LayoutParams(-1, dp(44)))
        col.addView(holdSw, LayoutParams(-1, dp(36)).apply { topMargin = dp(6) })
        col.addView(modeLcd, LayoutParams(-1, -2).apply { topMargin = dp(6) })

        val row = LinearLayout(a).apply { orientation = HORIZONTAL }
        row.addView(resonance, LayoutParams(0, dp(130), 1f))
        row.addView(col, LayoutParams(dp(126), dp(130)).apply { marginStart = dp(8) })
        addView(row, LayoutParams(-1, dp(130)).apply { topMargin = dp(6) })

        syncChips()
    }

    /** Re-read engine state (call after a preset load / undo). */
    fun refresh() {
        syncChips()
        resonance.refresh()
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        syncChips()
    }

    override fun onDetachedFromWindow() {
        stopNote()
        super.onDetachedFromWindow()
    }

    override fun onWindowVisibilityChanged(visibility: Int) {
        if (visibility != VISIBLE) stopNote()
        super.onWindowVisibilityChanged(visibility)
    }

    // ---------------------------------------------------------------- chips

    private fun syncChips() {
        val mi = mModel?.let { NativeEngine.nativeGetParam(it.id).roundToInt() } ?: -1
        val ei = mExc?.let { NativeEngine.nativeGetParam(it.id).roundToInt() } ?: -1
        var sel = -1
        for (i in PHYS_MAP.QUICK.indices) {
            val q = PHYS_MAP.QUICK[i]
            val okM = PHYS_MAP.optionIndex(mModel, q.model, PHYS_MAP.MODELS) == mi
            val okE = mExc == null || PHYS_MAP.optionIndex(mExc, q.exciter, PHYS_MAP.EXCITERS) == ei
            if (okM && okE) { sel = i; break }
        }
        chips.selected = sel
        modeLcd.text = if (mModel == null) "" else
            PHYS_MAP.optionName(mModel, mi, PHYS_MAP.MODELS).uppercase() +
                (if (mExc == null) "" else " · " + PHYS_MAP.optionName(mExc, ei, PHYS_MAP.EXCITERS).uppercase())
    }

    private fun onChip(i: Int) {
        val q = PHYS_MAP.QUICK.getOrNull(i) ?: return
        History.checkpoint()
        mModel?.let { NativeEngine.setParam(it.id, PHYS_MAP.optionIndex(it, q.model, PHYS_MAP.MODELS).toFloat()) }
        mExc?.let { NativeEngine.setParam(it.id, PHYS_MAP.optionIndex(it, q.exciter, PHYS_MAP.EXCITERS).toFloat()) }
        refresh()
        onModelChanged?.invoke()
    }

    // ---------------------------------------------------------------- audition (key 60, velocity .8)

    private fun startNote() {
        if (sounding) return
        sounding = true
        testBtn.lit = true
        NativeEngine.noteOn(AUDITION_ID, AUDITION_KEY, 0.8f)
    }

    private fun stopNote() {
        if (!sounding) return
        sounding = false
        testBtn.lit = false
        NativeEngine.noteOff(AUDITION_ID, AUDITION_KEY)
    }

    private fun onTestTouch(e: MotionEvent) {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                parent?.requestDisallowInterceptTouchEvent(true)
                if (holdSw.on && sounding) stopNote() else startNote()
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> if (!holdSw.on) stopNote()
        }
    }

    private companion object {
        /** Reserved note id for the audition note (KeyboardView ids start at 0x100000). */
        const val AUDITION_ID = 0x7E000001
        const val AUDITION_KEY = 60
    }
}
