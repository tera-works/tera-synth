package com.hsynth.app

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.view.MotionEvent
import android.view.View
import kotlin.math.max
import kotlin.math.min

/**
 * The player's channel table: 16 rows (one per MIDI channel / part) with
 * mute, instrument, level meter, volume (drag), solo, pan, expression, bend,
 * mod, reverb / chorus sends and a keyboard showing the keys that sound.
 * Drawn in one pass (cheap enough for ~30 fps updates).
 */
class ChannelTableView(ctx: Context) : View(ctx) {
    /** Called when the user changes the mix: (part, Mix id, value). */
    var onMix: ((Int, Int, Float) -> Unit)? = null

    val preset = Array(16) { "" }
    val gm = Array(16) { "" }
    val used = BooleanArray(16)
    private val state = FloatArray(5 + 16 * 12)
    private val masks = IntArray(64)
    private val level = FloatArray(16)
    val userMute = BooleanArray(16)
    val solo = BooleanArray(16)

    private val d = resources.displayMetrics.density
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)
    private val txt = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Ui.TEXT; typeface = Hw.LABEL }
    private val r = RectF()

    // column layout: fractions of the width (the keyboard takes the rest)
    private val cols = floatArrayOf(0.03f, 0.04f, 0.17f, 0.06f, 0.09f, 0.04f, 0.045f, 0.04f, 0.045f, 0.035f, 0.035f, 0.035f)
    private val heads = arrayOf("", "CH", "Sound / GM", "Level", "Volume", "Solo", "Pan", "Exp", "Bend", "Mod", "Rev", "Cho").map { L10n.t(it) }
    private val x = FloatArray(cols.size + 1)

    private val headerH get() = 24 * d
    private val rowH get() = (height - headerH) / 16f

    // fixed row height (the player puts the table in a ScrollView on small screens)
    override fun onMeasure(widthSpec: Int, heightSpec: Int) {
        val lpW = layoutParams?.width ?: -1
        setMeasuredDimension(if (lpW > 0) lpW else MeasureSpec.getSize(widthSpec), (headerH + 16 * 38 * d).toInt())
    }

    fun update(st: FloatArray, m: IntArray) {
        System.arraycopy(st, 0, state, 0, min(st.size, state.size))
        System.arraycopy(m, 0, masks, 0, min(m.size, masks.size))
        for (k in 0 until 16) level[k] = max(state[5 + k * 12], level[k] * 0.82f)
        invalidate()
    }

    private fun partColor(k: Int, alpha: Int = 255): Int {
        val hsv = floatArrayOf(k * 360f / 16f, 0.65f, 0.95f)
        return Color.HSVToColor(alpha, hsv)
    }

    private fun layoutCols() {
        var acc = 6 * d
        x[0] = acc
        for (i in cols.indices) {
            acc += cols[i] * width
            x[i + 1] = acc
        }
    }

    override fun onDraw(c: Canvas) {
        layoutCols()
        c.drawColor(Ui.BG)
        // header
        txt.textSize = 11 * d
        txt.color = Ui.DIM
        for (i in heads.indices) if (heads[i].isNotEmpty()) c.drawText(heads[i], x[i] + 2 * d, headerH - 8 * d, txt)
        c.drawText(L10n.t("Keyboard"), x[cols.size] + 8 * d, headerH - 8 * d, txt)
        val anySolo = solo.any { it }
        for (k in 0 until 16) {
            val top = headerH + k * rowH
            val mid = top + rowH / 2
            val o = 5 + k * 12
            val active = used[k] || state[o + 8] > 0
            val alpha = if (active) 255 else 90
            if (k % 2 == 0) {
                p.color = Color.rgb(19, 22, 28)
                c.drawRect(0f, top, width.toFloat(), top + rowH, p)
            }
            val muted = state[o + 10] > 0.5f
            // mute (speaker)
            txt.textSize = 15 * d
            txt.color = if (muted) Ui.ERR else Color.argb(alpha, 200, 205, 215)
            c.drawText(if (muted) "✕" else "♪", x[0] + 4 * d, mid + 5 * d, txt)
            // colour + channel
            p.color = partColor(k, alpha)
            c.drawCircle(x[1] + 6 * d, mid, 4 * d, p)
            txt.textSize = 14 * d
            txt.color = Color.argb(alpha, 220, 224, 232)
            c.drawText("${k + 1}", x[1] + 14 * d, mid + 5 * d, txt)
            // instrument
            val name = if (preset[k].isNotEmpty()) preset[k] else L10n.t(if (used[k]) "(no sound)" else "(unused)")
            txt.textSize = 13 * d
            c.drawText(ellipsize(name, x[3] - x[2] - 6 * d), x[2] + 2 * d, mid - 1 * d, txt)
            if (gm[k].isNotEmpty()) {
                txt.textSize = 10 * d
                txt.color = Color.argb(alpha, 140, 148, 162)
                c.drawText(ellipsize(gm[k], x[3] - x[2] - 6 * d), x[2] + 2 * d, mid + 11 * d, txt)
            }
            // level meter
            bar(c, x[3], x[4], mid, min(1f, level[k] * 1.4f), partColor(k, alpha), alpha)
            // volume (mix) with CC7 shown as a thin marker
            val vol = state[o + 9]
            bar(c, x[4], x[5], mid, vol, Color.argb(alpha, 79, 195, 247), alpha, 10 * d)
            txt.textSize = 11 * d
            txt.color = Color.argb(alpha, 255, 255, 255)
            c.drawText("${(vol * 100).toInt()}%", x[4] + 6 * d, mid + 4 * d, txt)
            // solo
            val so = solo[k]
            p.color = if (so) Color.rgb(255, 193, 7) else Color.argb(alpha, 45, 50, 60)
            r.set(x[5] + 2 * d, mid - 9 * d, x[6] - 4 * d, mid + 9 * d)
            c.drawRoundRect(r, 3 * d, 3 * d, p)
            txt.color = if (so) Color.BLACK else Color.argb(alpha, 200, 205, 215)
            c.drawText("S", x[5] + (x[6] - x[5]) / 2 - 4 * d, mid + 4 * d, txt)
            // pan (centre marker), expression, bend (centre), mod, reverb, chorus
            center(c, x[6], x[7], mid, state[o + 3], alpha)
            bar(c, x[7], x[8], mid, state[o + 2] * state[o + 1], Color.argb(alpha, 79, 195, 247), alpha)
            center(c, x[8], x[9], mid, state[o + 4], alpha, Color.rgb(255, 167, 38))
            bar(c, x[9], x[10], mid, state[o + 5], Color.argb(alpha, 129, 199, 132), alpha)
            bar(c, x[10], x[11], mid, state[o + 6], Color.argb(alpha, 186, 104, 200), alpha)
            bar(c, x[11], x[12], mid, state[o + 7], Color.argb(alpha, 77, 182, 172), alpha)
            // keyboard C1 .. B7
            keyboard(c, x[12] + 8 * d, width - 4 * d, top + 3 * d, top + rowH - 3 * d, k, alpha)
            if (muted || (anySolo && !so)) {
                p.color = Color.argb(110, 10, 12, 16)
                c.drawRect(0f, top, width.toFloat(), top + rowH, p)
            }
        }
    }

    private fun ellipsize(s: String, w: Float): String {
        if (txt.measureText(s) <= w) return s
        var e = s
        while (e.isNotEmpty() && txt.measureText("$e…") > w) e = e.dropLast(1)
        return "$e…"
    }

    private fun bar(c: Canvas, x0: Float, x1: Float, mid: Float, v: Float, color: Int, alpha: Int, h: Float = 6 * d) {
        val l = x0 + 3 * d
        val rgt = x1 - 5 * d
        p.color = Color.argb(alpha, 38, 42, 52)
        r.set(l, mid - h / 2, rgt, mid + h / 2)
        c.drawRoundRect(r, 2 * d, 2 * d, p)
        p.color = color
        r.set(l, mid - h / 2, l + (rgt - l) * v.coerceIn(0f, 1f), mid + h / 2)
        c.drawRoundRect(r, 2 * d, 2 * d, p)
    }

    private fun center(c: Canvas, x0: Float, x1: Float, mid: Float, v: Float, alpha: Int, color: Int = Color.rgb(79, 195, 247)) {
        val l = x0 + 3 * d
        val rgt = x1 - 5 * d
        p.color = Color.argb(alpha, 38, 42, 52)
        r.set(l, mid - 3 * d, rgt, mid + 3 * d)
        c.drawRoundRect(r, 2 * d, 2 * d, p)
        val cx = (l + rgt) / 2 + (rgt - l) / 2 * v.coerceIn(-1f, 1f)
        p.color = Color.argb(alpha, Color.red(color), Color.green(color), Color.blue(color))
        c.drawRect(cx - 1.5f * d, mid - 7 * d, cx + 1.5f * d, mid + 7 * d, p)
    }

    private fun sounding(part: Int, key: Int) = (masks[part * 4 + (key shr 5)] ushr (key and 31)) and 1 != 0

    private fun keyboard(c: Canvas, x0: Float, x1: Float, y0: Float, y1: Float, part: Int, alpha: Int) {
        val first = 24
        val last = 107  // C1 .. B7
        var whites = 0
        for (n in first..last) if (!isBlack(n)) whites++
        val ww = (x1 - x0) / whites
        val col = partColor(part)
        var wi = 0
        for (n in first..last) {
            if (isBlack(n)) continue
            val l = x0 + wi * ww
            p.color = if (sounding(part, n)) col else Color.argb(alpha, 232, 234, 240)
            c.drawRect(l + 0.5f, y0, l + ww - 0.5f, y1, p)
            if (n % 12 == 0 && rowH > 24 * d) {
                txt.textSize = 7 * d
                txt.color = Color.argb(alpha, 120, 120, 130)
                c.drawText("C${n / 12 - 1}", l + 1 * d, y1 - 2 * d, txt)
            }
            wi++
        }
        wi = 0
        for (n in first..last) {
            if (isBlack(n)) {
                val l = x0 + wi * ww - ww * 0.3f
                p.color = if (sounding(part, n)) col else Color.argb(alpha, 26, 28, 34)
                c.drawRect(l, y0, l + ww * 0.6f, y0 + (y1 - y0) * 0.6f, p)
            } else {
                wi++
            }
        }
    }

    private fun isBlack(n: Int) = when (n % 12) { 1, 3, 6, 8, 10 -> true; else -> false }

    // ------------------------------------------------------------ touch

    private var dragPart = -1

    override fun onTouchEvent(e: MotionEvent): Boolean {
        layoutCols()
        val k = ((e.y - headerH) / rowH).toInt()
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                if (k !in 0 until 16) return false
                when {
                    e.x < x[1] -> { userMute[k] = !userMute[k]; applyMutes() }
                    e.x >= x[5] && e.x < x[6] -> { solo[k] = !solo[k]; applyMutes() }
                    e.x >= x[4] && e.x < x[5] -> { dragPart = k; setVolume(k, e.x) }
                    else -> return false
                }
                return true
            }
            MotionEvent.ACTION_MOVE -> if (dragPart >= 0) { setVolume(dragPart, e.x); return true }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> dragPart = -1
        }
        return dragPart >= 0
    }

    private fun setVolume(k: Int, px: Float) {
        val l = x[4] + 3 * d
        val rgt = x[5] - 5 * d
        val v = ((px - l) / (rgt - l)).coerceIn(0f, 1f)
        state[5 + k * 12 + 9] = v
        onMix?.invoke(k, 0, v)  // Mix::Volume
        invalidate()
    }

    /** Mute = the user's mute, or another channel is soloed. */
    fun applyMutes() {
        val anySolo = solo.any { it }
        for (k in 0 until 16) {
            val m = userMute[k] || (anySolo && !solo[k])
            onMix?.invoke(k, 2, if (m) 1f else 0f)  // Mix::Mute
        }
        invalidate()
    }
}
