package com.hsynth.app

import android.view.Choreographer
import android.view.View

/**
 * One shared, auto-stopping frame loop for every animated view (scope, tuner, playheads, sweeps).
 *
 * - One [Choreographer.FrameCallback], one fixed `Array<Client?>(16)`: nothing is allocated per frame.
 * - A client only runs while it is *eligible*:
 *   `hostActive && view.isShown && view.hasWindowFocus() && !coverCheck(view)`.
 *   An ineligible client is parked (it is skipped and, when nobody is eligible, no further frame is requested),
 *   so a hidden tab, a covered stage or a switched-off screen costs zero CPU and zero wake-ups.
 * - Clients are re-evaluated on [poke]; views call it from `onVisibilityAggregated`, `onWindowFocusChanged`
 *   and when a sheet opens/closes. [pauseAll] / [resumeAll] are for screen-off / standby transitions.
 * - Frames are requested with a delay matching the next due client, so a 20 fps client does not wake the
 *   process at the display's 120 Hz.
 *
 * UI thread only.
 */
object FrameTicker {
    interface Client {
        /** Desired period in ms (e.g. 33 = 30 fps). Read every frame; must not allocate. */
        val intervalMs: Int

        /** View that decides eligibility (null = only [hostActive] matters). */
        val tickView: View?

        /** Called at most once per [intervalMs]. Must not allocate. */
        fun onFrame(nowMs: Long)
    }

    const val MAX_CLIENTS = 16

    /** False while the screen is off / the app is in standby (see [pauseAll]). */
    @Volatile
    var hostActive: Boolean = true
        set(v) {
            if (field == v) return
            field = v
            if (v) poke() else cancel()
        }

    /**
     * True when a sheet / modal covers [View] so animating it is wasted power. Set by `SheetHost` on attach
     * (it points at `SheetHost.coversView`); null = nothing covers anything.
     */
    @Volatile
    var coverCheck: ((View) -> Boolean)? = null

    private val clients = arrayOfNulls<Client>(MAX_CLIENTS)
    private val due = LongArray(MAX_CLIENTS)
    private var count = 0
    private var posted = false
    private var choreo: Choreographer? = null

    /** Early-fire tolerance (ms): a vsync that lands slightly before the due time still fires the client. */
    private const val SLACK_MS = 4L

    private val callback = Choreographer.FrameCallback { frameTimeNanos -> onFrame(frameTimeNanos / 1_000_000L) }

    val clientCount: Int get() = count

    /** @return false if all [MAX_CLIENTS] slots are taken (the client is then simply not animated). */
    fun register(c: Client): Boolean {
        for (i in 0 until MAX_CLIENTS) if (clients[i] === c) { poke(); return true }
        for (i in 0 until MAX_CLIENTS) if (clients[i] == null) {
            clients[i] = c
            due[i] = 0L
            count++
            poke()
            return true
        }
        return false
    }

    fun unregister(c: Client) {
        for (i in 0 until MAX_CLIENTS) if (clients[i] === c) {
            clients[i] = null
            count--
        }
        if (count <= 0) { count = 0; cancel() }
    }

    /** Something that affects eligibility changed (visibility, focus, sheet): re-evaluate right away. */
    fun poke() {
        if (count == 0) return
        cancel()
        schedule(0L)
    }

    /** Screen off / standby: park every client and stop the Choreographer loop. */
    fun pauseAll() { hostActive = false }

    /** Screen on / wake: clients run again as soon as they are eligible. */
    fun resumeAll() { hostActive = true }

    // ------------------------------------------------------------------ internals

    private fun eligible(c: Client): Boolean {
        if (!hostActive) return false
        val v = c.tickView ?: return true
        if (!v.isAttachedToWindow || !v.isShown || !v.hasWindowFocus()) return false
        val cover = coverCheck
        return cover == null || !cover(v)
    }

    private fun schedule(delayMs: Long) {
        if (posted || !hostActive || count == 0) return
        val ch = choreo ?: Choreographer.getInstance().also { choreo = it }
        posted = true
        if (delayMs <= 0L) ch.postFrameCallback(callback) else ch.postFrameCallbackDelayed(callback, delayMs)
    }

    private fun cancel() {
        if (!posted) return
        choreo?.removeFrameCallback(callback)
        posted = false
    }

    private fun onFrame(nowMs: Long) {
        posted = false
        if (!hostActive) return
        var minWait = Long.MAX_VALUE
        for (i in 0 until MAX_CLIENTS) {
            val c = clients[i] ?: continue
            if (!eligible(c)) continue
            val interval = c.intervalMs.coerceAtLeast(1).toLong()
            if (nowMs + SLACK_MS >= due[i]) {
                // drift-free period; if we are far behind (just un-parked) restart from now
                due[i] = if (nowMs - due[i] > interval * 2L) nowMs + interval else due[i] + interval
                c.onFrame(nowMs)
                // onFrame may have unregistered itself
                if (clients[i] !== c) continue
            }
            val wait = due[i] - nowMs
            if (wait < minWait) minWait = wait
        }
        // Nobody eligible -> do not re-post; poke() wakes us when visibility / focus / sheets change.
        if (minWait != Long.MAX_VALUE) schedule((minWait - 8L).coerceAtLeast(0L))
    }
}
