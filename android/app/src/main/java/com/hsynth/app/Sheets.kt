package com.hsynth.app

import android.animation.ValueAnimator
import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.drawable.GradientDrawable
import android.view.Gravity
import android.view.MotionEvent
import android.view.VelocityTracker
import android.view.View
import android.view.ViewConfiguration
import android.view.ViewGroup
import android.view.ViewParent
import android.view.animation.AccelerateInterpolator
import android.view.animation.DecelerateInterpolator
import android.widget.FrameLayout
import kotlin.math.min

/** Where a [Sheet] is docked inside the [SheetHost]. */
enum class Edge { BOTTOM, RIGHT, CENTER }

/**
 * One modal drawer (Master EQ, TOOLS, Memo, Slot editor …). Build [view] once and cache it in the owner;
 * the host re-parents it on every [SheetHost.open], so re-opening allocates nothing.
 *
 * Geometry ([sizeDp] / [crossDp], all in dp, clamped to the host):
 *  - BOTTOM: height = [sizeDp] (default 232), width = [crossDp] or the full host width
 *  - RIGHT : width  = [sizeDp] (default 340), height = [crossDp] or the full host height
 *  - CENTER: width  = [sizeDp] (default 560), height = [crossDp] (default 300)
 *
 * The host reserves an 8 dp inset on the grab-handle side (top for BOTTOM / CENTER, left for RIGHT), so the
 * sheet content never has to care about the handle.
 */
abstract class Sheet(val edge: Edge, val sizeDp: Int = 0, val crossDp: Int = 0) {
    /** Content, built once and cached by the owner. */
    abstract val view: View

    /** Panel surface colour (use [Hw.WELL] for a pure-black reading sheet). */
    open val surface: Int get() = Hw.PANEL

    /** Sheet became visible: start tickers, refresh values. */
    open fun onShow() {}

    /** Sheet starts closing (or was replaced): stop tickers. */
    open fun onHide() {}

    /** Set by the host while the sheet is open. */
    var host: SheetHost? = null
        internal set

    val isShowing: Boolean get() = host?.current === this

    /** Close this sheet (e.g. from its ✕ button). */
    fun dismiss() {
        val h = host ?: return
        if (h.current === this) h.close()
    }
}

/**
 * Overlay that hosts at most one [Sheet] at a time. Put it as the LAST child of the content `FrameLayout`
 * (`contentHost = FrameLayout { content, sheetHost }`) so the telemetry strip and the rail stay live.
 *
 * - `GONE` while idle (zero cost); content is cached by the sheets themselves.
 * - Black scrim `#B3000000` consumes every touch: tapping it, [onBackPressed] or a swipe on the grab handle
 *   closes the sheet, and a stray tap can never reach a slot / scene underneath.
 * - Enter / exit: 160 ms [android.view.ViewPropertyAnimator] (framework driven, no per-frame code), skipped when
 *   the system animator scale is 0 (or animators are disabled by power saving).
 * - While open, [coversView] tells [FrameTicker] which views are hidden under the scrim so they are parked.
 */
class SheetHost(ctx: Context) : FrameLayout(ctx) {
    var current: Sheet? = null
        private set

    val isOpen: Boolean get() = current != null

    private val d = resources.displayMetrics.density
    private val decel = DecelerateInterpolator()
    private val accel = AccelerateInterpolator()
    private val scrim = View(ctx)
    private val panel = Panel(ctx)
    private val radius = 12f * d
    private val bgBottom = bg(floatArrayOf(radius, radius, radius, radius, 0f, 0f, 0f, 0f))
    private val bgRight = bg(floatArrayOf(radius, radius, 0f, 0f, 0f, 0f, radius, radius))
    private val bgCenter = bg(floatArrayOf(radius, radius, radius, radius, radius, radius, radius, radius))
    private var leaving: Sheet? = null
    private var gen = 0
    private var panelW = 0
    private var panelH = 0
    private val coverFn: (View) -> Boolean = { coversView(it) }

    init {
        visibility = GONE
        scrim.setBackgroundColor(SCRIM)
        scrim.isClickable = true
        scrim.setOnClickListener { close() }
        addView(scrim, LayoutParams(-1, -1))
        addView(panel, LayoutParams(-2, -2))
    }

    // ------------------------------------------------------------------ public API

