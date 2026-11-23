package com.hsynth.app

import android.annotation.SuppressLint
import android.content.Context
import android.content.Intent
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Rect
import android.graphics.RectF
import android.net.Uri
import android.view.Gravity
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import android.view.View
import android.view.ViewConfiguration
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import org.json.JSONObject
import java.io.File
import java.util.concurrent.CancellationException
import kotlin.math.abs
import kotlin.math.ceil
import kotlin.math.floor
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

/*
 * B1 — SONG > AUDIO page (backing-track player) and the shared zero-allocation waveform view.
 *
 * [WaveSeekView] is reused by the PADS editor (START / END handles), and — as [SampleEditView] — by the B2 sampling
 * wizard (START / END / LOOP S / LOOP E handles in sample frames). Its domain is a plain `0..span` double (seconds for
 * the backing track, 0..1 for a pad, frames for a take), so one implementation covers all three.
 */

/**
 * Min / max waveform on a pure OLED well: one `drawLines` call per frame (the line array is rebuilt only when the peaks,
 * the size, the zoom or the pan change — never in [onDraw]), up to 4 draggable handles (22 dp hit radius), shaded
 * regions between handle pairs (0,1) and (2,3), a playhead that only invalidates its own strip, zoom ×1 / ×4 / ×16
 * (buttons, pinch) with horizontal pan, tap = seek, long-press = [onLongPress].
 *
 * Time ruler: drawn when [secPerUnit] > 0; labels are formatted into a [CharArray], no per-frame strings.
 * With [tick] = true the view registers a 20 fps [FrameTicker] client that calls [onPoll] (only while really shown).
 */
