package com.hsynth.app

import android.os.Process

/**
 * B3 — drains the engine's external-MIDI out queue and forwards every record to the chosen output slot.
 *
 * The engine copies events of zones routed EXT / BOTH into an SPSC queue (realtime-safe); this daemon thread
 * (`THREAD_PRIORITY_URGENT_AUDIO`, like `MidiInput.Receiver`) blocks in `nativeExtMidiNext(200)` and writes each
 * `[slot, len, b0, (b1), (b2)]` record to `MidiOutput.sendSlot`. The block timeout lets [stop] take effect within
 * 200 ms without any interrupt tricks. [start] / [stop] are idempotent and UI-thread callable.
 */
class ExtMidiPump(private val a: MainActivity) {
    @Volatile private var generation = 0
    private var thread: Thread? = null

    val running: Boolean get() = thread?.isAlive == true

    @Synchronized
    fun start() {
        if (running) return
        val gen = ++generation
        thread = Thread({
            try { Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO) } catch (_: Throwable) {}
            loop(gen)
        }, "hsynth-ext-midi").apply { isDaemon = true; start() }
    }

    @Synchronized
    fun stop() {
        generation++            // the worker leaves after its current (≤ 200 ms) wait
        thread = null
    }

    private fun loop(gen: Int) {
        while (gen == generation) {
            val b = try { NativeEngine.nativeExtMidiNext(200) } catch (_: Throwable) { return } ?: continue
            dispatch(b)
        }
    }

    /** `[slot, len, bytes...]*` — a truncated tail is dropped, never sent half-way. */
    private fun dispatch(b: ByteArray) {
        var i = 0
        val out = a.midiOut
        while (i + 2 <= b.size) {
            val slot = b[i].toInt() and 0xFF
            val len = b[i + 1].toInt() and 0xFF
            if (len == 0 || i + 2 + len > b.size) return
            out.sendSlot(slot, b, len, i + 2)
            i += 2 + len
        }
    }
}
