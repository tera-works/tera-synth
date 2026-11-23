package com.hsynth.app

import android.annotation.SuppressLint
import android.app.AlertDialog
import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.widget.LinearLayout
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import kotlin.math.abs
import kotlin.math.roundToInt

/**
 * Phase 3 A6 (Rhythm pattern bar + Strum preview) and A2 (Vintage / Spread panel).
 *
 * Rhythm is not a preset parameter: [RhythmStore] mirrors the engine's `nativeSetRhythmJson` config, persists it in
 * `files/rhythm.json` (debounced, tmp + rename on the loader) and always boots with `enabled = false`.
 * Strum / Vintage / Spread are plain engine params (`strum.*`, `voice.vintage*`, `voice.panspread`).
 */
object RhythmStore {
    var enabled = false
        private set
    var pattern = 0
    /** −60 … 0 dB. */
    var levelDb = -12f
    /** 0 … 0.5 (fraction of a step the off-beats are delayed). */
    var swing = 0f

    /** The 16 groove names reported by the engine (falls back to "Groove n"). */
    var names: List<String> = List(PATTERNS) { "Groove ${it + 1}" }
        private set

    const val PATTERNS = 16
    const val MIN_DB = -60f

    private var file: File? = null
    private val handler = Handler(Looper.getMainLooper())
    private val saveRun = Runnable { writeNow() }
    private val listeners = ArrayList<() -> Unit>()

    fun addListener(l: () -> Unit) { if (!listeners.contains(l)) listeners += l }
    fun removeListener(l: () -> Unit) { listeners.remove(l) }
    fun clearListeners() = listeners.clear()
    private fun fire() { for (i in listeners.indices.reversed()) listeners.getOrNull(i)?.invoke() }

    private fun json(on: Boolean): String = JSONObject().apply {
        put("enabled", on)
        put("pattern", pattern)
        put("levelDb", levelDb.toDouble())
        put("swing", swing.toDouble())
        put("fill", false)
    }.toString()

    private fun parse(text: String): Boolean = try {
        val j = JSONObject(text)
        pattern = j.optInt("pattern", pattern).coerceIn(0, PATTERNS - 1)
        levelDb = j.optDouble("levelDb", levelDb.toDouble()).toFloat().coerceIn(MIN_DB, 0f)
        swing = j.optDouble("swing", swing.toDouble()).toFloat().coerceIn(0f, 0.5f)
        true
    } catch (_: Exception) {
        false
    }

    /** Startup: engine defaults + groove names, then `files/rhythm.json`; the groove itself is forced OFF. */
    fun loadAndApply(a: MainActivity) {
        val f = File(a.filesDir, "rhythm.json")
        file = f
        try {
            val arr = JSONArray(NativeEngine.nativeRhythmPatternsJson())
            if (arr.length() > 0) names = List(arr.length()) { arr.optString(it, "Groove ${it + 1}") }
        } catch (_: Throwable) {}
        try { parse(NativeEngine.nativeGetRhythmJson()) } catch (_: Throwable) {}
        if (f.isFile) {
            val ok = runCatching { parse(f.readText()) }.getOrDefault(false)
            if (!ok) a.log("rhythm.json is invalid — using the defaults", Ui.WARN)
        }
        enabled = false
        try {
            NativeEngine.nativeSetRhythmJson(json(false))
            NativeEngine.nativeSetRhythmFill(false)
        } catch (_: Throwable) {}
    }

    /** Re-read the run flag from the engine (a footswitch / remote may have changed it). */
    fun syncFromEngine() {
        try { enabled = JSONObject(NativeEngine.nativeGetRhythmJson()).optBoolean("enabled", enabled) } catch (_: Throwable) {}
    }

    /** After the user changed a setting: send it and save (debounced). */
    fun push() {
        NativeEngine.nativeSetRhythmJson(json(enabled))
        handler.removeCallbacks(saveRun)
        handler.postDelayed(saveRun, 500L)
        fire()
    }