@SuppressLint("ViewConstructor")
open class WaveSeekView(
    ctx: Context,
    private val tick: Boolean = false,
    var accent: Int = Hw.MOD_OSC,
) : View(ctx), FrameTicker.Client {
    companion object {
        const val HANDLES = 4
        private val ZOOMS = floatArrayOf(1f, 4f, 16f)
        private val STEPS = doubleArrayOf(0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0)
    }

    private val d = resources.displayMetrics.density

    /** Length of the domain (seconds, frames, 1.0 …). */
    var span = 1.0
        set(v) { field = max(1e-9, v); rebuild(); invalidate() }

    /** Seconds per domain unit for the ruler; 0 = no ruler. */
    var secPerUnit = 1.0
        set(v) { field = v; invalidate() }

    var showPlayhead = true
    var position = 0.0
        private set

    /** Handle values in domain units; negative = inactive (hidden, not draggable). */
    val hv = DoubleArray(HANDLES) { -1.0 }
    val hLabel = Array(HANDLES) { "" }
    val hColor = IntArray(HANDLES) { accent }

    /** Region fill between handles (0,1) and (2,3). */
    val pairColor = IntArray(2) { accent }

    /** Darken everything outside handle pair 0 (trim view). */
    var dimOutside = false
        set(v) { field = v; invalidate() }

    var onSeek: ((Double) -> Unit)? = null
    /** (handle, value, final). Fires continuously while dragging, with final = true on release. */
    var onHandle: ((Int, Double, Boolean) -> Unit)? = null
    var onLongPress: ((Double) -> Unit)? = null
    var onZoomChanged: ((Int) -> Unit)? = null
    /** 20 fps hook (only with `tick = true`). */
    var onPoll: (() -> Unit)? = null
    /** The page scrolls to keep the playhead in view while zoomed. */
    var followPlayhead = true

    var zoomStep = 0
        private set
    private var zoom = 1f
    private var viewStart = 0f

    private var peaks = FloatArray(0)      // interleaved [min, max] per column
    private var cols = 0
    private var pts = FloatArray(0)
    private var nLines = 0
    private var lineStep = 1

    private val rulerH = 12f * d
    private val bg = Paint(Paint.ANTI_ALIAS_FLAG)
    private val line = Paint().apply { isAntiAlias = false }
    private val fill = Paint()
    private val txt = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Hw.MONO; textSize = 8.5f * d; color = Hw.INK_DIM; textAlign = Paint.Align.LEFT }
    private val flagTxt = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Hw.BOLD; textSize = 9.5f * d; color = Color_BLACK; textAlign = Paint.Align.CENTER }
    private val r = RectF()
    private val dirty = Rect()
    private val ruler = CharArray(10)

    private var playX = -1f
    private val slop = ViewConfiguration.get(ctx).scaledTouchSlop
    private var downX = 0f
    private var lastX = 0f
    private var mode = 0                 // 0 none, 1 handle, 2 tap / pan / scrub
    private var dragHandle = -1
    private var moved = false
    private var longFired = false
    private var scaleAcc = 1f
    private val longRun = Runnable {
        if (mode == 2 && !moved) { longFired = true; onLongPress?.invoke(xToVal(downX)) }
    }
    private val scale = ScaleGestureDetector(ctx, object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
        override fun onScaleBegin(detector: ScaleGestureDetector): Boolean { scaleAcc = 1f; return true }
        override fun onScale(detector: ScaleGestureDetector): Boolean {
            scaleAcc *= detector.scaleFactor
            if (scaleAcc > 1.35f) { setZoomStep(zoomStep + 1); scaleAcc = 1f }
            else if (scaleAcc < 0.74f) { setZoomStep(zoomStep - 1); scaleAcc = 1f }
            return true
        }
    })

    override val intervalMs: Int get() = 50
    override val tickView: View? get() = this
    override fun onFrame(nowMs: Long) { onPoll?.invoke() }

    init {
        isClickable = true
        for (i in 0 until HANDLES) hColor[i] = accent
    }

    // ------------------------------------------------------------------ data

    /** [data] = interleaved min / max per column (as `nativeAudioPeaks`, `nativePadPeaks`, `nativeSamplerPeaks`). */
    fun setPeaks(data: FloatArray?) {
        peaks = data ?: FloatArray(0)
        cols = peaks.size / 2
        rebuild()
        invalidate()
    }

    fun hasPeaks(): Boolean = cols > 0

    fun setPosition(p: Double) {
        if (p == position) return
        position = p
        if (!showPlayhead) return
        if (zoom > 1f && followPlayhead && mode == 0) {
            val f = (p / span).toFloat()
            if (f < viewStart || f > viewStart + 1f / zoom) {
                viewStart = (f - 0.05f / zoom).coerceIn(0f, 1f - 1f / zoom)
                rebuild()
                invalidate()
                playX = valToX(p)
                return
            }
        }
        val nx = valToX(p)
        val ox = playX
        playX = nx
        if (width == 0) return
        val l = (min(nx, if (ox < 0f) nx else ox) - 3f * d).toInt()
        val rr = (max(nx, if (ox < 0f) nx else ox) + 3f * d).toInt() + 1
        dirty.set(l.coerceAtLeast(0), 0, rr.coerceAtMost(width), height)
        @Suppress("DEPRECATION")
        invalidate(dirty)
    }

    fun setZoomStep(s: Int) {
        val n = s.coerceIn(0, ZOOMS.size - 1)
        if (n == zoomStep) return
        val center = viewStart + 0.5f / zoom
        zoomStep = n
        zoom = ZOOMS[n]
        viewStart = (center - 0.5f / zoom).coerceIn(0f, 1f - 1f / zoom)
        rebuild()
        invalidate()
        onZoomChanged?.invoke(n)
    }

    val zoomFactor: Int get() = zoom.toInt()

    // ------------------------------------------------------------------ geometry

    private fun valToX(v: Double): Float = ((v / span - viewStart) * zoom * width).toFloat()
    private fun xToVal(x: Float): Double =
        ((viewStart + (x / width.coerceAtLeast(1)) / zoom).toDouble() * span).coerceIn(0.0, span)

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        lineStep = max(1, d.roundToInt())
        val n = (w / lineStep) + 1
        if (pts.size < n * 4) pts = FloatArray(n * 4)
        rebuild()
    }

    /** Builds the min / max line list for the visible window. Called on data / size / zoom / pan change only. */
    private fun rebuild() {
        val w = width
        val h = height
        nLines = 0
        if (w <= 0 || h <= 0 || cols == 0) return
        val top = if (secPerUnit > 0.0) rulerH else 0f
        val mid = top + (h - top) / 2f
        val amp = (h - top) / 2f - 2f * d
        val step = lineStep
        var n = 0
        var x = 0
        while (x < w) {
            val f0 = (viewStart + x.toFloat() / (w * zoom)) * cols
            val f1 = (viewStart + (x + step).toFloat() / (w * zoom)) * cols
            var c0 = floor(f0).toInt()
            var c1 = max(c0 + 1, ceil(f1).toInt())
            if (c0 >= cols) break
            if (c0 < 0) c0 = 0
            if (c1 > cols) c1 = cols
            var lo = 1f
            var hi = -1f
            for (c in c0 until c1) {
                val a = peaks[2 * c]
                val b = peaks[2 * c + 1]
                if (a < lo) lo = a
                if (b > hi) hi = b
            }
            if (hi < lo) { lo = 0f; hi = 0f }
            lo = lo.coerceIn(-1f, 1f)
            hi = hi.coerceIn(-1f, 1f)
            var y0 = mid - hi * amp
            var y1 = mid - lo * amp
            if (y1 - y0 < 1f) { y0 -= 0.5f; y1 += 0.5f }
            val o = n * 4
            val px = x + step / 2f
            pts[o] = px; pts[o + 1] = y0; pts[o + 2] = px; pts[o + 3] = y1
            n++
            x += step
        }
        nLines = n
    }

    // ------------------------------------------------------------------ touch

    private fun nearestHandle(x: Float): Int {
        var best = -1
        var bd = 22f * d
        for (i in 0 until HANDLES) {
            if (hv[i] < 0.0) continue
            val dx = abs(valToX(hv[i]) - x)
            if (dx <= bd) { bd = dx; best = i }
        }
        return best
    }

    private fun setHandle(i: Int, raw: Double) {
        val gap = span * 0.004
        var v = raw.coerceIn(0.0, span)
        val partner = i xor 1
        if (hv[partner] >= 0.0) {
            v = if (i % 2 == 0) min(v, hv[partner] - gap) else max(v, hv[partner] + gap)
            v = v.coerceIn(0.0, span)
        }
        hv[i] = v
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        scale.onTouchEvent(e)
        if (scale.isInProgress) {
            removeCallbacks(longRun)
            mode = 0
            return true
        }
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                parent?.requestDisallowInterceptTouchEvent(true)
                downX = e.x
                lastX = e.x
                moved = false
                longFired = false
                dragHandle = nearestHandle(e.x)
                mode = if (dragHandle >= 0) 1 else 2
                if (mode == 2) postDelayed(longRun, ViewConfiguration.getLongPressTimeout().toLong())
            }
            MotionEvent.ACTION_MOVE -> {
                if (!moved && abs(e.x - downX) > slop) { moved = true; removeCallbacks(longRun) }
                if (mode == 1) {
                    setHandle(dragHandle, xToVal(e.x))
                    onHandle?.invoke(dragHandle, hv[dragHandle], false)
                    invalidate()
                } else if (mode == 2 && moved) {
                    if (zoom > 1f) {
                        viewStart = (viewStart - (e.x - lastX) / (width.coerceAtLeast(1) * zoom)).coerceIn(0f, 1f - 1f / zoom)
                        rebuild()
                        playX = valToX(position)
                        invalidate()
                    } else {
                        onSeek?.invoke(xToVal(e.x))
                    }
                }
                lastX = e.x
            }
            MotionEvent.ACTION_UP -> {
                removeCallbacks(longRun)
                if (mode == 1) {
                    onHandle?.invoke(dragHandle, hv[dragHandle], true)
                } else if (mode == 2 && !moved && !longFired) {
                    onSeek?.invoke(xToVal(e.x))
                    performClick()
                }
                mode = 0
                dragHandle = -1
            }
            MotionEvent.ACTION_CANCEL -> {
                removeCallbacks(longRun)
                mode = 0
                dragHandle = -1
            }
        }
        return true
    }

    override fun performClick(): Boolean = super.performClick()

    // ------------------------------------------------------------------ draw

    private fun fmtRuler(sec: Double, fine: Boolean): Int {
        val b = ruler
        if (fine) {
            val t = (sec * 10.0 + 0.5).toInt()
            val s = t / 10
            var n = 0
            val m = s / 60
            if (m > 0) { b[n++] = '0' + (m % 10); b[n++] = ':'; b[n++] = '0' + ((s % 60) / 10); b[n++] = '0' + (s % 10) }
            else { if (s >= 10) b[n++] = '0' + ((s / 10) % 10); b[n++] = '0' + (s % 10) }
            b[n++] = '.'
            b[n++] = '0' + (t % 10)
            return n
        }
        val s = (sec + 0.5).toInt()
        val m = s / 60
        var n = 0
        if (m >= 10) b[n++] = '0' + ((m / 10) % 10)
        b[n++] = '0' + (m % 10)
        b[n++] = ':'
        b[n++] = '0' + ((s % 60) / 10)
        b[n++] = '0' + (s % 10)
        return n
    }

    override fun onDraw(c: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        if (w <= 0f || h <= 0f) return
        val rad = 6f * d
        r.set(0f, 0f, w, h)
        bg.style = Paint.Style.FILL
        bg.color = Hw.WELL
        c.drawRoundRect(r, rad, rad, bg)
        val top = if (secPerUnit > 0.0) rulerH else 0f

        // ruler
        if (secPerUnit > 0.0) {
            val visSec = span * secPerUnit / zoom
            var step = STEPS[STEPS.size - 1]
            for (s in STEPS) if (visSec / s <= 7.0) { step = s; break }
            val startSec = viewStart * span * secPerUnit
            var t = ceil(startSec / step) * step
            val endSec = startSec + visSec
            val fine = step < 1.0
            fill.color = Hw.LED_OFF
            var guard = 0
            while (t <= endSec && guard++ < 40) {
                val x = ((t / secPerUnit / span - viewStart) * zoom * w).toFloat()
                c.drawRect(x, top - 3f * d, x + d, h, fill)
                val n = fmtRuler(t, fine)
                c.drawText(ruler, 0, n, x + 2f * d, 9f * d, txt)
                t += step
            }
        }

        // region fills (between handle pairs)
        for (p in 0..1) {
            val a = hv[2 * p]
            val b = hv[2 * p + 1]
            if (a < 0.0 || b < 0.0) continue
            fill.color = Hw.withAlpha(pairColor[p], 34)
            c.drawRect(max(0f, valToX(a)), top, min(w, valToX(b)), h, fill)
        }

        // centre line + waveform
        fill.color = Hw.LED_OFF
        val mid = top + (h - top) / 2f
        c.drawRect(0f, mid, w, mid + d, fill)
        if (nLines > 0) {
            line.color = accent
            line.strokeWidth = lineStep.toFloat()
            c.drawLines(pts, 0, nLines * 4, line)
        }

        // trim: dim the outside of pair 0
        if (dimOutside && hv[0] >= 0.0 && hv[1] >= 0.0) {
            fill.color = 0x99000000.toInt()
            val x0 = valToX(hv[0]).coerceIn(0f, w)
            val x1 = valToX(hv[1]).coerceIn(0f, w)
            if (x0 > 0f) c.drawRect(0f, top, x0, h, fill)
            if (x1 < w) c.drawRect(x1, top, w, h, fill)
        }

        // playhead
        if (showPlayhead) {
            val x = valToX(position)
            if (x in 0f..w) {
                fill.color = Hw.SILK
                c.drawRect(x - d * 0.75f, top, x + d * 0.75f, h, fill)
            }
        }

        // handles
        for (i in 0 until HANDLES) {
            val v = hv[i]
            if (v < 0.0) continue
            val x = valToX(v)
            if (x < -2f * d || x > w + 2f * d) continue
            fill.color = hColor[i]
            c.drawRect(x - d, top, x + d, h, fill)
            val fw = 15f * d
            val fh = 13f * d
            if (i % 2 == 0) r.set(x, top, x + fw, top + fh) else r.set(x - fw, top, x, top + fh)
            c.drawRoundRect(r, 2f * d, 2f * d, fill)
            val lab = hLabel[i]
            if (lab.isNotEmpty()) c.drawText(lab, r.centerX(), r.centerY() + 3.4f * d, flagTxt)
        }

        // hairline border
        bg.style = Paint.Style.STROKE
        bg.strokeWidth = d
        bg.color = Hw.PANEL_EDGE
        r.set(d / 2f, d / 2f, w - d / 2f, h - d / 2f)
        c.drawRoundRect(r, rad, rad, bg)
    }

    // ------------------------------------------------------------------ ticker

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        if (tick) FrameTicker.register(this)
    }

    override fun onDetachedFromWindow() {
        removeCallbacks(longRun)
        if (tick) FrameTicker.unregister(this)
        super.onDetachedFromWindow()
    }

    override fun onVisibilityAggregated(isVisible: Boolean) {
        super.onVisibilityAggregated(isVisible)
        if (tick) FrameTicker.poke()
    }

    override fun onWindowFocusChanged(hasWindowFocus: Boolean) {
        super.onWindowFocusChanged(hasWindowFocus)
        if (tick) FrameTicker.poke()
    }
}

