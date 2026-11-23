package com.hsynth.app

import android.annotation.SuppressLint
import android.app.AlertDialog
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.drawable.GradientDrawable
import android.text.TextUtils
import android.view.GestureDetector
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.roundToInt

/** LCD number: drag up / down to change, double tap for the default. */
@SuppressLint("ViewConstructor")
class DragNumber(ctx: Context, private val min: Float, private val max: Float, private val step: Float, private val def: Float,
                 private val format: (Float) -> String, private val get: () -> Float, private val set: (Float) -> Unit) : TextView(ctx) {
    /** Optional dynamic default (e.g. a zone strip whose part changes with the page); overrides [def]. */
    var defProvider: (() -> Float)? = null
    private val d = resources.displayMetrics.density
    private var downY = 0f
    private var downV = 0f
    private val gestures = GestureDetector(ctx, object : GestureDetector.SimpleOnGestureListener() {
        override fun onDoubleTap(e: MotionEvent): Boolean { set(defProvider?.invoke() ?: def); refresh(); return true }
    })

    init {
        textSize = 13f
        setTextColor(Hw.LCD_FG)
        typeface = Hw.MONO
        background = Hw.lcdBg(ctx)
        gravity = Gravity.CENTER
        setPadding((4 * d).toInt(), (6 * d).toInt(), (4 * d).toInt(), (6 * d).toInt())
        maxLines = 1
        refresh()
    }

    fun refresh() { text = format(get()) }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        gestures.onTouchEvent(e)
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> { parent?.requestDisallowInterceptTouchEvent(true); downY = e.y; downV = get() }
            MotionEvent.ACTION_MOVE -> {
                val steps = ((downY - e.y) / (10 * d)).toInt()
                val v = (downV + steps * step).coerceIn(min, max)
                if (v != get()) { set(v); refresh() }
            }
        }
        return true
    }
}

/**
 * MULTI (tab 1): workstation-style zone view on the Obsidian surface.
 * 16 parts (sound, MIDI channel, key range, transpose, level, pan, mute, effect send), the part
 * being edited and 8 scenes (whole multis). Parts on the same channel layer; key ranges split.
 *
 * Layout: one compact control bar (MULTI switch, 4 / 8 / 16 zone density + page, export / import),
 * a scene strip, an 88-key range banner and a switchable zone area that never scrolls.
 */
class MultiPanel(private val a: MainActivity) {
    companion object {
        const val REQ_SETUP_EXPORT = 120
        const val REQ_SETUP_IMPORT = 121
        private const val PREF_DENSITY = "multiZoneDensity"
        private const val PREF_PAGE = "multiZonePage"
        private const val SCENES = 8
    }

    val view: LinearLayout
    /** Fired after a scene was recalled or stored (Stage scene strip sync). */
    var onSceneChanged: (() -> Unit)? = null
    val currentSceneIndex: Int get() = currentScene

    private val d = { v: Int -> Ui.dp(a, v) }
    private val portrait = Ui.portrait(a)
    private val sceneButtons = mutableListOf<LedButton>()
    private val names = Array(SCENES) { "" }
    private var storeMode = false
    private val sceneDir = File(a.filesDir, "scenes").apply { mkdirs() }
    private var currentScene = -1
    private val multiSwitch: ToggleSwitch
    private val storeBtn: LedButton

    private var density = a.prefs.getInt(PREF_DENSITY, 4).let { if (it == 8 || it == 16) it else 4 }
    private var page = a.prefs.getInt(PREF_PAGE, 0).coerceIn(0, 16 / density - 1)
    private val densityButtons = mutableListOf<Pair<Int, LedButton>>()
    private val pageButtons = mutableListOf<LedButton>()
    private val banner = KeyRangeBannerView(a)
    private val zones4 = mutableListOf<Zone4>()
    private val zones8 = mutableListOf<Zone8>()
    private val cells16 = mutableListOf<Cell16>()
    private val grid4: LinearLayout
    private val grid8: LinearLayout
    private val grid16: LinearLayout

    private val noteNames = arrayOf("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
    private fun note(v: Float) = v.roundToInt().let { noteNames[it % 12] + (it / 12 - 1) }
    private fun rangeText(k: Int): String {
        val lo = mix(k, 4).roundToInt()
        val hi = mix(k, 5).roundToInt()
        val base = if (lo <= 0 && hi >= 127) "FULL" else "${note(lo.toFloat())}–${note(hi.toFloat())}"
        val fl = mix(k, 8).roundToInt()
        val fh = mix(k, 9).roundToInt()
        // B3: E / B badge for external / both zones
        val rt = when (mix(k, 10).roundToInt()) { 1 -> " E"; 2 -> " B"; else -> "" }
        return (if (fl > 0 || fh > 0) "$base ◢$fl◣$fh" else base) + rt
    }

    // ------------------------------------------------------------ B3 external MIDI routing

    private val extSheet: ExtZoneSheet by lazy { ExtZoneSheet(a).also { it.onChanged = { refresh() } } }

    /** Long-press of a route pill / zone card: the external-MIDI sheet for part [k]. */
    private fun openExtZone(k: Int) {
        extSheet.bind(k)
        a.openSheet(extSheet)
    }

    /** `INT` cyan / `EXT` amber / `BOTH` violet. */
    private fun routeStyle(c: Chip, mode: Int) {
        when (mode) {
            1 -> { c.text = "EXT"; c.style(Hw.withAlpha(Hw.MOD_FILTER, 36), Hw.withAlpha(Hw.MOD_FILTER, 170), Hw.MOD_FILTER) }
            2 -> { c.text = "BOTH"; c.style(Hw.withAlpha(Hw.MOD_ENV, 36), Hw.withAlpha(Hw.MOD_ENV, 170), Hw.MOD_ENV) }
            else -> { c.text = "INT"; c.style(Hw.withAlpha(Hw.MOD_OSC, 36), Hw.withAlpha(Hw.MOD_OSC, 170), Hw.MOD_OSC) }
        }
    }

    private fun cycleRoute(k: Int) {
        val m = (mix(k, 10).roundToInt() + 1) % 3
        setMix(k, 10, m.toFloat())
        val port = mix(k, 11).roundToInt()
        Hw.onParamTouched?.invoke("P${k + 1} ROUTE", (arrayOf("INT", "EXT", "BOTH")[m]) + if (m != 0 && port == 0) "  (long-press: set OUT PORT)" else "")
        refresh()
    }

    init {
        // ---- row 1: MULTI switch, zone density + page, export / import
        multiSwitch = ToggleSwitch(a, "Multi", vertical = false).apply {
            onToggle = { on -> NativeEngine.nativeSetMulti(on); if (!on) a.setEditPart(0); refresh() }
        }
        for (n in intArrayOf(4, 8, 16)) {
            val b = compactButton("$n ZONES", led = true, accent = Hw.MOD_OSC) { setDensity(n) }
            densityButtons += n to b
        }
        for (pg in 0 until 4) pageButtons += compactButton("", led = false, accent = Hw.MOD_OSC) { setPage(pg) }
        val exportBtn = compactButton(L10n.t("Export"), led = false, accent = Hw.MOD_OSC) { exportSetup() }
        val importBtn = compactButton(L10n.t("Import"), led = false, accent = Hw.MOD_OSC) { importSetup() }

        val bar: ViewGroup
        val btnH = d(34)
        if (portrait) {
            bar = FlowLayout(a, d(6), d(4))
            bar.addView(multiSwitch, ViewGroup.LayoutParams(-2, -2))
            densityButtons.forEach { bar.addView(it.second, ViewGroup.LayoutParams(-2, btnH)) }
            pageButtons.forEach { bar.addView(it, ViewGroup.LayoutParams(-2, btnH)) }
            bar.addView(exportBtn, ViewGroup.LayoutParams(-2, btnH))
            bar.addView(importBtn, ViewGroup.LayoutParams(-2, btnH))
        } else {
            bar = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
            fun lp(start: Int = 4) = LinearLayout.LayoutParams(-2, btnH).apply { marginStart = d(start) }
            bar.addView(multiSwitch, LinearLayout.LayoutParams(-2, -2))
            densityButtons.forEachIndexed { i, (_, b) -> bar.addView(b, lp(if (i == 0) 10 else 4)) }
            pageButtons.forEachIndexed { i, b -> bar.addView(b, lp(if (i == 0) 12 else 4)) }
            bar.addView(View(a), LinearLayout.LayoutParams(0, 1, 1f))
            bar.addView(exportBtn, lp())
            bar.addView(importBtn, lp())
        }

        // ---- row 2: scenes (always one tap away)
        val sceneRow = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        sceneRow.addView(Hw.engraved(a, "SCENE", 10f, tr = false, color = Hw.MOD_FX), LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(6) })
        for (i in 0 until SCENES) {
            val b = compactButton("${i + 1}", led = true, accent = Hw.MOD_FX) { sceneTapped(i) }
            sceneButtons += b
            sceneRow.addView(b, LinearLayout.LayoutParams(0, -1, 1f).apply { marginEnd = d(4) })
        }
        storeBtn = compactButton(L10n.t("Store"), led = true, accent = Hw.MOD_RED) {}
        storeBtn.setOnClickListener { storeMode = !storeMode; storeBtn.lit = storeMode }
        sceneRow.addView(storeBtn, LinearLayout.LayoutParams(-2, -1))

        // ---- row 3: key range banner
        banner.onPick = { k -> a.setEditPart(k); refresh() }

        // ---- row 4: zone views
        repeat(4) { zones4 += Zone4() }
        repeat(8) { zones8 += Zone8() }
        repeat(16) { cells16 += Cell16() }
        grid4 = grid(zones4.map { it.card }, if (portrait) 2 else 4)
        grid8 = grid(zones8.map { it.card }, if (portrait) 4 else 8)
        grid16 = grid(cells16.map { it.card }, if (portrait) 4 else 8)
        val zoneArea = FrameLayout(a).apply {
            addView(grid4, FrameLayout.LayoutParams(-1, -1))
            addView(grid8, FrameLayout.LayoutParams(-1, -1))
            addView(grid16, FrameLayout.LayoutParams(-1, -1))
        }

        view = Ui.column(a).apply {
            setBackgroundColor(Hw.CHASSIS)
            setPadding(d(8), d(6), d(8), d(8))
            addView(bar, LinearLayout.LayoutParams(-1, -2))
            addView(sceneRow, LinearLayout.LayoutParams(-1, d(32)).apply { topMargin = d(4) })
            addView(banner, LinearLayout.LayoutParams(-1, d(36)).apply { topMargin = d(5) })
            addView(zoneArea, LinearLayout.LayoutParams(-1, 0, 1f).apply { topMargin = d(5) })
        }
        loadSceneNames()
        updateDensityUi()
    }