    fun setEnabled(on: Boolean) {
        enabled = on
        NativeEngine.nativeSetRhythmJson(json(on))
        fire()
    }

    fun setFill(on: Boolean) = NativeEngine.nativeSetRhythmFill(on)

    fun name(i: Int): String = names.getOrNull(i) ?: "Groove ${i + 1}"

    private fun writeNow() {
        val f = file ?: return
        val text = json(false)   // never persisted as running
        AppServices.loader.execute {
            runCatching {
                val tmp = File(f.parentFile, f.name + ".tmp")
                tmp.writeText(text)
                if (!tmp.renameTo(f)) { f.writeText(text); tmp.delete() }
            }
        }
    }
}

// ====================================================================================================== bar

/**
 * Drum-groove bar: `◀ GROOVE ▶`, `▶ PLAY`, momentary `FILL` (ACTION_DOWN → fill on, UP / CANCEL → off), `VOL` bar and a
 * `SWING` number. [compact] (PLAY tab, two 34 dp rows ≤ 72 dp) adds a second row with the Strum toggle, window time,
 * order and swing; the EDIT version ([compact] = false) is a single wide row (Strum has its own section there).
 */
@SuppressLint("ViewConstructor", "ClickableViewAccessibility")
class RhythmBar(private val a: MainActivity, private val compact: Boolean) : LinearLayout(a) {
    private val d = resources.displayMetrics.density
    private fun dp(v: Int) = (v * d).toInt()

    private val grooveLcd = Hw.lcd(a, "", 12f).apply {
        gravity = Gravity.CENTER
        setPadding(dp(4), 0, dp(4), 0)
        isClickable = true
        setOnClickListener { chooseGroove() }
    }
    private val playBtn = small("▶ PLAY", Hw.MOD_FX, led = true).apply {
        setOnClickListener { RhythmStore.setEnabled(!RhythmStore.enabled) }
    }
    private val fillBtn = small("FILL", Hw.MOD_FILTER, led = false)
    private val vol = HSegBar(a, Hw.MOD_FX, 16, 44f)
    private val swing = DragNumber(
        a, 0f, 50f, 1f, 0f, { "SWG ${it.roundToInt()}%" },
        { RhythmStore.swing * 100f }, { RhythmStore.swing = it / 100f; RhythmStore.push() }
    )

    // compact: strum controls
    private val strumOn = Params.metaOf("strum.on")
    private val strumBtn = small("STRUM", Hw.MOD_OSC, led = true).apply { setOnClickListener { toggleStrum() } }
    private val strumTime = DragNumber(
        a, 0f, 40f, 1f, 12f, { "${it.roundToInt()} ms" },
        { Params.get("strum.window") }, { Params.set("strum.window", it) }
    )
    private val strumOrder = Hw.lcd(a, "", 11.5f).apply {
        gravity = Gravity.CENTER
        setPadding(dp(2), 0, dp(2), 0)
        isClickable = true
        setOnClickListener { cycleStrumOrder() }
    }

    private val sync: () -> Unit = { refresh() }

