package com.hsynth.app

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.view.MotionEvent
import android.view.View

/**
 * Multi-touch piano keyboard with OLED Night Keys mode (Proposal B)
 * and Obsidian Cyan active-key glow.
 */
class KeyboardView(context: Context) : View(context) {
    enum class SlideMode { GLISS, BEND }

    var firstNote = 48              // must be a white key
        set(v) { field = v; allNotesOff(); invalidate() }
    var whiteKeyCount = 15
        set(v) { field = v.coerceIn(5, 36); allNotesOff(); invalidate() }
    var slideMode = SlideMode.GLISS
    /** Dark OLED-saving keybed for dark stages (Proposal B NIGHT KEYS). */
    var nightKeys = false
        set(v) { if (field != v) { field = v; applyPalette(); invalidate() } }
    /** Semitones added to the notes this keyboard plays (the keys shown stay where they are). */
    var transpose = 0
        set(v) { field = v.coerceIn(-24, 24); allNotesOff() }
    private fun snd(key: Int) = (key + transpose).coerceIn(0, 127)
    var fixedVelocity: Float? = null
    var onNotesChanged: ((Int) -> Unit)? = null
    /** Every note played (on, key, velocity), e.g. for MIDI out. */
    var onNote: ((Boolean, Int, Float) -> Unit)? = null

    private data class Touch(var noteId: Int, var key: Int, val downX: Float, val downY: Float, var lastTimbre: Float = -1f)

    private val d = resources.displayMetrics.density
    private val touches = HashMap<Int, Touch>()
    private var nextNoteId = 0x100000
    private val held = IntArray(128)

