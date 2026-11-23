package com.hsynth.app

import android.annotation.SuppressLint
import android.app.AlertDialog
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.SystemClock
import android.text.TextUtils
import android.view.GestureDetector
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.LinearLayout
import android.widget.TextView
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt
import kotlin.math.sin

/**
 * Obsidian Instrument design system for HSynth on Pixel 7 OLED (20:9 landscape).
 *
 * True-black OLED canvas (#05070A) + hairline dark micro-surfaces + semantic module
 * color coding (Cyan OSC/Sound, Amber Filter, Violet Env/Mod, Emerald FX, Red Live/Panic).
 * All custom views avoid per-frame Paint/Shader/Rect allocations in onDraw() so UI redraws
 * never induce GC jitter during low-latency audio callbacks.
 */
object Hw {
    // Core Obsidian OLED surfaces
    val CHASSIS = Color.rgb(5, 7, 10)          // #05070A true OLED near-black
    val PANEL = Color.rgb(15, 19, 27)          // #0F131B Surface 1
    val PANEL_RAISED = Color.rgb(22, 27, 38)   // #161B26 Surface 2
    val PANEL_EDGE = Color.rgb(35, 43, 58)     // #232B3A crisp 1dp hairline
    val PANEL_HI = Color.rgb(51, 65, 85)       // #334155 hover / selected hairline
    val WELL = Color.rgb(0, 0, 0)              // #000000 pure OLED off pixels (graphs / fader wells)

    // Ink hierarchy
    val SILK = Color.rgb(230, 237, 247)        // #E6EDF7 Ink 1
    val ENGRAVE = Color.rgb(139, 151, 171)     // #8B97AB Ink 2
    val INK_DIM = Color.rgb(86, 97, 119)       // #566177 Ink 3

    // Semantic module colors
    val MOD_OSC = Color.rgb(34, 211, 238)      // #22D3EE Electric Cyan (OSC / Sound / Volume)
    val MOD_FILTER = Color.rgb(245, 158, 11)   // #F59E0B Waldorf Amber (Filter / Cutoff)
    val MOD_ENV = Color.rgb(168, 85, 247)      // #A855F7 Neon Violet (Env / LFO / Mod / Macro)
    val MOD_FX = Color.rgb(16, 185, 129)       // #10B981 Mint Emerald (FX / Reverb / Sends)
    val MOD_RED = Color.rgb(239, 68, 68)       // #EF4444 Signal Red (Live / Rec / Panic / Clip)
    val MOD_GOOD = Color.rgb(52, 211, 153)     // #34D399 Active / MIDI LED

    // Legacy aliases preserved for MonitorPanel / PlayerPanel / ChannelTableView compatibility
    val LED = MOD_OSC
    val LED_OFF = Color.rgb(32, 39, 52)
    val LCD_BG = Color.rgb(4, 6, 10)           // #04060A OLED dark readout well
    val LCD_FG = Color.rgb(230, 237, 247)      // crisp tabular white-ice readout
    val LCD_DIM = Color.rgb(112, 126, 150)
    val KEY = Color.rgb(20, 25, 35)
    val KEY_EDGE = PANEL_EDGE

    val PART_COLORS = intArrayOf(
        Color.rgb(34, 211, 238),   // Cyan
        Color.rgb(245, 158, 11),   // Amber
        Color.rgb(168, 85, 247),   // Violet
        Color.rgb(16, 185, 129),   // Emerald
        Color.rgb(56, 189, 248),   // Sky
        Color.rgb(251, 146, 60),   // Orange
        Color.rgb(244, 114, 182),  // Pink
        Color.rgb(163, 230, 53),   // Lime
    )

    fun partColor(index: Int): Int = PART_COLORS[index.and(7)]

    fun categoryColor(cat: String): Int = when (cat.trim().lowercase()) {
        "piano", "acoustic piano", "grand" -> Color.rgb(226, 232, 240)
        "e.piano", "ep", "electric piano", "keys" -> Color.rgb(251, 146, 60)
        "organ" -> Color.rgb(163, 230, 53)
        "pad", "strings", "choir", "ambient" -> Color.rgb(192, 132, 252)
        "bass" -> Color.rgb(59, 130, 246)
        "lead", "brass" -> Color.rgb(244, 114, 182)
        "synth", "polysynth", "arp", "pluck" -> Color.rgb(56, 189, 248)
        "drums", "percussion", "rhythm", "fx", "sfx" -> Color.rgb(16, 185, 129)
        else -> MOD_OSC
    }

    fun withAlpha(color: Int, alpha: Int): Int =
        Color.argb(alpha.coerceIn(0, 255), Color.red(color), Color.green(color), Color.blue(color))

    // Fonts: Rajdhani (labels, buttons) and Share Tech Mono (tabular readouts)
    lateinit var LABEL: Typeface
    lateinit var BOLD: Typeface
    lateinit var MONO: Typeface

    fun init(ctx: Context) {
        if (::LABEL.isInitialized) return
        fun load(f: String, fallback: Typeface) = try {
            Typeface.createFromAsset(ctx.assets, "fonts/$f")
        } catch (_: Exception) { fallback }
        LABEL = load("Rajdhani-SemiBold.ttf", Typeface.DEFAULT)
        BOLD = load("Rajdhani-Bold.ttf", Typeface.DEFAULT_BOLD)
        MONO = load("ShareTechMono-Regular.ttf", Typeface.MONOSPACE)
    }

    /** Shown in the status strip while a control is touched: (label, value). */
    var onParamTouched: ((String, String) -> Unit)? = null

    fun dp(ctx: Context, v: Float) = v * ctx.resources.displayMetrics.density

    /** Flat dark Obsidian card with crisp 1dp hairline border. */
    fun panelBg(ctx: Context, color: Int = PANEL, stroke: Int = PANEL_EDGE, radiusDp: Float = 8f): GradientDrawable =
        GradientDrawable().apply {
            setColor(color)
            cornerRadius = dp(ctx, radiusDp)
            setStroke(dp(ctx, 1f).toInt().coerceAtLeast(1), stroke)
        }

    /** Recessed OLED readout well (selectors, search fields, status chips). */
    fun lcdBg(ctx: Context, stroke: Int = PANEL_EDGE, radiusDp: Float = 6f): GradientDrawable =
        GradientDrawable().apply {
            setColor(LCD_BG)
            cornerRadius = dp(ctx, radiusDp)
            setStroke(dp(ctx, 1f).toInt().coerceAtLeast(1), stroke)
        }

    /** Flat precision instrument key cap. */
    fun keyBg(ctx: Context, pressed: Boolean = false, accent: Int = 0): GradientDrawable =
        GradientDrawable().apply {
            setColor(if (pressed) PANEL_RAISED else if (accent != 0) withAlpha(accent, 34) else KEY)
            cornerRadius = dp(ctx, 6f)
            setStroke(
                dp(ctx, 1f).toInt().coerceAtLeast(1),
                when {
                    pressed -> PANEL_HI
                    accent != 0 -> withAlpha(accent, 170)
                    else -> PANEL_EDGE
                }
            )
        }

