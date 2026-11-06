package com.hsynth.app

import android.annotation.SuppressLint
import android.app.AlertDialog
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.RadialGradient
import android.graphics.RectF
import android.graphics.Shader
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
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
 * Hardware-synth look: chassis, engraved panel sections, knobs, LED buttons
 * and LCD read-outs. Every screen is built from these.
 */
object Hw {
    val CHASSIS = Color.rgb(14, 15, 17)
    val PANEL = Color.rgb(31, 33, 37)
    val PANEL_EDGE = Color.rgb(8, 9, 10)
    val PANEL_HI = Color.rgb(58, 61, 68)
    val SILK = Color.rgb(214, 218, 226)     // silkscreen print
    val ENGRAVE = Color.rgb(160, 166, 178)
    val LED = Color.rgb(255, 140, 26)
    val LED_OFF = Color.rgb(64, 40, 24)
    val LCD_BG = Color.rgb(12, 18, 8)
    val LCD_FG = Color.rgb(174, 234, 0)
    val LCD_DIM = Color.rgb(96, 132, 22)
    val KEY = Color.rgb(44, 47, 53)          // button cap
    val KEY_EDGE = Color.rgb(10, 11, 13)

    // fonts: Rajdhani (labels, buttons) and Share Tech Mono (LCDs), both SIL OFL
    // (assets/fonts); Japanese falls back to the system font
    lateinit var LABEL: Typeface
    lateinit var BOLD: Typeface
    lateinit var MONO: Typeface

    fun init(ctx: Context) {
        if (::LABEL.isInitialized) return
        fun load(f: String, fallback: Typeface) = try { Typeface.createFromAsset(ctx.assets, "fonts/$f") } catch (e: Exception) { fallback }
        LABEL = load("Rajdhani-SemiBold.ttf", Typeface.DEFAULT)
        BOLD = load("Rajdhani-Bold.ttf", Typeface.DEFAULT_BOLD)
        MONO = load("ShareTechMono-Regular.ttf", Typeface.MONOSPACE)
    }

    /** Shown in the header LCD while a control is touched: (label, value). */
    var onParamTouched: ((String, String) -> Unit)? = null

    fun dp(ctx: Context, v: Float) = v * ctx.resources.displayMetrics.density