    private val white = Paint(Paint.ANTI_ALIAS_FLAG)
    private val whitePressed = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Hw.MOD_OSC }
    private val black = Paint(Paint.ANTI_ALIAS_FLAG)
    private val blackPressed = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Hw.MOD_FILTER }
    private val blackStroke = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = 1f * d }
    private val border = Paint().apply { style = Paint.Style.STROKE; strokeWidth = 1.2f * d }
    private val label = Paint(Paint.ANTI_ALIAS_FLAG).apply { textSize = 10.5f * d; textAlign = Paint.Align.CENTER; typeface = Hw.MONO }
    private val touchPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Hw.withAlpha(Hw.MOD_OSC, 90) }
    private val rect = RectF()

    init { applyPalette() }

    private fun applyPalette() {
        if (nightKeys) {
            white.color = Color.rgb(20, 26, 36)       // deep slate OLED-friendly natural keys
            black.color = Color.rgb(6, 8, 12)         // pure dark accidental keys
            border.color = Color.rgb(40, 50, 68)
            blackStroke.color = Color.rgb(54, 67, 90)
            label.color = Hw.ENGRAVE
        } else {
            white.color = Color.rgb(232, 237, 245)
            black.color = Color.rgb(14, 17, 24)
            border.color = Color.rgb(42, 48, 60)
            blackStroke.color = Color.rgb(48, 56, 72)
            label.color = Color.rgb(86, 97, 119)
        }
    }

    private fun isBlack(note: Int) = when (note % 12) { 1, 3, 6, 8, 10 -> true; else -> false }

    private fun whiteIndexToNote(index: Int): Int {
        var n = firstNote
        var i = 0
        while (i < index) { n++; if (!isBlack(n)) i++ }
        return n
    }

    private fun whiteWidth() = width.toFloat() / whiteKeyCount

    private fun noteAt(x: Float, y: Float): Int {
        val ww = whiteWidth()
        val wi = (x / ww).toInt().coerceIn(0, whiteKeyCount - 1)
        val wn = whiteIndexToNote(wi)
        if (y < height * 0.62f) {
            val local = x - wi * ww
            if (local > ww * 0.68f && isBlack(wn + 1) && wi < whiteKeyCount - 1) return wn + 1
            if (local < ww * 0.32f && wn - 1 >= firstNote && isBlack(wn - 1)) return wn - 1
        }
        return wn
    }

    private fun velocityAt(y: Float, note: Int): Float {
        fixedVelocity?.let { return it }
        val keyHeight = if (isBlack(note)) height * 0.62f else height.toFloat()
        return (0.25f + 0.75f * (y / keyHeight)).coerceIn(0.05f, 1f)
    }

    override fun onDraw(canvas: Canvas) {
        val ww = whiteWidth()
        val h = height.toFloat()
        canvas.drawColor(Hw.WELL)
        val rad = 4f * d
        for (i in 0 until whiteKeyCount) {
            val n = whiteIndexToNote(i)
            rect.set(i * ww + 0.5f * d, 0f, (i + 1) * ww - 0.5f * d, h - 1f * d)
            canvas.drawRoundRect(rect, rad, rad, if (held[n] > 0) whitePressed else white)
            canvas.drawRoundRect(rect, rad, rad, border)
            if (n % 12 == 0) canvas.drawText("C${n / 12 - 1}", i * ww + ww / 2f, h - 8f * d, label)
        }
        for (i in 0 until whiteKeyCount - 1) {
            val n = whiteIndexToNote(i)
            if (isBlack(n + 1)) {
                val cx = (i + 1) * ww
                rect.set(cx - ww * 0.31f, 0f, cx + ww * 0.31f, h * 0.61f)
                canvas.drawRoundRect(rect, 3.5f * d, 3.5f * d, if (held[n + 1] > 0) blackPressed else black)
                canvas.drawRoundRect(rect, 3.5f * d, 3.5f * d, blackStroke)
            }
        }
        for (t in touches.values) {
            canvas.drawCircle(t.downX, t.downY, 14f * d, touchPaint)
        }
    }

    private fun press(pointerId: Int, x: Float, y: Float, size: Float) {
        val key = noteAt(x, y)
        val id = nextNoteId++
        val t = Touch(id, key, x, y)
        touches[pointerId] = t
        held[key]++
        NativeEngine.noteOn(id, snd(key), velocityAt(y, key))
        onNote?.invoke(true, snd(key), velocityAt(y, key))
        sendExpression(t, x, y, size)
        onNotesChanged?.invoke(touches.size)
    }

    private fun release(pointerId: Int) {
        val t = touches.remove(pointerId) ?: return
        held[t.key] = (held[t.key] - 1).coerceAtLeast(0)
        NativeEngine.noteOff(t.noteId, snd(t.key))
        onNote?.invoke(false, snd(t.key), 0f)
        onNotesChanged?.invoke(touches.size)
    }

    private fun sendExpression(t: Touch, x: Float, y: Float, size: Float) {
        val timbre = (1f - y / height).coerceIn(0f, 1f)
        if (kotlin.math.abs(timbre - t.lastTimbre) > 0.004f) {
            NativeEngine.perNote(NativeEngine.PerNote.TIMBRE, t.noteId, snd(t.key), timbre)
            t.lastTimbre = timbre
        }
        NativeEngine.perNote(NativeEngine.PerNote.PRESSURE, t.noteId, snd(t.key), ((size - 0.05f) * 4f).coerceIn(0f, 1f))
        if (slideMode == SlideMode.BEND) {
            val semis = (x - t.downX) / whiteWidth() * 1.7f
            NativeEngine.perNote(NativeEngine.PerNote.PITCH, t.noteId, snd(t.key), semis)
            NativeEngine.perNote(NativeEngine.PerNote.X, t.noteId, snd(t.key), ((x - t.downX) / whiteWidth()).coerceIn(-1f, 1f))
        }
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val i = e.actionIndex
                press(e.getPointerId(i), e.getX(i), e.getY(i), e.getSize(i))
            }
            MotionEvent.ACTION_MOVE -> {
                for (i in 0 until e.pointerCount) {
                    val pid = e.getPointerId(i)
                    val t = touches[pid] ?: continue
                    val x = e.getX(i)
                    val y = e.getY(i)
                    if (slideMode == SlideMode.GLISS) {
                        val key = noteAt(x.coerceIn(0f, width - 1f), y.coerceIn(0f, height - 1f))
                        if (key != t.key) {
                            release(pid)
                            press(pid, x, y, e.getSize(i))
                            continue
                        }
                    }
                    sendExpression(t, x, y, e.getSize(i))
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> release(e.getPointerId(e.actionIndex))
            MotionEvent.ACTION_CANCEL -> allNotesOff()
        }
        invalidate()
        return true
    }

    fun allNotesOff() {
        for (pid in touches.keys.toList()) release(pid)
        held.fill(0)
        invalidate()
    }

    fun setExternalHeld(key: Int, on: Boolean) {
        if (key !in 0..127) return
        held[key] = if (on) held[key] + 1 else (held[key] - 1).coerceAtLeast(0)
        postInvalidate()
    }
}

