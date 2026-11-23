package com.hsynth.app

import android.annotation.SuppressLint
import android.app.AlertDialog
import android.util.SparseArray
import android.view.Gravity
import android.view.View
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import kotlin.math.abs
import kotlin.math.roundToInt

/**
 * Phase 3 A3 — State A↔B Morph and Nord-style 1-tap Morph Learn.
 *
 * Engine contract: `perf.morph` (0..1 position), `morph.source`, `morph.curve` are ordinary params; per-param routes
 * `(id, a, b)` live in the engine (`nativeMorph*`). [MorphUi] is the single Kotlin-side mirror: the learn flag, the
 * route table [ranges] (raw `[a, b]` per param id, filled by exactly ONE `nativeMorphDump()` per [refresh] — never per
 * knob) and a tiny listener list for the EDIT top bar / the sheet.
 */
object MorphUi {
    const val POS = "perf.morph"
    const val SOURCE = "morph.source"
    const val CURVE = "morph.curve"

    /** True while MORPH LEARN is on: touching a numeric knob assigns its end-point instead of changing the sound. */
    var learning = false
        private set

    /** Routes by param id → raw `[a, b]` (not normalised). Read by `ParamControl.refresh()` for the knob dual arc. */
    val ranges = SparseArray<FloatArray>()

    /** Number of assigned routes (after the last [refresh]). */
    val routeCount: Int get() = ranges.size()

    private val listeners = ArrayList<() -> Unit>()

    /** Listener fires after [refresh] / learn toggle / route changes. Cleared by `MainActivity.buildUi`. */
    fun addListener(l: () -> Unit) { if (!listeners.contains(l)) listeners += l }
    fun removeListener(l: () -> Unit) { listeners.remove(l) }
    fun clearListeners() = listeners.clear()
    private fun fire() { for (i in listeners.indices.reversed()) listeners.getOrNull(i)?.invoke() }

    /** One `nativeMorphDump()` → [ranges]. Call after a preset / part load and after any assign / clear. */
    fun refresh() {
        val dump = try { NativeEngine.nativeMorphDump() } catch (_: Throwable) { FloatArray(0) }
        ranges.clear()
        var i = 0
        while (i + 2 < dump.size) {
            ranges.put(dump[i].roundToInt(), floatArrayOf(dump[i + 1], dump[i + 2]))
            i += 3
        }
        fire()
    }

    /** Turn learning on / off. Turning it ON forces the morph position to A (0) so "current value = A" is exact. */
    fun setLearning(on: Boolean) {
        if (learning == on) return
        learning = on
        if (on && Params.metaOf(POS) != null) Params.set(POS, 0f)
        fire()
    }

    /** Which params learn? Numeric ones only; the morph's own controls and global performance settings never. */
    fun learnable(meta: ParamMeta): Boolean =
        (meta.kind == "float" || meta.kind == "int") && !meta.name.startsWith("morph.") && !meta.name.startsWith("perf.")

    /** Start value (A) of [meta]: its existing route's A, else [current] (the value at touch-down). */
    fun startOf(meta: ParamMeta, current: Float): Float = ranges.get(meta.id)?.get(0) ?: current

    /**
     * Assign / move the end-point of [meta]'s route to [bVal] with start [aVal]. Updates [ranges] locally (so knobs
     * follow the finger); callers [refresh] once when the gesture ends to pick up the engine's clamped values.
     */
    fun learnSet(meta: ParamMeta, aVal: Float, bVal: Float) {
        NativeEngine.nativeMorphAssign(meta.id, aVal, bVal)
        val r = ranges.get(meta.id)
        if (r != null) { r[0] = aVal; r[1] = bVal } else ranges.put(meta.id, floatArrayOf(aVal, bVal))
    }

    /** Remove one route and re-read the table. */
    fun clear(id: Int) {
        NativeEngine.nativeMorphClear(id)
        refresh()
    }

    fun clearAll() {
        NativeEngine.nativeMorphClearAll()
        refresh()
    }

    /** True when `morph.source` currently is the Super Knob. */
    fun sourceIsSuper(): Boolean {
        val m = Params.metaOf(SOURCE) ?: return false
        val idx = m.options.indexOfFirst { it.equals("super", ignoreCase = true) }
        return idx >= 0 && NativeEngine.nativeGetParam(m.id).roundToInt() == idx
    }