private const val Color_BLACK = 0xFF000000.toInt()

// ================================================================================================== AUDIO page

/**
 * Backing-track player (WAV / FLAC / MP3 / AAC): LCD, transport, A / B / LOOP, OPEN… (SAF → [AudioDecode] → engine),
 * volume / speed / pitch / output route, and a small queue strip. 846 × 346 dp in landscape; the first row is a
 * [FlowLayout], so it wraps in portrait.
 */
class AudioPage(private val a: MainActivity) {
    private class Track(val file: File, val title: String)

    val view: View
    private val d = { v: Int -> Ui.dp(a, v) }

    private val titleTv = Ui.text(a, "—", 15f, Hw.LCD_FG, bold = true, tr = false).apply { typeface = Hw.MONO; maxLines = 1 }
    private val infoTv = Ui.text(a, "", 11f, Hw.LCD_DIM, tr = false).apply { typeface = Hw.MONO; maxLines = 1 }
    private val prevBtn: LedButton
    private val playBtn: LedButton
    private val stopBtn: LedButton
    private val nextBtn: LedButton
    private val aBtn: LedButton
    private val bBtn: LedButton
    private val loopBtn: LedButton
    private val wave = WaveSeekView(a, tick = true, accent = Hw.MOD_OSC)
    private val zoomTv = Ui.text(a, "×1", 11f, Hw.ENGRAVE, bold = true, tr = false).apply { typeface = Hw.MONO; gravity = Gravity.CENTER }
    private val queueTv = Ui.text(a, "", 12f, Hw.ENGRAVE, tr = false).apply { typeface = Hw.MONO; maxLines = 1 }
    private val volume = HSegBar(a, Hw.MOD_OSC, 20, 34f)
    private val speedNum: DragNumber
    private val pitchNum: DragNumber