    /** Instrument micro-label (uppercase for Latin text). */
    fun engraved(ctx: Context, s: String, size: Float = 10.5f, tr: Boolean = true, color: Int = ENGRAVE) = TextView(ctx).apply {
        val t = if (tr) L10n.t(s) else s
        text = if (t.any { it.code > 0x2FF }) t else t.uppercase()
        textSize = size + 0.5f
        letterSpacing = 0.08f
        setTextColor(color)
        typeface = BOLD
        maxLines = 1
        ellipsize = TextUtils.TruncateAt.END
    }

    /** Crisp tabular mono readout box on OLED dark well. */
    fun lcd(ctx: Context, s: String = "", size: Float = 13f, color: Int = LCD_FG) = TextView(ctx).apply {
        text = s
        textSize = size
        setTextColor(color)
        typeface = MONO
        background = lcdBg(ctx)
        val p = dp(ctx, 8f).toInt()
        setPadding(p, dp(ctx, 5f).toInt(), p, dp(ctx, 5f).toInt())
        maxLines = 1
        ellipsize = TextUtils.TruncateAt.END
    }

    /**
     * Obsidian panel section: dark micro-surface with hairline border and optional module accent bar.
     *
     * With [collapsible] = true the header row becomes a 26 dp tap target with a ▾ / ▸ chevron at its right end:
     * tapping toggles [collapsed], which hides / shows [body] (Phase 2 Scope, fine-tune knob drawers, …).
     * [startCollapsed] sets the initial state; [onToggle] reports user taps so callers can persist the state.
     * Without [collapsible] the class behaves exactly as before.
     */
    class Section(
        ctx: Context,
        title: String,
        tr: Boolean = true,
        accent: Int = 0,
        private val collapsible: Boolean = false,
        startCollapsed: Boolean = false,
    ) : LinearLayout(ctx) {
        val body: LinearLayout
        private val head: TextView
        private val headGap = dp(ctx, 6f).toInt()
        private val rule = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = if (accent != 0) withAlpha(accent, 90) else PANEL_EDGE
        }
        private val bar = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = accent
        }
        private val chevP = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = if (accent != 0) accent else ENGRAVE
        }
        private val chev = android.graphics.Path()
        private val hasAccent = accent != 0

        /** Collapsed state (only meaningful when [collapsible]). */
        var collapsed = false
            set(v) {
                if (!collapsible || field == v) return
                field = v
                body.visibility = if (v) GONE else VISIBLE
                (head.layoutParams as LayoutParams).bottomMargin = if (v) 0 else headGap
                head.requestLayout()
                invalidate()
            }

        /** Fired after the user toggled the header; argument = new [collapsed]. */
        var onToggle: ((Boolean) -> Unit)? = null

        init {
            orientation = VERTICAL
            background = panelBg(ctx)
            val p = dp(ctx, 10f).toInt()
            setPadding(p, dp(ctx, 7f).toInt(), p, p)
            head = engraved(ctx, title, 11f, tr, if (hasAccent) accent else SILK)
            if (hasAccent) head.setPadding(dp(ctx, 8f).toInt(), 0, 0, 0)
            if (collapsible) {
                head.minHeight = dp(ctx, 26f).toInt()
                head.gravity = Gravity.CENTER_VERTICAL
                head.isClickable = true
                head.setOnClickListener {
                    collapsed = !collapsed
                    onToggle?.invoke(collapsed)
                }
            }
            addView(head, LayoutParams(-1, -2).apply { bottomMargin = headGap })
            body = LinearLayout(ctx).apply { orientation = VERTICAL }
            addView(body, LayoutParams(-1, -2))
            if (collapsible && startCollapsed) collapsed = true
        }

        override fun dispatchDraw(c: Canvas) {
            super.dispatchDraw(c)
            val y = head.top + head.height / 2f
            if (hasAccent) {
                val bx = paddingLeft.toFloat()
                c.drawRoundRect(bx, head.top + dp(context, 2f), bx + dp(context, 3f), head.bottom - dp(context, 2f), dp(context, 1.5f), dp(context, 1.5f), bar)
            }
            val x0 = head.left + head.paddingLeft + head.paint.measureText(head.text.toString()) + dp(context, 8f)
            var x1 = width - paddingRight.toFloat()
            if (collapsible) {
                // chevron: ▾ expanded, ▸ collapsed (preallocated Path, rewound each draw)
                val s = dp(context, 4f)
                val cx = x1 - s - dp(context, 2f)
                chev.rewind()
                if (collapsed) {
                    chev.moveTo(cx - s * 0.6f, y - s)
                    chev.lineTo(cx + s * 0.8f, y)
                    chev.lineTo(cx - s * 0.6f, y + s)
                } else {
                    chev.moveTo(cx - s, y - s * 0.6f)
                    chev.lineTo(cx + s, y - s * 0.6f)
                    chev.lineTo(cx, y + s * 0.8f)
                }
                chev.close()
                c.drawPath(chev, chevP)
                x1 -= dp(context, 18f)
            }
            if (x1 > x0) c.drawRect(x0, y, x1, y + dp(context, 1f), rule)
        }
    }

    /** Convenience for [Section] with a collapsible header (Phase 2 Scope, drawers of fine-tune knobs …). */
    fun collapsibleSection(ctx: Context, title: String, tr: Boolean = true, accent: Int = 0, collapsed: Boolean = false) =
        Section(ctx, title, tr, accent, collapsible = true, startCollapsed = collapsed)

    fun sectionLp(ctx: Context) = LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = dp(ctx, 8f).toInt() }
}

/**
 * Modern pill toggle switch with OLED accent LED.
 * Zero per-frame allocations in onDraw().
 */