    /** Stage chip: bind / unbind the Super Knob to the morph position. */
    fun toggleSuperSource() {
        val m = Params.metaOf(SOURCE) ?: return
        val sup = m.options.indexOfFirst { it.equals("super", ignoreCase = true) }
        val man = m.options.indexOfFirst { it.equals("manual", ignoreCase = true) }.coerceAtLeast(0)
        if (sup < 0) return
        History.checkpoint()
        NativeEngine.setParam(m.id, (if (sourceIsSuper()) man else sup).toFloat())
        fire()
    }
}

// ====================================================================================================== sheet

/**
 * MORPH drawer: RIGHT, 340 dp. Header · `SET A / CAPTURE B / ⇄ SWAP` · `A ▸ morph slider ◂ B` (synced with
 * `perf.morph`) · source radio · curve radio · MORPH LEARN toggle + route count + `LIST` · info or scrolling route
 * list (name · A→B · ✕) · `CLEAR ALL` (confirm). Built once (lazily) and cached by `MainActivity.morphSheet`; its
 * only ticker (10 Hz slider sync) exists while the sheet is visible.
 */
class MorphSheet(private val a: MainActivity) : Sheet(Edge.RIGHT, 340) {
    override val view: View by lazy { build() }

    private val d = a.resources.displayMetrics.density
    private fun dp(v: Int) = (v * d).toInt()

    private val posMeta = Params.metaOf(MorphUi.POS)
    private val bar = HSegBar(a, Hw.MOD_ENV, 24, 40f)
    private lateinit var srcRow: SegRow
    private lateinit var curveRow: SegRow
    private lateinit var learnBtn: LedButton
    private lateinit var countText: TextView
    private lateinit var listBtn: LedButton
    private lateinit var info: TextView
    private lateinit var listScroll: ScrollView
    private lateinit var listBox: LinearLayout
    private lateinit var stateA: TextView
    private lateinit var stateB: TextView
    private var showList = false
    private var dragging = false
    private val byId: Map<Int, ParamMeta> by lazy { Params.meta.associateBy { it.id } }

    private val SRC_NAMES = arrayOf("manual", "super", "modwheel", "expression", "aftertouch")
    private val CURVE_NAMES = arrayOf("linear", "easein", "easeout", "s")

    private val changed: () -> Unit = { syncAll() }

    private val ticker = object : FrameTicker.Client {
        override val intervalMs: Int get() = 100
        override val tickView: View? get() = view
        override fun onFrame(nowMs: Long) { if (!dragging) syncPos() }
    }

    override fun onShow() {
        view
        MorphUi.refresh()
        MorphUi.addListener(changed)
        syncAll()
        FrameTicker.register(ticker)
    }

    override fun onHide() {
        FrameTicker.unregister(ticker)
        MorphUi.removeListener(changed)
        // knobs on the (covered) EDIT page may have changed morph position / routes meanwhile
        if (a.visible) a.ui.post { runCatching { a.edit.refreshValues() } }
    }

    // ------------------------------------------------------------------ build

    private fun chipText(t: String, color: Int, onClick: () -> Unit) = TextView(a).apply {
        text = t
        textSize = 12f
        typeface = Hw.BOLD
        gravity = Gravity.CENTER
        setTextColor(color)
        background = Hw.keyBg(a)
        isClickable = true
        setOnClickListener { onClick() }
    }

