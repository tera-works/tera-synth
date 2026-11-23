package com.hsynth.app

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import android.widget.LinearLayout
import kotlin.math.cos
import kotlin.math.roundToInt
import kotlin.math.sin

/**
 * Phase 3 A4 — Piano Realism (pedal noise · soft release · dynamic compression · lid · half-damper).
 *
 * Pure metadata: the engine registers the plain params below in group [PIANO_MAP.GROUP]; every view degrades
 * gracefully when a name is missing (`Params.metaOf(n) ?: skip`). As in `Physical.kt`, views never register
 * `Params.addListener` (see the note there) — hosts call [PianoPanel.refresh].
 */
object PIANO_MAP {
    const val GROUP = "Piano"
    const val PEDAL_NOISE = "piano.pedalnoise"
    const val SOFT_RELEASE = "piano.softrelease"
    const val DYN_COMP = "piano.dyncomp"
    const val LID = "piano.lid"
    const val HALF_DAMP = "piano.halfdamp"

    /** Knob order (the 5th is optional: shown only when the engine exposes it). */
    val KNOBS = listOf(PEDAL_NOISE, SOFT_RELEASE, DYN_COMP, LID, HALF_DAMP)

    /** True once the engine registered at least one piano parameter. */
    fun available(): Boolean = KNOBS.any { Params.metaOf(it) != null }
}

// ====================================================================================================== lid

/**
 * 64 × 40 dp side view of a grand piano: fixed trapezoid case + a lid hinged at the back whose angle follows
 * `piano.lid` (closed → half stick → full stick, 0 … 50°). Geometry is rebuilt only in [setNormalized] /
 * onSizeChanged; `onDraw` allocates nothing (two preallocated [Path]s, three [Paint]s).
 */
@SuppressLint("ViewConstructor")
class LidView(ctx: Context) : View(ctx) {
    private val d = resources.displayMetrics.density
    private val meta = Params.metaOf(PIANO_MAP.LID)
    private val body = Path()
    private val lid = Path()
    private val fillP = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val strokeP = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 1.4f * d
        strokeJoin = Paint.Join.ROUND
        strokeCap = Paint.Cap.ROUND
    }
    private var n = 0f              // 0 closed … 1 full stick
    private var stickOn = false
    private var sx = 0f             // stick: point on the lid underside …
    private var sy = 0f
    private var sy2 = 0f            // … down to the case top

    init {
        contentDescription = "Piano lid"
    }

    override fun onMeasure(widthSpec: Int, heightSpec: Int) {
        setMeasuredDimension((64 * d).toInt(), (40 * d).toInt())
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        val top = h * 0.70f
        val bot = h * 0.94f
        body.reset()
        body.moveTo(w * 0.05f, bot)
        body.lineTo(w * 0.95f, bot)
        body.lineTo(w * 0.88f, top)
        body.lineTo(w * 0.12f, top)
        body.close()
        buildLid()
    }

    private fun buildLid() {
        val w = width.toFloat()
        val h = height.toFloat()
        if (w <= 0f) return
        val hx = w * 0.88f                 // hinge (back edge of the case top)
        val hy = h * 0.70f
        val len = w * 0.76f
        val th = 2.6f * d
        val ang = Math.toRadians((n * 50f).toDouble())
        val ux = -cos(ang).toFloat()       // lid direction: leftwards, rising with the angle
        val uy = -sin(ang).toFloat()
        val tx = hx + ux * len
        val ty = hy + uy * len
        lid.reset()
        lid.moveTo(hx, hy)
        lid.lineTo(tx, ty)
        lid.lineTo(tx, ty - th)
        lid.lineTo(hx, hy - th)
        lid.close()
        stickOn = n > 0.08f
        sx = hx + ux * len * 0.62f
        sy = hy + uy * len * 0.62f
        sy2 = hy
    }

    /** [v] = lid position normalised 0 (closed) … 1 (full stick). */
    fun setNormalized(v: Float) {
        val c = v.coerceIn(0f, 1f)
        if (c == n) return
        n = c
        buildLid()
        invalidate()
    }

    /** Re-read `piano.lid` from the engine. */
    fun refresh() {
        val m = meta ?: return
        setNormalized(m.toNormalized(NativeEngine.nativeGetParam(m.id)))
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        refresh()
    }

    override fun onDraw(c: Canvas) {
        // case
        fillP.color = Hw.KEY
        c.drawPath(body, fillP)
        strokeP.color = Hw.ENGRAVE
        c.drawPath(body, strokeP)
        // lid
        val open = n > 0.02f
        fillP.color = if (open) Hw.withAlpha(Hw.MOD_OSC, 60) else Hw.PANEL_RAISED
        c.drawPath(lid, fillP)
        strokeP.color = if (open) Hw.MOD_OSC else Hw.ENGRAVE
        c.drawPath(lid, strokeP)
        // prop stick
        if (stickOn) {
            strokeP.color = Hw.SILK
            c.drawLine(sx, sy, sx, sy2, strokeP)
        }
    }
}

// ====================================================================================================== panel

/**
 * The Piano Realism block: the stock knobs ([PIANO_MAP.KNOBS], missing ones skipped) + a [LidView].
 * [largeKnobs] = 60 dp knobs in a single centred row (the STAGE [PianoSheet]); otherwise 48 dp knobs in a wrapping
 * flow (the EDIT > Voice section). Call [refresh] after a preset load / undo / when the sheet opens.
 */