    private fun mix(k: Int, id: Int) = NativeEngine.nativeGetPartMix(k, id)
    private fun setMix(k: Int, id: Int, v: Float) = NativeEngine.nativeSetPartMix(k, id, v)
    private fun isMuted(k: Int) = mix(k, 2) > 0.5f
    private fun partActive(k: Int) = NativeEngine.nativeIsMulti() || k == 0

    private fun compactButton(text: String, led: Boolean, accent: Int, onClick: () -> Unit) =
        LedButton(a, text, tr = false, led = led, accentColor = accent).apply {
            textSize = 12f
            minHeight = 0
            minimumHeight = 0
            setPadding(if (led) d(17) else d(9), 0, d(9), 0)
            setOnClickListener { onClick() }
        }

    /** Rows × cols grid of equally weighted cards. */
    private fun grid(cards: List<View>, cols: Int): LinearLayout {
        val g = LinearLayout(a).apply { orientation = LinearLayout.VERTICAL }
        val rows = (cards.size + cols - 1) / cols
        for (r in 0 until rows) {
            val row = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL }
            for (c in 0 until cols) {
                val i = r * cols + c
                if (i >= cards.size) break
                row.addView(cards[i], LinearLayout.LayoutParams(0, -1, 1f).apply { if (c < cols - 1) marginEnd = d(4) })
            }
            g.addView(row, LinearLayout.LayoutParams(-1, 0, 1f).apply { if (r < rows - 1) bottomMargin = d(4) })
        }
        return g
    }

    // ------------------------------------------------------------ density / pages

    private fun pages() = 16 / density

    private fun setDensity(n: Int) {
        density = n
        page = (NativeEngine.nativeEditPart() / density).coerceIn(0, pages() - 1)
        a.prefs.edit().putInt(PREF_DENSITY, density).putInt(PREF_PAGE, page).apply()
        refresh()
    }

    private fun setPage(p: Int) {
        page = p.coerceIn(0, pages() - 1)
        a.prefs.edit().putInt(PREF_PAGE, page).apply()
        refresh()
    }

    private fun updateDensityUi() {
        densityButtons.forEach { (n, b) -> if (b.lit != (n == density)) b.lit = n == density }
        val np = pages()
        pageButtons.forEachIndexed { i, b ->
            if (np <= 1 || i >= np) { b.visibility = View.GONE; return@forEachIndexed }
            b.visibility = View.VISIBLE
            val t = "${i * density + 1}–${(i + 1) * density}"
            if (b.text.toString() != t) b.text = t
            if (b.lit != (i == page)) b.lit = i == page
        }
        grid4.visibility = if (density == 4) View.VISIBLE else View.GONE
        grid8.visibility = if (density == 8) View.VISIBLE else View.GONE
        grid16.visibility = if (density == 16) View.VISIBLE else View.GONE
    }

    private fun updateBanner() {
        val first = if (density == 16) 0 else page * density
        val count = if (density == 16) 16 else density
        val edit = NativeEngine.nativeEditPart()
        banner.begin()
        for (k in first until first + count) {
            if (!partActive(k)) continue
            if (k > 0 && a.partNames[k].isEmpty()) continue
            banner.add(k, mix(k, 4).roundToInt(), mix(k, 5).roundToInt(), isMuted(k), mix(k, 8).roundToInt(), mix(k, 9).roundToInt(),
                mix(k, 10).roundToInt())
        }
        banner.end(edit)
    }

    // ------------------------------------------------------------ zone views

    private fun editBadgeStyle(c: Chip, k: Int, sel: Boolean) {
        val col = Hw.partColor(k)
        if (sel) c.style(Hw.withAlpha(col, 70), col, Color.WHITE) else c.style(Hw.KEY, Hw.withAlpha(col, 110), col)
    }

    private fun muteStyle(c: Chip, muted: Boolean, showOn: Boolean, short: Boolean = false) {
        when {
            muted -> { c.text = if (short) "M" else "MUTE"; c.style(Hw.withAlpha(Hw.MOD_RED, 60), Hw.MOD_RED, Color.WHITE) }
            showOn -> { c.text = "ON"; c.style(Hw.withAlpha(Hw.MOD_FX, 36), Hw.withAlpha(Hw.MOD_FX, 170), Hw.MOD_FX) }
            else -> { c.text = if (short) "M" else "MUTE"; c.style(Hw.KEY, Hw.PANEL_EDGE, Hw.INK_DIM) }
        }
    }

    private fun toggleMute(k: Int) {
        setMix(k, 2, if (isMuted(k)) 0f else 1f)
        refresh()
    }

    private fun soundLcd(size: Float, lines: Int) = TextView(a).apply {
        textSize = size
        typeface = Hw.BOLD
        setTextColor(Hw.SILK)
        background = Hw.lcdBg(a)
        maxLines = lines
        ellipsize = TextUtils.TruncateAt.END
        gravity = Gravity.CENTER_VERTICAL
        setPadding(d(6), 0, d(4), 0)
    }

    private fun smallNum(id: Int, minV: Float, maxV: Float, stepV: Float, def: Float, part: () -> Int, fmt: (Float) -> String) =
        DragNumber(a, minV, maxV, stepV, def, fmt, { mix(part(), id) }, { setMix(part(), id, it); if (id == 4 || id == 5 || id == 8 || id == 9) updateBanner() }).apply {
            textSize = 11.5f
            setPadding(d(2), d(2), d(2), d(2))
        }

    private fun fmtVol(v: Float) = "${(v * 100).roundToInt()}"
    private fun fmtPan(v: Float) = (v * 50).roundToInt().let { if (it == 0) "C" else if (it < 0) "L${-it}" else "R$it" }
    private fun fmtTrn(v: Float) = v.roundToInt().let { if (it > 0) "+$it" else "$it" }

    /** 4-zone strip: badge / CH / MUTE header, sound, tall level fader + LOW HIGH TRN PAN FX column. */
    private inner class Zone4 {
        var part = 0
        val card = ZoneCard(a)
        private val badge = Chip(a, 12f)
        private val ch = smallNum(3, 1f, 16f, 1f, 1f, { part }) { "CH ${it.roundToInt()}" }.apply { defProvider = { (part + 1).toFloat() } }
        private val mute = Chip(a, 11f)
        private val route = Chip(a, 11f)
        private val sound = soundLcd(12.5f, 1)
        private val fader = SegmentedFader(a, "LEVEL", Hw.MOD_OSC)
        private val nums: List<DragNumber>

        init {
            card.setPadding(d(6), d(9), d(6), d(6))
            badge.setOnClickListener { a.setEditPart(part); refresh() }
            mute.setOnClickListener { toggleMute(part) }
            sound.setOnClickListener { choosePreset(part) }
            val head = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
            head.addView(badge, LinearLayout.LayoutParams(d(40), -1).apply { marginEnd = d(4) })
            head.addView(ch, LinearLayout.LayoutParams(0, -1, 1f).apply { marginEnd = d(4) })
            head.addView(mute, LinearLayout.LayoutParams(d(50), -1))
            card.addView(head, LinearLayout.LayoutParams(-1, d(28)))
            card.addView(sound, LinearLayout.LayoutParams(-1, d(28)).apply { topMargin = d(4) })

            fader.onChange = { v ->
                setMix(part, 0, v)
                fader.valueText = fmtVol(v)
                Hw.onParamTouched?.invoke("P${part + 1} LEVEL", fader.valueText)
            }
            fader.onReset = { setMix(part, 0, 1f); fader.value = 1f; fader.valueText = fmtVol(1f) }

            // S2: 56 dp fader + a 2 x 4 cell grid (11 dp engraved label over a DragNumber), no extra height:
            //   LOW  HIGH / XF L  XF H (0-24 st crossfade) / TRN  PAN / FX  -
            val body = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL }
            body.addView(fader, LinearLayout.LayoutParams(d(56), -1))
            val grid = LinearLayout(a).apply { orientation = LinearLayout.VERTICAL }
            val made = ArrayList<DragNumber>()
            fun cell(lbl: String, n: DragNumber?): View = LinearLayout(a).apply {
                orientation = LinearLayout.VERTICAL
                if (n != null) {
                    addView(Hw.engraved(a, lbl, 9f, tr = false), LinearLayout.LayoutParams(-1, d(11)))
                    addView(n, LinearLayout.LayoutParams(-1, 0, 1f))
                    made += n
                }
            }
            fun row(l: View, r: View) = LinearLayout(a).apply {
                orientation = LinearLayout.HORIZONTAL
                addView(l, LinearLayout.LayoutParams(0, -1, 1f).apply { marginEnd = d(3) })
                addView(r, LinearLayout.LayoutParams(0, -1, 1f))
            }
            // B3: ROUTE pill — tap cycles INT / EXT / BOTH (Mix id 10), long-press opens the external-MIDI sheet
            fun routeCell(): View = LinearLayout(a).apply {
                orientation = LinearLayout.VERTICAL
                addView(Hw.engraved(a, "ROUTE", 9f, tr = false), LinearLayout.LayoutParams(-1, d(11)))
                addView(route, LinearLayout.LayoutParams(-1, 0, 1f))
            }
            route.setOnClickListener { cycleRoute(part) }
            route.setOnLongClickListener { openExtZone(part); true }
            val rows = listOf(
                row(cell("LOW", smallNum(4, 0f, 127f, 1f, 0f, { part }) { note(it) }),
                    cell("HIGH", smallNum(5, 0f, 127f, 1f, 127f, { part }) { note(it) })),
                row(cell("XF L", smallNum(8, 0f, 24f, 1f, 0f, { part }) { "${it.roundToInt()}st" }),
                    cell("XF H", smallNum(9, 0f, 24f, 1f, 0f, { part }) { "${it.roundToInt()}st" })),
                row(cell("TRN", smallNum(6, -48f, 48f, 1f, 0f, { part }) { fmtTrn(it) }),
                    cell("PAN", smallNum(1, -1f, 1f, 0.05f, 0f, { part }) { fmtPan(it) })),
                row(cell("FX", smallNum(7, 0f, 1f, 0.05f, 1f, { part }) { fmtVol(it) }), routeCell()),
            )
            rows.forEach { grid.addView(it, LinearLayout.LayoutParams(-1, 0, 1f).apply { topMargin = d(1); bottomMargin = d(1) }) }
            nums = made
            body.addView(grid, LinearLayout.LayoutParams(0, -1, 1f).apply { marginStart = d(6) })
            card.addView(body, LinearLayout.LayoutParams(-1, 0, 1f).apply { topMargin = d(4) })
        }

        fun bind(k: Int, edit: Int) {
            part = k
            val col = Hw.partColor(k)
            val muted = isMuted(k)
            card.accent = col
            card.editSel = k == edit
            card.alpha = if (partActive(k)) 1f else 0.4f
            badge.text = "Z${k + 1}"
            editBadgeStyle(badge, k, k == edit)
            ch.refresh()
            muteStyle(mute, muted, showOn = false)
            routeStyle(route, mix(k, 10).roundToInt())
            sound.text = a.partNames[k].ifEmpty { "—" }
            fader.accent = col
            fader.muted = muted
            fader.value = mix(k, 0)
            fader.valueText = fmtVol(fader.value)
            nums.forEach { it.refresh() }
        }
    }

    /** 8-zone strip: badge, sound, compact level fader, CH / range read-out and ON / MUTE pill. */
    private inner class Zone8 {
        var part = 0
        val card = ZoneCard(a)
        private val badge = Chip(a, 11.5f)
        private val sound = soundLcd(10.5f, 2).apply { setPadding(d(4), 0, d(2), 0) }
        private val fader = SegmentedFader(a, "LEVEL", Hw.MOD_OSC, segments = 16, compact = true)
        private val ch = smallNum(3, 1f, 16f, 1f, 1f, { part }) { "CH ${it.roundToInt()}" }.apply {
            defProvider = { (part + 1).toFloat() }
            textSize = 10.5f
            setPadding(0, 0, 0, 0)
        }
        private val rng = TextView(a).apply {
            textSize = 9.5f
            typeface = Hw.MONO
            setTextColor(Hw.ENGRAVE)
            gravity = Gravity.CENTER
            maxLines = 1
            ellipsize = TextUtils.TruncateAt.END
        }
        private val mute = Chip(a, 10.5f)

        init {
            card.setPadding(d(4), d(8), d(4), d(4))
            card.setOnLongClickListener { openExtZone(part); true }   // B3: long-press the card = external-MIDI sheet
            badge.setOnClickListener { a.setEditPart(part); refresh() }
            sound.setOnClickListener { choosePreset(part) }
            mute.setOnClickListener { toggleMute(part) }
            fader.onChange = { v ->
                setMix(part, 0, v)
                fader.valueText = fmtVol(v)
                Hw.onParamTouched?.invoke("P${part + 1} LEVEL", fader.valueText)
            }
            fader.onReset = { setMix(part, 0, 1f); fader.value = 1f; fader.valueText = fmtVol(1f) }
            card.addView(badge, LinearLayout.LayoutParams(-1, d(24)))
            card.addView(sound, LinearLayout.LayoutParams(-1, d(30)).apply { topMargin = d(3) })
            card.addView(fader, LinearLayout.LayoutParams(-1, 0, 1f).apply { topMargin = d(2) })
            card.addView(ch, LinearLayout.LayoutParams(-1, d(20)))
            card.addView(rng, LinearLayout.LayoutParams(-1, d(15)))
            card.addView(mute, LinearLayout.LayoutParams(-1, d(24)).apply { topMargin = d(2) })
        }

        fun bind(k: Int, edit: Int) {
            part = k
            val col = Hw.partColor(k)
            val muted = isMuted(k)
            card.accent = col
            card.editSel = k == edit
            card.alpha = if (partActive(k)) 1f else 0.4f
            badge.text = "Z${k + 1}"
            editBadgeStyle(badge, k, k == edit)
            sound.text = a.partNames[k].ifEmpty { "—" }
            fader.accent = col
            fader.muted = muted
            fader.value = mix(k, 0)
            fader.valueText = fmtVol(fader.value)
            ch.refresh()
            rng.text = rangeText(k)
            muteStyle(mute, muted, showOn = true)
        }
    }

    /** 16-zone cell: P# badge + M pill, sound, draggable horizontal level bar, CH / range read-out. */
    private inner class Cell16(val part: Int = cells16.size) {
        val card = ZoneCard(a)
        private val badge = Chip(a, 11f)
        private val mute = Chip(a, 10.5f)
        private val sound = soundLcd(10.5f, 2).apply { setPadding(d(4), 0, d(2), 0) }
        private val level = HSegBar(a, Hw.MOD_OSC, 12, 22f)
        private val info = TextView(a).apply {
            textSize = 9.5f
            typeface = Hw.MONO
            setTextColor(Hw.ENGRAVE)
            maxLines = 1
            ellipsize = TextUtils.TruncateAt.END
            gravity = Gravity.CENTER_VERTICAL
        }

        init {
            card.setPadding(d(4), d(7), d(4), d(4))
            card.setOnLongClickListener { openExtZone(part); true }   // B3: long-press the card = external-MIDI sheet
            badge.text = "P${part + 1}"
            badge.setOnClickListener { a.setEditPart(part); refresh() }
            mute.setOnClickListener { toggleMute(part) }
            sound.setOnClickListener { choosePreset(part) }
            level.onChange = { v ->
                setMix(part, 0, v)
                level.valueText = fmtVol(v)
                Hw.onParamTouched?.invoke("P${part + 1} LEVEL", level.valueText)
            }
            level.onReset = { setMix(part, 0, 1f); level.value = 1f; level.valueText = fmtVol(1f) }
            val head = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL }
            head.addView(badge, LinearLayout.LayoutParams(0, -1, 1f).apply { marginEnd = d(3) })
            head.addView(mute, LinearLayout.LayoutParams(d(26), -1))
            card.addView(head, LinearLayout.LayoutParams(-1, d(22)))
            card.addView(sound, LinearLayout.LayoutParams(-1, 0, 1f).apply { topMargin = d(3) })
            card.addView(level, LinearLayout.LayoutParams(-1, d(20)).apply { topMargin = d(3) })
            card.addView(info, LinearLayout.LayoutParams(-1, d(14)).apply { topMargin = d(2) })
        }

        fun bind(edit: Int) {
            val col = Hw.partColor(part)
            val muted = isMuted(part)
            card.accent = col
            card.editSel = part == edit
            card.alpha = if (partActive(part)) 1f else 0.4f
            editBadgeStyle(badge, part, part == edit)
            muteStyle(mute, muted, showOn = false, short = true)
            sound.text = a.partNames[part].ifEmpty { "—" }
            level.accent = col
            level.muted = muted
            level.value = mix(part, 0)
            level.valueText = fmtVol(level.value)
            info.text = "CH${mix(part, 3).roundToInt()} · ${rangeText(part)}"
        }
    }

    private fun choosePreset(k: Int) {
        val list = a.libraryPanel.presetEntries()
        AlertDialog.Builder(a).setTitle(L10n.f("Sound for part %d", k + 1))
            .setItems(list.map { it.title }.toTypedArray()) { _, w ->
                if (!NativeEngine.nativeIsMulti() && k > 0) { NativeEngine.nativeSetMulti(true) }
                a.setEditPart(k)
                a.loadPreset(list[w].file) { refresh() }
            }.show()
    }

    // ------------------------------------------------------------ scenes

    private fun sceneFile(i: Int) = File(sceneDir, "scene${i + 1}.json")
    private fun sceneMetaFile(i: Int) = File(sceneDir, "scene${i + 1}.names.json")

    fun sceneLabel(i: Int): String = names.getOrNull(i)?.takeIf { it.isNotEmpty() } ?: "SC ${i + 1}"

    fun sceneExists(i: Int): Boolean = i in 0 until SCENES && sceneFile(i).exists()

    private fun loadSceneNames() {
        for (i in 0 until SCENES) {
            names[i] = runCatching { JSONObject(sceneMetaFile(i).readText()).optString("name") }.getOrDefault("")
        }
        updateScenes()
    }

    private fun updateScenes() {
        sceneButtons.forEachIndexed { i, b ->
            b.text = if (names[i].isNotEmpty()) "${i + 1} ${names[i]}" else "${i + 1}"
            if (b.lit != (i == currentScene)) b.lit = i == currentScene
            b.alpha = if (sceneExists(i)) 1f else 0.5f
        }
    }

    private fun sceneTapped(i: Int) {
        if (storeMode) storeScene(i) else recallScene(i)
    }

    private fun storeScene(i: Int) {
        val file = sceneFile(i)
        val input = EditText(a).apply { setText(names[i].ifEmpty { "Scene ${i + 1}" }); setSelectAllOnFocus(true) }
        AlertDialog.Builder(a).setTitle(L10n.f("Store scene %d", i + 1)).setView(input)
            .setPositiveButton(L10n.t("Save")) { _, _ ->
                val name = input.text.toString().trim().ifEmpty { "Scene ${i + 1}" }
                NativeEngine.nativeSetMulti(true)
                val err = NativeEngine.nativeSaveMulti(file.absolutePath, name, a.library.libraryRoot.absolutePath)
                if (err.isNotEmpty()) { a.log("scene save failed: $err", Ui.ERR); return@setPositiveButton }
                sceneMetaFile(i).writeText(JSONObject().put("name", name)
                    .put("parts", JSONArray(a.partNames.toList())).toString())
                names[i] = name
                currentScene = i
                storeMode = false
                storeBtn.lit = false
                a.log("scene ${i + 1} stored")
                updateScenes()
                refresh()
                onSceneChanged?.invoke()
            }.setNegativeButton(L10n.t("Cancel"), null).show()
    }

    /** Recalls scene [i] (whole multi) if it was stored; updates Multi + Stage when loaded. */
    fun recallScene(i: Int) {
        if (!sceneExists(i)) {
            a.log("scene ${i + 1} is empty", Ui.WARN)
            return
        }
        val file = sceneFile(i)
        SstSweep.start()
        a.loader.execute {
            val r = JSONObject(NativeEngine.nativeLoadMulti(file.absolutePath, a.library.libraryRoot.absolutePath))
            a.runOnUiThread {
                if (!r.optBoolean("ok")) { a.log("scene load failed: ${r.optString("error")}", Ui.ERR); return@runOnUiThread }
                runCatching {
                    val p = JSONObject(sceneMetaFile(i).readText()).optJSONArray("parts")
                    if (p != null) for (k in 0 until minOf(16, p.length())) a.partNames[k] = p.getString(k)
                }
                currentScene = i
                a.setEditPart(NativeEngine.nativeEditPart())
                a.log("scene ${i + 1} recalled")
                updateScenes()
                refresh()
                onSceneChanged?.invoke()
            }
        }
    }

    /**
     * Footswitch / host action: recall the next ([delta] > 0) or previous existing scene, wrapping around.
     * Starts from the current scene (or, when none is active, before the first / after the last). No-op without stored scenes.
     */
    fun stepScene(delta: Int) {
        if (delta == 0) return
        val dir = if (delta > 0) 1 else -1
        var i = if (currentScene in 0 until SCENES) currentScene else if (dir > 0) -1 else SCENES
        for (k in 0 until SCENES) {
            i = ((i + dir) % SCENES + SCENES) % SCENES
            if (sceneExists(i)) { recallScene(i); return }
        }
        a.log("no stored scenes to step through", Ui.WARN)
    }

    // ------------------------------------------------------------ setup files

    private fun exportSetup() {
        val intent = android.content.Intent(android.content.Intent.ACTION_CREATE_DOCUMENT).apply {
            addCategory(android.content.Intent.CATEGORY_OPENABLE)
            type = "application/json"
            putExtra(android.content.Intent.EXTRA_TITLE, "setup.json")
        }
        @Suppress("DEPRECATION")
        a.startActivityForResult(intent, REQ_SETUP_EXPORT)
    }

    private fun importSetup() {
        val intent = android.content.Intent(android.content.Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(android.content.Intent.CATEGORY_OPENABLE)
            type = "*/*"
            putExtra(android.content.Intent.EXTRA_MIME_TYPES, arrayOf("application/json", "text/plain", "application/octet-stream"))
        }
        @Suppress("DEPRECATION")
        a.startActivityForResult(intent, REQ_SETUP_IMPORT)
    }

    private fun hostSettings() = JSONObject().apply {
        put("voices", a.prefs.getInt("voices", 256))
        put("quality", a.prefs.getInt("quality", 1))
        put("mpe", a.prefs.getBoolean("mpe", false))
        put("sstOn", a.prefs.getBoolean("sstOn", true))
        put("sstMs", a.prefs.getInt("sstMs", 300))
    }

    private fun applyHostSettings(h: JSONObject) {
        val e = a.prefs.edit()
        if (h.has("voices")) h.optInt("voices", 256).coerceIn(8, 256).let { NativeEngine.nativeSetMaxVoices(it); e.putInt("voices", it) }
        if (h.has("quality")) h.optInt("quality", 1).coerceIn(0, 2).let { NativeEngine.nativeSetQuality(it); e.putInt("quality", it) }
        if (h.has("mpe")) h.optBoolean("mpe").let { NativeEngine.nativeSetMpe(it); e.putBoolean("mpe", it) }
        if (h.has("sstOn") || h.has("sstMs")) {
            val on = h.optBoolean("sstOn", a.prefs.getBoolean("sstOn", true))
            val ms = h.optInt("sstMs", a.prefs.getInt("sstMs", 300)).coerceIn(20, 2000)
            e.putBoolean("sstOn", on).putInt("sstMs", ms)
            SstSweep.applySettings(on, ms)
        }
        e.apply()
    }

    fun onActivityResult(requestCode: Int, data: android.content.Intent?) {
        val u = data?.data ?: return
        val root = a.library.libraryRoot.absolutePath
        when (requestCode) {
            REQ_SETUP_EXPORT -> {
                val name = a.library.displayName(u).substringBeforeLast('.').ifEmpty { "Setup" }
                val json = NativeEngine.nativeSetupJson(name, root, hostSettings().toString(), JSONArray(a.partNames.toList()).toString())
                a.loader.execute {
                    try {
                        a.library.exportText(u, json)
                        a.ui.post { a.log(L10n.t("setup exported")) }
                    } catch (e: Exception) {
                        a.ui.post { a.log("export failed: ${e.message}", Ui.ERR) }
                    }
                }
            }
            REQ_SETUP_IMPORT -> a.loader.execute {
                val text = try {
                    a.contentResolver.openInputStream(u)?.use { it.readBytes().decodeToString() } ?: ""
                } catch (e: Exception) { "" }
                applySetup(text)
            }
        }
    }

    /** Applies a setup (multi state, MIDI learn map, host settings). Call on the loader thread. */
    fun applySetup(text: String) {
        val root = a.library.libraryRoot.absolutePath
        val r = JSONObject(NativeEngine.nativeSetupApply(text, root))
        a.runOnUiThread {
            if (!r.optBoolean("ok")) { a.log(L10n.f("setup import failed: %s", r.optString("error")), Ui.ERR); return@runOnUiThread }
            r.optJSONArray("warnings")?.let { w -> for (i in 0 until w.length()) a.log("  " + w.getString(i), Ui.WARN) }
            if (r.optBoolean("state")) {
                val p = r.optJSONArray("parts")
                if (p != null) for (k in 0 until minOf(16, p.length())) a.partNames[k] = p.getString(k)
                a.currentPresetName = a.partNames[0].ifEmpty { a.currentPresetName }
                a.setEditPart(NativeEngine.nativeEditPart())
            }
            r.optJSONObject("host")?.let { applyHostSettings(it) }
            if (r.optBoolean("midiLearn")) NativeEngine.nativeLearnSave(File(a.filesDir, "midi_map.json").absolutePath)
            a.log(L10n.f("setup \"%s\" imported", r.optString("name")))
            refresh()
            a.live.refresh()
        }
    }

    fun refresh() {
        multiSwitch.on = NativeEngine.nativeIsMulti()
        page = page.coerceIn(0, pages() - 1)
        updateDensityUi()
        val edit = NativeEngine.nativeEditPart()
        when (density) {
            4 -> zones4.forEachIndexed { j, z -> z.bind(page * 4 + j, edit) }
            8 -> zones8.forEachIndexed { j, z -> z.bind(page * 8 + j, edit) }
            else -> cells16.forEach { it.bind(edit) }
        }
        updateBanner()
        updateScenes()
    }

    // ============================================================ zone view widgets

    /** Obsidian zone card: panel surface, 3dp part-colour top bar, accent hairline when it is the edit part. */
    private class ZoneCard(ctx: Context) : LinearLayout(ctx) {
        var accent = Hw.MOD_OSC
            set(v) { if (field != v) { field = v; updateBg(); invalidate() } }
        var editSel = false
            set(v) { if (field != v) { field = v; updateBg(); invalidate() } }
        private val dd = resources.displayMetrics.density
        private val bg = Hw.panelBg(ctx, radiusDp = 8f)
        private val p = Paint(Paint.ANTI_ALIAS_FLAG)
        private val r = RectF()

        init {
            orientation = VERTICAL
            background = bg
            setWillNotDraw(false)
        }

        private fun updateBg() {
            bg.setColor(if (editSel) Hw.PANEL_RAISED else Hw.PANEL)
            bg.setStroke(max(1, (if (editSel) 1.5f * dd else dd).toInt()), if (editSel) accent else Hw.PANEL_EDGE)
        }

        override fun onDraw(c: Canvas) {
            super.onDraw(c)
            r.set(0f, 0f, width.toFloat(), height.toFloat())
            c.save()
            c.clipRect(0f, 0f, width.toFloat(), 3f * dd)
            p.color = accent
            c.drawRoundRect(r, 8f * dd, 8f * dd, p)
            c.restore()
            SstSweep.drawTopSweep(c, width.toFloat(), height.toFloat(), 8f * dd, dd)
        }

        override fun onAttachedToWindow() { super.onAttachedToWindow(); SstSweep.attach(this) }
        override fun onDetachedFromWindow() { SstSweep.detach(this); super.onDetachedFromWindow() }
    }

    /** Small rounded label / pill whose fill + hairline are restyled in place (no drawable churn). */
    private class Chip(ctx: Context, size: Float) : TextView(ctx) {
        private val dd = resources.displayMetrics.density
        private val bg = GradientDrawable().apply { cornerRadius = 6f * dd }

        init {
            textSize = size
            typeface = Hw.BOLD
            letterSpacing = 0.05f
            gravity = Gravity.CENTER
            maxLines = 1
            ellipsize = TextUtils.TruncateAt.END
            includeFontPadding = false
            setPadding((3 * dd).toInt(), 0, (3 * dd).toInt(), 0)
            background = bg
            isClickable = true
            style(Hw.KEY, Hw.PANEL_EDGE, Hw.SILK)
        }

        fun style(fill: Int, stroke: Int, ink: Int) {
            bg.setColor(fill)
            bg.setStroke(max(1, dd.toInt()), stroke)
            setTextColor(ink)
        }
    }

    /**
     * 88-key split / layer banner on a pure OLED well: C1..C7 ruler, one coloured bar per visible
     * active zone (KeyLow..KeyHigh), lane-packed so splits share a lane and layers stack.
     * Tap a bar to make that part the edit part. Zero allocations in onDraw().
     */
    private class KeyRangeBannerView(ctx: Context) : View(ctx) {
        var onPick: ((Int) -> Unit)? = null

        private val dd = resources.displayMetrics.density
        private val part = IntArray(16)
        private val low = IntArray(16)
        private val high = IntArray(16)
        private val lane = IntArray(16)
        private val muted = BooleanArray(16)
        private val fadeLo = IntArray(16)
        private val fadeHi = IntArray(16)
        /** B3 route per bar: 0 INT (filled), 1 EXT (outline only), 2 BOTH (filled + amber top edge). */
        private val route = IntArray(16)
        /** Equal-power ramp tables: rampGain[n][j] = sin((j + .5) / n * pi / 2) for an n-slice ramp (n = 1..12). */
        private val rampGain = Array(13) { n -> FloatArray(n) { j -> kotlin.math.sin((j + 0.5f) / n * (Math.PI / 2).toFloat()) } }
        private val laneEnd = IntArray(16)
        private val order = IntArray(16)
        private var n = 0
        private var lanes = 1
        private var edit = -1
        private val octaveLabels = Array(7) { "C${it + 1}" }
        private val partLabels = Array(16) { "P${it + 1}" }
        private val p = Paint(Paint.ANTI_ALIAS_FLAG)
        private val t = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Hw.MONO; textAlign = Paint.Align.CENTER }
        private val tb = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Hw.BOLD; textAlign = Paint.Align.LEFT }
        private val r = RectF()
        private var downX = 0f
        private var downY = 0f

        fun begin() { n = 0 }

        fun add(k: Int, lo: Int, hi: Int, isMuted: Boolean, fLo: Int = 0, fHi: Int = 0, routeMode: Int = 0) {
            if (n >= 16) return
            part[n] = k
            route[n] = routeMode.coerceIn(0, 2)
            fadeLo[n] = fLo.coerceIn(0, 24)
            fadeHi[n] = fHi.coerceIn(0, 24)
            low[n] = minOf(lo, hi).coerceIn(0, 127)
            high[n] = maxOf(lo, hi).coerceIn(0, 127)
            muted[n] = isMuted
            n++
        }

        fun end(editPart: Int) {
            edit = editPart
            // order by low key (insertion sort, n <= 16), then greedy lane packing
            for (i in 0 until n) order[i] = i
            for (i in 1 until n) {
                val v = order[i]
                var j = i - 1
                while (j >= 0 && low[order[j]] > low[v]) { order[j + 1] = order[j]; j-- }
                order[j + 1] = v
            }
            lanes = 0
            for (oi in 0 until n) {
                val i = order[oi]
                var l = 0
                while (l < lanes && laneEnd[l] >= low[i]) l++
                if (l == lanes) lanes++
                lane[i] = l
                laneEnd[l] = high[i]
            }
            if (lanes == 0) lanes = 1
            invalidate()
        }

        private fun keyX(note: Float, x0: Float, w: Float) = x0 + (note - 21f) / 88f * w

        @SuppressLint("ClickableViewAccessibility")
        override fun onTouchEvent(e: MotionEvent): Boolean {
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN -> { downX = e.x; downY = e.y }
                MotionEvent.ACTION_UP -> {
                    if (abs(e.x - downX) > 10f * dd || abs(e.y - downY) > 10f * dd) return true
                    val x0 = 4f * dd
                    val w = width - 8f * dd
                    val top = 13f * dd
                    val laneH = (height - top - 3f * dd) / lanes
                    val l = ((e.y - top) / laneH).toInt().coerceIn(0, lanes - 1)
                    val note = 21f + (e.x - x0) / w * 88f
                    for (i in 0 until n) {
                        if (lane[i] == l && note >= low[i] - 0.5f && note <= high[i] + 1.5f) { onPick?.invoke(part[i]); break }
                    }
                }
            }
            return true
        }

        /**
         * Stepped equal-power ramp straddling [edgeX] (centred: half the [fade] semitones outside, half inside),
         * rising toward the zone body ([rising] = low edge) or falling away from it (high edge). At most 12 slices,
         * one reused [Paint], no gradient, no Path.
         */
        private fun drawRamp(c: Canvas, edgeX: Float, fade: Int, rising: Boolean, y0: Float, y1: Float, col: Int, baseA: Int, x0: Float, kw: Float) {
            val ns = fade.coerceAtMost(12)
            val half = keyW(kw) * fade / 2f
            val sliceW = half * 2f / ns
            val minX = x0
            val maxX = x0 + kw
            for (j in 0 until ns) {
                val sx0 = (edgeX - half + j * sliceW).coerceIn(minX, maxX)
                val sx1 = (edgeX - half + (j + 1) * sliceW).coerceIn(minX, maxX)
                if (sx1 <= sx0) continue
                val g = if (rising) rampGain[ns][j] else rampGain[ns][ns - 1 - j]
                p.color = Hw.withAlpha(col, (baseA * g).toInt())
                r.set(sx0, y0, sx1, y1)
                c.drawRect(r, p)
            }
        }

        private fun keyW(kw: Float) = kw / 88f

        override fun onDraw(c: Canvas) {
            val w = width.toFloat()
            val h = height.toFloat()
            if (w <= 0f || h <= 0f) return
            val rad = 6f * dd
            r.set(0f, 0f, w, h)
            p.style = Paint.Style.FILL
            p.color = Hw.WELL
            c.drawRoundRect(r, rad, rad, p)
            p.style = Paint.Style.STROKE
            p.strokeWidth = dd
            p.color = Hw.PANEL_EDGE
            r.inset(dd / 2f, dd / 2f)
            c.drawRoundRect(r, rad, rad, p)
            p.style = Paint.Style.FILL

            val x0 = 4f * dd
            val kw = w - 8f * dd
            val top = 13f * dd
            val bottom = h - 3f * dd

            // ruler: octave lines + C1..C7 labels
            t.textSize = 8.5f * dd
            t.color = Hw.INK_DIM
            for (o in 0 until 7) {
                val x = keyX((24 + o * 12).toFloat(), x0, kw)
                p.color = Hw.LED_OFF
                c.drawRect(x, top - 2f * dd, x + dd, bottom, p)
                c.drawText(octaveLabels[o], x, 9.5f * dd, t)
            }

            // zone bars
            val laneH = (bottom - top) / lanes
            val gap = if (laneH > 4f * dd) 1.5f * dd else 0.5f * dd
            tb.textSize = (laneH * 0.62f).coerceIn(6f * dd, 10f * dd)
            for (i in 0 until n) {
                val k = part[i]
                val col = Hw.partColor(k)
                val lo = low[i].coerceIn(21, 108).toFloat()
                val hi = high[i].coerceIn(21, 108).toFloat()
                val bx0 = keyX(lo, x0, kw)
                val bx1 = keyX(hi + 1f, x0, kw).coerceAtLeast(bx0 + 2f * dd)
                val by0 = top + lane[i] * laneH + gap / 2f
                val by1 = by0 + laneH - gap
                val sel = k == edit
                val baseA = if (muted[i]) 40 else if (sel) 255 else 150
                val fl = fadeLo[i]
                val fh = fadeHi[i]
                // S2: the crossfade is centred on the zone edge (KeyLow - 0.5 / KeyHigh + 0.5), i.e. on bx0 / bx1:
                // the full-level body starts / ends half a fade inside, the stepped ramps straddle the edge.
                val inL = if (fl > 0) keyX(lo + fl / 2f, x0, kw) else bx0
                val inR = if (fh > 0) keyX(hi + 1f - fh / 2f, x0, kw) else bx1
                val rt = route[i]
                p.style = Paint.Style.FILL
                p.color = Hw.withAlpha(col, baseA)
                if (rt == 1) {
                    // EXT: outline only (the sound is not made here)
                    r.set(bx0, by0, bx1, by1)
                    p.style = Paint.Style.STROKE
                    p.strokeWidth = 1.5f * dd
                    r.inset(0.75f * dd, 0.75f * dd)
                    c.drawRoundRect(r, 2f * dd, 2f * dd, p)
                    p.style = Paint.Style.FILL
                } else {
                    if (fl == 0 && fh == 0) {
                        r.set(bx0, by0, bx1, by1)
                        c.drawRoundRect(r, 2f * dd, 2f * dd, p)
                    } else if (inR > inL) {
                        r.set(inL, by0, inR, by1)
                        c.drawRect(r, p)
                    }
                    if (fl > 0) drawRamp(c, bx0, fl, true, by0, by1, col, baseA, x0, kw)
                    if (fh > 0) drawRamp(c, bx1, fh, false, by0, by1, col, baseA, x0, kw)
                    if (rt == 2) {
                        // BOTH: amber top edge
                        p.color = Hw.withAlpha(Hw.MOD_FILTER, if (muted[i]) 90 else 255)
                        c.drawRect(bx0, by0, bx1, by0 + 2f * dd, p)
                    }
                }
                r.set(bx0, by0, bx1, by1)
                if (sel) {
                    p.style = Paint.Style.STROKE
                    p.strokeWidth = 1.2f * dd
                    p.color = Color.WHITE
                    c.drawRoundRect(r, 2f * dd, 2f * dd, p)
                    p.style = Paint.Style.FILL
                }
                if (laneH >= 9f * dd && bx1 - bx0 >= 20f * dd) {
                    tb.color = if (rt == 1) (if (muted[i]) Hw.INK_DIM else col) else if (sel) Color.BLACK else if (muted[i]) Hw.INK_DIM else Hw.CHASSIS
                    c.drawText(partLabels[k], bx0 + 3f * dd, (by0 + by1) / 2f + tb.textSize * 0.36f, tb)
                }
            }
        }
    }
}