    private fun build(): View {
        val root = LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(10), dp(2), dp(10), dp(8))
        }

        // header
        val head = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        head.addView(Hw.engraved(a, "MORPH", 12f, tr = false, color = Hw.MOD_ENV), LinearLayout.LayoutParams(0, -2, 1f))
        head.addView(
            LedButton(a, L10n.t("AI 2D"), tr = false, led = false, accentColor = Hw.MOD_FX).apply {
                minHeight = 0; minimumHeight = 0; setPadding(dp(10), 0, dp(10), 0); textSize = 12f
                setOnClickListener { a.openSheet(a.aiMorphSheet) }   // replaces this drawer with the Edge TPU 2D pad
            }, LinearLayout.LayoutParams(-2, dp(28)).apply { marginEnd = dp(6) }
        )
        head.addView(
            LedButton(a, "✕", tr = false, led = false).apply {
                minHeight = 0; minimumHeight = 0; setPadding(0, 0, 0, 0)
                setOnClickListener { dismiss() }
            }, LinearLayout.LayoutParams(dp(36), dp(28))
        )
        root.addView(head, LinearLayout.LayoutParams(-1, dp(32)))

        // state: SET A / CAPTURE B / SWAP
        val state = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL }
        fun stateBtn(label: String, accent: Int, onClick: () -> Unit) = LedButton(a, label, tr = false, led = false, accentColor = accent).apply {
            textSize = 11.5f
            minHeight = 0; minimumHeight = 0
            setPadding(dp(2), 0, dp(2), 0)
            setOnClickListener { onClick() }
        }
        val setA = stateBtn("A ●  SET A", Hw.MOD_ENV) { setStateA() }
        val capB = stateBtn("B ○  CAPTURE B", Hw.MOD_ENV) { captureB() }
        val swap = stateBtn("⇄  SWAP", Hw.MOD_ENV) { swapAB() }
        state.addView(setA, LinearLayout.LayoutParams(0, -1, 1f).apply { marginEnd = dp(4) })
        state.addView(capB, LinearLayout.LayoutParams(0, -1, 1.15f).apply { marginEnd = dp(4) })
        state.addView(swap, LinearLayout.LayoutParams(0, -1, 0.9f))
        root.addView(state, LinearLayout.LayoutParams(-1, dp(44)).apply { topMargin = dp(4) })

        // slider row: [A] bar [B]  (tap A / B = audition)
        val slider = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        stateA = chipText("A", Hw.SILK) { setPos(0f) }
        stateB = chipText("B", Hw.SILK) { setPos(1f) }
        bar.onDragStart = { dragging = true; History.checkpoint() }
        bar.onDragEnd = { dragging = false }
        bar.onChange = { v -> writePos(v) }
        bar.onReset = { setPos(0f) }
        slider.addView(stateA, LinearLayout.LayoutParams(dp(30), dp(36)))
        slider.addView(bar, LinearLayout.LayoutParams(0, dp(36), 1f).apply { marginStart = dp(4); marginEnd = dp(4) })
        slider.addView(stateB, LinearLayout.LayoutParams(dp(30), dp(36)))
        if (posMeta == null) slider.alpha = 0.4f
        root.addView(slider, LinearLayout.LayoutParams(-1, dp(40)).apply { topMargin = dp(6) })

        // source
        root.addView(Hw.engraved(a, "Source", 9f), LinearLayout.LayoutParams(-2, dp(14)).apply { topMargin = dp(4) })
        srcRow = SegRow(a, arrayOf("MANUAL", "SUPER", "MOD", "EXPR", "AT"), Hw.MOD_ENV) { i -> pickEnum(MorphUi.SOURCE, SRC_NAMES, i) }
        root.addView(srcRow, LinearLayout.LayoutParams(-1, dp(32)))

        // curve
        root.addView(Hw.engraved(a, "Curve", 9f), LinearLayout.LayoutParams(-2, dp(14)).apply { topMargin = dp(4) })
        curveRow = SegRow(a, arrayOf("LIN", "EASE-IN", "EASE-OUT", "S"), Hw.MOD_ENV) { i -> pickEnum(MorphUi.CURVE, CURVE_NAMES, i) }
        root.addView(curveRow, LinearLayout.LayoutParams(-1, dp(30)))

        // learn row
        val learn = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        learnBtn = LedButton(a, "MORPH LEARN", tr = false, led = true, accentColor = Hw.MOD_ENV).apply {
            textSize = 11.5f
            minHeight = 0; minimumHeight = 0
            setPadding(dp(18), 0, dp(6), 0)
            setOnClickListener { MorphUi.setLearning(!MorphUi.learning) }
        }
        countText = Hw.engraved(a, "", 10f, tr = false, color = Hw.ENGRAVE).apply { gravity = Gravity.CENTER_VERTICAL }
        listBtn = LedButton(a, "LIST", tr = false, led = false, accentColor = Hw.MOD_ENV).apply {
            textSize = 11f
            minHeight = 0; minimumHeight = 0
            setPadding(0, 0, 0, 0)
            setOnClickListener { showList = !showList; syncAll() }
        }
        learn.addView(learnBtn, LinearLayout.LayoutParams(dp(134), -1))
        learn.addView(countText, LinearLayout.LayoutParams(0, -1, 1f).apply { marginStart = dp(8) })
        learn.addView(listBtn, LinearLayout.LayoutParams(dp(50), -1))
        root.addView(learn, LinearLayout.LayoutParams(-1, dp(38)).apply { topMargin = dp(8) })

        // info / list (flexible)
        val flex = android.widget.FrameLayout(a)
        info = Ui.text(a, "Touch a knob while LEARN is on to set its end-point (B). Its current value is A.", 11f, Hw.ENGRAVE).apply {
            maxLines = 5
        }
        flex.addView(info, android.widget.FrameLayout.LayoutParams(-1, -2, Gravity.TOP))
        listBox = LinearLayout(a).apply { orientation = LinearLayout.VERTICAL }
        listScroll = ScrollView(a).apply {
            isVerticalScrollBarEnabled = false
            visibility = View.GONE
            addView(listBox)
        }
        flex.addView(listScroll, android.widget.FrameLayout.LayoutParams(-1, -1))
        root.addView(flex, LinearLayout.LayoutParams(-1, 0, 1f).apply { topMargin = dp(6) })

        // footer
        root.addView(
            LedButton(a, "CLEAR ALL", tr = false, led = false, accentColor = Hw.MOD_RED).apply {
                textSize = 11.5f
                minHeight = 0; minimumHeight = 0
                setTextColor(Hw.MOD_RED)
                setOnClickListener { confirmClearAll() }
            }, LinearLayout.LayoutParams(-1, dp(34)).apply { topMargin = dp(6) }
        )
        return root
    }

    // ------------------------------------------------------------------ actions

    private fun setStateA() {
        History.checkpoint()
        NativeEngine.nativeMorphSetA()
        MorphUi.refresh()
        a.ui.post { runCatching { a.edit.refreshValues() } }
    }

    private fun captureB() {
        History.checkpoint()
        val n = NativeEngine.nativeMorphCaptureB()
        // the sound *is* B now: the slider jumps to 1.0
        if (posMeta != null) Params.set(MorphUi.POS, 1f)
        MorphUi.refresh()
        Hw.onParamTouched?.invoke("MORPH", "$n " + L10n.t("params"))
        a.ui.post { runCatching { a.edit.refreshValues() } }
    }

    /** SWAP: exchange every route's end-points and mirror the position, so the sound stays (for linear curves). */
    private fun swapAB() {
        val n = MorphUi.ranges.size()
        if (n == 0) return
        History.checkpoint()
        for (i in 0 until n) {
            val id = MorphUi.ranges.keyAt(i)
            val r = MorphUi.ranges.valueAt(i)
            NativeEngine.nativeMorphAssign(id, r[1], r[0])
        }
        if (posMeta != null) Params.set(MorphUi.POS, 1f - NativeEngine.nativeGetParam(posMeta.id))
        MorphUi.refresh()
        a.ui.post { runCatching { a.edit.refreshValues() } }
    }

    private fun confirmClearAll() {
        if (MorphUi.routeCount == 0) return
        AlertDialog.Builder(a)
            .setTitle(L10n.t("Clear all morph routes"))
            .setMessage(L10n.f("Remove all %d assigned morph routes? This cannot be undone.", MorphUi.routeCount))
            .setNegativeButton(android.R.string.cancel, null)
            .setPositiveButton(android.R.string.ok) { _, _ ->
                MorphUi.clearAll()
                a.ui.post { runCatching { a.edit.refreshValues() } }
            }
            .show()
    }

    private fun pickEnum(param: String, names: Array<String>, i: Int) {
        val m = Params.metaOf(param) ?: return
        var idx = m.options.indexOfFirst { it.equals(names[i], ignoreCase = true) }
        if (idx < 0) idx = i.coerceAtMost((m.options.size - 1).coerceAtLeast(0))
        History.checkpoint()
        NativeEngine.setParam(m.id, idx.toFloat())
        syncEnums()
    }

    private fun writePos(v: Float) {
        val m = posMeta ?: return
        NativeEngine.setParam(m.id, v)
        bar.valueText = "%.2f".format(v)
        Hw.onParamTouched?.invoke("MORPH", bar.valueText)
    }

    private fun setPos(v: Float) {
        val m = posMeta ?: return
        History.checkpoint()
        NativeEngine.setParam(m.id, v)
        bar.value = v
        bar.valueText = "%.2f".format(v)
        syncStateChips(v)
    }

    // ------------------------------------------------------------------ sync

    private fun syncPos() {
        val m = posMeta ?: return
        val v = NativeEngine.nativeGetParam(m.id)
        if (abs(v - bar.value) > 1e-4f) bar.value = v
        val t = "%.2f".format(v)
        if (t != bar.valueText) bar.valueText = t
        syncStateChips(v)
    }

    private fun syncStateChips(v: Float) {
        stateA.setTextColor(if (v < 0.5f) Hw.MOD_ENV else Hw.ENGRAVE)
        stateB.setTextColor(if (v >= 0.5f) Hw.MOD_ENV else Hw.ENGRAVE)
    }

    private fun enumIndexFor(param: String, names: Array<String>): Int {
        val m = Params.metaOf(param) ?: return -1
        val cur = m.options.getOrNull(NativeEngine.nativeGetParam(m.id).roundToInt()) ?: return -1
        return names.indexOfFirst { it.equals(cur, ignoreCase = true) }
    }

    private fun syncEnums() {
        srcRow.selected = enumIndexFor(MorphUi.SOURCE, SRC_NAMES)
        curveRow.selected = enumIndexFor(MorphUi.CURVE, CURVE_NAMES)
    }

    private fun syncAll() {
        syncPos()
        syncEnums()
        learnBtn.lit = MorphUi.learning
        val n = MorphUi.routeCount
        countText.text = L10n.f("%d params assigned", n)
        listBtn.lit = showList
        listScroll.visibility = if (showList) View.VISIBLE else View.GONE
        info.visibility = if (showList) View.GONE else View.VISIBLE
        if (showList) rebuildList()
    }

    private fun rebuildList() {
        listBox.removeAllViews()
        val n = MorphUi.ranges.size()
        if (n == 0) {
            listBox.addView(Ui.text(a, "No routes yet.", 11.5f, Hw.INK_DIM))
            return
        }
        // stable order: by param label
        val rows = ArrayList<Pair<ParamMeta?, Int>>(n)
        for (i in 0 until n) { val id = MorphUi.ranges.keyAt(i); rows += byId[id] to id }
        rows.sortBy { (it.first?.label ?: "~").lowercase() }
        for ((m, id) in rows) {
            val r = MorphUi.ranges.get(id) ?: continue
            val row = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
            val title = (m?.label ?: "#$id")
            val grp = m?.group?.let { if (it.length > 12) it.take(12) else it } ?: ""
            row.addView(
                Ui.text(a, if (grp.isEmpty()) title else "$title · $grp", 11f, Hw.SILK, tr = false).apply { maxLines = 1 },
                LinearLayout.LayoutParams(0, -2, 1f)
            )
            val av = m?.format(r[0]) ?: "%.2f".format(r[0])
            val bv = m?.format(r[1]) ?: "%.2f".format(r[1])
            row.addView(
                Ui.text(a, "$av→$bv", 10.5f, Hw.MOD_ENV, tr = false).apply { typeface = Hw.MONO; maxLines = 1 },
                LinearLayout.LayoutParams(-2, -2).apply { marginStart = dp(4) }
            )
            row.addView(
                TextView(a).apply {
                    text = "✕"
                    textSize = 13f
                    gravity = Gravity.CENTER
                    setTextColor(Hw.MOD_RED)
                    isClickable = true
                    setOnClickListener {
                        MorphUi.clear(id)
                        a.ui.post { runCatching { a.edit.refreshValues() } }
                    }
                }, LinearLayout.LayoutParams(dp(34), dp(30))
            )
            listBox.addView(row, LinearLayout.LayoutParams(-1, dp(30)))
        }
    }
}