@SuppressLint("ViewConstructor")
class PianoPanel(private val a: MainActivity, largeKnobs: Boolean) : LinearLayout(a) {
    val controls = ArrayList<ParamControl>()
    val lid = LidView(a)

    /** A knob (or the lid) changed by the user: hosts refresh the quick strip / graphs. */
    var onEdited: (() -> Unit)? = null

    val isAvailable: Boolean get() = controls.isNotEmpty()

    init {
        orientation = VERTICAL
        gravity = Gravity.CENTER_HORIZONTAL
        val d = resources.displayMetrics.density
        val knob = if (largeKnobs) 60f else 48f

        val row: LinearLayout?
        val flow: FlowLayout?
        if (largeKnobs) {
            row = LinearLayout(a).apply { orientation = HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
            flow = null
        } else {
            row = null
            flow = FlowLayout(a, (8 * d).toInt(), (8 * d).toInt())
        }
        fun add(v: View) {
            if (row != null) row.addView(v, LayoutParams(-2, -2).apply { marginEnd = (10 * d).toInt() })
            else flow?.addView(v)
        }

        for (name in PIANO_MAP.KNOBS) {
            val m = Params.metaOf(name) ?: continue
            val c = ParamControl(a, m, knobSize = knob, accent = Hw.MOD_OSC) { ctl ->
                if (ctl.meta.name == PIANO_MAP.LID) lid.refresh()
                onEdited?.invoke()
            }
            controls += c
            add(c.view)
        }
        if (Params.metaOf(PIANO_MAP.LID) != null) {
            val cell = FrameLayout(a)
            cell.addView(lid, FrameLayout.LayoutParams((64 * d).toInt(), (40 * d).toInt(), Gravity.CENTER))
            // same height as a knob (cap + label + value) so the lid sits level with the knobs
            val cellLp = LayoutParams((64 * d).toInt(), ((knob + 36f) * d).toInt())
            if (row != null) row.addView(cell, cellLp) else flow?.addView(cell, cellLp)
        }

        if (row != null) addView(row, LayoutParams(-2, -2))
        if (flow != null) addView(flow, LayoutParams(-1, -2))
        if (!isAvailable) {
            addView(Hw.engraved(a, "Piano parameters not available", 10.5f, tr = true, color = Hw.INK_DIM), LayoutParams(-2, -2))
        }
    }

    /** Re-read every control + the lid from the engine. */
    fun refresh() {
        for (c in controls) c.refresh()
        lid.refresh()
    }

    companion object {
        private var catKey: String? = null
        private var catIsPiano = false

        /**
         * Should the STAGE `PIANO` chip show for the current preset? Requires the engine's piano params, then any of:
         * the preset's library category is "Piano", its name says piano / grand, or layer A is a `pcm` layer.
         * Electric pianos are excluded from the category / name checks.
         */
        fun isPianoPreset(a: MainActivity): Boolean {
            if (!PIANO_MAP.available()) return false

            val file = a.currentPreset
            val key = file?.absolutePath
            if (key != null) {
                if (key != catKey) {
                    catKey = key
                    catIsPiano = try {
                        a.library.presets().firstOrNull { it.file == file }?.category?.let { looksPiano(it) } ?: false
                    } catch (_: Exception) { false }
                }
                if (catIsPiano) return true
            }
            if (looksPiano(a.currentPresetName)) return true

            val t = Params.metaOf("a.type") ?: return false
            val pcm = t.options.indexOfFirst { it.equals("pcm", ignoreCase = true) }
            return pcm >= 0 && NativeEngine.nativeGetParam(t.id).roundToInt() == pcm
        }

        private fun looksPiano(s: String): Boolean {
            val l = s.trim().lowercase()
            if (l.isEmpty()) return false
            if (l.startsWith("e.") || l.contains("e.piano") || l.contains("electric")) return false
            return l.contains("piano") || l.contains("grand")
        }
    }
}

// ====================================================================================================== sheet

/**
 * STAGE `PIANO` chip sheet: BOTTOM, 176 dp. Header (title + ✕) over one centred row of 60 dp knobs + [LidView],
 * reusing [PianoPanel] with `largeKnobs = true`.
 */
class PianoSheet(private val a: MainActivity) : Sheet(Edge.BOTTOM, 176) {
    override val view: View by lazy { build() }
    private val panel by lazy { PianoPanel(a, largeKnobs = true) }

    override fun onShow() {
        view
        panel.refresh()
    }

    private fun build(): View {
        val d = a.resources.displayMetrics.density
        val root = LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            setPadding((12 * d).toInt(), (2 * d).toInt(), (12 * d).toInt(), (6 * d).toInt())
        }
        val head = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        head.addView(Hw.engraved(a, "Piano realism", 11f), LinearLayout.LayoutParams(0, -2, 1f))
        val close = LedButton(a, "✕", tr = false, led = false).apply {
            minHeight = 0
            minimumHeight = 0
            setPadding(0, 0, 0, 0)
            setOnClickListener { dismiss() }
        }
        head.addView(close, LinearLayout.LayoutParams((36 * d).toInt(), (28 * d).toInt()))
        root.addView(head, LinearLayout.LayoutParams(-1, (28 * d).toInt()))

        val body = FrameLayout(a)
        body.addView(panel, FrameLayout.LayoutParams(-1, -2, Gravity.CENTER_VERTICAL))
        root.addView(body, LinearLayout.LayoutParams(-1, 0, 1f))
        return root
    }
}