class ToggleSwitch(
    ctx: Context,
    label: String,
    tr: Boolean = true,
    private val vertical: Boolean = true,
    var accent: Int = Hw.MOD_OSC
) : View(ctx) {
    var on = false
        set(v) { if (field != v) { field = v; invalidate() } else { field = v } }
    var onToggle: ((Boolean) -> Unit)? = null
    private val text = if (tr) L10n.t(label) else label
    private val d = resources.displayMetrics.density
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)
    private val txt = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Hw.BOLD; color = Hw.ENGRAVE; textSize = 11.5f * d }
    private val r = RectF()

    init { isClickable = true; setOnClickListener { on = !on; onToggle?.invoke(on) } }

    override fun onMeasure(widthSpec: Int, heightSpec: Int) {
        val label = if (text.any { it.code > 0x2FF }) text else text.uppercase()
        if (vertical) {
            setMeasuredDimension(max(58 * d, txt.measureText(label) + 8 * d).toInt().coerceAtMost((112 * d).toInt()), (82 * d).toInt())
        } else {
            val w = (48 * d + txt.measureText(label) + 6 * d).toInt()
            setMeasuredDimension(if (MeasureSpec.getMode(widthSpec) == MeasureSpec.EXACTLY) MeasureSpec.getSize(widthSpec) else w, (36 * d).toInt())
        }
    }

    private fun drawPill(c: Canvas, cx: Float, cy: Float) {
        val pw = 34 * d
        val ph = 18 * d
        r.set(cx - pw / 2f, cy - ph / 2f, cx + pw / 2f, cy + ph / 2f)
        p.style = Paint.Style.FILL
        p.color = if (on) Hw.withAlpha(accent, 58) else Hw.WELL
        c.drawRoundRect(r, ph / 2f, ph / 2f, p)
        p.style = Paint.Style.STROKE
        p.strokeWidth = 1.2f * d
        p.color = if (on) accent else Hw.PANEL_EDGE
        c.drawRoundRect(r, ph / 2f, ph / 2f, p)
        p.style = Paint.Style.FILL
        val thumbR = ph * 0.34f
        val tx = if (on) r.right - ph / 2f else r.left + ph / 2f
        p.color = if (on) accent else Hw.INK_DIM
        c.drawCircle(tx, cy, thumbR, p)
    }

    override fun onDraw(c: Canvas) {
        val label = if (text.any { it.code > 0x2FF }) text else text.uppercase()
        if (vertical) {
            val cx = width / 2f
            drawPill(c, cx, 32 * d)
            txt.textAlign = Paint.Align.CENTER
            txt.color = if (on) Hw.SILK else Hw.ENGRAVE
            c.drawText(fit(label, width - 4 * d), cx, 66 * d, txt)
        } else {
            val cy = height / 2f
            drawPill(c, 20 * d, cy)
            txt.textAlign = Paint.Align.LEFT
            txt.color = if (on) Hw.SILK else Hw.ENGRAVE
            c.drawText(fit(label, width - 44 * d), 42 * d, cy + 4.2f * d, txt)
        }
    }

    private fun fit(s: String, w: Float): String {
        if (w <= 0f || txt.measureText(s) <= w) return s
        var e = s
        while (e.length > 1 && txt.measureText("$e…") > w) e = e.dropLast(1)
        return "$e…"
    }
}

/** Lays children out left to right and wraps to new lines. */
class FlowLayout(ctx: Context, private val hGap: Int = Hw.dp(ctx, 8f).toInt(), private val vGap: Int = Hw.dp(ctx, 8f).toInt()) : ViewGroup(ctx) {
    override fun onMeasure(widthSpec: Int, heightSpec: Int) {
        val maxW = MeasureSpec.getSize(widthSpec) - paddingLeft - paddingRight
        var x = 0
        var y = 0
        var lineH = 0
        var usedW = 0
        for (i in 0 until childCount) {
            val c = getChildAt(i)
            if (c.visibility == GONE) continue
            measureChild(c, MeasureSpec.makeMeasureSpec(maxW, MeasureSpec.AT_MOST), MeasureSpec.makeMeasureSpec(0, MeasureSpec.UNSPECIFIED))
            if (x > 0 && x + c.measuredWidth > maxW) {
                x = 0
                y += lineH + vGap
                lineH = 0
            }
            x += c.measuredWidth + hGap
            usedW = max(usedW, x - hGap)
            lineH = max(lineH, c.measuredHeight)
        }
        val w = if (MeasureSpec.getMode(widthSpec) == MeasureSpec.UNSPECIFIED) usedW else MeasureSpec.getSize(widthSpec)
        setMeasuredDimension(w, y + lineH + paddingTop + paddingBottom)
    }

    override fun onLayout(changed: Boolean, l: Int, t: Int, r: Int, b: Int) {
        val maxW = r - l - paddingLeft - paddingRight
        var x = 0
        var y = 0
        var lineH = 0
        for (i in 0 until childCount) {
            val c = getChildAt(i)
            if (c.visibility == GONE) continue
            if (x > 0 && x + c.measuredWidth > maxW) {
                x = 0
                y += lineH + vGap
                lineH = 0
            }
            c.layout(paddingLeft + x, paddingTop + y, paddingLeft + x + c.measuredWidth, paddingTop + y + c.measuredHeight)
            x += c.measuredWidth + hGap
            lineH = max(lineH, c.measuredHeight)
        }
    }
}

/** Flat Obsidian push button with optional semantic accent LED & active hairline highlight. */
class LedButton(
    ctx: Context,
    label: String,
    tr: Boolean = true,
    private val led: Boolean = true,
    accentColor: Int = Hw.MOD_OSC
) : TextView(ctx) {
    var accent: Int = accentColor
        set(v) { field = v; updateLook() }
    var lit = false
        set(v) { field = v; updateLook() }

    private val p = Paint(Paint.ANTI_ALIAS_FLAG)
    private val d = resources.displayMetrics.density
    private var down = false

    init {
        text = if (tr) L10n.t(label) else label
        textSize = 13.5f
        typeface = Hw.BOLD
        letterSpacing = 0.04f
        gravity = Gravity.CENTER
        maxLines = 1
        ellipsize = TextUtils.TruncateAt.END
        val lp = if (led) (18 * d).toInt() else (11 * d).toInt()
        setPadding(lp, (6 * d).toInt(), (11 * d).toInt(), (6 * d).toInt())
        minHeight = (36 * d).toInt()
        isClickable = true
        updateLook()
    }

    private fun updateLook() {
        background = Hw.keyBg(context, pressed = down, accent = if (lit) accent else 0)
        setTextColor(if (lit) Color.WHITE else Hw.SILK)
        invalidate()
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> { down = true; updateLook() }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> { down = false; updateLook() }
        }
        return super.onTouchEvent(e)
    }

    override fun onDraw(c: Canvas) {
        super.onDraw(c)
        if (!led) return
        val cx = 9 * d
        val cy = height / 2f
        if (lit) {
            p.color = Hw.withAlpha(accent, 90)
            c.drawCircle(cx, cy, 4.8f * d, p)
        }
        p.color = if (lit) accent else Hw.LED_OFF
        c.drawCircle(cx, cy, 2.7f * d, p)
    }
}

/**
 * Precision Waldorf/Obsidian 270° vector arc rotary knob.
 * Relative vertical/horizontal drag (never jumps on touch), double-tap reset, long-press MIDI learn.
 * Zero allocations in onDraw().
 */
