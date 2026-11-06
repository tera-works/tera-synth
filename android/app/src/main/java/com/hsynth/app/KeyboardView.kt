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
 * Multi-touch piano keyboard.
 *
 *  - Velocity: from the touch position along the key (further down = louder),
 *    or fixed.
 *  - Per-note expression for every finger independently:
 *      vertical movement  -> NoteTimbre (0..1)
 *      touch size/pressure -> NotePressure
 *      horizontal slide    -> "gliss" (retrigger on the new key) or
 *                             "bend" (continuous per-note pitch, MPE style)
 */
class KeyboardView(context: Context) : View(context) {
    enum class SlideMode { GLISS, BEND }

    var firstNote = 48              // must be a white key
        set(v) { field = v; allNotesOff(); invalidate() }
    var whiteKeyCount = 15
        set(v) { field = v.coerceIn(5, 36); allNotesOff(); invalidate() }
    var slideMode = SlideMode.GLISS
    /** Semitones added to the notes this keyboard plays (the keys shown stay where they are). */
    var transpose = 0
        set(v) { field = v.coerceIn(-24, 24); allNotesOff() }
    private fun snd(key: Int) = (key + transpose).coerceIn(0, 127)
    var fixedVelocity: Float? = null
    var onNotesChanged: ((Int) -> Unit)? = null
    /** Every note played (on, key, velocity), e.g. for MIDI out. */
    var onNote: ((Boolean, Int, Float) -> Unit)? = null

    private data class Touch(var noteId: Int, var key: Int, val downX: Float, val downY: Float, var lastTimbre: Float = -1f)

    private val touches = HashMap<Int, Touch>()
    private var nextNoteId = 0x100000
    private val held = IntArray(128)

    private val white = Paint().apply { color = Color.rgb(236, 238, 242) }
    private val whitePressed = Paint().apply { color = Color.rgb(255, 196, 140) }
    private val black = Paint().apply { color = Color.rgb(24, 26, 32) }
    private val blackPressed = Paint().apply { color = Color.rgb(230, 120, 30) }
    private val border = Paint().apply { color = Color.rgb(60, 64, 72); style = Paint.Style.STROKE; strokeWidth = 2f }
    private val label = Paint().apply { color = Color.rgb(90, 96, 110); textSize = 28f; isAntiAlias = true; textAlign = Paint.Align.CENTER }
    private val touchPaint = Paint().apply { color = Color.argb(110, 255, 160, 60); isAntiAlias = true }
    private val rect = RectF()

    private fun isBlack(note: Int) = when (note % 12) { 1, 3, 6, 8, 10 -> true; else -> false }

    private fun whiteIndexToNote(index: Int): Int {
        var n = firstNote
        var i = 0
        while (i < index) { n++; if (!isBlack(n)) i++ }
        return n
    }

    private fun whiteWidth() = width.toFloat() / whiteKeyCount

    /** Note under (x, y), black keys take precedence in the upper 62 %. */
    private fun noteAt(x: Float, y: Float): Int {
        val ww = whiteWidth()
        val wi = (x / ww).toInt().coerceIn(0, whiteKeyCount - 1)
        val wn = whiteIndexToNote(wi)
        if (y < height * 0.62f) {
            val local = x - wi * ww
            // black key to the right of this white key
            if (local > ww * 0.68f && isBlack(wn + 1) && wi < whiteKeyCount - 1) return wn + 1
            // black key to the left
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
        for (i in 0 until whiteKeyCount) {
            val n = whiteIndexToNote(i)
            rect.set(i * ww, 0f, (i + 1) * ww, h)
            canvas.drawRect(rect, if (held[n] > 0) whitePressed else white)
            canvas.drawRect(rect, border)
            if (n % 12 == 0) canvas.drawText("C${n / 12 - 1}", i * ww + ww / 2, h - 18f, label)
        }
        for (i in 0 until whiteKeyCount - 1) {
            val n = whiteIndexToNote(i)
            if (isBlack(n + 1)) {
                val cx = (i + 1) * ww
                rect.set(cx - ww * 0.32f, 0f, cx + ww * 0.32f, h * 0.62f)
                canvas.drawRoundRect(rect, 6f, 6f, if (held[n + 1] > 0) blackPressed else black)
            }
        }
        for (t in touches.values) {
            canvas.drawCircle(t.downX, t.downY, 30f, touchPaint)
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
        // touch-size based pseudo pressure (capacitive screens have no force sensor)
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

    /** Highlight keys played from MIDI. */
    fun setExternalHeld(key: Int, on: Boolean) {
        if (key !in 0..127) return
        held[key] = if (on) held[key] + 1 else (held[key] - 1).coerceAtLeast(0)
        postInvalidate()
    }
}

/**
 * Vertical controller strip. Spring mode returns to the centre when released
 * (pitch bend); otherwise it holds its value (mod wheel).
 */
class ControlStrip(context: Context, private val title: String, private val spring: Boolean, private val bipolar: Boolean,
                   private val onChange: (Float) -> Unit) : View(context) {
    private var value = if (bipolar) 0f else 0f
    private val d = resources.displayMetrics.density
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)
    private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Hw.ENGRAVE; textSize = 12.5f * d; textAlign = Paint.Align.CENTER; typeface = Hw.BOLD }

    // drawn as a pitch / mod wheel in a recessed slot, the ridges move with the value
    override fun onDraw(canvas: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        canvas.drawColor(Hw.PANEL)
        canvas.drawText(L10n.t(title).uppercase(), w / 2, 16 * d, text)
        val top = 24 * d
        val bot = h - 8 * d
        val l = 10 * d
        val r = w - 10 * d
        p.shader = null
        p.color = Color.rgb(8, 9, 11)
        canvas.drawRoundRect(l - 3 * d, top - 3 * d, r + 3 * d, bot + 3 * d, 6 * d, 6 * d, p)
        p.shader = android.graphics.LinearGradient(0f, top, 0f, bot,
            intArrayOf(Color.rgb(20, 21, 25), Color.rgb(70, 74, 84), Color.rgb(20, 21, 25)), null, android.graphics.Shader.TileMode.CLAMP)
        canvas.drawRoundRect(l, top, r, bot, 4 * d, 4 * d, p)
        p.shader = null
        // ridges
        val t = if (bipolar) (value + 1f) / 2f else value
        val span = bot - top
        val step = 9 * d
        p.color = Color.argb(120, 0, 0, 0)
        p.strokeWidth = 1.5f * d
        var y = top + ((1f - t) * span * 0.6f) % step
        while (y < bot) {
            val fade = 1f - kotlin.math.abs((y - (top + bot) / 2) / (span / 2))
            p.alpha = (40 + 110 * fade).toInt()
            canvas.drawLine(l + 2 * d, y, r - 2 * d, y, p)
            y += step
        }
        // position marker
        val my = bot - 6 * d - t * (span - 12 * d)
        p.color = Hw.LED
        canvas.drawRoundRect(l + 1 * d, my - 3 * d, r - 1 * d, my + 3 * d, 2 * d, 2 * d, p)
        if (bipolar) {
            p.color = Color.argb(140, 255, 255, 255)
            canvas.drawRect(l - 3 * d, (top + bot) / 2 - 0.5f * d, l, (top + bot) / 2 + 0.5f * d, p)
        }
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        val h = height.toFloat()
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
