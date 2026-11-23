package com.hsynth.app

import android.annotation.SuppressLint
import android.app.AlertDialog
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RadialGradient
import android.graphics.RectF
import android.graphics.Shader
import android.text.InputType
import android.view.Gravity
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.View
import android.view.ViewConfiguration
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import org.json.JSONArray
import org.json.JSONObject
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.Random
import kotlin.math.max
import kotlin.math.min

/**
 * Phase 5 — AI Morph: an X/Y pad whose sound comes from a tiny network (compiled on the Edge TPU) that interpolates
 * between up to 16 anchor sounds. This file has NO dependency on the Phase 5 JNI: [AiMorphSheet] talks to the engine
 * through [NpuMorphBridge] (Wave 2 implements it with `NativeEngine.*`).
 *
 * All JSON-returning calls follow the app convention: `{"ok":true,…}` or `{"ok":false,"error":"…"}` (an empty string is
 * treated as success). Calls that load / compile run on `a.loader`; [eval] runs on the UI thread while dragging.
 */
interface NpuMorphBridge {
    /** `{"device":"Tensor G2","backend":"NNAPI (Edge TPU)"|"CPU","dtype":"fp16","params":12345,"lastUs":210,"compiled":true}` */
    fun statusJson(): String

    /** Store the CURRENT sound (all morphable params) as anchor [slot] (0..15) at (x, y) ∈ [0,1]². */
    fun captureAnchor(slot: Int, x: Float, y: Float, name: String): Boolean

    /** Load [path] (preset JSON) off-line into anchor [slot] at (x, y) — the live sound is not touched. Returns JSON. */
    fun loadPresetAnchor(slot: Int, x: Float, y: Float, path: String, libRoot: String): String

    fun removeAnchor(slot: Int): Boolean
    fun clearAnchors(): Boolean

    /** (Re)build and compile the morph network for the current anchors on the NPU. Returns JSON. */
    fun compileJson(): String

    /** Evaluate at (x, y), write the resulting parameters into the engine; returns the inference time in ms (< 0 = not compiled / failed). */
    fun eval(x: Float, y: Float): Float

    /** `[{"slot":0,"x":0.12,"y":0.12,"name":"Warm Pad"},…]` (or `{"anchors":[…]}`). */
    fun anchorsJson(): String
}

object AiMorphUi {
    const val MAX_ANCHORS = 16

    private val NODE = IntArray(MAX_ANCHORS).also {
        val base = Hw.PART_COLORS
        for (i in 0 until 8) it[i] = base[i]
        val extra = intArrayOf(
            Color.rgb(251, 113, 133), Color.rgb(45, 212, 191), Color.rgb(129, 140, 248), Color.rgb(250, 204, 21),
            Color.rgb(217, 70, 239), Color.rgb(96, 165, 250), Color.rgb(74, 222, 128), Color.rgb(253, 186, 116),
        )
        for (i in 0 until 8) it[8 + i] = extra[i]
    }

    fun color(slot: Int): Int = NODE[slot and (MAX_ANCHORS - 1)]
}

// ====================================================================================================== pad view

/**
 * The XY field. Obsidian well (#000) with a soft radial glow per anchor whose strength follows the cursor's proximity
 * (Voronoi-like influence), influence beams cursor → anchor, up to 16 coloured nodes with short names, a draggable
 * crosshair (24 dp grab radius) and a live telemetry badge. **Nothing is allocated in [onDraw]**: shaders / paths / text
 * buffers are built in [onSizeChanged] and in the setters, positions are applied with one reused [Matrix].
 *
 * Touch: touching within 24 dp of the cursor grabs it (relative, no jump); elsewhere the cursor jumps to the finger and
 * follows it. Tapping a node jumps onto it; long-pressing a node calls [onEditAnchor]. [onMove] is throttled to ~36 Hz
 * while dragging and always fires once more on release.
 */
class AiMorphPadView(ctx: Context) : View(ctx) {
    var onMove: ((Float, Float) -> Unit)? = null
    var onEditAnchor: ((Int) -> Unit)? = null
    var onDragStart: (() -> Unit)? = null
    var onDragEnd: (() -> Unit)? = null

    /** Shown in the centre while there are no anchors (set by the sheet, already translated). */
    var emptyHint: String = ""
        set(v) { field = v; invalidate() }

    var cursorX = 0.5f
        private set
    var cursorY = 0.5f
        private set

    private val d = resources.displayMetrics.density
    private val cap = AiMorphUi.MAX_ANCHORS

    // ---- anchors
    private var aCount = 0
    private val aSlot = IntArray(cap)
    private val aX = FloatArray(cap)
    private val aY = FloatArray(cap)
    private val aName = Array(cap) { "" }
    private val aNameW = FloatArray(cap)
    private val wBuf = FloatArray(cap)

    // ---- geometry
    private val padL = 16f * d
    private val padR = 16f * d
    private val padT = 14f * d
    private val padB = 24f * d
    private var innerW = 1f
    private var innerH = 1f
    private val well = RectF()
    private val clip = Path()
    private var grid = FloatArray(0)
    private val glowShader = arrayOfNulls<RadialGradient>(cap)
    private var cursorShader: RadialGradient? = null
    private val m = Matrix()