class KnobView(
    ctx: Context,
    private val label: String,
    private val bipolar: Boolean = false,
    sizeDp: Float = 50f,
    var accent: Int = Hw.MOD_OSC
) : View(ctx) {
    var value = 0f
        set(v) { field = v.coerceIn(0f, 1f); invalidate() }
    var valueText = ""
        set(v) { field = v; invalidate() }
    var onChange: ((Float) -> Unit)? = null
    var onReset: (() -> Unit)? = null
    var onTouchState: ((Boolean) -> Unit)? = null
    var onLongPress: (() -> Unit)? = null

    /** Morph A / B end-points and live morphed position, normalised 0..1; NaN = none / hidden. */
    var morphA = Float.NaN
        private set
    var morphB = Float.NaN
        private set
    var morphNow = Float.NaN
        private set

    /** Set the morph dual-arc preview (all normalised, NaN = none). Redraws only when something changed. */
    fun setMorph(a: Float, b: Float, now: Float) {
        if (sameF(a, morphA) && sameF(b, morphB) && sameF(now, morphNow)) return
        morphA = a
        morphB = b
        morphNow = now
        invalidate()
    }

    private fun sameF(x: Float, y: Float) = (x.isNaN() && y.isNaN()) || x == y

    private val d = resources.displayMetrics.density
    private val knob = sizeDp * d
    private val w = max(knob + 14 * d, 60 * d)
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)
    private val arcPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeCap = Paint.Cap.ROUND
    }
    private val txt = Paint(Paint.ANTI_ALIAS_FLAG).apply { textAlign = Paint.Align.CENTER }
    private val arc = RectF()
    private val arcOuter = RectF()
    private val morphPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeCap = Paint.Cap.BUTT
    }
    private var downY = 0f
    private var downX = 0f
    private var downV = 0f
    private val gestures = GestureDetector(ctx, object : GestureDetector.SimpleOnGestureListener() {
        override fun onDoubleTap(e: MotionEvent): Boolean { onReset?.invoke(); return true }
        override fun onLongPress(e: MotionEvent) { if (abs(e.y - downY) < 8 * d) onLongPress?.invoke() }
    })

    override fun onMeasure(widthSpec: Int, heightSpec: Int) {
        setMeasuredDimension(w.toInt(), (knob + 36 * d).toInt())
    }

    override fun onDraw(c: Canvas) {
        val cx = width / 2f
        val r = knob / 2f
        val cy = r + 4 * d
        val trackR = r - 2.5f * d
        arc.set(cx - trackR, cy - trackR, cx + trackR, cy + trackR)

        // Background 270° track
        arcPaint.strokeWidth = 3.2f * d
        arcPaint.color = Color.rgb(26, 33, 48)
        c.drawArc(arc, 135f, 270f, false, arcPaint)

        // Active glowing value arc
        arcPaint.strokeWidth = 3.4f * d
        arcPaint.color = accent
        if (bipolar) {
            val sweep = (value - 0.5f) * 270f
            if (abs(sweep) > 0.5f) c.drawArc(arc, 270f, sweep, false, arcPaint)
        } else if (value > 0.002f) {
            c.drawArc(arc, 135f, value * 270f, false, arcPaint)
        }

        // Morph dual arc (A<->B end-points) just outside the track + live position dot
        if (!morphA.isNaN() && !morphB.isNaN()) {
            val mr = trackR + 3.5f * d
            arcOuter.set(cx - mr, cy - mr, cx + mr, cy + mr)
            val lo = min(morphA, morphB)
            val hi = max(morphA, morphB)
            morphPaint.strokeWidth = 2f * d
            morphPaint.color = Hw.withAlpha(Hw.MOD_ENV, 220)
            if (hi - lo > 0.002f) c.drawArc(arcOuter, 135f + 270f * lo, 270f * (hi - lo), false, morphPaint)
            // end ticks: A dim, B bright
            morphPaint.strokeWidth = 1.6f * d
            val aa = Math.toRadians(135.0 + 270.0 * morphA)
            val ca = cos(aa).toFloat()
            val sa = sin(aa).toFloat()
            morphPaint.color = Hw.withAlpha(Hw.MOD_ENV, 120)
            c.drawLine(cx + (mr - 2.5f * d) * ca, cy + (mr - 2.5f * d) * sa, cx + (mr + 1.5f * d) * ca, cy + (mr + 1.5f * d) * sa, morphPaint)
            val ab = Math.toRadians(135.0 + 270.0 * morphB)
            val cb = cos(ab).toFloat()
            val sb = sin(ab).toFloat()
            morphPaint.color = Hw.MOD_ENV
            c.drawLine(cx + (mr - 2.5f * d) * cb, cy + (mr - 2.5f * d) * sb, cx + (mr + 1.5f * d) * cb, cy + (mr + 1.5f * d) * sb, morphPaint)
            if (!morphNow.isNaN()) {
                val an = Math.toRadians(135.0 + 270.0 * morphNow)
                p.style = Paint.Style.FILL
                p.color = Color.WHITE
                c.drawCircle(cx + mr * cos(an).toFloat(), cy + mr * sin(an).toFloat(), 1.5f * d, p)
            }
        }

        // Precision matte dark cap
        val capR = trackR - 5.5f * d
        p.style = Paint.Style.FILL
        p.color = if (isPressed) Hw.PANEL_RAISED else Color.rgb(12, 16, 24)
        c.drawCircle(cx, cy, capR, p)
        p.style = Paint.Style.STROKE
        p.strokeWidth = 1f * d
        p.color = if (isPressed) accent else Hw.PANEL_EDGE
        c.drawCircle(cx, cy, capR, p)

        // Pointer tick
        val ang = Math.toRadians(135.0 + 270.0 * value)
        val cosA = cos(ang).toFloat()
        val sinA = sin(ang).toFloat()
        p.strokeCap = Paint.Cap.ROUND
        p.strokeWidth = 2.2f * d
        p.color = if (isPressed) Color.WHITE else Hw.SILK
        c.drawLine(cx + capR * 0.25f * cosA, cy + capR * 0.25f * sinA, cx + (capR - 2.5f * d) * cosA, cy + (capR - 2.5f * d) * sinA, p)

        // Label & tabular value
        txt.textSize = 10.5f * d
        txt.color = Hw.ENGRAVE
        txt.typeface = Hw.BOLD
        val lbl = if (label.any { it.code > 0x2FF }) label else label.uppercase()
        c.drawText(fit(lbl, width - 2 * d), cx, knob + 16 * d, txt)

        txt.textSize = 11f * d
        txt.color = if (isPressed) accent else Hw.SILK
        txt.typeface = Hw.MONO
        c.drawText(fit(valueText, width - 2 * d), cx, knob + 29 * d, txt)
    }

    private fun fit(s: String, w: Float): String {
        if (txt.measureText(s) <= w) return s
        var e = s
        while (e.length > 1 && txt.measureText("$e…") > w) e = e.dropLast(1)
        return "$e…"
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        gestures.onTouchEvent(e)
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                parent?.requestDisallowInterceptTouchEvent(true)
                downY = e.y
                downX = e.x
                downV = value
                isPressed = true
                onTouchState?.invoke(true)
                invalidate()
            }
            MotionEvent.ACTION_MOVE -> {
                val delta = ((downY - e.y) + (e.x - downX) * 0.5f) / (170 * d)
                val v = (downV + delta).coerceIn(0f, 1f)
                if (v != value) {
                    value = v
                    onChange?.invoke(v)
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                isPressed = false
                onTouchState?.invoke(false)
                invalidate()
            }
        }
        return true
    }
}

/**
 * Nord Stage 4 × Obsidian Instrument 20-segment vertical LED bar-graph fader.
 * Pure #000000 OLED well, relative drag (no value jump on touch), double-tap reset,
 * optional long-press on label to reassign parameter. Zero allocations in onDraw().
 */
