package com.hsynth.app

import android.view.View

/**
 * A5 — the single native scope tap shared by every consumer (AudioScopeView, Master EQ backdrop, ...).
 *
 * Reference counted: the first [acquire] calls `nativeScopeEnable(true)` and registers ONE [FrameTicker] client
 * (33 ms = 30 fps) that fetches `wave` / `spec` with one JNI call per frame and then invalidates the owning views;
 * the last [release] calls `nativeScopeEnable(false)` and unregisters the ticker. Nothing is posted while idle.
 *
 * Auto-stop (zero CPU / zero OLED when nobody looks): owners that are [View]s only count while they are attached,
 * shown, have window focus and are not hidden behind a sheet ([FrameTicker.coverCheck]). When no owner qualifies
 * the ticker is parked (no further frame requested) and the engine tap is switched off; [FrameTicker.poke] (visibility /
 * focus / sheet changes) and [sync] (screen off / standby, called from `MainActivity`) re-evaluate it. UI thread only.
 */
object ScopeData : FrameTicker.Client {
    /** Pitch-aligned waveform, −1..1. */
    val wave = FloatArray(512)

    /** 64 log-spaced bins (20 Hz–20 kHz), 0..1 over −72..0 dB. */
    val spec = FloatArray(64)

    /** Peak-hold of [spec] (decays in [onFrame], never in onDraw). */
    val specPeak = FloatArray(64)

    /** Fundamental in Hz, 0 = unknown. */
    @JvmStatic
    var f0 = 0f
        private set

    /** Bumped for every fresh frame (views can cheaply see whether anything changed). */
    @JvmStatic
    var frame = 0
        private set

    private const val MAX_OWNERS = 12
    private const val PEAK_DECAY = 0.012f
    private val owners = arrayOfNulls<Any>(MAX_OWNERS)
    private var count = 0
    private var engineOn = false
    private var parked: View? = null   // never attached: its isAttachedToWindow == false parks the ticker

    /** True while the native tap is switched on. */
    val tapOn: Boolean get() = engineOn

    override val intervalMs: Int get() = 33

    override val tickView: View?
        get() {
            var anyPlain = false
            for (i in 0 until count) {
                val o = owners[i]
                if (o is View) {
                    if (viewOk(o)) { setEngine(true); return o }
                } else anyPlain = true
            }
            if (anyPlain) { setEngine(true); return null }
            setEngine(false)
            return parked
        }

    /** Register [c] as a consumer. Views are invalidated after every frame; any other object just keeps the tap alive. */
    fun acquire(c: Any) {
        for (i in 0 until count) if (owners[i] === c) return
        if (count >= MAX_OWNERS) return
        if (c is View && parked == null) parked = View(c.context)
        owners[count++] = c
        if (count == 1) {
            java.util.Arrays.fill(wave, 0f)
            java.util.Arrays.fill(spec, 0f)
            java.util.Arrays.fill(specPeak, 0f)
            f0 = 0f
            FrameTicker.register(this)
        } else FrameTicker.poke()
        sync()
    }

    fun release(c: Any) {
        var at = -1
        for (i in 0 until count) if (owners[i] === c) { at = i; break }
        if (at < 0) return
        for (i in at until count - 1) owners[i] = owners[i + 1]
        owners[--count] = null
        if (count == 0) {
            FrameTicker.unregister(this)
            setEngine(false)
        } else sync()
    }

    /** Re-evaluate eligibility right now (screen off / standby, visibility); cheap. */
    fun sync() {
        if (count == 0) { setEngine(false); return }
        setEngine(FrameTicker.hostActive && anyEligible())
        FrameTicker.poke()
    }

    private fun anyEligible(): Boolean {
        for (i in 0 until count) {
            val o = owners[i]
            if (o !is View || viewOk(o)) return true
        }
        return false
    }

    private fun viewOk(v: View): Boolean {
        if (!v.isAttachedToWindow || !v.isShown || !v.hasWindowFocus()) return false
        val cover = FrameTicker.coverCheck
        return cover == null || !cover(v)
    }

    private fun setEngine(on: Boolean) {
        if (on == engineOn) return
        engineOn = on
        NativeEngine.nativeScopeEnable(on)
    }

    override fun onFrame(nowMs: Long) {
        if (!engineOn) return
        f0 = NativeEngine.nativeScopeFrame(wave, spec)
        for (i in 0 until 64) {
            val v = spec[i]
            val p = specPeak[i] - PEAK_DECAY
            specPeak[i] = if (v > p) v else if (p > 0f) p else 0f
        }
        frame++
        for (i in 0 until count) {
            val o = owners[i]
            if (o is View && o.isShown) o.invalidate()
        }
    }
}
