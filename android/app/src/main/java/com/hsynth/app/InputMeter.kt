package com.hsynth.app

import android.content.Context
import android.content.pm.PackageManager
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.view.Gravity
import android.view.View
import android.widget.LinearLayout
import android.widget.TextView
import kotlin.math.log10
import kotlin.math.max

/**
 * Single reader of the engine's audio-input level (U4, tuner, sampler, vocoder, looper …).
 *
 * `nativeInputPeak()` is destructive (it resets the peak), so two readers starve each other. This object polls the
 * NON-destructive `nativeInputPeakPeek()` **once per stats tick** (see `MainActivity.statsTick`) and only while an
 * [InputStatusRow] is actually shown, caches [level] / [db] and fans out to the shown rows.
 * UI thread only.
 */
object InputMeter {
    /** Smoothed peak 0..1 (fast attack, ~0.7 decay per tick). */
    var level = 0f
        private set

    /** [level] in dB (−99 = silence / input off). */
    var db = -99f
        private set

    private val rows = ArrayList<InputStatusRow>(4)

    internal fun register(r: InputStatusRow) { if (!rows.contains(r)) rows.add(r) }
    internal fun unregister(r: InputStatusRow) { rows.remove(r) }

    /** Call once per stats tick. Cheap no-op when no row is on screen. */
    fun poll() {
        var shown = false
        for (i in rows.indices) if (rows[i].isShown) { shown = true; break }
        if (!shown) return
        if (NativeEngine.nativeInputEnabled()) {
            level = max(NativeEngine.nativeInputPeakPeek(), level * 0.7f)
            db = if (level > 1e-5f) 20f * log10(level) else -99f
        } else {
            level = 0f
            db = -99f
        }
        for (i in rows.indices) if (rows[i].isShown) rows[i].onLevel()
    }
}

/**
 * One reusable input status composite (30 dp high):
 * `[LCD "INPUT ● ON  −24 dB" | "INPUT ○ OFF — mic permission needed"] [level bar 120×12] [Enable Audio Input]`.
 *
 * One tap on the button goes through `MainActivity.setAudioInput` (which owns the RECORD_AUDIO permission flow); the
 * row follows live through `MainActivity.inputListeners` (permission dialog result) and [InputMeter] (level).
 * Listener / meter registration follows attach / detach, so rebuilt EDIT pages never leak a row.
 */
class InputStatusRow(private val a: MainActivity) : LinearLayout(a) {
    private val d = resources.displayMetrics.density
    private val lcd: TextView = Hw.lcd(a, "", 11.5f).apply {
        setPadding((8 * d).toInt(), 0, (8 * d).toInt(), 0)
        gravity = Gravity.CENTER_VERTICAL
    }
    private val meter = MeterBar(a)
    private val button = LedButton(a, "Enable Audio Input", led = true, accentColor = Hw.MOD_FX).apply {
        textSize = 11f
        minHeight = 0
        minimumHeight = 0
        setPadding((17 * d).toInt(), 0, (8 * d).toInt(), 0)
        setOnClickListener { a.setAudioInput(!NativeEngine.nativeInputEnabled()) }
    }
    private val listener: () -> Unit = { refresh() }
    private var enabled = false
    private var shownDb = Int.MIN_VALUE

    init {
        orientation = HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        addView(lcd, LayoutParams(0, (30 * d).toInt(), 1f).apply { marginEnd = (6 * d).toInt() })
        addView(meter, LayoutParams((120 * d).toInt(), (12 * d).toInt()).apply { marginEnd = (6 * d).toInt() })
        addView(button, LayoutParams((150 * d).toInt(), (30 * d).toInt()))
        refresh()
    }

    /** Re-read enabled state + permission and update LCD / button (called on listener events, not per frame). */
    fun refresh() {
        enabled = NativeEngine.nativeInputEnabled()
        val granted = a.checkSelfPermission(android.Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED
        shownDb = Int.MIN_VALUE
        if (enabled) {
            lcd.setTextColor(Hw.MOD_GOOD)
            lcd.text = "INPUT ● ON"
        } else {
            lcd.setTextColor(if (granted) Hw.ENGRAVE else Hw.MOD_FILTER)
            lcd.text = if (granted) "INPUT ○ OFF" else L10n.t("INPUT ○ OFF — mic permission needed")
            meter.level = 0f
        }
        button.text = L10n.t(if (enabled) "Disable Audio Input" else "Enable Audio Input")
        button.lit = enabled
    }

    /** From [InputMeter.poll] (one stats tick, only while shown). */
    internal fun onLevel() {
        if (!enabled) return
        meter.level = InputMeter.level
        val q = InputMeter.db.toInt()
        if (q != shownDb) {
            shownDb = q
            lcd.text = if (q <= -99) "INPUT ● ON  -- dB" else "INPUT ● ON  $q dB"
        }
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        a.addInputListener(listener)
        InputMeter.register(this)
        refresh()
    }

    override fun onDetachedFromWindow() {
        a.removeInputListener(listener)
        InputMeter.unregister(this)
        super.onDetachedFromWindow()
    }

    /** 16-segment horizontal level meter (non-interactive, zero allocations in onDraw). */
    private class MeterBar(ctx: Context) : View(ctx) {
        var level = 0f
            set(v) { val c = v.coerceIn(0f, 1f); if (c != field) { field = c; invalidate() } }
        private val dd = resources.displayMetrics.density
        private val p = Paint(Paint.ANTI_ALIAS_FLAG)
        private val r = RectF()
        private val dim = Color.rgb(18, 22, 31)

        override fun onDraw(c: Canvas) {
            val w = width.toFloat()
            val h = height.toFloat()
            if (w <= 0f) return
            val segs = 16
            val gap = 1.5f * dd
            val sw = ((w - gap * (segs - 1)) / segs).coerceAtLeast(1f)
            // −60 dB .. 0 dB across the bar
            val db = if (level > 1e-5f) 20f * log10(level) else -99f
            val lit = (((db + 60f) / 60f) * segs).toInt().coerceIn(0, segs)
            for (i in 0 until segs) {
                p.color = when {
                    i >= lit -> dim
                    i >= segs - 1 -> Hw.MOD_RED
                    i >= segs - 3 -> Hw.MOD_FILTER
                    else -> Hw.MOD_FX
                }
                val x = i * (sw + gap)
                r.set(x, 0f, x + sw, h)
                c.drawRoundRect(r, dd, dd, p)
            }
        }
    }
}