class SegmentedFader(
    ctx: Context,
    var label: String,
    var accent: Int = Hw.MOD_OSC,
    private val segments: Int = 20,
    private val redTop: Boolean = false,
    private val compact: Boolean = false
) : View(ctx) {
    var value: Float = 0f
        set(v) { field = v.coerceIn(0f, 1f); invalidate() }
    var valueText: String = ""
        set(v) { field = v; invalidate() }
    var muted: Boolean = false
        set(v) { field = v; invalidate() }
    var onChange: ((Float) -> Unit)? = null
    var onDragEnd: (() -> Unit)? = null
    var onReset: (() -> Unit)? = null
    var onLongPressLabel: (() -> Unit)? = null

    private val d = resources.displayMetrics.density
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)
    private val t = Paint(Paint.ANTI_ALIAS_FLAG).apply { textAlign = Paint.Align.CENTER }
    private val r = RectF()
    private var downY = 0f
    private var startV = 0f
    private var dragging = false
    private val labelH get() = (if (compact) 22f else 32f) * d
    private val topValH get() = (if (compact) 18f else 28f) * d

    private val gest = GestureDetector(ctx, object : GestureDetector.SimpleOnGestureListener() {
        override fun onDoubleTap(e: MotionEvent): Boolean { onReset?.invoke(); return true }
        override fun onLongPress(e: MotionEvent) {
            if (!dragging && e.y >= height - labelH - 8 * d) onLongPressLabel?.invoke()
        }
    })

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        gest.onTouchEvent(e)
        val trackH = (height - topValH - labelH).coerceAtLeast(40f * d)
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                parent?.requestDisallowInterceptTouchEvent(true)
                downY = e.y
                startV = value
                dragging = false
                invalidate()
            }
            MotionEvent.ACTION_MOVE -> {
                if (!dragging && abs(e.y - downY) > 5f * d) {
                    dragging = true
                    downY = e.y
                }
                if (dragging) {
                    val nv = (startV + (downY - e.y) / trackH).coerceIn(0f, 1f)
                    if (nv != value) {
                        value = nv
                        onChange?.invoke(nv)
                    }
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                if (dragging) onDragEnd?.invoke()
                dragging = false
                invalidate()
            }
        }
        return true
    }

    override fun onDraw(c: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        if (w <= 0f || h <= 0f) return

        // Top tabular readout
        t.typeface = Hw.MONO
        t.textSize = (if (compact) 10f else 12.5f) * d
        t.color = if (muted) Hw.INK_DIM else if (dragging) accent else Hw.SILK
        val vs = fit(valueText, w - 4 * d)
        c.drawText(vs, w / 2f, topValH - 6 * d, t)

        // Pure OLED black well
        val trackTop = topValH
        val trackBot = (h - labelH).coerceAtLeast(trackTop + 24 * d)
        val tw = min(w * 0.62f, (if (compact) 28f else 42f) * d)
        val x0 = (w - tw) / 2f
        val x1 = x0 + tw
        r.set(x0 - 2 * d, trackTop, x1 + 2 * d, trackBot)
        p.style = Paint.Style.FILL
        p.color = Hw.WELL
        c.drawRoundRect(r, 6 * d, 6 * d, p)
        p.style = Paint.Style.STROKE
        p.strokeWidth = 1f * d
        p.color = if (dragging) accent else Hw.PANEL_EDGE
        c.drawRoundRect(r, 6 * d, 6 * d, p)

        // 20 LED segments
        p.style = Paint.Style.FILL
        val pad = 4 * d
        val innerH = (trackBot - trackTop - pad * 2).coerceAtLeast(10f)
        val gap = (if (compact) 1.5f else 2f) * d
        val segH = ((innerH - gap * (segments - 1)) / segments).coerceAtLeast(1.5f * d)
        val litCount = (value * segments + 0.001f).roundToInt().coerceIn(0, segments)
        val activeCol = if (muted) Hw.INK_DIM else accent
        val dimCol = Color.rgb(18, 22, 31)

        for (i in 0 until segments) {
            val yBot = trackBot - pad - i * (segH + gap)
            val yTop = yBot - segH
            val isLit = i < litCount
            p.color = when {
                !isLit -> dimCol
                redTop && !muted && i >= segments - 2 -> Hw.MOD_RED
                else -> activeCol
            }
            r.set(x0 + 2 * d, yTop, x1 - 2 * d, yBot)
            c.drawRoundRect(r, 1.5f * d, 1.5f * d, p)
        }

        // Crisp thumb bar indicator at current level
        if (litCount > 0) {
            val ty = trackBot - pad - (litCount - 1) * (segH + gap) - segH / 2f
            p.color = if (dragging) Color.WHITE else Hw.withAlpha(Color.WHITE, 210)
            r.set(x0, ty - 1.2f * d, x1, ty + 1.2f * d)
            c.drawRoundRect(r, 1.2f * d, 1.2f * d, p)
        }

        // Bottom label
        t.typeface = Hw.BOLD
        t.textSize = (if (compact) 9.5f else 11.5f) * d
        t.color = if (muted) Hw.INK_DIM else if (dragging) Color.WHITE else Hw.ENGRAVE
        val lb = if (label.any { it.code > 0x2FF }) label else label.uppercase()
        c.drawText(fit(lb, w - 2 * d), w / 2f, h - (if (compact) 6f else 10f) * d, t)
    }

    private fun fit(s: String, maxW: Float): String {
        if (maxW <= 0f || t.measureText(s) <= maxW) return s
        var e = s
        while (e.length > 1 && t.measureText("$e…") > maxW) e = e.dropLast(1)
        return "$e…"
    }
}

/**
 * Hold-safe PANIC button: requires a 650 ms hold with animated red progress sweep.
 */
class HoldPanicButton(
    ctx: Context,
    private val label: String = "HOLD · PANIC",
    private val holdMs: Long = 650L,
    private val action: () -> Unit
) : View(ctx) {
    private var downAt = 0L
    private var fired = false
    private val d = resources.displayMetrics.density
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)
    private val t = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Hw.BOLD; textAlign = Paint.Align.CENTER }
    private val r = RectF()
    private val tick = object : Runnable {
        override fun run() {
            if (downAt == 0L) return
            if (!fired && SystemClock.elapsedRealtime() - downAt >= holdMs) {
                fired = true
                performHapticFeedback(android.view.HapticFeedbackConstants.LONG_PRESS)
                action()
            }
            invalidate()
            if (!fired) postOnAnimation(this)
        }
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> { downAt = SystemClock.elapsedRealtime(); fired = false; postOnAnimation(tick) }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                downAt = 0L
                postDelayed({ fired = false; invalidate() }, 450)
                invalidate()
            }
        }
        return true
    }

    override fun onDraw(c: Canvas) {
        val rad = 7f * d
        r.set(0f, 0f, width.toFloat(), height.toFloat())
        p.style = Paint.Style.FILL
        p.color = if (fired) Hw.MOD_RED else Color.rgb(34, 12, 14)
        c.drawRoundRect(r, rad, rad, p)
        if (downAt > 0L && !fired) {
            val k = ((SystemClock.elapsedRealtime() - downAt).toFloat() / holdMs).coerceIn(0f, 1f)
            p.color = Color.rgb(185, 36, 36)
            c.save()
            c.clipRect(0f, 0f, width * k, height.toFloat())
            c.drawRoundRect(r, rad, rad, p)
            c.restore()
        }
        p.style = Paint.Style.STROKE
        p.strokeWidth = 1.2f * d
        p.color = if (downAt > 0L || fired) Hw.MOD_RED else Hw.withAlpha(Hw.MOD_RED, 140)
        r.inset(p.strokeWidth / 2f, p.strokeWidth / 2f)
        c.drawRoundRect(r, rad, rad, p)
        t.textSize = 12f * d
        t.letterSpacing = 0.08f
        t.color = if (fired || downAt > 0L) Color.WHITE else Color.rgb(252, 165, 165)
        c.drawText(if (fired) "ALL SOUND OFF" else label, width / 2f, height / 2f + t.textSize * 0.36f, t)
    }
}