/**
 * Obsidian vertical controller ribbon (Pitch Bend / Mod Wheel).
 * Zero per-frame allocations in onDraw().
 */
class ControlStrip(
    context: Context,
    private val title: String,
    private val spring: Boolean,
    private val bipolar: Boolean,
    private val accent: Int = if (bipolar) Hw.MOD_FILTER else Hw.MOD_OSC,
    private val onChange: (Float) -> Unit
) : View(context) {
    private var value = 0f
    private val d = resources.displayMetrics.density
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Hw.ENGRAVE
        textSize = 10f * d
        textAlign = Paint.Align.CENTER
        typeface = Hw.BOLD
        letterSpacing = 0.08f
    }
    private val r = RectF()

    override fun onDraw(canvas: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        canvas.drawColor(Hw.CHASSIS)
        canvas.drawText(L10n.t(title).uppercase(), w / 2f, 14f * d, text)
        val top = 20f * d
        val bot = h - 6f * d
        val l = 8f * d
        val rr = w - 8f * d
        val span = (bot - top).coerceAtLeast(10f)

        // Pure black OLED well
        r.set(l, top, rr, bot)
        p.style = Paint.Style.FILL
        p.color = Hw.WELL
        canvas.drawRoundRect(r, 5f * d, 5f * d, p)
        p.style = Paint.Style.STROKE
        p.strokeWidth = 1f * d
        p.color = Hw.PANEL_EDGE
        canvas.drawRoundRect(r, 5f * d, 5f * d, p)

        // Center tick (bipolar)
        val midY = (top + bot) / 2f
        if (bipolar) {
            p.color = Hw.PANEL_HI
            canvas.drawLine(l + 3 * d, midY, rr - 3 * d, midY, p)
        }

        // Active fill bar
        val t = if (bipolar) (value + 1f) / 2f else value
        val my = bot - 5f * d - t * (span - 10f * d)
        p.style = Paint.Style.FILL
        p.color = Hw.withAlpha(accent, 90)
        if (bipolar) {
            val y0 = minOf(midY, my)
            val y1 = maxOf(midY, my)
            r.set(l + 3 * d, y0, rr - 3 * d, y1)
        } else {
            r.set(l + 3 * d, my, rr - 3 * d, bot - 2 * d)
        }
        canvas.drawRoundRect(r, 2.5f * d, 2.5f * d, p)

        // Glowing thumb bar
        p.color = accent
        r.set(l + 2 * d, my - 3f * d, rr - 2 * d, my + 3f * d)
        canvas.drawRoundRect(r, 2.5f * d, 2.5f * d, p)
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        val h = height.toFloat().coerceAtLeast(1f)
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_MOVE -> {
                val t = 1f - (e.y / h).coerceIn(0f, 1f)
                value = if (bipolar) t * 2f - 1f else t
                onChange(value)
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> if (spring) {
                value = 0f
                onChange(0f)
            }
        }
        invalidate()
        return true
    }
}