    // ---- paints (all preallocated)
    private val bgP = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Hw.WELL }
    private val edgeP = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Hw.PANEL_EDGE; style = Paint.Style.STROKE; strokeWidth = d }
    private val gridP = Paint().apply { color = Hw.withAlpha(Hw.PANEL_HI, 70); strokeWidth = d }
    private val glowP = Paint(Paint.ANTI_ALIAS_FLAG)
    private val beamP = Paint(Paint.ANTI_ALIAS_FLAG).apply { strokeCap = Paint.Cap.ROUND }
    private val nodeFillP = Paint(Paint.ANTI_ALIAS_FLAG)
    private val nodeRingP = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = 1.6f * d }
    private val nameP = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Hw.ENGRAVE; textSize = 10f * d; textAlign = Paint.Align.CENTER; typeface = Hw.MONO
    }
    private val crossP = Paint().apply { color = Hw.withAlpha(Hw.SILK, 60); strokeWidth = d }
    private val curRingP = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.WHITE; style = Paint.Style.STROKE; strokeWidth = 2f * d }
    private val curDotP = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.WHITE }
    private val grabP = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Hw.withAlpha(Hw.MOD_OSC, 70); style = Paint.Style.STROKE; strokeWidth = d }
    private val readP = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Hw.ENGRAVE; textSize = 11f * d; typeface = Hw.MONO }
    private val hintP = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Hw.INK_DIM; textSize = 12f * d; textAlign = Paint.Align.CENTER; typeface = Hw.LABEL
    }
    private val badgeBgP = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Hw.withAlpha(Color.BLACK, 215) }
    private val badgeEdgeP = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = d }
    private val badgeTextP = Paint(Paint.ANTI_ALIAS_FLAG).apply { textSize = 10.5f * d; typeface = Hw.BOLD; letterSpacing = 0.04f }

    // ---- cached text
    private val readout = CharArray(20)
    private var readoutLen = 0
    private val telem = CharArray(40)
    private var telemLen = 0
    private var telemNpu = false
    private var telemKey = Int.MIN_VALUE
    private val badge = RectF()

    // ---- touch
    private val slop = ViewConfiguration.get(ctx).scaledTouchSlop.toFloat()
    private val grabR = 24f * d
    private val nodeHitR = 20f * d
    private val longPressMs = ViewConfiguration.getLongPressTimeout().toLong()
    private var mode = NONE
    private var pressIdx = -1
    private var longFired = false
    private var downX = 0f
    private var downY = 0f
    private var grabDx = 0f
    private var grabDy = 0f
    private var lastEmit = 0L
    private var dragging = false
    private val longPress = Runnable {
        if (mode == ANCHOR && pressIdx in 0 until aCount) {
            longFired = true
            performHapticFeedback(HapticFeedbackConstants.LONG_PRESS)
            onEditAnchor?.invoke(aSlot[pressIdx])
        }
    }

    init {
        updateReadout()
        setTelemetry(false, -1f)
    }

    // ------------------------------------------------------------------ public API

    /** Replace the anchors. Arrays are copied (no references kept); at most 16. */
    fun setAnchors(count: Int, slots: IntArray, xs: FloatArray, ys: FloatArray, names: Array<String>) {
        aCount = min(count, cap)
        for (i in 0 until aCount) {
            aSlot[i] = slots[i] and (cap - 1)
            aX[i] = xs[i].coerceIn(0f, 1f)
            aY[i] = ys[i].coerceIn(0f, 1f)
            val n = names[i]
            aName[i] = if (n.length > 10) n.substring(0, 9) + "…" else n
            aNameW[i] = nameP.measureText(aName[i])
        }
        invalidate()
    }

    /** Programmatic cursor move (no [onMove]). */
    fun setCursor(x: Float, y: Float) {
        cursorX = x.coerceIn(0f, 1f)
        cursorY = y.coerceIn(0f, 1f)
        updateReadout()
        invalidate()
    }

    /** Live badge: `"● EDGE TPU · 0.21 ms"` / `"● CPU · 0.05 ms"`; [ms] < 0 shows `--`. Allocation-free. */
    fun setTelemetry(npu: Boolean, ms: Float) {
        val key = if (ms < 0f) -1 else (min(ms, 999f) * 100f + 0.5f).toInt()
        if (key == telemKey && npu == telemNpu) return
        telemKey = key
        telemNpu = npu
        val pre = if (npu) PRE_NPU else PRE_CPU
        var p = 0
        for (c in pre) telem[p++] = c
        if (key < 0) { telem[p++] = '-'; telem[p++] = '-' } else p = putFixed2(telem, p, key)
        for (c in POST) telem[p++] = c
        telemLen = p
        badgeEdgeP.color = if (npu) Hw.withAlpha(Hw.MOD_OSC, 150) else Hw.withAlpha(Hw.MOD_FILTER, 150)
        badgeTextP.color = if (npu) Hw.MOD_OSC else Hw.MOD_FILTER
        layoutBadge()
        invalidate()
    }

    // ------------------------------------------------------------------ layout

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        innerW = max(1f, w - padL - padR)
        innerH = max(1f, h - padT - padB)
        well.set(0.5f * d, 0.5f * d, w - 0.5f * d, h - 0.5f * d)
        clip.rewind()
        clip.addRoundRect(well, 10f * d, 10f * d, Path.Direction.CW)
        // 3 + 3 interior grid lines
        grid = FloatArray(6 * 4)
        for (i in 1..3) {
            val x = padL + innerW * i / 4f
            val y = padT + innerH * i / 4f
            val o = (i - 1) * 8
            grid[o] = x; grid[o + 1] = padT; grid[o + 2] = x; grid[o + 3] = padT + innerH
            grid[o + 4] = padL; grid[o + 5] = y; grid[o + 6] = padL + innerW; grid[o + 7] = y
        }
        val r = 0.34f * (w + h) / 2f
        for (i in 0 until cap) {
            val c = AiMorphUi.color(i)
            glowShader[i] = RadialGradient(0f, 0f, r, Hw.withAlpha(c, 120), Hw.withAlpha(c, 0), Shader.TileMode.CLAMP)
        }
        cursorShader = RadialGradient(0f, 0f, 80f * d, Hw.withAlpha(Hw.MOD_OSC, 70), Hw.withAlpha(Hw.MOD_OSC, 0), Shader.TileMode.CLAMP)
        layoutBadge()
    }

    private fun layoutBadge() {
        if (width == 0) return
        val tw = badgeTextP.measureText(telem, 0, telemLen)
        val h = 20f * d
        val right = width - 9f * d
        badge.set(right - tw - 16f * d, 9f * d, right, 9f * d + h)
    }

    private fun px(x: Float) = padL + x * innerW
    private fun py(y: Float) = padT + y * innerH

    // ------------------------------------------------------------------ draw (zero allocation)

    override fun onDraw(c: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        c.drawRoundRect(well, 10f * d, 10f * d, bgP)

        val save = c.save()
        c.clipPath(clip)

        // influence weights (inverse square distance, normalised)
        var sum = 0f
        for (i in 0 until aCount) {
            val dx = aX[i] - cursorX
            val dy = aY[i] - cursorY
            val v = 1f / (dx * dx + dy * dy + 0.004f)
            wBuf[i] = v
            sum += v
        }
        if (sum > 0f) for (i in 0 until aCount) wBuf[i] /= sum

        // anchor glow field
        for (i in 0 until aCount) {
            val sh = glowShader[aSlot[i]] ?: continue
            val x = px(aX[i])
            val y = py(aY[i])
            m.setTranslate(x, y)
            sh.setLocalMatrix(m)
            glowP.shader = sh
            glowP.alpha = (50 + 205 * wBuf[i]).toInt().coerceIn(0, 255)
            c.drawCircle(x, y, 0.34f * (w + h) / 2f, glowP)
        }
        glowP.shader = null

        c.drawLines(grid, gridP)

        // cursor glow
        val cx = px(cursorX)
        val cy = py(cursorY)
        cursorShader?.let {
            m.setTranslate(cx, cy)
            it.setLocalMatrix(m)
            glowP.shader = it
            glowP.alpha = 255
            c.drawCircle(cx, cy, 80f * d, glowP)
            glowP.shader = null
        }

        // influence beams cursor → anchors
        for (i in 0 until aCount) {
            val wt = wBuf[i]
            if (wt < 0.03f) continue
            beamP.color = Hw.withAlpha(AiMorphUi.color(aSlot[i]), (40 + 190 * wt).toInt().coerceIn(0, 255))
            beamP.strokeWidth = (0.6f + 2.6f * wt) * d
            c.drawLine(cx, cy, px(aX[i]), py(aY[i]), beamP)
        }

        // anchor nodes + names
        for (i in 0 until aCount) {
            val col = AiMorphUi.color(aSlot[i])
            val x = px(aX[i])
            val y = py(aY[i])
            val r = (5.5f + 2.5f * wBuf[i]) * d
            nodeFillP.color = Hw.withAlpha(col, 235)
            c.drawCircle(x, y, r, nodeFillP)
            nodeRingP.color = Hw.withAlpha(Color.WHITE, 120)
            c.drawCircle(x, y, r + 2.5f * d, nodeRingP)
            val half = aNameW[i] / 2f
            val tx = x.coerceIn(half + 4f * d, w - half - 4f * d)
            c.drawText(aName[i], tx, y + r + 14f * d, nameP)
        }

        // crosshair + cursor
        c.drawLine(padL, cy, padL + innerW, cy, crossP)
        c.drawLine(cx, padT, cx, padT + innerH, crossP)
        if (dragging) c.drawCircle(cx, cy, grabR, grabP)
        c.drawCircle(cx, cy, 9f * d, curRingP)
        c.drawCircle(cx, cy, 2.5f * d, curDotP)

        c.restoreToCount(save)
        c.drawRoundRect(well, 10f * d, 10f * d, edgeP)

        if (aCount == 0 && emptyHint.isNotEmpty()) c.drawText(emptyHint, w / 2f, py(0.5f) + 44f * d, hintP)

        // X / Y readout
        c.drawText(readout, 0, readoutLen, 12f * d, h - 8f * d, readP)

        // telemetry badge
        c.drawRoundRect(badge, 10f * d, 10f * d, badgeBgP)
        c.drawRoundRect(badge, 10f * d, 10f * d, badgeEdgeP)
        c.drawText(telem, 0, telemLen, badge.left + 8f * d, badge.bottom - 6f * d, badgeTextP)
    }

    // ------------------------------------------------------------------ touch

    private fun nearestNode(x: Float, y: Float): Int {
        var best = -1
        var bd = nodeHitR * nodeHitR
        for (i in 0 until aCount) {
            val dx = px(aX[i]) - x
            val dy = py(aY[i]) - y
            val q = dx * dx + dy * dy
            if (q <= bd) { bd = q; best = i }
        }
        return best
    }

    private fun moveTo(x: Float, y: Float, now: Long, force: Boolean) {
        cursorX = ((x - padL) / innerW).coerceIn(0f, 1f)
        cursorY = ((y - padT) / innerH).coerceIn(0f, 1f)
        updateReadout()
        invalidate()
        if (force || now - lastEmit >= EMIT_MS) {
            lastEmit = now
            onMove?.invoke(cursorX, cursorY)
        }
    }

    private fun beginDrag() {
        dragging = true
        onDragStart?.invoke()
    }

    private fun endDrag() {
        if (dragging) {
            dragging = false
            invalidate()
            onDragEnd?.invoke()
        }
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                parent?.requestDisallowInterceptTouchEvent(true)
                downX = e.x; downY = e.y
                longFired = false
                val ni = nearestNode(e.x, e.y)
                if (ni >= 0) {
                    mode = ANCHOR
                    pressIdx = ni
                    postDelayed(longPress, longPressMs)
                } else {
                    mode = DRAG
                    beginDrag()
                    val dx = px(cursorX) - e.x
                    val dy = py(cursorY) - e.y
                    if (dx * dx + dy * dy <= grabR * grabR) { grabDx = dx; grabDy = dy } else { grabDx = 0f; grabDy = 0f }
                    moveTo(e.x + grabDx, e.y + grabDy, e.eventTime, true)
                }
            }
            MotionEvent.ACTION_MOVE -> {
                if (mode == ANCHOR) {
                    val dx = e.x - downX
                    val dy = e.y - downY
                    if (dx * dx + dy * dy > slop * slop) {
                        removeCallbacks(longPress)
                        mode = DRAG
                        grabDx = 0f; grabDy = 0f
                        beginDrag()
                        moveTo(e.x, e.y, e.eventTime, true)
                    }
                } else if (mode == DRAG) {
                    moveTo(e.x + grabDx, e.y + grabDy, e.eventTime, false)
                }
            }
            MotionEvent.ACTION_UP -> {
                removeCallbacks(longPress)
                if (mode == ANCHOR && !longFired && pressIdx in 0 until aCount) {
                    beginDrag()
                    moveTo(px(aX[pressIdx]), py(aY[pressIdx]), e.eventTime, true)
                } else if (mode == DRAG) {
                    moveTo(e.x + grabDx, e.y + grabDy, e.eventTime, true)
                }
                mode = NONE
                endDrag()
            }
            MotionEvent.ACTION_CANCEL -> {
                removeCallbacks(longPress)
                mode = NONE
                endDrag()
            }
        }
        return true
    }

    override fun onDetachedFromWindow() {
        removeCallbacks(longPress)
        super.onDetachedFromWindow()
    }

    // ------------------------------------------------------------------ text buffers

    private fun updateReadout() {
        var p = 0
        readout[p++] = 'X'; readout[p++] = ' '
        p = putFixed2(readout, p, (cursorX * 100f + 0.5f).toInt())
        readout[p++] = ' '; readout[p++] = ' '; readout[p++] = 'Y'; readout[p++] = ' '
        p = putFixed2(readout, p, (cursorY * 100f + 0.5f).toInt())
        readoutLen = p
    }

    /** Writes [hundredths] as `d.dd` (any number of integer digits) at [p0]; returns the next free index. */
    private fun putFixed2(b: CharArray, p0: Int, hundredths: Int): Int {
        var p = p0
        val iv = hundredths.coerceIn(0, 99_999)
        val ip = iv / 100
        val fp = iv % 100
        var div = 1
        while (ip / div >= 10) div *= 10
        while (div > 0) { b[p++] = '0' + (ip / div) % 10; div /= 10 }
        b[p++] = '.'
        b[p++] = '0' + fp / 10
        b[p++] = '0' + fp % 10
        return p
    }

    private companion object {
        const val NONE = 0
        const val DRAG = 1
        const val ANCHOR = 2
        const val EMIT_MS = 28L          // ≈ 36 Hz
        val PRE_NPU = "● EDGE TPU · ".toCharArray()
        val PRE_CPU = "● CPU · ".toCharArray()
        val POST = " ms".toCharArray()
    }
}