/** Compact stereo L/R segmented LED meter for the 26dp telemetry bar. */
class MiniStereoMeter(ctx: Context) : View(ctx) {
    private var levelL = 0f
    private var levelR = 0f
    private val d = resources.displayMetrics.density
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)
    private val r = RectF()

    fun setPeak(peak: Float) {
        val db = if (peak > 1e-5f) 20f * kotlin.math.log10(peak) else -99f
        val norm = ((db + 48f) / 48f).coerceIn(0f, 1f)
        levelL = max(norm, levelL * 0.78f)
        levelR = max(norm * 0.96f, levelR * 0.78f)
        invalidate()
    }

    override fun onDraw(c: Canvas) {
        val segs = 10
        val gap = 1.5f * d
        val segW = ((width - gap * (segs - 1)) / segs).coerceAtLeast(2f)
        val rowH = ((height - 3f * d) / 2f).coerceAtLeast(2.5f * d)
        drawRow(c, 1f * d, rowH, segs, segW, gap, levelL)
        drawRow(c, height - rowH - 1f * d, rowH, segs, segW, gap, levelR)
    }

    private fun drawRow(c: Canvas, y: Float, h: Float, segs: Int, segW: Float, gap: Float, v: Float) {
        val lit = (v * segs + 0.01f).roundToInt().coerceIn(0, segs)
        for (i in 0 until segs) {
            val x = i * (segW + gap)
            p.color = when {
                i >= lit -> Color.rgb(20, 25, 36)
                i >= segs - 1 -> Hw.MOD_RED
                i >= segs - 3 -> Hw.MOD_FILTER
                else -> Hw.MOD_FX
            }
            r.set(x, y, x + segW, y + h)
            c.drawRoundRect(r, 1f * d, 1f * d, p)
        }
    }
}

/** Left 52dp vertical rail mode button (Stage / Multi / Edit / Lib / Play / Song) with crisp vector icons. */
class RailNavButton(ctx: Context, private val title: String, private val glyph: String, var accent: Int = Hw.MOD_OSC) : View(ctx) {
    var active = false
        set(v) { if (field != v) { field = v; invalidate() } }
    private val d = resources.displayMetrics.density
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)
    private val strokeP = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 1.5f * d
        strokeCap = Paint.Cap.ROUND
        strokeJoin = Paint.Join.ROUND
    }
    private val lblP = Paint(Paint.ANTI_ALIAS_FLAG).apply { textAlign = Paint.Align.CENTER; typeface = Hw.BOLD }
    private val r = RectF()
    private val wavePath = android.graphics.Path()

    init { isClickable = true }

    override fun onDraw(c: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        if (active) {
            r.set(4 * d, 2 * d, w - 4 * d, h - 2 * d)
            p.style = Paint.Style.FILL
            p.color = Hw.PANEL_RAISED
            c.drawRoundRect(r, 6 * d, 6 * d, p)
            p.color = accent
            r.set(1.5f * d, 7 * d, 4.5f * d, h - 7 * d)
            c.drawRoundRect(r, 1.5f * d, 1.5f * d, p)
        }

        val col = if (active) accent else Hw.ENGRAVE
        val cx = w / 2f
        val cy = h * 0.40f
        p.style = Paint.Style.FILL
        p.color = col
        strokeP.color = col

        when (title) {
            "STAGE" -> {
                // Crisp hardware diamond + center dot
                val s = 7.5f * d
                wavePath.reset()
                wavePath.moveTo(cx, cy - s)
                wavePath.lineTo(cx + s, cy)
                wavePath.lineTo(cx, cy + s)
                wavePath.lineTo(cx - s, cy)
                wavePath.close()
                c.drawPath(wavePath, strokeP)
                c.drawCircle(cx, cy, 2.2f * d, p)
            }
            "MULTI" -> {
                // 4 vertical mixer bars
                val heights = floatArrayOf(0.55f, 0.95f, 0.70f, 0.40f)
                val bw = 2.6f * d
                val gap = 2.2f * d
                val totalW = bw * 4 + gap * 3
                val x0 = cx - totalW / 2f
                val bot = cy + 6.5f * d
                val maxH = 13f * d
                for (i in 0..3) {
                    val x = x0 + i * (bw + gap)
                    r.set(x, bot - maxH * heights[i], x + bw, bot)
                    c.drawRoundRect(r, 1f * d, 1f * d, p)
                }
            }
            "EDIT" -> {
                // Crisp sine / filter curve
                wavePath.reset()
                wavePath.moveTo(cx - 8 * d, cy + 2 * d)
                wavePath.cubicTo(cx - 4 * d, cy - 10 * d, cx + 1 * d, cy + 10 * d, cx + 8 * d, cy - 3 * d)
                c.drawPath(wavePath, strokeP)
                c.drawCircle(cx - 1.5f * d, cy, 2f * d, p)
            }
            "LIB" -> {
                // 2x2 rounded tile grid
                val sz = 5.5f * d
                val g = 2.5f * d
                for (row in 0..1) for (colIdx in 0..1) {
                    val x = cx - sz - g / 2f + colIdx * (sz + g)
                    val y = cy - sz - g / 2f + row * (sz + g)
                    r.set(x, y, x + sz, y + sz)
                    c.drawRoundRect(r, 1.3f * d, 1.3f * d, strokeP)
                }
            }
            "PLAY" -> {
                // Crisp monochrome 4-key mini piano vector icon (replaces color emoji!)
                val kw = 16f * d
                val kh = 12f * d
                val l = cx - kw / 2f
                val t = cy - kh / 2f
                r.set(l, t, l + kw, t + kh)
                c.drawRoundRect(r, 2f * d, 2f * d, strokeP)
                val step = kw / 4f
                for (i in 1..3) c.drawLine(l + i * step, t, l + i * step, t + kh, strokeP)
                val bkW = 2.4f * d
                val bkH = kh * 0.56f
                for (i in intArrayOf(1, 2, 3)) {
                    val bx = l + i * step - bkW / 2f
                    r.set(bx, t, bx + bkW, t + bkH)
                    c.drawRect(r, p)
                }
            }
            else -> {
                // SONG: crisp play triangle
                val s = 6.5f * d
                wavePath.reset()
                wavePath.moveTo(cx - s * 0.7f, cy - s)
                wavePath.lineTo(cx + s * 0.95f, cy)
                wavePath.lineTo(cx - s * 0.7f, cy + s)
                wavePath.close()
                c.drawPath(wavePath, p)
            }
        }

        lblP.textSize = 8.5f * d
        lblP.letterSpacing = 0.06f
        lblP.color = if (active) Color.WHITE else Hw.INK_DIM
        c.drawText(title, cx, h * 0.80f, lblP)
    }
}