    init {
        orientation = VERTICAL
        val gap = dp(4)
        val rowH = dp(34)

        fun arrow(t: String, delta: Int) = small(t, Hw.ENGRAVE, led = false).apply {
            setOnClickListener { stepGroove(delta) }
        }

        val row1 = LinearLayout(a).apply { orientation = HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        row1.addView(arrow("◀", -1), LayoutParams(dp(28), -1).apply { marginEnd = dp(2) })
        row1.addView(grooveLcd, LayoutParams(0, -1, if (compact) 1.5f else 1f))
        row1.addView(arrow("▶", 1), LayoutParams(dp(28), -1).apply { marginStart = dp(2); marginEnd = gap })
        row1.addView(playBtn, LayoutParams(dp(66), -1).apply { marginEnd = gap })
        row1.addView(fillBtn, LayoutParams(dp(54), -1).apply { marginEnd = gap })
        row1.addView(vol, LayoutParams(0, -1, 1f).apply { if (!compact) marginEnd = gap })
        if (!compact) row1.addView(swing, LayoutParams(dp(78), -1))
        addView(row1, LayoutParams(-1, rowH))

        vol.onChange = { n ->
            RhythmStore.levelDb = RhythmStore.MIN_DB * (1f - n)
            vol.valueText = dbText(RhythmStore.levelDb)
            RhythmStore.push()
        }
        vol.onReset = {
            RhythmStore.levelDb = -12f
            RhythmStore.push()
            refresh()
        }

        // momentary FILL: on while the finger is down
        fillBtn.setOnTouchListener { v, e ->
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN -> { RhythmStore.setFill(true); (v as LedButton).lit = true }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> { RhythmStore.setFill(false); (v as LedButton).lit = false }
            }
            false   // keep the button's own pressed look
        }

        if (compact) {
            val row2 = LinearLayout(a).apply { orientation = HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
            row2.addView(strumBtn, LayoutParams(dp(72), -1).apply { marginEnd = gap })
            row2.addView(strumTime, LayoutParams(dp(66), -1).apply { marginEnd = gap })
            row2.addView(strumOrder, LayoutParams(0, -1, 1f).apply { marginEnd = gap })
            row2.addView(swing, LayoutParams(dp(78), -1))
            if (strumOn == null) { strumBtn.visibility = GONE; strumTime.visibility = GONE; strumOrder.visibility = GONE }
            addView(row2, LayoutParams(-1, rowH).apply { topMargin = gap })
        }
        refresh()
    }

    private fun small(t: String, accent: Int, led: Boolean) = LedButton(a, t, tr = false, led = led, accentColor = accent).apply {
        textSize = 11f
        minHeight = 0
        minimumHeight = 0
        setPadding(if (led) dp(15) else 0, 0, if (led) dp(4) else 0, 0)
    }

    private fun dbText(db: Float) = if (db <= RhythmStore.MIN_DB + 0.5f) "−∞" else "${db.roundToInt()}dB"

    private fun stepGroove(delta: Int) {
        val n = RhythmStore.names.size.coerceAtLeast(1)
        RhythmStore.pattern = (RhythmStore.pattern + delta + n) % n
        RhythmStore.push()
        refresh()
    }

    private fun chooseGroove() {
        val items = Array(RhythmStore.names.size) { "%02d  %s".format(it + 1, RhythmStore.names[it]) }
        AlertDialog.Builder(a)
            .setTitle(L10n.t("Groove"))
            .setSingleChoiceItems(items, RhythmStore.pattern) { dlg, which ->
                RhythmStore.pattern = which
                RhythmStore.push()
                refresh()
                dlg.dismiss()
            }
            .show()
    }

    private fun toggleStrum() {
        val m = strumOn ?: return
        History.checkpoint()
        NativeEngine.setParam(m.id, if (NativeEngine.nativeGetParam(m.id) > 0.5f) 0f else 1f)
        refresh()
    }

    private fun cycleStrumOrder() {
        val m = Params.metaOf("strum.order") ?: return
        if (m.options.isEmpty()) return
        History.checkpoint()
        NativeEngine.setParam(m.id, ((NativeEngine.nativeGetParam(m.id).roundToInt() + 1) % m.options.size).toFloat())
        refresh()
    }

    /** Re-read the store (+ the strum params in compact mode). Cheap: a handful of native reads. */
    fun refresh() {
        RhythmStore.syncFromEngine()
        grooveLcd.text = "%02d  %s".format(RhythmStore.pattern + 1, RhythmStore.name(RhythmStore.pattern))
        playBtn.lit = RhythmStore.enabled
        val n = (1f - RhythmStore.levelDb / RhythmStore.MIN_DB).coerceIn(0f, 1f)
        if (abs(vol.value - n) > 1e-4f) vol.value = n
        vol.valueText = dbText(RhythmStore.levelDb)
        swing.refresh()
        if (compact && strumOn != null) {
            strumBtn.lit = NativeEngine.nativeGetParam(strumOn.id) > 0.5f
            strumTime.refresh()
            val m = Params.metaOf("strum.order")
            strumOrder.text = m?.let { it.options.getOrNull(NativeEngine.nativeGetParam(it.id).roundToInt()) }?.uppercase() ?: "—"
        }
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        RhythmStore.addListener(sync)
        refresh()
    }

    override fun onDetachedFromWindow() {
        RhythmStore.removeListener(sync)
        super.onDetachedFromWindow()
    }
}

// ====================================================================================================== strum

/**
 * 120 × 40 dp strum preview: 6 vertical "strings"; each starts lower by its trigger time (`strum.spacing` × its place
 * in `strum.order`, plus a fixed pseudo-jitter × `strum.jitter`), so the staggered tops show direction and spread.
 * Dimmed while `strum.on` is off. Geometry is rebuilt in [refresh] (never in onDraw); onDraw only draws one
 * `drawLines` + the dots.
 */
@SuppressLint("ViewConstructor")
class StrumView(ctx: Context) : View(ctx) {
    private val d = resources.displayMetrics.density
    private val metaOn = Params.metaOf("strum.on")
    private val metaOrder = Params.metaOf("strum.order")
    private val metaSpacing = Params.metaOf("strum.spacing")
    private val metaJitter = Params.metaOf("strum.jitter")