// ====================================================================================================== sheet

/**
 * AI MORPH drawer (CENTER 830 × 356 dp). Left: [AiMorphPadView] (470 × 300). Right (320 × 300): hardware status card,
 * `AUTO 4 PRESETS` / `+ CAPTURE HERE` / `RECOMPILE NPU`, the anchor list and `SAVE AS PRESET…`.
 *
 * Every engine call that can take time (load a preset anchor, compile on the NPU) runs on `a.loader`; the pad drives
 * [NpuMorphBridge.eval] directly on the UI thread (sub-millisecond), skipped while a job is running.
 * Built once (lazily) and cached by the owner.
 */
class AiMorphSheet(private val a: MainActivity, private val bridge: NpuMorphBridge) : Sheet(Edge.CENTER, 830, 356) {
    override val view: View by lazy { build() }

    private val d = a.resources.displayMetrics.density
    private fun dp(v: Int) = (v * d).toInt()

    private class Anchor(val slot: Int, val x: Float, val y: Float, val name: String)

    private val anchors = ArrayList<Anchor>()
    private lateinit var pad: AiMorphPadView
    private lateinit var listBox: LinearLayout
    private lateinit var headInfo: TextView
    private lateinit var stDevice: TextView
    private lateinit var stBackend: TextView
    private lateinit var stDtype: TextView
    private lateinit var stParams: TextView
    private lateinit var stLast: TextView
    private lateinit var btnAuto: LedButton
    private lateinit var btnCapture: LedButton
    private lateinit var btnCompile: LedButton
    private lateinit var btnSave: LedButton
    private var busy = false
    private var npu = false
    private val rnd = Random()

