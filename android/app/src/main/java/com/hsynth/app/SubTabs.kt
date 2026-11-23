package com.hsynth.app

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.view.MotionEvent
import android.view.View
import kotlin.math.max

/**
 * Obsidian sub-tab strip: ONE custom [View] (no children), 26 dp high.
 *
 * Pills are `max(56 dp, text + 24 dp)` wide (compressed evenly if they cannot fit), the selected pill gets an
 * accent label, a faint accent fill and a 2 dp accent underline; an optional per-tab badge dot (e.g. red "REC"
 * on PHRASE) sits at the pill's top-right corner.
 *
 * Pill rectangles and the (ellipsized) label strings are computed in [onSizeChanged] / [setLabels],
 * never in [onDraw], which only fills pre-allocated [Paint]s / [RectF]s.
 */
class SubTabBar(
    ctx: Context,
    labels: Array<String>,
    var accent: Int = Hw.MOD_OSC,
    private val tr: Boolean = true,
) : View(ctx) {
    var onSelect: ((Int) -> Unit)? = null

    /** Selected tab. Setting it only redraws; use [onSelect] for user taps. */
    var selected = 0
        set(v) { val c = v.coerceIn(0, max(0, count - 1)); if (c != field) { field = c; invalidate() } }

    val count: Int get() = shown.size

    private val d = resources.displayMetrics.density
    private var raw: Array<String> = labels
    private var shown: Array<String> = Array(labels.size) { "" }
    private var badge = IntArray(labels.size)          // 0 = none, otherwise the dot colour
    private var left = FloatArray(labels.size)
    private var right = FloatArray(labels.size)
    private var textW = FloatArray(labels.size)
    private var pressedIdx = -1

    private val fillP = Paint(Paint.ANTI_ALIAS_FLAG)
    private val txtP = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        typeface = Hw.BOLD
        textSize = 11.5f * d
        letterSpacing = 0.06f
        textAlign = Paint.Align.CENTER
    }
    private val rule = Paint().apply { color = Hw.PANEL_EDGE }
    private val r = RectF()
    private var baseline = 0f

    init {
        isClickable = true
        rebuildLabels()
    }

    /** Replace the tab labels (keeps [selected] if still valid). Badges are cleared. */
    fun setLabels(labels: Array<String>) {
        raw = labels
        shown = Array(labels.size) { "" }
        badge = IntArray(labels.size)
        left = FloatArray(labels.size)
        right = FloatArray(labels.size)
        textW = FloatArray(labels.size)
        if (selected >= labels.size) selected = max(0, labels.size - 1)
        rebuildLabels()
        layoutPills()
        requestLayout()
        invalidate()
    }

    /** Show a small dot on tab [index] in [color] (e.g. [Hw.MOD_RED]); pass 0 to hide it. */
    fun setBadge(index: Int, color: Int) {
        if (index !in badge.indices || badge[index] == color) return
        badge[index] = color
        invalidate()
    }

    private fun rebuildLabels() {
        for (i in raw.indices) {
            val t = if (tr) L10n.t(raw[i]) else raw[i]
            shown[i] = if (t.any { it.code > 0x2FF }) t else t.uppercase()
            textW[i] = txtP.measureText(shown[i])
        }
    }

    private fun desiredWidth(): Float {
        if (shown.isEmpty()) return 0f
        var w = 0f
        for (i in shown.indices) w += max(56f * d, textW[i] + 24f * d)
        return w + (shown.size - 1) * 4f * d
    }

    override fun onMeasure(widthSpec: Int, heightSpec: Int) {
        val mode = MeasureSpec.getMode(widthSpec)
        val size = MeasureSpec.getSize(widthSpec)
        val w = when (mode) {
            MeasureSpec.EXACTLY -> size
            MeasureSpec.AT_MOST -> desiredWidth().toInt().coerceAtMost(size)
            else -> desiredWidth().toInt()
        }
        setMeasuredDimension(w, (26 * d).toInt())
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        baseline = h / 2f - (txtP.ascent() + txtP.descent()) / 2f
        layoutPills()
    }

    private fun layoutPills() {
        val n = shown.size
        val w = width.toFloat()
        if (n == 0 || w <= 0f) return
        rebuildLabels()   // un-ellipsize after a previous compressed layout
        val gap = 4f * d
        val want = desiredWidth()
        val avail = w
        if (want <= avail) {
            var x = 0f
            for (i in 0 until n) {
                val pw = max(56f * d, textW[i] + 24f * d)
                left[i] = x
                right[i] = x + pw
                x += pw + gap
            }
        } else {
            // does not fit: share the width evenly, ellipsize labels to the pill
            val pw = (avail - gap * (n - 1)) / n
            for (i in 0 until n) {
                left[i] = i * (pw + gap)
                right[i] = left[i] + pw
            }
            for (i in 0 until n) {
                var s = if (tr) L10n.t(raw[i]) else raw[i]
                if (!s.any { it.code > 0x2FF }) s = s.uppercase()
                val maxW = pw - 10f * d
                if (txtP.measureText(s) > maxW) {
                    while (s.length > 1 && txtP.measureText("$s…") > maxW) s = s.dropLast(1)
                    s = "$s…"
                }
                shown[i] = s
            }
        }
    }

    private fun hit(x: Float): Int {
        for (i in shown.indices) if (x >= left[i] - 2f * d && x <= right[i] + 2f * d) return i
        return -1
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                pressedIdx = hit(e.x)
                invalidate()
            }
            MotionEvent.ACTION_MOVE -> {
                val h = hit(e.x)
                if (h != pressedIdx) { pressedIdx = h; invalidate() }
            }
            MotionEvent.ACTION_UP -> {
                val i = pressedIdx
                pressedIdx = -1
                invalidate()
                if (i >= 0 && hit(e.x) == i) {
                    performClick()
                    if (i != selected) {
                        selected = i
                        onSelect?.invoke(i)
                    }
                }
            }
            MotionEvent.ACTION_CANCEL -> { pressedIdx = -1; invalidate() }
        }
        return true
    }

    override fun performClick(): Boolean = super.performClick()

    override fun onDraw(c: Canvas) {
        val h = height.toFloat()
        // bottom hairline
        c.drawRect(0f, h - d, width.toFloat(), h, rule)
        for (i in shown.indices) {
            val sel = i == selected
            val l = left[i]
            val rr = right[i]
            if (sel || i == pressedIdx) {
                r.set(l, 2f * d, rr, h - 3f * d)
                fillP.style = Paint.Style.FILL
                fillP.color = if (sel) Hw.withAlpha(accent, 36) else Hw.PANEL_RAISED
                c.drawRoundRect(r, 6f * d, 6f * d, fillP)
            }
            txtP.color = if (sel) accent else if (i == pressedIdx) Hw.SILK else Hw.ENGRAVE
            c.drawText(shown[i], (l + rr) / 2f, baseline - d, txtP)
            if (sel) {
                fillP.color = accent
                r.set(l + 8f * d, h - 3f * d, rr - 8f * d, h - d)
                c.drawRoundRect(r, d, d, fillP)
            }
            val b = badge[i]
            if (b != 0) {
                fillP.color = b
                c.drawCircle(rr - 6f * d, 7f * d, 2.6f * d, fillP)
            }
        }
    }
}