    private val queue = ArrayList<Track>()
    private var qi = -1
    private var loaded = false
    private var loading = false
    private var loadToken = 0
    private var dur = 0.0
    private var sr = 0
    private var ch = 0
    private var playing = false
    private var wasPlaying = false
    private var userHalted = true
    private var markerA = -1.0
    private var markerB = -1.0
    private var loopOn = false
    private var volumeV = 0.8f
    private var speedPct = 100f
    private var pitchSt = 0f
    private var shownSec = -1
    private var decoding = false
    private var decodeCancel: AudioCancel? = null
    private val pending = ArrayList<Uri>()
    private var firstOfBatch = true
    /** Phase 5: ✦ STEMS (NPU) button, progress, and the 4-stem mixer (VOCALS / DRUMS / BASS / KEYS). */
    private val stems = StemPanel(a, { if (loaded) queue.getOrNull(qi)?.file else null }) { pushLoop() }

    init {
        fun btn(label: String, accent: Int, withLed: Boolean = false, onClick: () -> Unit) =
            LedButton(a, label, tr = false, led = withLed, accentColor = accent).apply {
                textSize = 14f
                minHeight = 0
                minimumHeight = 0
                setPadding(if (withLed) d(14) else d(4), 0, d(4), 0)
                setOnClickListener { onClick() }
            }
        prevBtn = btn("⏮", Hw.MOD_OSC) { prev() }
        playBtn = btn("▶", Hw.MOD_GOOD) { toggle() }.apply { textSize = 18f }
        stopBtn = btn("■", Hw.MOD_OSC) { stop() }
        nextBtn = btn("⏭", Hw.MOD_OSC) { next() }
        aBtn = btn("A", Hw.MOD_FILTER, true) { setA() }.apply { setOnLongClickListener { clearMarkers(); true } }
        bBtn = btn("B", Hw.MOD_FILTER, true) { setB() }.apply { setOnLongClickListener { clearMarkers(); true } }
        loopBtn = btn("LOOP", Hw.MOD_FILTER, true) { toggleLoop() }.apply { textSize = 12f }
        val openBtn = btn("OPEN…", Hw.MOD_OSC) { openFiles() }.apply { textSize = 13f }

        val lcd = LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_VERTICAL
            background = Hw.lcdBg(a)
            setPadding(d(10), d(2), d(10), d(2))
            addView(titleTv, LinearLayout.LayoutParams(-1, -2))
            addView(infoTv, LinearLayout.LayoutParams(-1, -2))
            setOnClickListener { if (decoding) { decodeCancel?.cancel(); pending.clear(); decoding = false; infoTv.text = L10n.t("cancelled") } }
        }