    private val lines = FloatArray(6 * 4)
    private val tips = FloatArray(6 * 2)
    private val strP = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = 1.6f * d; strokeCap = Paint.Cap.ROUND }
    private val tipP = Paint(Paint.ANTI_ALIAS_FLAG)
    private val bgP = Paint().apply { color = Hw.WELL }
    private val frameP = Paint().apply { color = Hw.PANEL_EDGE; style = Paint.Style.STROKE; strokeWidth = d }
    private var on = true
    private var lastKey = Float.NaN

    init {
        contentDescription = "Strum preview"
    }

    override fun onMeasure(widthSpec: Int, heightSpec: Int) {
        setMeasuredDimension((120 * d).toInt(), (40 * d).toInt())
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        lastKey = Float.NaN
        refresh()
    }

    /** Re-read the strum params and rebuild the geometry. */
    fun refresh() {
        val w = width.toFloat()
        val h = height.toFloat()
        if (w <= 0f) return
        val isOn = metaOn?.let { NativeEngine.nativeGetParam(it.id) > 0.5f } ?: true
        val order = metaOrder?.let { m -> m.options.getOrNull(NativeEngine.nativeGetParam(m.id).roundToInt()) } ?: "up"
        val spacing = metaSpacing?.let { NativeEngine.nativeGetParam(it.id) } ?: 15f
        val jitter = metaJitter?.let { NativeEngine.nativeGetParam(it.id) } ?: 0f
        val key = spacing * 31f + jitter * 7f + order.length * 1000f + (order.firstOrNull()?.code ?: 0) + (if (isOn) 1e6f else 0f)
        if (key == lastKey) return
        lastKey = key
        on = isOn

        // place of each string in the strum sequence
        val place = IntArray(6)   // tiny, only on param change
        for (s in 0 until 6) {
            place[s] = when (order.lowercase()) {
                "down" -> 5 - s
                "alternate" -> ZIGZAG[s]
                else -> s            // up / asplayed (low → high)
            }
        }
        val pad = 10f * d
        val top = 6f * d
        val bottom = h - 5f * d
        // total travel of the last string = 5 steps of spacing (200 ms → 0.62 of the height)
        val travel = (bottom - top) * 0.62f
        val maxMs = 5f * 200f
        for (s in 0 until 6) {
            val x = pad + (w - 2f * pad) * s / 5f
            val t = place[s] * spacing + JIT[s] * jitter
            val y = (top + travel * (t / maxMs).coerceIn(0f, 1f) * 1f).coerceIn(top, bottom - 4f * d)
            lines[s * 4] = x; lines[s * 4 + 1] = y; lines[s * 4 + 2] = x; lines[s * 4 + 3] = bottom
            tips[s * 2] = x; tips[s * 2 + 1] = y
        }
        invalidate()
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        lastKey = Float.NaN
        refresh()
    }