// ============================================================================================== B3 external MIDI zone sheet

/**
 * `ExtZoneSheet` (CENTER 560 × 270): ROUTE INT / EXT / BOTH, OUT PORT (slot 1..4, names come from SYS > MIDI), OUT CH,
 * BANK MSB / LSB / PC (-1 = off), SEND NOW and EXT NOTES OFF. Everything is a part-mix item (ids 10..15), so scenes and
 * set-list slots persist it through the engine's multi file and re-send Bank / PC on recall.
 */
class ExtZoneSheet(private val a: MainActivity) : Sheet(Edge.CENTER, 560, 270) {
    override val view: View by lazy { build() }

    /** Fired after any value changed (MultiPanel re-binds its zones and the banner). */
    var onChanged: (() -> Unit)? = null

    private var part = 0
    private val d = a.resources.displayMetrics.density
    private fun dp(v: Int) = (v * d).toInt()

    private lateinit var title: TextView
    private lateinit var routeRow: SegRow
    private lateinit var portBox: LinearLayout
    private lateinit var chNum: DragNumber
    private lateinit var msbNum: DragNumber
    private lateinit var lsbNum: DragNumber
    private lateinit var pcNum: DragNumber
    private lateinit var status: TextView

    private fun mix(id: Int) = NativeEngine.nativeGetPartMix(part, id)
    private fun setMix(id: Int, v: Float) {
        NativeEngine.nativeSetPartMix(part, id, v)
        onChanged?.invoke()
    }