    /** Open [s] (replacing any other open sheet). Re-opening the open sheet is a no-op. */
    fun open(s: Sheet) {
        if (current === s) return
        current?.let { prev ->
            current = null
            prev.onHide()
            finish(prev)
        }
        leaving?.let { finish(it) }

        val token = ++gen
        panel.animate().cancel()
        scrim.animate().cancel()

        current = s
        s.host = this
        val content = s.view
        (content.parent as? ViewGroup)?.removeView(content)
        panel.removeAllViews()
        panel.edge = s.edge
        panel.background = when (s.edge) { Edge.BOTTOM -> bgBottom; Edge.RIGHT -> bgRight; Edge.CENTER -> bgCenter }
        (panel.background as GradientDrawable).setColor(s.surface)
        val inset = (HANDLE_INSET_DP * d).toInt()
        panel.setPadding(if (s.edge == Edge.RIGHT) inset else 0, if (s.edge == Edge.RIGHT) 0 else inset, 0, 0)
        panel.addView(content, LayoutParams(-1, -1))
        applySize(s)

        visibility = VISIBLE
        when (s.edge) {
            Edge.BOTTOM -> { panel.translationX = 0f; panel.translationY = panelH.toFloat(); panel.alpha = 1f }
            Edge.RIGHT -> { panel.translationX = panelW.toFloat(); panel.translationY = 0f; panel.alpha = 1f }
            Edge.CENTER -> { panel.translationX = 0f; panel.translationY = 24f * d; panel.alpha = 0f }
        }
        scrim.alpha = 0f
        s.onShow()
        FrameTicker.poke()

        if (!ValueAnimator.areAnimatorsEnabled()) {
            settleOpen()
            return
        }
        scrim.animate().alpha(1f).setDuration(DURATION_MS).setInterpolator(decel).start()
        panel.animate().translationX(0f).translationY(0f).alpha(1f)
            .setDuration(DURATION_MS).setInterpolator(decel)
            .withEndAction { if (gen == token) FrameTicker.poke() }
            .start()
    }

    /** Close the open sheet (animated). Safe to call when nothing is open. */
    fun close() {
        val s = current ?: return
        current = null
        leaving = s
        val token = ++gen
        s.onHide()
        FrameTicker.poke()

        if (!ValueAnimator.areAnimatorsEnabled() || visibility != VISIBLE) {
            finish(s)
            return
        }
        scrim.animate().alpha(0f).setDuration(DURATION_MS).setInterpolator(accel).start()
        val p = panel.animate().setDuration(DURATION_MS).setInterpolator(accel)
        when (s.edge) {
            Edge.BOTTOM -> p.translationY(panelH.toFloat())
            Edge.RIGHT -> p.translationX(panelW.toFloat())
            Edge.CENTER -> p.translationY(24f * d).alpha(0f)
        }
        p.withEndAction { if (gen == token) finish(s) }.start()
    }

    fun toggle(s: Sheet) { if (current === s) close() else open(s) }

    /** Back key: closes the open sheet first. @return true if it consumed the key. */
    fun onBackPressed(): Boolean {
        if (current == null) return false
        close()
        return true
    }

    /**
     * True when a sheet is open and [v] lives under this host's content area but is not part of the sheet itself,
     * i.e. it is hidden behind the scrim. Used by [FrameTicker] (no allocations).
     */
    fun coversView(v: View): Boolean {
        if (current == null || isDescendant(v, panel)) return false
        val area = parent as? View ?: return true
        return isDescendant(v, area)
    }