    override fun onShow() {
        view
        reloadAnchors()
        refreshStatus()
    }

    override fun onHide() {
        // knobs on the (covered) EDIT page may have changed while morphing
        if (a.visible) a.ui.post { runCatching { a.edit.refreshValues() } }
    }

    // ------------------------------------------------------------------ build

    private fun build(): View {
        val root = LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(10), dp(2), dp(10), dp(8))
        }

        // header: title · busy / info · ✕
        val head = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        head.addView(Hw.engraved(a, "AI MORPH", 12f, tr = false, color = Hw.MOD_OSC), LinearLayout.LayoutParams(-2, -2))
        headInfo = Hw.engraved(a, "", 10f, tr = false, color = Hw.ENGRAVE).apply { gravity = Gravity.CENTER_VERTICAL }
        head.addView(headInfo, LinearLayout.LayoutParams(0, -2, 1f).apply { marginStart = dp(10) })
        head.addView(
            LedButton(a, "✕", tr = false, led = false).apply {
                minHeight = 0; minimumHeight = 0; setPadding(0, 0, 0, 0)
                setOnClickListener { dismiss() }
            }, LinearLayout.LayoutParams(dp(36), dp(28))
        )
        root.addView(head, LinearLayout.LayoutParams(-1, dp(32)))

        val body = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL }

        // ---- left: pad
        pad = AiMorphPadView(a).apply {
            emptyHint = L10n.t("Tap AUTO 4 PRESETS or + CAPTURE HERE")
            onDragStart = { History.checkpoint() }
            onMove = { x, y -> evalAt(x, y) }
            onDragEnd = { refreshStatus(); a.ui.post { runCatching { a.edit.refreshValues() } } }
            onEditAnchor = { slot -> anchorMenu(slot) }
        }
        body.addView(pad, LinearLayout.LayoutParams(0, dp(300), 470f))

        // ---- right column
        val right = LinearLayout(a).apply { orientation = LinearLayout.VERTICAL }

        val card = LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            background = Hw.panelBg(a)
            setPadding(dp(8), dp(5), dp(8), dp(5))
        }
        stDevice = statRow(card, "DEVICE")
        stBackend = statRow(card, "BACKEND")
        stDtype = statRow(card, "DTYPE")
        stParams = statRow(card, "PARAMS")
        stLast = statRow(card, "LAST")
        right.addView(card, LinearLayout.LayoutParams(-1, -2))

        btnAuto = actionButton("AUTO 4 PRESETS", Hw.MOD_OSC) { autoFour() }
        right.addView(btnAuto, LinearLayout.LayoutParams(-1, dp(34)).apply { topMargin = dp(5) })

        val two = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL }
        btnCapture = actionButton("+ CAPTURE HERE", Hw.MOD_ENV) { captureHere() }
        btnCompile = actionButton("RECOMPILE NPU", Hw.MOD_FX) { recompile() }
        two.addView(btnCapture, LinearLayout.LayoutParams(0, -1, 1f).apply { marginEnd = dp(4) })
        two.addView(btnCompile, LinearLayout.LayoutParams(0, -1, 1f))
        right.addView(two, LinearLayout.LayoutParams(-1, dp(34)).apply { topMargin = dp(4) })

        listBox = LinearLayout(a).apply { orientation = LinearLayout.VERTICAL }
        val scroll = ScrollView(a).apply {
            isVerticalScrollBarEnabled = false
            background = Hw.lcdBg(a)
            addView(listBox)
        }
        right.addView(scroll, LinearLayout.LayoutParams(-1, 0, 1f).apply { topMargin = dp(5) })

        btnSave = actionButton("SAVE AS PRESET…", Hw.MOD_GOOD) { promptSave() }
        right.addView(btnSave, LinearLayout.LayoutParams(-1, dp(34)).apply { topMargin = dp(5) })

        body.addView(right, LinearLayout.LayoutParams(0, dp(300), 320f).apply { marginStart = dp(10) })
        root.addView(body, LinearLayout.LayoutParams(-1, dp(300)))
        return root
    }

    private fun statRow(parent: LinearLayout, label: String): TextView {
        val row = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        row.addView(Hw.engraved(a, label, 8.5f, tr = false, color = Hw.INK_DIM), LinearLayout.LayoutParams(dp(58), -2))
        val v = Ui.text(a, "—", 11f, Hw.SILK, tr = false).apply { typeface = Hw.MONO; maxLines = 1 }
        row.addView(v, LinearLayout.LayoutParams(0, -2, 1f))
        parent.addView(row, LinearLayout.LayoutParams(-1, dp(15)))
        return v
    }

    private fun actionButton(label: String, accent: Int, onClick: () -> Unit) =
        LedButton(a, label, tr = false, led = false, accentColor = accent).apply {
            textSize = 11.5f
            minHeight = 0; minimumHeight = 0
            setPadding(dp(4), 0, dp(4), 0)
            setOnClickListener { if (!busy) onClick() }
        }

    // ------------------------------------------------------------------ state sync

    private fun refreshStatus() {
        val j = runCatching { JSONObject(bridge.statusJson()) }.getOrNull() ?: return
        val backend = j.optString("backend", "")
        val bl = backend.lowercase(Locale.ROOT)
        npu = if (j.has("npu")) j.optBoolean("npu") else (bl.contains("npu") || bl.contains("tpu") || bl.contains("nnapi") || bl.contains("edge"))
        val us = j.optDouble("lastUs", -1.0)
        stDevice.text = j.optString("device", "—").ifEmpty { "—" }
        stBackend.text = backend.ifEmpty { "—" }
        stBackend.setTextColor(if (npu) Hw.MOD_OSC else Hw.MOD_FILTER)
        stDtype.text = j.optString("dtype", "—").ifEmpty { "—" }
        val params = j.optLong("params", -1L)
        stParams.text = when {
            params < 0 -> j.optString("params", "—").ifEmpty { "—" }
            params >= 10_000 -> "%.1fk".format(Locale.US, params / 1000.0)
            else -> params.toString()
        }
        stLast.text = when {
            us < 0 -> "—"
            us >= 1000 -> "%.2f ms".format(Locale.US, us / 1000.0)
            else -> "%.0f µs".format(Locale.US, us)
        }
        pad.setTelemetry(npu, if (us < 0) -1f else (us / 1000.0).toFloat())
    }

    private fun reloadAnchors() {
        anchors.clear()
        val raw = runCatching { bridge.anchorsJson() }.getOrDefault("")
        val arr: JSONArray? = runCatching {
            val t = raw.trim()
            if (t.startsWith("[")) JSONArray(t) else JSONObject(t).optJSONArray("anchors")
        }.getOrNull()
        for (i in 0 until (arr?.length() ?: 0)) {
            val o = arr!!.optJSONObject(i) ?: continue
            val slot = o.optInt("slot", i).coerceIn(0, AiMorphUi.MAX_ANCHORS - 1)
            anchors += Anchor(slot, o.optDouble("x", 0.5).toFloat(), o.optDouble("y", 0.5).toFloat(), o.optString("name", "Anchor ${slot + 1}"))
        }
        val n = min(anchors.size, AiMorphUi.MAX_ANCHORS)
        pad.setAnchors(
            n,
            IntArray(n) { anchors[it].slot }, FloatArray(n) { anchors[it].x }, FloatArray(n) { anchors[it].y },
            Array(n) { anchors[it].name },
        )
        rebuildList()
    }

    private fun rebuildList() {
        listBox.removeAllViews()
        if (anchors.isEmpty()) {
            listBox.addView(
                Ui.text(a, "No anchors yet. Long-press an anchor to pick a preset or remove it.", 11f, Hw.INK_DIM).apply {
                    setPadding(dp(8), dp(6), dp(8), dp(6))
                    maxLines = 4
                }
            )
            return
        }
        for (an in anchors.sortedBy { it.slot }) {
            val row = LinearLayout(a).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                setPadding(dp(8), 0, dp(8), 0)
                isClickable = true
                setOnClickListener { jumpTo(an) }
                setOnLongClickListener { anchorMenu(an.slot); true }
            }
            row.addView(TextView(a).apply { text = "●"; textSize = 12f; setTextColor(AiMorphUi.color(an.slot)) }, LinearLayout.LayoutParams(dp(18), -2))
            row.addView(Ui.text(a, an.name, 12f, Hw.SILK, tr = false).apply { maxLines = 1 }, LinearLayout.LayoutParams(0, -2, 1f))
            row.addView(
                Ui.text(a, "%.2f,%.2f".format(Locale.US, an.x, an.y), 10.5f, Hw.ENGRAVE, tr = false).apply { typeface = Hw.MONO; maxLines = 1 },
                LinearLayout.LayoutParams(-2, -2)
            )
            listBox.addView(row, LinearLayout.LayoutParams(-1, dp(30)))
        }
    }

    private fun setBusy(b: Boolean, label: String = "") {
        busy = b
        headInfo.text = if (b) "● $label" else ""
        for (btn in arrayOf(btnAuto, btnCapture, btnCompile, btnSave)) btn.alpha = if (b) 0.4f else 1f
    }

    // ------------------------------------------------------------------ morphing

    private fun evalAt(x: Float, y: Float) {
        if (busy) return
        val ms = try { bridge.eval(x, y) } catch (_: Throwable) { -1f }
        pad.setTelemetry(npu, ms)
    }

    private fun jumpTo(an: Anchor) {
        if (busy) return
        History.checkpoint()
        pad.setCursor(an.x, an.y)
        evalAt(an.x, an.y)
        refreshStatus()
        a.ui.post { runCatching { a.edit.refreshValues() } }
    }

    /**
     * Run [job] on the loader thread; it returns an error text (null = ok). Afterwards the anchors / status are
     * re-read and the sound at the cursor is re-evaluated so the engine matches the pad.
     */
    private fun runJob(label: String, job: () -> String?) {
        if (busy) return
        setBusy(true, label)
        a.loader.execute {
            val err = try { job() } catch (t: Throwable) { t.message ?: t.javaClass.simpleName }
            a.ui.post {
                if (a.isDestroyed) return@post
                setBusy(false)
                if (err != null) a.log("AI Morph: $err", Ui.ERR)
                reloadAnchors()
                if (err == null && anchors.size >= 2) evalAt(pad.cursorX, pad.cursorY)
                refreshStatus()
                runCatching { a.edit.refreshValues() }
            }
        }
    }

    /** null when [json] reports success (or is empty), else the error text. */
    private fun errorOf(json: String?): String? {
        val s = json?.trim().orEmpty()
        if (s.isEmpty() || !s.startsWith("{")) return if (s.isEmpty()) null else s
        val o = runCatching { JSONObject(s) }.getOrNull() ?: return s
        val err = o.optString("error", "")
        return if (!o.optBoolean("ok", true) || err.isNotEmpty()) err.ifEmpty { "failed" } else null
    }

    /** Compile on the NPU, but only when there is something to interpolate between. */
    private fun compileIfReady(count: Int): String? = if (count >= 2) errorOf(bridge.compileJson()) else null

    private fun freeSlot(): Int? = (0 until AiMorphUi.MAX_ANCHORS).firstOrNull { s -> anchors.none { it.slot == s } }

    private fun autoFour() {
        val bad1 = " (invalid)"
        val bad2 = "（読み込めません）"
        val all = a.library.presets().filter { !it.title.endsWith(bad1) && !it.title.endsWith(bad2) }
        if (all.size < 2) {
            a.log("AI Morph: the Library needs at least 2 presets", Ui.WARN)
            return
        }
        val picks = pickDiverse(all, 4)
        val root = a.library.libraryRoot.absolutePath
        pad.setCursor(0.5f, 0.5f)
        History.checkpoint()
        runJob(L10n.t("Compiling 4 presets…")) {
            bridge.clearAnchors()
            for ((i, e) in picks.withIndex()) {
                val c = CORNERS[i]
                val err = errorOf(bridge.loadPresetAnchor(i, c[0], c[1], e.file.absolutePath, root))
                if (err != null) return@runJob "${e.title}: $err"
            }
            compileIfReady(picks.size)
        }
    }

    /** Round-robin over shuffled categories, so 4 picks differ in character whenever the library allows it. */
    private fun pickDiverse(all: List<Library.Entry>, n: Int): List<Library.Entry> {
        val groups = all.groupBy { it.category.trim().lowercase().ifEmpty { "~" } }.values
            .map { it.shuffled(rnd) }.shuffled(rnd)
        val out = ArrayList<Library.Entry>(n)
        var round = 0
        while (out.size < n) {
            var added = false
            for (g in groups) if (round < g.size && out.size < n) { out += g[round]; added = true }
            if (!added) break
            round++
        }
        return out
    }

    private fun captureHere() {
        val slot = freeSlot()
        if (slot == null) {
            a.log("AI Morph: all ${AiMorphUi.MAX_ANCHORS} anchors are in use", Ui.WARN)
            return
        }
        val x = pad.cursorX
        val y = pad.cursorY
        val name = a.currentPresetName
        val after = anchors.size + 1
        runJob(L10n.t("Capturing…")) {
            if (!bridge.captureAnchor(slot, x, y, name)) "capture failed" else compileIfReady(after)
        }
    }

    private fun recompile() {
        val n = anchors.size
        if (n < 2) {
            a.log("AI Morph: add at least 2 anchors first", Ui.WARN)
            return
        }
        runJob(L10n.t("Compiling on NPU…")) { compileIfReady(n) }
    }

    // ------------------------------------------------------------------ anchor menu

    private fun anchorMenu(slot: Int) {
        val an = anchors.firstOrNull { it.slot == slot } ?: return
        val items = arrayOf(L10n.t("Load preset from Library…"), L10n.t("Remove anchor"))
        AlertDialog.Builder(a)
            .setTitle(an.name)
            .setItems(items) { _, which -> if (which == 0) pickPreset(an) else removeAnchor(an) }
            .setNegativeButton(L10n.t("Cancel"), null)
            .show()
    }

    private fun pickPreset(an: Anchor) {
        val list = a.library.presets()
        if (list.isEmpty()) {
            a.log("AI Morph: the Library has no presets", Ui.WARN)
            return
        }
        val names = Array(list.size) { (if (list[it].user) "★ " else "") + list[it].title }
        AlertDialog.Builder(a)
            .setTitle(L10n.t("Pick preset"))
            .setItems(names) { _, i ->
                val e = list[i]
                val count = anchors.size
                val root = a.library.libraryRoot.absolutePath
                runJob(L10n.t("Loading preset…")) {
                    errorOf(bridge.loadPresetAnchor(an.slot, an.x, an.y, e.file.absolutePath, root)) ?: compileIfReady(count)
                }
            }
            .setNegativeButton(L10n.t("Cancel"), null)
            .show()
    }

    private fun removeAnchor(an: Anchor) {
        val after = anchors.size - 1
        runJob(L10n.t("Removing…")) {
            if (!bridge.removeAnchor(an.slot)) "remove failed" else compileIfReady(after)
        }
    }

    // ------------------------------------------------------------------ save

    private fun promptSave() {
        val stamp = SimpleDateFormat("MMdd-HHmm", Locale.US).format(Date())
        val input = EditText(a).apply {
            inputType = InputType.TYPE_CLASS_TEXT
            setText("AI Morph $stamp")
            setSelectAllOnFocus(true)
        }
        AlertDialog.Builder(a)
            .setTitle(L10n.t("Save as preset"))
            .setView(input)
            .setPositiveButton(L10n.t("Save")) { _, _ ->
                val name = input.text.toString().trim().ifEmpty { "AI Morph" }
                // make sure the engine holds exactly the sound under the cursor
                runCatching { bridge.eval(pad.cursorX, pad.cursorY) }
                a.savePreset(name) { f -> if (f != null) a.log("AI Morph: saved \"$name\" to the User library") }
            }
            .setNegativeButton(L10n.t("Cancel"), null)
            .show()
    }

    private companion object {
        /** Auto-map corners: TL, TR, BL, BR. */
        val CORNERS = arrayOf(
            floatArrayOf(0.12f, 0.12f), floatArrayOf(0.88f, 0.12f),
            floatArrayOf(0.12f, 0.88f), floatArrayOf(0.88f, 0.88f),
        )
    }
}

