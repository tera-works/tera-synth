package com.hsynth.app

import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.os.SystemClock
import android.view.View
import kotlin.math.min

/**
 * S1 — Seamless Sound Transition indicator.
 *
 * [start] is called by every Kotlin path that changes the sound (preset / set / slot / scene recall). A single
 * [FrameTicker] client (30 fps, only for the duration of the transition) then drives [progress]:
 *  - while the engine reports a transition (`nativeSstProgress() >= 0`) the sweep follows it exactly;
 *  - if it never does (engine busy loading, SST off in the engine …) a timed sweep of `transition + 250 ms`
 *    (capped at 1.2 s) from the trigger time gives the same visuals.
 *
 * Consumers ([attach]ed views) are invalidated every frame and draw the 3 dp emerald top-edge sweep with
 * [drawTopSweep]. Nothing is drawn and no frame is requested while idle. UI thread only.
 */
object SstSweep : FrameTicker.Client {
    /** −1 = idle, 0..1 during a transition. */
    @JvmStatic
    var progress = -1f
        private set

    /** Mirror of the engine setting (see [applySettings]); when false [start] does nothing. */
    var enabled = true
        private set
    var transitionMs = 300
        private set

    private var startMs = 0L
    private var windowMs = 550L
    private var sawNative = false
    private val views = arrayOfNulls<View>(24)
    private var viewCount = 0
    private val p = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Hw.MOD_FX }
    private val r = RectF()

    override val intervalMs: Int get() = 33
    override val tickView: View? get() = null

    /** Applies (and remembers) the SST setting: engine + indicator. Call on startup and from the SYS page. */
    fun applySettings(on: Boolean, ms: Int) {
        enabled = on
        transitionMs = ms.coerceIn(20, 2000)
        windowMs = min(1200L, transitionMs + 250L)
        NativeEngine.nativeSetSst(on, transitionMs)
        if (!on) finish()
    }

    /** A sound change was triggered: begin a sweep (restarts a running one). */
    fun start() {
        if (!enabled) return
        startMs = SystemClock.uptimeMillis()
        sawNative = false
        progress = 0f
        FrameTicker.register(this)
        invalidateViews()
    }

    fun attach(v: View) {
        for (i in 0 until viewCount) if (views[i] === v) return
        if (viewCount < views.size) views[viewCount++] = v
    }

    fun detach(v: View) {
        for (i in 0 until viewCount) if (views[i] === v) {
            views[i] = views[viewCount - 1]
            views[viewCount - 1] = null
            viewCount--
            return
        }
    }

    override fun onFrame(nowMs: Long) {
        if (progress < 0f) { FrameTicker.unregister(this); return }
        val np = NativeEngine.nativeSstProgress()
        if (np >= 0f) {
            sawNative = true
            progress = np.coerceAtMost(1f)
        } else if (sawNative) {
            finish()
            return
        } else {
            val t = (SystemClock.uptimeMillis() - startMs).toFloat() / windowMs
            if (t >= 1f) { finish(); return }
            progress = t
        }
        invalidateViews()
    }

    private fun finish() {
        val was = progress >= 0f
        progress = -1f
        FrameTicker.unregister(this)
        if (was) invalidateViews()
    }

    private fun invalidateViews() {
        for (i in 0 until viewCount) views[i]?.invalidate()
    }

    /**
     * Draws the 3 dp emerald sweep along the top edge of a rounded card (`width × progress`, clipped so it follows
     * the corner radius). Does nothing while idle. Zero allocations.
     */
    @JvmStatic
    fun drawTopSweep(c: Canvas, w: Float, h: Float, radiusPx: Float, density: Float) {
        val pr = progress
        if (pr < 0f || w <= 0f) return
        c.save()
        c.clipRect(0f, 0f, w * pr, 3f * density)
        r.set(0f, 0f, w, h)
        c.drawRoundRect(r, radiusPx, radiusPx, p)
        c.restore()
    }
}