    /** Metal plate: flat, hard edge, a light line along the top. */
    fun panelBg(ctx: Context, color: Int = PANEL): android.graphics.drawable.Drawable {
        val edge = GradientDrawable().apply { setColor(PANEL_EDGE); cornerRadius = dp(ctx, 2f) }
        val hi = GradientDrawable().apply { setColor(PANEL_HI); cornerRadius = dp(ctx, 2f) }
        val face = GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM,
            intArrayOf(Color.rgb(38, 40, 45), color)).apply { cornerRadius = dp(ctx, 1.5f) }
        val e = dp(ctx, 1f).toInt().coerceAtLeast(1)
        return android.graphics.drawable.LayerDrawable(arrayOf(edge, hi, face)).apply {
            setLayerInset(1, e, e, e, e)
            setLayerInset(2, e, 2 * e, e, e)
        }
    }

    fun lcdBg(ctx: Context) = GradientDrawable().apply {
        setColor(LCD_BG)
        cornerRadius = dp(ctx, 2f)
        setStroke(dp(ctx, 2f).toInt(), Color.rgb(4, 5, 6))
    }

    /** Square-ish key cap with a bevel (lit top edge, dark bottom). */
    fun keyBg(ctx: Context, pressed: Boolean = false): android.graphics.drawable.Drawable {
        val e = dp(ctx, 1f).toInt().coerceAtLeast(1)
        val edge = GradientDrawable().apply { setColor(KEY_EDGE); cornerRadius = dp(ctx, 3f) }
        val bevel = GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM,
            if (pressed) intArrayOf(Color.rgb(24, 25, 29), Color.rgb(40, 42, 48)) else intArrayOf(Color.rgb(96, 101, 112), Color.rgb(26, 28, 32)))
            .apply { cornerRadius = dp(ctx, 2.5f) }
        val face = GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM,
            if (pressed) intArrayOf(Color.rgb(34, 36, 41), Color.rgb(40, 43, 49)) else intArrayOf(Color.rgb(60, 64, 72), KEY))
            .apply { cornerRadius = dp(ctx, 2f) }
        return android.graphics.drawable.LayerDrawable(arrayOf(edge, bevel, face)).apply {
            setLayerInset(1, e, e, e, e)
            setLayerInset(2, 2 * e, 2 * e, 2 * e, 3 * e)
        }
    }

    /** Silkscreen panel label: upper case for Latin text. */
    fun engraved(ctx: Context, s: String, size: Float = 11f, tr: Boolean = true) = TextView(ctx).apply {
        val t = if (tr) L10n.t(s) else s
        text = if (t.any { it.code > 0x2FF }) t else t.uppercase()
        textSize = size + 1f
        letterSpacing = 0.06f
        setTextColor(ENGRAVE)
        typeface = BOLD
        maxLines = 1
        ellipsize = android.text.TextUtils.TruncateAt.END
    }

    /** LCD read-out (dot-matrix style font, green on black). */
    fun lcd(ctx: Context, s: String = "", size: Float = 14f) = TextView(ctx).apply {
        text = s
        textSize = size + 1f
        setTextColor(LCD_FG)
        typeface = MONO
        background = lcdBg(ctx)
        val p = dp(ctx, 8f).toInt()
        setPadding(p, p / 2, p, p / 2)
        maxLines = 1
        ellipsize = android.text.TextUtils.TruncateAt.END
    }

    /** Panel section: silkscreen title with a rule, on a metal plate; add controls to [body]. */
    class Section(ctx: Context, title: String, tr: Boolean = true) : LinearLayout(ctx) {
        val body: LinearLayout
        private val head: TextView
        private val rule = Paint().apply { color = Color.rgb(84, 88, 98) }
        init {
            orientation = VERTICAL
            background = panelBg(ctx)
            val p = dp(ctx, 10f).toInt()
            setPadding(p, dp(ctx, 6f).toInt(), p, p)
            head = engraved(ctx, title, 12f, tr).apply { setTextColor(SILK) }
            // full width like the body: a wrap-content section then sizes to its widest content
            addView(head, LayoutParams(-1, -2).apply { bottomMargin = dp(ctx, 6f).toInt() })
            body = LinearLayout(ctx).apply { orientation = VERTICAL }
            addView(body, LayoutParams(-1, -2))
        }
        // silkscreen rule after the title (drawn, so it never widens the section)
        override fun dispatchDraw(c: Canvas) {
            super.dispatchDraw(c)
            val y = head.top + head.height / 2f
            val x0 = head.left + head.paddingLeft + head.paint.measureText(head.text.toString()) + dp(context, 8f)
            val x1 = width - paddingRight.toFloat()
            if (x1 > x0) c.drawRect(x0, y, x1, y + dp(context, 1f), rule)
        }
    }

    fun sectionLp(ctx: Context) = LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = dp(ctx, 8f).toInt() }
}

/**
 * On / off as on a hardware panel: an LED over a slide switch. [vertical]
 * stacks LED, switch and label (parameter panels); otherwise they sit in a
 * row (settings, effect slots). Tap anywhere to toggle.
 */