    fun bind(k: Int) {
        part = k.coerceIn(0, 15)
        if (::title.isInitialized) refreshUi()
    }

    override fun onShow() { refreshUi() }

    private fun dm(id: Int, min: Float, max: Float, def: Float, fmt: (Float) -> String) =
        DragNumber(a, min, max, 1f, def, fmt, { mix(id) }) { setMix(id, it) }.apply { textSize = 13f }

    private fun off(v: Float) = if (v < 0f) "—" else v.roundToInt().toString()

    private fun build(): View {
        title = Hw.engraved(a, "", 12f, tr = false, color = Hw.MOD_FILTER)
        val close = LedButton(a, "✕", tr = false, led = false, accentColor = Hw.MOD_RED).apply {
            textSize = 14f; minHeight = 0; minimumHeight = 0; setPadding(0, 0, 0, 0)
            setOnClickListener { dismiss() }
        }
        val header = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(title, LinearLayout.LayoutParams(0, -2, 1f))
            addView(close, LinearLayout.LayoutParams(dp(34), dp(28)))
        }

        routeRow = SegRow(a, arrayOf("INT", "EXT", "BOTH"), Hw.MOD_FILTER) { i -> setMix(10, i.toFloat()); routeRow.selected = i }
        portBox = LinearLayout(a)
        chNum = dm(12, 1f, 16f, 1f) { "${it.roundToInt()}" }
        msbNum = dm(13, -1f, 127f, -1f) { off(it) }
        lsbNum = dm(14, -1f, 127f, -1f) { off(it) }
        pcNum = dm(15, -1f, 127f, -1f) { off(it) }