    override fun onDraw(c: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        c.drawRoundRect(0.5f * d, 0.5f * d, w - 0.5f * d, h - 0.5f * d, 5f * d, 5f * d, bgP)
        c.drawRoundRect(0.5f * d, 0.5f * d, w - 0.5f * d, h - 0.5f * d, 5f * d, 5f * d, frameP)
        strP.color = if (on) Hw.withAlpha(Hw.MOD_OSC, 190) else Hw.INK_DIM
        c.drawLines(lines, 0, lines.size, strP)
        tipP.color = if (on) Hw.SILK else Hw.ENGRAVE
        for (s in 0 until 6) c.drawCircle(tips[s * 2], tips[s * 2 + 1], 2.2f * d, tipP)
    }

    private companion object {
        val ZIGZAG = intArrayOf(0, 2, 4, 5, 3, 1)         // alternate: low, then high, converging
        val JIT = floatArrayOf(0.7f, -0.4f, 0.2f, -0.9f, 0.5f, -0.1f)
    }
}

// ====================================================================================================== vintage

/**
 * 8 dots on a line showing how far apart the voices are panned: x = centre + spread × {−1 … 1} × ½W.
 * Redrawn only when the value changes ([setSpread]); `onDraw` = one line + 8 circles.
 */
@SuppressLint("ViewConstructor")
class SpreadFanView(ctx: Context) : View(ctx) {
    private val d = resources.displayMetrics.density
    private val meta = Params.metaOf("voice.panspread")
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)
    private var spread = 0f

    init {
        contentDescription = "Voice spread"
    }

    override fun onMeasure(widthSpec: Int, heightSpec: Int) {
        setMeasuredDimension((110 * d).toInt(), (44 * d).toInt())
    }

    fun setSpread(v: Float) {
        val c = v.coerceIn(0f, 1f)
        if (c == spread) return
        spread = c
        invalidate()
    }

    fun refresh() {
        val m = meta ?: return
        setSpread(m.toNormalized(NativeEngine.nativeGetParam(m.id)))
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        refresh()
    }

    override fun onDraw(c: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        val cx = w / 2f
        val cy = h / 2f
        p.style = Paint.Style.FILL
        p.color = Hw.WELL
        c.drawRoundRect(0.5f * d, 0.5f * d, w - 0.5f * d, h - 0.5f * d, 5f * d, 5f * d, p)
        p.style = Paint.Style.STROKE
        p.strokeWidth = d
        p.color = Hw.PANEL_EDGE
        c.drawRoundRect(0.5f * d, 0.5f * d, w - 0.5f * d, h - 0.5f * d, 5f * d, 5f * d, p)
        c.drawLine(8f * d, cy, w - 8f * d, cy, p)
        p.style = Paint.Style.FILL
        val half = (w / 2f - 10f * d) * spread
        for (i in 0 until 8) {
            val x = cx + FAN[i] * half
            // centre dots drawn brighter so a fully closed fan still reads as one point
            p.color = if (i == 3 || i == 4) Hw.MOD_OSC else Hw.withAlpha(Hw.MOD_OSC, 170)
            c.drawCircle(x, cy, 3f * d, p)
        }
    }

    private companion object {
        val FAN = floatArrayOf(-1f, -0.71f, -0.43f, -0.14f, 0.14f, 0.43f, 0.71f, 1f)
    }
}