        val row1 = FlowLayout(a, d(6), d(6)).apply {
            addView(lcd, ViewGroup.LayoutParams(d(300), d(56)))
            addView(prevBtn, ViewGroup.LayoutParams(d(48), d(48)))
            addView(playBtn, ViewGroup.LayoutParams(d(64), d(48)))
            addView(stopBtn, ViewGroup.LayoutParams(d(48), d(48)))
            addView(nextBtn, ViewGroup.LayoutParams(d(48), d(48)))
            addView(aBtn, ViewGroup.LayoutParams(d(52), d(48)))
            addView(bBtn, ViewGroup.LayoutParams(d(52), d(48)))
            addView(loopBtn, ViewGroup.LayoutParams(d(60), d(48)))
            addView(openBtn, ViewGroup.LayoutParams(d(90), d(48)))
        }

        // waveform + zoom overlay
        wave.hLabel[0] = "A"; wave.hLabel[1] = "B"
        wave.pairColor[0] = Hw.MOD_FILTER
        wave.hColor[0] = Hw.MOD_FILTER; wave.hColor[1] = Hw.MOD_FILTER
        wave.onSeek = { s -> if (loaded) NativeEngine.nativeAudioSeek(s) }
        wave.onHandle = { i, v, _ -> if (i == 0) markerA = v else markerB = v; pushLoop() }
        wave.onLongPress = { s -> placeMarker(s) }
        wave.onPoll = { poll() }
        wave.onZoomChanged = { zoomTv.text = "×${wave.zoomFactor}" }
        fun zbtn(t: String, dz: Int) = LedButton(a, t, tr = false, led = false).apply {
            textSize = 14f
            minHeight = 0
            minimumHeight = 0
            setPadding(0, 0, 0, 0)
            setOnClickListener { wave.setZoomStep(wave.zoomStep + dz) }
        }
        val zoomBox = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(zbtn("−", -1), LinearLayout.LayoutParams(d(30), d(24)))
            addView(zoomTv, LinearLayout.LayoutParams(d(30), d(24)))
            addView(zbtn("+", 1), LinearLayout.LayoutParams(d(30), d(24)))
        }
        val waveBox = FrameLayout(a).apply {
            addView(wave, FrameLayout.LayoutParams(-1, -1))
            addView(zoomBox, FrameLayout.LayoutParams(-2, -2, Gravity.END or Gravity.BOTTOM).apply { setMargins(0, 0, d(4), d(4)) })
        }

        // volume / speed / pitch / route
        volume.value = volumeV
        volume.valueText = "${(volumeV * 100).roundToInt()}"
        volume.onChange = { v ->
            volumeV = v
            volume.valueText = "${(v * 100).roundToInt()}"
            NativeEngine.nativeAudioSetVolume(v)
        }
        volume.onReset = { volume.value = 0.8f; volume.onChange?.invoke(0.8f) }
        speedNum = DragNumber(a, 50f, 150f, 1f, 100f, { "${it.roundToInt()}%" }, { speedPct }) { v ->
            speedPct = v
            NativeEngine.nativeAudioSetSpeed(v / 100f)
        }.apply { textSize = 13f }
        pitchNum = DragNumber(a, -12f, 12f, 1f, 0f, { (if (it > 0.4f) "+" else "") + "${it.roundToInt()} st" }, { pitchSt }) { v ->
            pitchSt = v
            NativeEngine.nativeAudioSetPitch(v)
        }.apply { textSize = 13f }
        val route = SegRow(a, arrayOf("MIX", "DIRECT"), Hw.MOD_OSC) { i ->
            NativeEngine.nativeAudioSetRoute(i == 1)
        }
        route.selected = 0

        fun group(label: String, v: View, w: Int, h: Int) = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(Hw.engraved(a, label, 10.5f, tr = false), LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(6) })
            addView(v, LinearLayout.LayoutParams(d(w), d(h)))
        }
        val row3 = FlowLayout(a, d(14), d(6)).apply {
            addView(group("VOLUME", volume, 220, 34), ViewGroup.LayoutParams(-2, -2))
            addView(group("SPEED", speedNum, 74, 34), ViewGroup.LayoutParams(-2, -2))
            addView(group("PITCH", pitchNum, 80, 34), ViewGroup.LayoutParams(-2, -2))
            addView(group("OUT", route, 150, 32), ViewGroup.LayoutParams(-2, -2))
        }

        // queue strip
        fun qbtn(t: String, dir: Int) = LedButton(a, t, tr = false, led = false).apply {
            textSize = 13f
            minHeight = 0
            minimumHeight = 0
            setPadding(0, 0, 0, 0)
            setOnClickListener { stepQueue(dir) }
        }
        val row4 = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(qbtn("◂", -1), LinearLayout.LayoutParams(d(34), d(28)))
            addView(queueTv, LinearLayout.LayoutParams(0, -2, 1f).apply { marginStart = d(8); marginEnd = d(8) })
            addView(qbtn("▸", 1), LinearLayout.LayoutParams(d(34), d(28)))
        }

        val col = LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, d(2), 0, d(2))
            addView(row1, LinearLayout.LayoutParams(-1, -2))
            addView(waveBox, LinearLayout.LayoutParams(-1, d(150)).apply { topMargin = d(6) })
            addView(row3, LinearLayout.LayoutParams(-1, -2).apply { topMargin = d(8) })
            addView(row4, LinearLayout.LayoutParams(-1, d(28)).apply { topMargin = d(6) })
            addView(stems.view, LinearLayout.LayoutParams(-1, -2).apply { topMargin = d(10); bottomMargin = d(6) })
        }
        view = ScrollView(a).apply { addView(col, FrameLayout.LayoutParams(-1, -2)) }

        NativeEngine.nativeAudioSetVolume(volumeV)
        NativeEngine.nativeAudioSetSpeed(1f)
        NativeEngine.nativeAudioSetPitch(0f)
        NativeEngine.nativeAudioSetRoute(false)
        refreshButtons()
        refreshQueue()
    }

    /** `MainActivity.buildUi` clears the request router; call again after every rebuild. */
    fun registerResults() {
        ActivityResults.register(ActivityResults.REQ_AUDIO_FILE) { data -> onPicked(data) }
    }

    // ------------------------------------------------------------------ state for PlayerPanel / PlaybackService

    val hasTrack: Boolean get() = loaded
    val isPlaying: Boolean get() = loaded && NativeEngine.nativeAudioState().let { it.size > 2 && it[2] > 0.5 }
    val title: String? get() = queue.getOrNull(qi)?.title
    fun lengthSec(): Double = dur
    fun positionSec(): Double = NativeEngine.nativeAudioState().getOrElse(0) { 0.0 }

    // ------------------------------------------------------------------ transport

    fun toggle() {
        if (!loaded) { if (queue.isNotEmpty()) selectTrack(max(qi, 0), true) else openFiles(); return }
        val on = !isPlaying
        userHalted = !on
        NativeEngine.nativeAudioPlay(on)
    }

    fun stop() {
        userHalted = true
        NativeEngine.nativeAudioPlay(false)
        if (loaded) NativeEngine.nativeAudioSeek(if (loopOn && markerA >= 0.0) markerA else 0.0)
    }

    fun prev() {
        if (!loaded) return
        if (positionSec() > 3.0 || queue.size < 2) { NativeEngine.nativeAudioSeek(0.0); return }
        stepQueue(-1)
    }

    fun next() { stepQueue(1) }

    private fun stepQueue(dir: Int) {
        if (queue.isEmpty()) return
        val n = ((qi + dir) % queue.size + queue.size) % queue.size
        selectTrack(n, playing)
    }

    // ------------------------------------------------------------------ loop markers

    private fun setA() {
        if (!loaded) return
        markerA = positionSec().coerceIn(0.0, dur)
        if (markerB >= 0.0 && markerB <= markerA) markerB = -1.0
        syncMarkers()
    }

    private fun setB() {
        if (!loaded) return
        val p = positionSec().coerceIn(0.0, dur)
        if (markerA < 0.0) markerA = 0.0
        if (p > markerA + 0.05) markerB = p
        syncMarkers()
    }

    private fun placeMarker(s: Double) {
        if (!loaded) return
        if (markerA < 0.0 || markerB >= 0.0) { markerA = s; markerB = -1.0 }
        else if (s > markerA + 0.05) markerB = s
        syncMarkers()
    }

    private fun clearMarkers() {
        markerA = -1.0
        markerB = -1.0
        loopOn = false
        syncMarkers()
    }

    private fun toggleLoop() {
        if (!loaded) return
        loopOn = !loopOn
        if (loopOn && (markerA < 0.0 || markerB < 0.0)) { markerA = 0.0; markerB = dur }
        syncMarkers()
    }

    private fun syncMarkers() {
        wave.hv[0] = markerA
        wave.hv[1] = markerB
        wave.invalidate()
        pushLoop()
        refreshButtons()
    }

    private fun pushLoop() {
        val ok = markerA >= 0.0 && markerB > markerA
        NativeEngine.nativeAudioSetLoop(if (ok) markerA else 0.0, if (ok) markerB else 0.0, loopOn && ok)
    }

    private fun refreshButtons() {
        aBtn.lit = markerA >= 0.0
        bBtn.lit = markerB >= 0.0
        loopBtn.lit = loopOn
        val t = if (playing) "⏸" else "▶"
        if (playBtn.text.toString() != t) playBtn.text = t
        playBtn.lit = playing
    }

    // ------------------------------------------------------------------ polling (20 fps, only while shown)

    private fun poll() {
        val s = NativeEngine.nativeAudioState()
        if (s.size < 3) return
        val pos = s[0]
        playing = s[2] > 0.5
        if (loaded) {
            if (s[1] > 0.0 && abs(s[1] - dur) > 0.01) { dur = s[1]; wave.span = dur }
            wave.setPosition(pos)
            val sec = pos.toInt()
            if (sec != shownSec && !decoding && !loading) { shownSec = sec; showInfo(pos) }
        }
        if (playing != wasPlaying) {
            refreshButtons()
            // ended by itself: advance to the next queued track
            if (!playing && !userHalted && loaded && dur > 0.0 && pos >= dur - 0.3 && !loopOn && queue.size > 1) {
                wasPlaying = playing
                stepQueue(1)
                userHalted = false
                NativeEngine.nativeAudioPlay(true)
                return
            }
            wasPlaying = playing
        }
    }

    private fun fmt(sec: Double): String {
        val t = sec.toInt().coerceAtLeast(0)
        return "%d:%02d".format(t / 60, t % 60)
    }

    private fun showInfo(pos: Double) {
        infoTv.text = "%s / %s   %.1f kHz  %d ch".format(fmt(pos), fmt(dur), sr / 1000.0, ch)
    }

    // ------------------------------------------------------------------ queue + loading

    private fun refreshQueue() {
        queueTv.text = if (queue.isEmpty()) L10n.t("(queue empty — OPEN… to add tracks)")
        else "${qi + 1}/${queue.size}  ${queue.getOrNull(qi)?.title ?: ""}"
    }

    private fun selectTrack(i: Int, autoPlay: Boolean) {
        if (i !in queue.indices) return
        qi = i
        val t = queue[i]
        val tok = ++loadToken
        loading = true
        loaded = false
        userHalted = true
        NativeEngine.nativeAudioPlay(false)
        stems.reset()
        titleTv.text = t.title
        infoTv.text = L10n.t("Loading…")
        refreshQueue()
        a.loader.execute {
            val r = try { JSONObject(NativeEngine.nativeAudioLoad(t.file.absolutePath)) } catch (e: Exception) {
                JSONObject().put("ok", false).put("error", e.message ?: "load failed")
            }
            val peaks = if (r.optBoolean("ok")) NativeEngine.nativeAudioPeaks(1024) else null
            a.ui.post {
                if (tok != loadToken || a.isDestroyed) return@post
                loading = false
                if (!r.optBoolean("ok")) {
                    infoTv.text = r.optString("error", "load failed")
                    a.log("audio load failed: ${r.optString("error")}", Ui.ERR)
                    return@post
                }
                loaded = true
                dur = r.optDouble("dur", 0.0)
                sr = r.optInt("sr", 0)
                ch = r.optInt("ch", 0)
                titleTv.text = r.optString("name", t.title).ifBlank { t.title }
                markerA = -1.0
                markerB = -1.0
                loopOn = false
                wave.span = dur.coerceAtLeast(0.001)
                wave.setZoomStep(0)
                wave.hv[0] = -1.0
                wave.hv[1] = -1.0
                wave.setPeaks(peaks)
                wave.setPosition(0.0)
                NativeEngine.nativeAudioSetLoop(0.0, 0.0, false)
                shownSec = -1
                showInfo(0.0)
                refreshButtons()
                a.log("audio: ${t.title} (${fmt(dur)})")
                if (autoPlay) { userHalted = false; NativeEngine.nativeAudioPlay(true) }
            }
        }
    }

    // ------------------------------------------------------------------ OPEN…

    private fun openFiles() {
        val i = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "audio/*"
            putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
        }
        @Suppress("DEPRECATION")
        a.startActivityForResult(i, ActivityResults.REQ_AUDIO_FILE)
    }

    private fun onPicked(data: Intent?) {
        if (data == null) return
        val uris = ArrayList<Uri>()
        data.clipData?.let { c -> for (k in 0 until c.itemCount) uris += c.getItemAt(k).uri }
        data.data?.let { if (uris.isEmpty()) uris += it }
        if (uris.isEmpty()) return
        decodeCancel?.cancel()
        pending.clear()
        pending.addAll(uris)
        firstOfBatch = true
        processNext()
    }

    private fun processNext() {
        if (pending.isEmpty()) { decoding = false; refreshQueue(); return }
        val u = pending.removeAt(0)
        decoding = true
        val cancel = AudioCancel()
        decodeCancel = cancel
        infoTv.text = L10n.t("DECODING 0%")
        AudioDecode.decodeAsync(a, u, cancel,
            onProgress = { p -> if (cancel === decodeCancel) infoTv.text = "DECODING ${(p * 100).toInt()}%  (tap to cancel)" },
            onDone = { res, err ->
                if (cancel === decodeCancel) {
                    if (res != null) {
                        queue += Track(res.wavFile, res.title)
                        if (firstOfBatch) { firstOfBatch = false; selectTrack(queue.lastIndex, false) } else refreshQueue()
                    } else if (err !is CancellationException) {
                        infoTv.text = err?.message ?: "decode failed"
                        a.log("audio decode failed: ${err?.message}", Ui.ERR)
                    }
                    processNext()
                }
            })
    }
}