class ToggleSwitch(ctx: Context, label: String, tr: Boolean = true, private val vertical: Boolean = true) : View(ctx) {
    var on = false
        set(v) { field = v; invalidate() }
    var onToggle: ((Boolean) -> Unit)? = null
    private val text = if (tr) L10n.t(label) else label
    private val d = resources.displayMetrics.density
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)
    private val txt = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Hw.BOLD; color = Hw.ENGRAVE; textSize = 12f * d }
    private val r = RectF()

    init { isClickable = true; setOnClickListener { on = !on; onToggle?.invoke(on) } }

    override fun onMeasure(widthSpec: Int, heightSpec: Int) {
        val label = if (text.any { it.code > 0x2FF }) text else text.uppercase()
        if (vertical) {
            setMeasuredDimension(max(64 * d, txt.measureText(label) + 8 * d).toInt().coerceAtMost((120 * d).toInt()), (92 * d).toInt())
        } else {
            val w = (52 * d + txt.measureText(label) + 4 * d).toInt()
            setMeasuredDimension(if (MeasureSpec.getMode(widthSpec) == MeasureSpec.EXACTLY) MeasureSpec.getSize(widthSpec) else w, (40 * d).toInt())
        }
    }

    private fun led(c: Canvas, cx: Float, cy: Float) {
        if (on) {
            p.shader = RadialGradient(cx, cy, 9 * d, Color.argb(150, 255, 140, 26), Color.argb(0, 255, 140, 26), Shader.TileMode.CLAMP)
            c.drawCircle(cx, cy, 9 * d, p)
            p.shader = null
        }
        p.color = Color.rgb(6, 6, 7)
        c.drawCircle(cx, cy, 4.2f * d, p)
        p.color = if (on) Hw.LED else Hw.LED_OFF
        c.drawCircle(cx, cy, 3.2f * d, p)
        if (on) { p.color = Color.argb(200, 255, 230, 190); c.drawCircle(cx - 1f * d, cy - 1f * d, 1.1f * d, p) }
    }

    /** Slot with a cap that sits at the "on" end (top / right). */
    private fun slide(c: Canvas, l: Float, t: Float, rr: Float, b: Float, horizontal: Boolean) {
        r.set(l, t, rr, b)
        p.color = Color.rgb(6, 7, 8)
        c.drawRoundRect(r, 2 * d, 2 * d, p)
        val inset = 2.5f * d
        val cap = RectF()
        if (horizontal) {
            val cw = (rr - l) * 0.5f
            val x0 = if (on) rr - inset - cw else l + inset
            cap.set(x0, t + inset, x0 + cw, b - inset)
        } else {
            val ch = (b - t) * 0.5f
            val y0 = if (on) t + inset else b - inset - ch
            cap.set(l + inset, y0, rr - inset, y0 + ch)
        }
        p.shader = LinearGradient(0f, cap.top, 0f, cap.bottom, Color.rgb(205, 209, 216), Color.rgb(120, 125, 135), Shader.TileMode.CLAMP)
        c.drawRoundRect(cap, 1.5f * d, 1.5f * d, p)
        p.shader = null
        // grip lines
        p.color = Color.argb(110, 0, 0, 0)
        p.strokeWidth = 1f * d
        if (horizontal) for (i in -1..1) c.drawLine(cap.centerX() + i * 3 * d, cap.top + 3 * d, cap.centerX() + i * 3 * d, cap.bottom - 3 * d, p)
        else for (i in -1..1) c.drawLine(cap.left + 3 * d, cap.centerY() + i * 3 * d, cap.right - 3 * d, cap.centerY() + i * 3 * d, p)
    }

    override fun onDraw(c: Canvas) {
        val label = if (text.any { it.code > 0x2FF }) text else text.uppercase()
        if (vertical) {
            val cx = width / 2f
            led(c, cx, 8 * d)
            slide(c, cx - 11 * d, 18 * d, cx + 11 * d, 62 * d, false)
            txt.textAlign = Paint.Align.CENTER
            c.drawText(fit(label, width.toFloat()), cx, 80 * d, txt)
        } else {
            val cy = height / 2f
            led(c, 8 * d, cy)
            slide(c, 18 * d, cy - 10 * d, 46 * d, cy + 10 * d, true)
            txt.textAlign = Paint.Align.LEFT
            c.drawText(fit(label, width - 52 * d), 52 * d, cy + 4.5f * d, txt)
        }
    }

    private fun fit(s: String, w: Float): String {
        if (txt.measureText(s) <= w) return s
        var e = s
        while (e.length > 1 && txt.measureText("$e…") > w) e = e.dropLast(1)
        return "$e…"
    }
}

/** Lays children out left to right and wraps to new lines. */
class FlowLayout(ctx: Context, private val hGap: Int = Hw.dp(ctx, 10f).toInt(), private val vGap: Int = Hw.dp(ctx, 8f).toInt()) : ViewGroup(ctx) {
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

/** Push button with an LED; [lit] lights it. */
class LedButton(ctx: Context, label: String, tr: Boolean = true, private val led: Boolean = true) : TextView(ctx) {
    var lit = false
        set(v) { field = v; invalidate() }
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)
    private val d = resources.displayMetrics.density

    init {
        text = if (tr) L10n.t(label) else label
        textSize = 15f
        setTextColor(Color.rgb(232, 235, 241))
        typeface = Hw.BOLD
        letterSpacing = 0.03f
        gravity = Gravity.CENTER
        maxLines = 1
        background = Hw.keyBg(ctx)
        val lp = if (led) (18 * d).toInt() else (12 * d).toInt()
        setPadding(lp, (8 * d).toInt(), (12 * d).toInt(), (8 * d).toInt())
        minHeight = (40 * d).toInt()
        isClickable = true
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> background = Hw.keyBg(context, true)
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> background = Hw.keyBg(context)
        }
        return super.onTouchEvent(e)
    }

    override fun onDraw(c: Canvas) {
        super.onDraw(c)
        if (!led) return
        val cx = 9 * d
        val cy = height / 2f
        if (lit) {
            p.color = Color.argb(90, Color.red(Hw.LED), Color.green(Hw.LED), Color.blue(Hw.LED))
            c.drawCircle(cx, cy, 5.5f * d, p)
        }
        p.color = Color.rgb(6, 6, 7)
        c.drawCircle(cx, cy, 4.2f * d, p)
        p.color = if (lit) Hw.LED else Hw.LED_OFF
        c.drawCircle(cx, cy, 3.2f * d, p)
    }
}