    // ------------------------------------------------------------------ internals

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        FrameTicker.coverCheck = coverFn
    }

    override fun onDetachedFromWindow() {
        if (FrameTicker.coverCheck === coverFn) FrameTicker.coverCheck = null
        super.onDetachedFromWindow()
    }

    // soft keyboard (adjustResize) / size change: keep the open sheet inside the host
    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        if (current != null) post { current?.let { applySize(it) } }
    }

    private fun isDescendant(v: View, anc: View): Boolean {
        if (v === anc) return true
        var p: ViewParent? = v.parent
        while (p != null) {
            if (p === anc) return true
            p = p.parent
        }
        return false
    }

    private fun bg(radii: FloatArray) = GradientDrawable().apply {
        setColor(Hw.PANEL)
        cornerRadii = radii
        setStroke(d.toInt().coerceAtLeast(1), Hw.PANEL_EDGE)
    }

    private fun applySize(s: Sheet) {
        val par = parent as? View
        val hostW = (par?.width ?: width).takeIf { it > 0 } ?: resources.displayMetrics.widthPixels
        val hostH = (par?.height ?: height).takeIf { it > 0 } ?: resources.displayMetrics.heightPixels
        val margin = (8f * d).toInt()
        val lp = panel.layoutParams as LayoutParams
        when (s.edge) {
            Edge.BOTTOM -> {
                panelH = min((s.sizeDp.takeIf { it > 0 } ?: 232).times(d).toInt(), hostH)
                panelW = if (s.crossDp > 0) min((s.crossDp * d).toInt(), hostW) else hostW
                lp.gravity = Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL
            }
            Edge.RIGHT -> {
                panelW = min((s.sizeDp.takeIf { it > 0 } ?: 340).times(d).toInt(), hostW)
                panelH = if (s.crossDp > 0) min((s.crossDp * d).toInt(), hostH) else hostH
                lp.gravity = Gravity.END or Gravity.CENTER_VERTICAL
            }
            Edge.CENTER -> {
                panelW = min((s.sizeDp.takeIf { it > 0 } ?: 560).times(d).toInt(), hostW - 2 * margin)
                panelH = min((s.crossDp.takeIf { it > 0 } ?: 300).times(d).toInt(), hostH - 2 * margin)
                lp.gravity = Gravity.CENTER
            }
        }
        lp.width = panelW
        lp.height = panelH
        panel.requestLayout()
    }

    private fun settleOpen() {
        panel.translationX = 0f
        panel.translationY = 0f
        panel.alpha = 1f
        scrim.alpha = 1f
        FrameTicker.poke()
    }

    private fun finish(s: Sheet) {
        if (leaving === s) leaving = null
        if (s.host === this && current !== s) s.host = null
        if (current == null) {
            visibility = GONE
            panel.removeAllViews()
            panel.translationX = 0f
            panel.translationY = 0f
            panel.alpha = 1f
            scrim.alpha = 1f
        } else if (s.view.parent === panel && current !== s) {
            panel.removeView(s.view)
        }
        FrameTicker.poke()
    }

    private fun snapBack() {
        if (!ValueAnimator.areAnimatorsEnabled()) { settleOpen(); return }
        scrim.animate().alpha(1f).setDuration(DURATION_MS).setInterpolator(decel).start()
        panel.animate().translationX(0f).translationY(0f).setDuration(DURATION_MS).setInterpolator(decel).start()
    }

    /** The sheet surface: consumes all touches, draws the 4×28 dp grab handle, swipe-to-dismiss from it. */
    private inner class Panel(ctx: Context) : FrameLayout(ctx) {
        var edge = Edge.BOTTOM
        private val slop = ViewConfiguration.get(ctx).scaledTouchSlop
        private val grabPx = GRAB_ZONE_DP * d
        private val handleP = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Hw.PANEL_HI }
        private val handleR = RectF()
        private var tracking = false
        private var dragging = false
        private var downRaw = 0f
        private var vt: VelocityTracker? = null

        init {
            isClickable = true       // never let a tap inside the panel fall through to the scrim
            setWillNotDraw(false)
        }

        private fun raw(e: MotionEvent) = if (edge == Edge.RIGHT) e.rawX else e.rawY
        private fun inGrab(e: MotionEvent) = if (edge == Edge.RIGHT) e.x < grabPx else e.y < grabPx
        private fun extent() = (if (edge == Edge.RIGHT) width else height).toFloat().coerceAtLeast(1f)

        private fun begin(e: MotionEvent) {
            tracking = inGrab(e)
            dragging = false
            vt?.recycle()
            vt = null
            if (tracking) {
                downRaw = raw(e)
                vt = VelocityTracker.obtain().also { it.addMovement(e) }
            }
        }

        private fun end() {
            tracking = false
            dragging = false
            vt?.recycle()
            vt = null
        }

        override fun onInterceptTouchEvent(e: MotionEvent): Boolean {
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN -> begin(e)
                MotionEvent.ACTION_MOVE -> if (tracking && !dragging) {
                    vt?.addMovement(e)
                    if (raw(e) - downRaw > slop) {
                        dragging = true
                        parent?.requestDisallowInterceptTouchEvent(true)
                    }
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> if (!dragging) end()
            }
            return dragging
        }

        @SuppressLint("ClickableViewAccessibility")
        override fun onTouchEvent(e: MotionEvent): Boolean {
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN -> begin(e)
                MotionEvent.ACTION_MOVE -> if (tracking) {
                    vt?.addMovement(e)
                    val dd = raw(e) - downRaw
                    if (!dragging && dd > slop) dragging = true
                    if (dragging) {
                        val t = dd.coerceAtLeast(0f)
                        if (edge == Edge.RIGHT) translationX = t else translationY = t
                        scrim.alpha = 1f - (t / extent()).coerceIn(0f, 1f)
                    }
                }
                MotionEvent.ACTION_UP -> {
                    if (dragging) {
                        val t = if (edge == Edge.RIGHT) translationX else translationY
                        vt?.let {
                            it.addMovement(e)
                            it.computeCurrentVelocity(1000)
                        }
                        val v = vt?.let { if (edge == Edge.RIGHT) it.xVelocity else it.yVelocity } ?: 0f
                        if (t / extent() > 0.3f || v > 900f * d) close() else snapBack()
                    }
                    end()
                }
                MotionEvent.ACTION_CANCEL -> {
                    if (dragging) snapBack()
                    end()
                }
            }
            return true
        }

        override fun dispatchDraw(c: Canvas) {
            super.dispatchDraw(c)
            val w = width.toFloat()
            val h = height.toFloat()
            if (edge == Edge.RIGHT) {
                handleR.set(3f * d, (h - 28f * d) / 2f, 7f * d, (h + 28f * d) / 2f)
            } else {
                handleR.set((w - 28f * d) / 2f, 3f * d, (w + 28f * d) / 2f, 7f * d)
            }
            c.drawRoundRect(handleR, 2f * d, 2f * d, handleP)
        }
    }

    companion object {
        /** Pure black, 70 % — darkens the stage without lighting extra OLED pixels. */
        const val SCRIM = 0xB3000000.toInt()
        const val DURATION_MS = 160L
        private const val HANDLE_INSET_DP = 8f
        private const val GRAB_ZONE_DP = 24f
    }
}