/**
 * One parameter as a hardware control: vector arc knob (float / int), OLED selector
 * (enum, tap for list) or pill switch (bool). Supports semantic module [accent].
 */
class ParamControl(
    private val ctx: Context,
    val meta: ParamMeta,
    knobSize: Float = 48f,
    lcdWidth: Float = 112f,
    accent: Int = Hw.MOD_OSC,
    private val onSet: ((ParamControl) -> Unit)? = null
) {
    val view: View
    private var knob: KnobView? = null
    private var lcd: TextView? = null
    private var toggle: ToggleSwitch? = null
    /** Value at touch-down while MORPH LEARN is on (A of the route being assigned). */
    private var learnA = Float.NaN

    init {
        val d = { v: Float -> Hw.dp(ctx, v).toInt() }
        when (meta.kind) {
            "enum" -> {
                val box = LinearLayout(ctx).apply { orientation = LinearLayout.VERTICAL }
                box.addView(Hw.engraved(ctx, meta.label, 10f, tr = false))
                val l = Hw.lcd(ctx, "", 12.5f).apply {
                    minWidth = d(lcdWidth)
                    background = Hw.lcdBg(ctx, stroke = Hw.withAlpha(accent, 110))
                    setOnClickListener { choose() }
                    setOnLongClickListener { learnMenu(); true }
                }
                box.addView(l, LinearLayout.LayoutParams(-2, -2).apply { topMargin = d(4f) })
                lcd = l
                view = box
            }
            "bool" -> {
                val t = ToggleSwitch(ctx, meta.label, tr = false, accent = accent)
                t.onToggle = { on -> History.checkpoint(); set(if (on) 1f else 0f) }
                t.setOnLongClickListener { learnMenu(); true }
                toggle = t
                view = t
            }
            else -> {
                val bip = meta.min < 0f && meta.max > 0f
                val k = KnobView(ctx, meta.label, bip, knobSize, accent = accent)
                k.onChange = { t -> set(meta.fromNormalized(t), fromKnob = true) }
                k.onReset = {
                    if (MorphUi.learning && MorphUi.learnable(meta)) {
                        // double-tap while learning: drop this param's morph route
                        if (MorphUi.ranges.get(meta.id) != null) MorphUi.clear(meta.id)
                        refresh()
                    } else {
                        History.checkpoint(); set(meta.def)
                    }
                }
                k.onTouchState = { down ->
                    if (down) {
                        History.checkpoint()
                        learnA = NativeEngine.nativeGetParam(meta.id)
                        Hw.onParamTouched?.invoke(meta.label, meta.format(learnA))
                    } else if (MorphUi.learning && MorphUi.learnable(meta)) {
                        // gesture over: snap the knob back to the (untouched) base value, re-read the engine's routes once
                        learnA = Float.NaN
                        MorphUi.refresh()
                        refresh()
                    }
                }
                k.onLongPress = { learnMenu() }
                knob = k
                view = k
            }
        }
        refresh()
    }

    private fun choose() {
        val cur = NativeEngine.nativeGetParam(meta.id).roundToInt()
        AlertDialog.Builder(ctx)
            .setTitle(meta.label)
            .setSingleChoiceItems(meta.options.toTypedArray(), cur) { dlg, which -> History.checkpoint(); set(which.toFloat()); dlg.dismiss() }
            .show()
    }

    private fun learnMenu() {
        val a = ctx as? MainActivity ?: return
        val cur = NativeEngine.nativeLearnDescribe(meta.id)
        val items = if (cur.isEmpty()) arrayOf("MIDI Learn") else arrayOf("MIDI Learn", "Clear MIDI ($cur)")
        AlertDialog.Builder(ctx).setTitle(meta.label).setItems(items) { _, w ->
            if (w == 0) a.learn(meta) else a.unlearn(meta)
        }.show()
    }

    fun set(v: Float, fromKnob: Boolean = false) {
        if (MorphUi.learning && MorphUi.learnable(meta)) {
            // MORPH LEARN: move the end-point B of this param's route; the base value (the sound) is not written
            val cur = NativeEngine.nativeGetParam(meta.id)
            val aVal = MorphUi.startOf(meta, if (learnA.isNaN()) cur else learnA)
            MorphUi.learnSet(meta, aVal, v)
            Hw.onParamTouched?.invoke(meta.label, meta.format(aVal) + "->" + meta.format(v))
            if (fromKnob) knob?.valueText = meta.format(v) else refresh()
            applyMorphArc()
            return
        }
        NativeEngine.setParam(meta.id, v)
        val now = NativeEngine.nativeGetParam(meta.id)
        Hw.onParamTouched?.invoke(meta.label, meta.format(now))
        if (fromKnob) knob?.valueText = meta.format(now) else refresh()
        onSet?.invoke(this)
    }

    fun refresh() {
        val v = NativeEngine.nativeGetParam(meta.id)
        knob?.let {
            it.value = norm(v)
            it.valueText = meta.format(v)
        }
        applyMorphArc()
        lcd?.text = meta.format(v) + "  ▾"
        toggle?.on = v > 0.5f
    }

    private fun norm(v: Float) =
        if (meta.kind == "int" && meta.max > meta.min) ((v - meta.min) / (meta.max - meta.min)).coerceIn(0f, 1f) else meta.toNormalized(v)

    /** Knob dual arc from [MorphUi.ranges] (filled by one `nativeMorphDump()`, never per knob). */
    private fun applyMorphArc() {
        val k = knob ?: return
        val r = MorphUi.ranges.get(meta.id)
        if (r == null) { k.setMorph(Float.NaN, Float.NaN, Float.NaN); return }
        val now = try { norm(NativeEngine.nativeMorphNow(meta.id)) } catch (_: Throwable) { Float.NaN }
        k.setMorph(norm(r[0]), norm(r[1]), now)
    }
}

/** Slim Obsidian OLED header/status strip with bottom 1dp hairline. */
class ChassisBar(ctx: Context) : LinearLayout(ctx) {
    private val p = Paint()
    private val d = resources.displayMetrics.density
    init {
        orientation = HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        setBackgroundColor(Color.rgb(4, 6, 10))
        setWillNotDraw(false)
    }
    override fun onDraw(c: Canvas) {
        super.onDraw(c)
        p.color = Hw.PANEL_EDGE
        c.drawRect(0f, height - d, width.toFloat(), height.toFloat(), p)
    }
}

/**
 * Horizontal segmented LED bar (promoted from `MultiPanel.LevelBar`): black OLED well, relative horizontal drag
 * (never jumps on touch), double-tap = [onReset], optional right-aligned [valueText]. Used by the STAGE SuperStrip,
 * click level, comp make-up and audio volume. Zero allocations in onDraw(): segment geometry is precomputed in
 * onSizeChanged, paints / rects are preallocated.
 *
 * @param segments number of LED segments
 * @param valueWidthDp width reserved for the value label on the right (0 = no label)
 */