/**
 * Rotary knob: drag up / down (or sideways) to turn, double tap for the
 * default. Value 0..1; [bipolar] draws the arc from the centre.
 */
class KnobView(ctx: Context, private val label: String, private val bipolar: Boolean = false, sizeDp: Float = 52f) : View(ctx) {
    var value = 0f
        set(v) { field = v.coerceIn(0f, 1f); invalidate() }
    var valueText = ""
        set(v) { field = v; invalidate() }
    var onChange: ((Float) -> Unit)? = null
    var onReset: (() -> Unit)? = null
    var onTouchState: ((Boolean) -> Unit)? = null
    var onLongPress: (() -> Unit)? = null

    private val d = resources.displayMetrics.density
    private val knob = sizeDp * d
    private val w = max(knob + 12 * d, 58 * d)
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)
    private val txt = Paint(Paint.ANTI_ALIAS_FLAG).apply { textAlign = Paint.Align.CENTER }
    private val arc = RectF()
    private var downY = 0f
    private var downX = 0f
    private var downV = 0f
    private val gestures = GestureDetector(ctx, object : GestureDetector.SimpleOnGestureListener() {
        override fun onDoubleTap(e: MotionEvent): Boolean { onReset?.invoke(); return true }
        override fun onLongPress(e: MotionEvent) { if (abs(e.y - downY) < 8 * d) onLongPress?.invoke() }
    })

    override fun onMeasure(widthSpec: Int, heightSpec: Int) {
        setMeasuredDimension(w.toInt(), (knob + 40 * d).toInt())
    }

    override fun onDraw(c: Canvas) {
        val cx = width / 2f
        val r = knob / 2f
        val cy = r + 4 * d
        // LED ring: 15 dots over 270 degrees (from 135); bipolar lights from the centre
        p.style = Paint.Style.FILL
        val dots = 15
        val ringR = r + 0.5f * d
        for (i in 0 until dots) {
            val t = i / (dots - 1f)
            val lit = if (bipolar) (t >= min(0.5f, value) - 0.001f && t <= max(0.5f, value) + 0.001f) else (value > 0f && t <= value + 0.001f)
            val ang = Math.toRadians(135.0 + 270.0 * t)
            val x = cx + (ringR * cos(ang)).toFloat()
            val y = cy + (ringR * sin(ang)).toFloat()
            p.color = Color.rgb(6, 6, 7)
            c.drawCircle(x, y, 2.4f * d, p)
            p.color = if (lit) Hw.LED else Hw.LED_OFF
            c.drawCircle(x, y, 1.7f * d, p)
        }
        // body: knurled skirt + flat cap
        val br = r - 6 * d
        p.color = Color.rgb(12, 13, 15)
        c.drawCircle(cx, cy + 1.5f * d, br + 1 * d, p)
        p.color = Color.rgb(36, 38, 43)
        c.drawCircle(cx, cy, br, p)
        p.style = Paint.Style.STROKE
        p.strokeWidth = 1f * d
        p.color = Color.rgb(20, 21, 24)
        for (i in 0 until 36) {
            val ang = Math.toRadians(i * 10.0)
            c.drawLine(cx + ((br - 3 * d) * cos(ang)).toFloat(), cy + ((br - 3 * d) * sin(ang)).toFloat(),
                cx + (br * cos(ang)).toFloat(), cy + (br * sin(ang)).toFloat(), p)
        }
        p.style = Paint.Style.FILL
        val capR = br - 3.5f * d
        p.shader = LinearGradient(0f, cy - capR, 0f, cy + capR, Color.rgb(72, 76, 85), Color.rgb(30, 32, 37), Shader.TileMode.CLAMP)
        c.drawCircle(cx, cy, capR, p)
        p.shader = null
        // pointer: a white line across the cap
        val ang = Math.toRadians((135.0 + 270.0 * value))
        p.style = Paint.Style.STROKE
        p.strokeCap = Paint.Cap.SQUARE
        p.strokeWidth = 2.4f * d
        p.color = Color.rgb(245, 246, 250)
        c.drawLine(cx + (capR * 0.25f * cos(ang)).toFloat(), cy + (capR * 0.25f * sin(ang)).toFloat(),
            cx + ((br - 1 * d) * cos(ang)).toFloat(), cy + ((br - 1 * d) * sin(ang)).toFloat(), p)
        p.style = Paint.Style.FILL
        // label and value
        txt.textSize = 12f * d
        txt.color = Hw.ENGRAVE
        txt.typeface = Hw.BOLD
        c.drawText(fit(if (label.any { it.code > 0x2FF }) label else label.uppercase(), width - 2 * d), cx, knob + 18 * d, txt)
        txt.textSize = 11.5f * d
        txt.color = if (isPressed) Hw.LED else Hw.LCD_FG
        txt.typeface = Hw.MONO
        c.drawText(fit(valueText, width - 2 * d), cx, knob + 32 * d, txt)
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
            }
            MotionEvent.ACTION_MOVE -> {
                // 180 dp of travel = full range; sideways counts too
                val delta = ((downY - e.y) + (e.x - downX) * 0.5f) / (180 * d)
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
 * One parameter as a hardware control: knob (float / int), LCD selector
 * (enum, tap for the list) or LED switch (bool). Keeps itself in sync.
 */
class ParamControl(private val ctx: Context, val meta: ParamMeta, knobSize: Float = 52f, lcdWidth: Float = 116f,
                   private val onSet: ((ParamControl) -> Unit)? = null) {
    val view: View
    private var knob: KnobView? = null
    private var lcd: TextView? = null
    private var toggle: ToggleSwitch? = null

    init {
        val d = { v: Float -> Hw.dp(ctx, v).toInt() }
        when (meta.kind) {
            "enum" -> {
                val box = LinearLayout(ctx).apply { orientation = LinearLayout.VERTICAL }
                box.addView(Hw.engraved(ctx, meta.label, 10.5f, tr = false))
                val l = Hw.lcd(ctx, "", 13f).apply {
                    minWidth = d(lcdWidth)
                    setOnClickListener { choose() }
                    setOnLongClickListener { learnMenu(); true }
                }
                box.addView(l, LinearLayout.LayoutParams(-2, -2).apply { topMargin = d(4f) })
                lcd = l
                view = box
            }
            "bool" -> {
                val t = ToggleSwitch(ctx, meta.label, tr = false)
                t.onToggle = { on -> History.checkpoint(); set(if (on) 1f else 0f) }
                t.setOnLongClickListener { learnMenu(); true }
                toggle = t
                view = t
            }
            else -> {
                val bip = meta.min < 0f && meta.max > 0f
                val k = KnobView(ctx, meta.label, bip, knobSize)
                k.onChange = { t -> set(meta.fromNormalized(t), fromKnob = true) }
                k.onReset = { History.checkpoint(); set(meta.def) }
                k.onTouchState = { down ->
                    if (down) {
                        History.checkpoint()
                        Hw.onParamTouched?.invoke(meta.label, meta.format(NativeEngine.nativeGetParam(meta.id)))
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

    /** Long press: assign / remove a MIDI controller. */
    private fun learnMenu() {
        val a = ctx as? MainActivity ?: return
        val cur = NativeEngine.nativeLearnDescribe(meta.id)
        val items = if (cur.isEmpty()) arrayOf("MIDI Learn") else arrayOf("MIDI Learn", "Clear MIDI ($cur)")
        AlertDialog.Builder(ctx).setTitle(meta.label).setItems(items) { _, w ->
            if (w == 0) a.learn(meta) else a.unlearn(meta)
        }.show()
    }

    fun set(v: Float, fromKnob: Boolean = false) {
        NativeEngine.setParam(meta.id, v)
        val now = NativeEngine.nativeGetParam(meta.id)
        Hw.onParamTouched?.invoke(meta.label, meta.format(now))
        if (fromKnob) knob?.valueText = meta.format(now) else refresh()
        onSet?.invoke(this)
    }

    fun refresh() {
        val v = NativeEngine.nativeGetParam(meta.id)
        knob?.let {
            it.value = if (meta.kind == "int" && meta.max > meta.min) (v - meta.min) / (meta.max - meta.min) else meta.toNormalized(v)
            it.valueText = meta.format(v)
        }
        lcd?.text = meta.format(v) + "  ▾"
        toggle?.on = v > 0.5f
    }
}

/** Header strip of the main window: a gradient bar. */
class ChassisBar(ctx: Context) : LinearLayout(ctx) {
    private val p = Paint()
    init {
        orientation = HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        setWillNotDraw(false)
    }
    override fun onDraw(c: Canvas) {
        p.shader = LinearGradient(0f, 0f, 0f, height.toFloat(), Color.rgb(38, 40, 46), Color.rgb(22, 23, 27), Shader.TileMode.CLAMP)
        c.drawRect(0f, 0f, width.toFloat(), height.toFloat(), p)
        p.shader = null
        p.color = Color.rgb(8, 9, 10)
        c.drawRect(0f, height - 2f, width.toFloat(), height.toFloat(), p)
    }
}