        val sendNowBtn = LedButton(a, "SEND NOW", tr = false, led = false, accentColor = Hw.MOD_FILTER).apply {
            textSize = 12f; minHeight = 0; minimumHeight = 0; setPadding(0, 0, 0, 0)
            setOnClickListener { sendNow() }
        }
        val allOff = LedButton(a, "EXT NOTES OFF", tr = false, led = false, accentColor = Hw.MOD_RED).apply {
            textSize = 12f; minHeight = 0; minimumHeight = 0; setPadding(dp(8), 0, dp(8), 0)
            setOnClickListener { notesOff() }
        }
        status = Ui.text(a, "", 11f, Hw.ENGRAVE, tr = false).apply { maxLines = 2 }

        val rowRoute = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(Hw.engraved(a, "ROUTE", 10f, tr = false), LinearLayout.LayoutParams(dp(62), -2))
            addView(routeRow, LinearLayout.LayoutParams(dp(330), dp(36)))
        }
        val rowPort = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(Hw.engraved(a, "OUT PORT", 10f, tr = false), LinearLayout.LayoutParams(dp(62), -2))
            addView(portBox, LinearLayout.LayoutParams(0, dp(34), 1f).apply { marginEnd = dp(14) })
            addView(Hw.engraved(a, "OUT CH", 10f, tr = false), LinearLayout.LayoutParams(-2, -2).apply { marginEnd = dp(6) })
            addView(chNum, LinearLayout.LayoutParams(dp(65), dp(34)))
        }
        val rowBank = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(Hw.engraved(a, "BANK MSB", 10f, tr = false), LinearLayout.LayoutParams(dp(62), -2))
            addView(msbNum, LinearLayout.LayoutParams(dp(78), dp(34)).apply { marginEnd = dp(10) })
            addView(Hw.engraved(a, "LSB", 10f, tr = false), LinearLayout.LayoutParams(-2, -2).apply { marginEnd = dp(6) })
            addView(lsbNum, LinearLayout.LayoutParams(dp(78), dp(34)).apply { marginEnd = dp(10) })
            addView(Hw.engraved(a, "PC", 10f, tr = false), LinearLayout.LayoutParams(-2, -2).apply { marginEnd = dp(6) })
            addView(pcNum, LinearLayout.LayoutParams(dp(78), dp(34)))
            addView(View(a), LinearLayout.LayoutParams(0, 1, 1f))
            addView(sendNowBtn, LinearLayout.LayoutParams(dp(90), dp(34)))
        }
        val rowHint = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(Ui.text(a, "Bank / PC are sent whenever a scene or set-list slot is recalled.", 11f, Hw.INK_DIM),
                LinearLayout.LayoutParams(0, -2, 1f).apply { marginEnd = dp(8) })
            addView(allOff, LinearLayout.LayoutParams(-2, dp(34)))
        }
        return LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(14), dp(2), dp(14), dp(10))
            addView(header, LinearLayout.LayoutParams(-1, dp(30)))
            addView(rowRoute, LinearLayout.LayoutParams(-1, dp(44)))
            addView(rowPort, LinearLayout.LayoutParams(-1, dp(44)))
            addView(rowBank, LinearLayout.LayoutParams(-1, dp(44)))
            addView(rowHint, LinearLayout.LayoutParams(-1, dp(40)))
            addView(status, LinearLayout.LayoutParams(-1, -2))
        }
    }

    private fun refreshUi() {
        title.text = "Z${part + 1} · EXTERNAL MIDI"
        routeRow.selected = mix(10).roundToInt().coerceIn(0, 2)
        portBox.removeAllViews()
        val items = ArrayList<String>(5)
        items += L10n.t("(none)")
        for (s in 1..MidiOutput.SLOTS) {
            val n = a.prefs.getString("extPort$s", "") ?: ""
            items += "$s · " + n.ifEmpty { L10n.t("(not set — SYS › MIDI)") }
        }
        portBox.addView(Ui.spinner(a, items, mix(11).roundToInt().coerceIn(0, MidiOutput.SLOTS)) { i -> setMix(11, i.toFloat()) },
            LinearLayout.LayoutParams(-1, -1))
        chNum.refresh(); msbNum.refresh(); lsbNum.refresh(); pcNum.refresh()
        status.text = ""
    }

    private fun slot() = mix(11).roundToInt()

    /** CC0 / CC32 / PC on the zone's channel to the chosen slot, as separate short messages. */
    private fun sendNow() {
        val s = slot()
        if (s !in 1..MidiOutput.SLOTS) { status.text = L10n.t("Choose an OUT PORT first."); return }
        val ch = (mix(12).roundToInt().coerceIn(1, 16) - 1)
        val msb = mix(13).roundToInt()
        val lsb = mix(14).roundToInt()
        val pc = mix(15).roundToInt()
        var n = 0
        if (msb >= 0) { NativeEngine.nativeExtSend(s, byteArrayOf((0xB0 or ch).toByte(), 0, msb.toByte())); n++ }
        if (lsb >= 0) { NativeEngine.nativeExtSend(s, byteArrayOf((0xB0 or ch).toByte(), 32, lsb.toByte())); n++ }
        if (pc >= 0) { NativeEngine.nativeExtSend(s, byteArrayOf((0xC0 or ch).toByte(), pc.toByte())); n++ }
        status.text = if (n == 0) L10n.t("Nothing to send — set BANK / PC first.") else L10n.f("Sent %d message(s) to port %d, channel %d.", n, s, ch + 1)
    }

    private fun notesOff() {
        val s = slot()
        if (s in 1..MidiOutput.SLOTS) NativeEngine.nativeExtMidiAllOff(s)
        else for (k in 1..MidiOutput.SLOTS) NativeEngine.nativeExtMidiAllOff(k)
        status.text = L10n.t("External notes off sent.")
    }
}