// ====================================================================================================== JNI bridge

/**
 * [NpuMorphBridge] over the Phase 5 `nativeNpu*` JNI calls. Never throws: a missing / failing native call becomes a
 * readable `{"ok":false,"error":…}` (or `false` / `-1f` / `[]`). Blocking calls (load, compile) belong on `a.loader`.
 */
class NativeNpuMorphBridge(private val eng: NativeEngine) : NpuMorphBridge {
    private fun err(t: Throwable) = JSONObject().put("ok", false).put("error", t.message ?: t.javaClass.simpleName).toString()

    override fun statusJson(): String = try {
        val raw = eng.nativeNpuStatus()
        val o = JSONObject(raw)
        val backend = o.optString("backend", "cpu")
        // the pad's card shows the human device name; mark whether the NPU really runs the net
        if (!o.has("npu")) o.put("npu", o.optBoolean("available", false) && backend != "cpu")
        if (backend == "edgetpu" && o.optString("device").let { it.isEmpty() || it == "edgetpu" }) o.put("device", "google-edgetpu")
        o.toString()
    } catch (t: Throwable) {
        JSONObject().put("device", "—").put("backend", "cpu").put("npu", false).put("error", t.message ?: "").toString()
    }

    override fun captureAnchor(slot: Int, x: Float, y: Float, name: String): Boolean =
        try { eng.nativeNpuMorphCaptureAnchor(slot, x, y, name) } catch (_: Throwable) { false }

    override fun loadPresetAnchor(slot: Int, x: Float, y: Float, path: String, libRoot: String): String =
        try { eng.nativeNpuMorphLoadPresetAnchor(slot, x, y, path, libRoot) } catch (t: Throwable) { err(t) }

    override fun removeAnchor(slot: Int): Boolean = try { eng.nativeNpuMorphRemoveAnchor(slot); true } catch (_: Throwable) { false }

    override fun clearAnchors(): Boolean = try { eng.nativeNpuMorphClearAnchors(); true } catch (_: Throwable) { false }

    override fun compileJson(): String = try { eng.nativeNpuMorphCompile() } catch (t: Throwable) { err(t) }

    override fun eval(x: Float, y: Float): Float = try { eng.nativeNpuMorphEval(x, y) } catch (_: Throwable) { -1f }

    override fun anchorsJson(): String = try { eng.nativeNpuMorphAnchorsJson() } catch (_: Throwable) { "[]" }
}