class HSegBar(
    ctx: Context,
    accent: Int = Hw.MOD_OSC,
    private val segments: Int = 12,
    private val valueWidthDp: Float = 22f
) : View(ctx) {
    var value = 0f
        set(v) { val c = v.coerceIn(0f, 1f); if (c != field) { field = c; invalidate() } }
    var valueText = ""
        set(v) { if (field != v) { field = v; invalidate() } }
    var accent: Int = accent
        set(v) { if (field != v) { field = v; invalidate() } }
    var muted = false
        set(v) { if (field != v) { field = v; invalidate() } }
    /** Called for every value change caused by a drag. */
    var onChange: ((Float) -> Unit)? = null
    /** Called on double-tap (reset to the default). */
    var onReset: (() -> Unit)? = null
    /** Called once when a drag starts (take a `History.checkpoint()` here) and once when it ends. */
    var onDragStart: (() -> Unit)? = null
    var onDragEnd: (() -> Unit)? = null

    private val dd = resources.displayMetrics.density
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)
    private val t = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Hw.MONO; textAlign = Paint.Align.RIGHT; textSize = 9.5f * dd }
    private val r = RectF()
    private val dimCol = Color.rgb(18, 22, 31)
    private var x0 = 0f
    private var segW = 1f
    private var trackW = 1f
    private var downX = 0f
    private var startV = 0f
    private var dragging = false
    private val gest = GestureDetector(ctx, object : GestureDetector.SimpleOnGestureListener() {
        override fun onDoubleTap(e: MotionEvent): Boolean { onReset?.invoke(); return true }
    })

    override fun onMeasure(widthSpec: Int, heightSpec: Int) {
        setMeasuredDimension(resolveSize((160 * dd).toInt(), widthSpec), resolveSize((24 * dd).toInt(), heightSpec))
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        val pad = 3f * dd
        val gap = 1.5f * dd
        x0 = pad
        val x1 = (w - pad - valueWidthDp * dd).coerceAtLeast(x0 + segments * 2f)
        trackW = (x1 - x0).coerceAtLeast(1f)
        segW = ((trackW - gap * (segments - 1)) / segments).coerceAtLeast(1f)
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        gest.onTouchEvent(e)
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                parent?.requestDisallowInterceptTouchEvent(true)
                downX = e.x
                startV = value
                dragging = false
            }
            MotionEvent.ACTION_MOVE -> {
                if (!dragging && abs(e.x - downX) > 4f * dd) {
                    dragging = true
                    downX = e.x
                    onDragStart?.invoke()
                    invalidate()
                }
                if (dragging) {
                    val nv = (startV + (e.x - downX) / trackW).coerceIn(0f, 1f)
                    if (nv != value) { value = nv; onChange?.invoke(nv) }
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                if (dragging) onDragEnd?.invoke()
                dragging = false
                invalidate()
            }
        }
        return true
    }

    override fun onDraw(c: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        if (w <= 0f || h <= 0f) return
        val rad = 4f * dd
        r.set(0f, 0f, w, h)
        p.style = Paint.Style.FILL
        p.color = Hw.WELL
        c.drawRoundRect(r, rad, rad, p)
        p.style = Paint.Style.STROKE
        p.strokeWidth = dd
        p.color = if (dragging) accent else Hw.PANEL_EDGE
        r.inset(dd / 2f, dd / 2f)
        c.drawRoundRect(r, rad, rad, p)
        p.style = Paint.Style.FILL

        val pad = 3f * dd
        val gap = 1.5f * dd
        val lit = (value * segments + 0.001f).roundToInt().coerceIn(0, segments)
        val on = if (muted) Hw.INK_DIM else accent
        for (i in 0 until segments) {
            val sx = x0 + i * (segW + gap)
            p.color = if (i < lit) on else dimCol
            r.set(sx, pad, sx + segW, h - pad)
            c.drawRoundRect(r, 1.2f * dd, 1.2f * dd, p)
        }
        if (valueWidthDp > 0f) {
            t.color = if (muted) Hw.INK_DIM else if (dragging) accent else Hw.SILK
            c.drawText(valueText, w - pad, h / 2f + 3.3f * dd, t)
        }
    }
}

/**
 * Compact drawn OLED pill (default 30 × 22 dp) with a 3 dp LED dot, for the top telemetry strip (`EQ`, `♩`).
 * Use the normal View click / long-click listeners. [ledColor] = 0 turns the LED off; [active] outlines the pill in
 * [accent]. Zero allocations in onDraw().
 */
class ChipButton(
    ctx: Context,
    label: String,
    tr: Boolean = false,
    private val widthDp: Float = 30f,
    accent: Int = Hw.MOD_OSC
) : View(ctx) {
    var label: String = if (tr) L10n.t(label) else label
        set(v) { if (field != v) { field = v; contentDescription = v; invalidate() } }
    var accent: Int = accent
        set(v) { if (field != v) { field = v; invalidate() } }
    /** LED colour; 0 = LED off. */
    var ledColor = 0
        set(v) { if (field != v) { field = v; invalidate() } }
    /** Lit / selected look: accent outline and bright label. */
    var active = false
        set(v) { if (field != v) { field = v; invalidate() } }

    private val d = resources.displayMetrics.density
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)
    private val t = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Hw.BOLD; textAlign = Paint.Align.CENTER; textSize = 10.5f * d }
    private val r = RectF()
    private var baseline = 0f

    init {
        isClickable = true
        contentDescription = this.label
    }

    override fun onMeasure(widthSpec: Int, heightSpec: Int) {
        setMeasuredDimension((widthDp * d).toInt(), (22 * d).toInt())
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        baseline = h / 2f - (t.ascent() + t.descent()) / 2f
    }

    override fun onDraw(c: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        r.set(d / 2f, d / 2f, w - d / 2f, h - d / 2f)
        p.style = Paint.Style.FILL
        p.color = if (isPressed) Hw.PANEL_RAISED else Hw.WELL
        c.drawRoundRect(r, h / 2f, h / 2f, p)
        p.style = Paint.Style.STROKE
        p.strokeWidth = d
        p.color = if (active) Hw.withAlpha(accent, 190) else if (isPressed) Hw.PANEL_HI else Hw.PANEL_EDGE
        c.drawRoundRect(r, h / 2f, h / 2f, p)
        p.style = Paint.Style.FILL

        t.color = if (active) Hw.SILK else Hw.ENGRAVE
        c.drawText(label, w / 2f - 1f * d, baseline, t)

        val cx = w - 6.5f * d
        val cy = 6f * d
        if (ledColor != 0) {
            p.color = Hw.withAlpha(ledColor, 80)
            c.drawCircle(cx, cy, 3f * d, p)
            p.color = ledColor
        } else {
            p.color = Hw.LED_OFF
        }
        c.drawCircle(cx, cy, 1.5f * d, p)
    }
}