/**
 * Vintage / Spread block (826 × 74): `VINTAGE` segmented bar (`voice.vintage`, "drift · slop") + `DRIFT RATE` and
 * `SPREAD` knobs ([ParamControl]s, so MIDI learn and morph learn work) + a [SpreadFanView]. Placed at the top of
 * EDIT > Voice and as the `extra` of the VA engine; the generic Voice flow excludes [NAMES] so nothing is duplicated.
 */
@SuppressLint("ViewConstructor")
class VintagePanel(private val a: MainActivity) : LinearLayout(a) {
    companion object {
        const val VINTAGE = "voice.vintage"
        const val RATE = "voice.vintage.rate"
        const val SPREAD = "voice.panspread"
        val NAMES = setOf(VINTAGE, RATE, SPREAD)
    }

    private val d = resources.displayMetrics.density
    private val metaVintage = Params.metaOf(VINTAGE)
    private val bar = HSegBar(a, Hw.MOD_OSC, 24, 40f)
    private val fan = SpreadFanView(a)
    val controls = ArrayList<ParamControl>()

    /** A control changed by the user: hosts refresh quick strip / graphs. */
    var onEdited: (() -> Unit)? = null

    init {
        orientation = HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL

        val left = LinearLayout(a).apply { orientation = VERTICAL }
        val cap = LinearLayout(a).apply { orientation = HORIZONTAL; gravity = Gravity.BOTTOM }
        cap.addView(Hw.engraved(a, "VINTAGE", 10f, tr = false, color = Hw.MOD_OSC), LayoutParams(-2, -2))
        cap.addView(
            Hw.engraved(a, "drift · slop", 9f, tr = false, color = Hw.INK_DIM),
            LayoutParams(-2, -2).apply { marginStart = (8 * d).toInt() }
        )
        left.addView(cap, LayoutParams(-1, (16 * d).toInt()))
        left.addView(bar, LayoutParams(-1, (28 * d).toInt()).apply { topMargin = (3 * d).toInt() })
        addView(left, LayoutParams((300 * d).toInt(), -2).apply { marginEnd = (12 * d).toInt() })

        bar.onDragStart = { History.checkpoint() }
        bar.onChange = { v ->
            metaVintage?.let { NativeEngine.setParam(it.id, it.fromNormalized(v)) }
            bar.valueText = "%d%%".format((v * 100f).roundToInt())
            Hw.onParamTouched?.invoke("VINTAGE", bar.valueText)
            onEdited?.invoke()
        }
        bar.onReset = {
            History.checkpoint()
            metaVintage?.let { NativeEngine.setParam(it.id, it.def) }
            refresh()
            onEdited?.invoke()
        }
        if (metaVintage == null) left.alpha = 0.4f

        for (name in listOf(RATE, SPREAD)) {
            val m = Params.metaOf(name) ?: continue
            val c = ParamControl(a, m, knobSize = 44f, accent = Hw.MOD_OSC) { ctl ->
                if (ctl.meta.name == SPREAD) fan.refresh()
                onEdited?.invoke()
            }
            controls += c
            addView(c.view, LayoutParams(-2, -2).apply { marginEnd = (6 * d).toInt() })
        }
        addView(fan, LayoutParams((110 * d).toInt(), (44 * d).toInt()).apply { marginStart = (6 * d).toInt() })
        refresh()
    }

    /** Re-read the three params from the engine. */
    fun refresh() {
        metaVintage?.let {
            val n = it.toNormalized(NativeEngine.nativeGetParam(it.id))
            bar.value = n
            bar.valueText = "%d%%".format((n * 100f).roundToInt())
        }
        for (c in controls) c.refresh()
        fan.refresh()
    }

    /** Used by [EditPanel] to decide whether the engine offers any of the vintage params at all. */
    val isAvailable: Boolean get() = metaVintage != null || controls.isNotEmpty()
}
