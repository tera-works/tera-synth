package com.hsynth.app

import android.app.AlertDialog
import android.view.Gravity
import android.view.View
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import kotlin.math.roundToInt

/**
 * Sound editor laid out like a synth's edit panels: section buttons along
 * the top, each page a stack of framed panels with knobs, LCD selectors and
 * LED switches. Generated from the engine's parameter metadata, so new engine
 * parameters appear without UI code changes.
 */
class EditPanel(private val a: MainActivity) {
    val view: LinearLayout
    private val content = Ui.column(a)
    private val pageButtons = mutableListOf<LedButton>()
    private val layerButtons = mutableListOf<LedButton>()
    private val layerBox: LinearLayout
    private var page = 0
    private var layer = 0
    private val controls = mutableListOf<ParamControl>()
    private val graphs = mutableListOf<View>()
    private var modMatrix: ModMatrixView? = null
    private var fmOp = 0

    private data class Page(val title: String, val groups: List<String>)

    private val pages = listOf(
        Page("OSC", emptyList()),  // layer common + the engine of the layer's type, built by buildOsc()
        Page("Filter", listOf("Filter", "Voice FX")),
        Page("Env", listOf("Amp Env", "Mod Env", "Env 3")),
        Page("LFO", listOf("LFO")),
        Page("Mod", listOf("Mod Matrix", "Macros", "Controllers")),
        Page("Arp / Seq", listOf("Arpeggiator", "Sequencer")),
        Page("Motion", listOf("MSEG", "Motion 1", "Motion 2", "Motion 3", "Motion 4")),
        Page("FX", emptyList()),   // chain editor, built by buildFx()
        Page("Voice", listOf("Voice", "Tuning", "Master")),
    )

    private fun ui(f: () -> Unit) = a.ui.post(f)

    init {
        val d = { v: Int -> Ui.dp(a, v) }
        val bar = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        pages.forEachIndexed { i, p ->
            val b = LedButton(a, p.title).apply { setOnClickListener { page = i; buildPage() } }
            pageButtons += b
            bar.addView(b, LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(6) })
        }
        layerBox = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(Hw.engraved(a, "Layer:", 12f), LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(6) })
        }
        for (l in 0..2) {
            val b = LedButton(a, "ABC"[l].toString(), tr = false).apply { setOnClickListener { layer = l; buildPage() } }
            layerButtons += b
            layerBox.addView(b, LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(6) })
        }
        // toolbar under the page buttons: layer selector (OSC) ... undo / redo / compare
        val toolbar = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(d(8), d(4), d(8), d(2))
        }
        toolbar.addView(layerBox, LinearLayout.LayoutParams(-2, -2))
        toolbar.addView(View(a), LinearLayout.LayoutParams(0, 1, 1f))
        val undoB = LedButton(a, "Undo", led = false).apply { textSize = 13f; setOnClickListener { History.undo() } }
        val redoB = LedButton(a, "Redo", led = false).apply { textSize = 13f; setOnClickListener { History.redo() } }
        val cmpB = LedButton(a, "Compare").apply { textSize = 13f; setOnClickListener { History.toggleCompare() } }
        for (b in listOf(undoB, redoB, cmpB)) toolbar.addView(b, LinearLayout.LayoutParams(-2, -2).apply { marginStart = d(6) })
        History.onChange = {
            undoB.alpha = if (History.canUndo()) 1f else 0.45f
            redoB.alpha = if (History.canRedo()) 1f else 0.45f
            cmpB.lit = History.comparing
        }
        History.onChange?.invoke()
        val scroll = ScrollView(a).apply { addView(content) }
        content.setPadding(d(10), d(8), d(10), d(24))
        view = Ui.column(a).apply {
            addView(HorizontalScrollView(a).apply {
                isHorizontalScrollBarEnabled = false
                setPadding(d(8), d(8), d(8), d(2))
                addView(bar)
            }, Ui.lp())
            addView(toolbar, Ui.lp())
            addView(scroll, Ui.lp(weight = 1f, height = 0).apply { width = -1 })
        }
        Params.addListener { buildPage() }  // layer types / sample names may have changed
        buildPage()
    }

    /** A framed panel with the controls of the given parameters. */
    private fun panel(title: String, metas: List<ParamMeta>, tr: Boolean = true, extra: View? = null, top: View? = null): Hw.Section {
        val s = Hw.Section(a, title, tr)
        if (top != null) s.body.addView(top, LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = Ui.dp(a, 8) })
        val flow = FlowLayout(a)
        for (m in metas) {
            val c = ParamControl(a, m) { pc ->
                if (pc.meta.name.endsWith(".type")) ui { buildPage() }
                graphs.forEach { it.invalidate() }
                if (pc.meta.name.endsWith(".fm.algo")) ui { buildPage() }
            }
            controls += c
            flow.addView(c.view)
        }
        s.body.addView(flow, LinearLayout.LayoutParams(-1, -2))
        if (extra != null) s.body.addView(extra, LinearLayout.LayoutParams(-1, -2).apply { topMargin = Ui.dp(a, 8) })
        content.addView(s, Hw.sectionLp(a))
        return s
    }

    private fun group(g: String) = Params.meta.filter { it.group == g }

    fun buildPage() {
        val p = pages[page]
        pageButtons.forEachIndexed { i, b -> b.lit = i == page }
        layerBox.visibility = if (p.title == "OSC") View.VISIBLE else View.INVISIBLE
        layerButtons.forEachIndexed { i, b -> b.lit = i == layer }
        content.removeAllViews()
        controls.clear()
        graphs.clear()
        modMatrix = null
        when (p.title) {
            "OSC" -> buildOsc()
            "FX" -> buildFx()
            else -> for (g in p.groups) {
                val metas = group(g)
                if (metas.isEmpty()) continue
                when (g) {
                    "Amp Env" -> panel(g, metas, top = graph(EnvGraphView(a, "amp")))
                    "Mod Env" -> panel(g, metas, top = graph(EnvGraphView(a, "env2")))
                    "Env 3" -> panel(g, metas, top = graph(EnvGraphView(a, "env3")))
                    "Filter" -> panel(g, metas, top = graph(FilterCurveView(a)))
                    "LFO" -> for (n in 1..3) panel("LFO $n", metas.filter { it.name.startsWith("lfo$n.") }, tr = false, top = graph(LfoView(a, n)))
                    "Sequencer" -> panel(g, metas.filter { !Regex("seq\\.s\\d+\\..*").matches(it.name) }, top = graph(StepSeqView(a)))
                    "MSEG" -> panel(g, metas.filter { !Regex("mseg\\.p\\d+\\..*").matches(it.name) }, top = graph(MsegView(a)))
                    "Mod Matrix" -> {
                        val s = Hw.Section(a, g)
                        s.body.addView(Ui.text(a, "out += source × via × amount   (via 'none' = 1)", 12f, Ui.DIM),
                            LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = Ui.dp(a, 6) })
                        val mm = ModMatrixView(a) { graphs.forEach { it.invalidate() } }
                        modMatrix = mm
                        s.body.addView(mm, LinearLayout.LayoutParams(-1, -2))
                        content.addView(s, Hw.sectionLp(a))
                    }
                    else -> panel(g, metas)
                }
            }
        }
    }

    fun refreshValues() {
        controls.forEach { it.refresh() }
        graphs.forEach { it.invalidate() }
        modMatrix?.refresh()
    }

    /** A graph that refreshes the page's knobs after it is dragged. */
    private fun graph(g: GraphView): GraphView {
        g.onEdited = { refreshValues() }
        graphs += g
        return g
    }

    // ------------------------------------------------------------------ oscillators

    private fun buildOsc() {
        val lname = "Layer " + "ABC"[layer]
        val typeMeta = Params.meta.first { it.name == "${"abc"[layer]}.type" }
        val type = typeMeta.options.getOrElse(NativeEngine.nativeGetParam(typeMeta.id).roundToInt()) { "" }
        panel(lname, group(lname))
        // the engine of the chosen type (VA / FM / PCM / WT / Organ)
        val engine = Params.meta.map { it.group }.distinct()
            .firstOrNull { it.startsWith("$lname / ") && it.removePrefix("$lname / ").equals(type, ignoreCase = true) }
        if (engine == null) {
            content.addView(Ui.text(a, L10n.f("%s engine: %s", L10n.t(lname), type), 13f, Ui.DIM, tr = false))
            return
        }
        if (type.equals("fm", ignoreCase = true)) {
            buildFm(engine, "abc"[layer])
            return
        }
        val d = { v: Int -> Ui.dp(a, v) }
        val extra: View? = when (type.lowercase()) {
            "pcm" -> sampleRow(L10n.f("Samples: %s", NativeEngine.nativeSampleSetName(layer).ifEmpty { L10n.t("(none)") }),
                "Choose…" to { a.libraryPanel.pickSamplesFor(layer) },
                "Unload" to { NativeEngine.nativeUnloadSamples(layer); buildPage() })
            "wt" -> sampleRow(L10n.f("User table: %s", NativeEngine.nativeSampleSetName(layer).ifEmpty { L10n.t("(none)") }),
                "Load .wav…" to { a.libraryPanel.pickWavetable(layer) },
                "Unload" to { NativeEngine.nativeUnloadSamples(layer); buildPage() })
            else -> null
        }
        panel(engine, group(engine), extra = extra)
        content.setPadding(d(10), d(8), d(10), d(24))
    }

    /** FM: algorithm diagram + global controls, then one operator at a time. */
    private fun buildFm(group: String, l: Char) {
        val d = { v: Int -> Ui.dp(a, v) }
        val metas = group(group)
        val algo = FmAlgoView(a, l)
        graph(algo)
        algo.onSelect = { i -> fmOp = i; buildPage() }
        val numOps = NativeEngine.nativeFmAlgorithm(Params.get("$l.fm.algo").roundToInt())[0]
        fmOp = fmOp.coerceIn(0, numOps - 1)
        algo.selected = fmOp
        panel(group, metas.filter { !it.name.startsWith("$l.fm.op") }, tr = false, top = algo)

        val ops = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL }
        for (i in 0 until 8) {
            ops.addView(LedButton(a, "OP ${i + 1}", tr = false).apply {
                lit = i == fmOp
                alpha = if (i < numOps) 1f else 0.4f
                textSize = 13f
                setOnClickListener { fmOp = i; buildPage() }
            }, LinearLayout.LayoutParams(0, -2, 1f).apply { marginEnd = d(4) })
        }
        content.addView(ops, LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = d(8) })
        val prefix = "$l.fm.op${fmOp + 1}"
        panel("Operator ${fmOp + 1}", metas.filter { it.name.startsWith("$prefix.") }, tr = false, top = graph(EnvGraphView(a, prefix)))
    }

    private fun sampleRow(label: String, vararg buttons: Pair<String, () -> Unit>): View {
        val d = { v: Int -> Ui.dp(a, v) }
        return LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(Hw.lcd(a, label, 13f), LinearLayout.LayoutParams(0, -2, 1f).apply { marginEnd = d(8) })
            for ((t, f) in buttons) addView(LedButton(a, t, led = false).apply { setOnClickListener { f() } },
                LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(6) })
        }
    }

    // ------------------------------------------------------------------ effects

    /** Block keys in engine order (slot option labels), e.g. "reverb". */
    private val blockKeys: List<String> by lazy { Params.meta.first { it.name == "fx.slot1" }.options }
    private fun blockTitle(i: Int): String =
        if (i <= 0) "–" else Params.meta.firstOrNull { it.name == "fx.${blockKeys[i]}.on" }?.group?.removePrefix("FX / ") ?: blockKeys[i]

    private var looperText: TextView? = null
    private val looperTick = object : Runnable {
        override fun run() {
            val t = looperText ?: return
            if (!t.isAttachedToWindow) return
            val s = NativeEngine.nativeLooperStatus()
            val names = arrayOf("empty", "recording", "playing", "overdubbing", "stopped")
            t.text = L10n.f("Looper: %s   %.1f / %.1f s", L10n.t(names.getOrElse(s[0].toInt()) { "?" }), s[2], s[1])
            a.ui.postDelayed(this, 250)
        }
    }

    private fun buildFx() {
        val d = { v: Int -> Ui.dp(a, v) }
        val slots = (1..12).map { Params.id("fx.slot$it") }
        val blocks = slots.map { NativeEngine.nativeGetParam(it).roundToInt() }
        val titles = blockKeys.indices.map { blockTitle(it) }

        // chain: 12 slots, two columns
        val chain = Hw.Section(a, "Effect chain (top to bottom)")
        val flow = FlowLayout(a)
        val seen = mutableSetOf<Int>()
        for (s in slots.indices) {
            val b = blocks[s]
            val line = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
            line.addView(Hw.engraved(a, "${s + 1}", 13f, tr = false), LinearLayout.LayoutParams(d(24), -2))
            val sel = Hw.lcd(a, titles.getOrElse(b) { "?" } + if (b > 0 && !seen.add(b)) L10n.t(" dup") else "", 13f).apply {
                setOnClickListener {
                    AlertDialog.Builder(a).setTitle(L10n.f("Slot %d", s + 1))
                        .setSingleChoiceItems(titles.toTypedArray(), b) { dlg, pos ->
                            dlg.dismiss()
                            NativeEngine.setParam(slots[s], pos.toFloat())
                            if (pos > 0) Params.set("fx.${blockKeys[pos]}.on", 1f)
                            buildPage()
                        }.show()
                }
            }
            line.addView(sel, LinearLayout.LayoutParams(d(140), -2).apply { marginEnd = d(6) })
            if (b > 0) {
                val onId = Params.id("fx.${blockKeys[b]}.on")
                line.addView(ToggleSwitch(a, "On", vertical = false).apply {
                    on = NativeEngine.nativeGetParam(onId) > 0.5f
                    onToggle = { v -> NativeEngine.setParam(onId, if (v) 1f else 0f) }
                }, LinearLayout.LayoutParams(d(86), -2).apply { marginEnd = d(6) })
            } else {
                line.addView(View(a), LinearLayout.LayoutParams(d(92), 1))
            }
            fun swap(o: Int) {
                NativeEngine.setParam(slots[s], blocks[o].toFloat())
                NativeEngine.setParam(slots[o], blocks[s].toFloat())
                buildPage()
            }
            line.addView(LedButton(a, "▲", led = false).apply { setOnClickListener { if (s > 0) swap(s - 1) } },
                LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(4) })
            line.addView(LedButton(a, "▼", led = false).apply { setOnClickListener { if (s < slots.size - 1) swap(s + 1) } })
            flow.addView(line)
        }
        chain.body.addView(flow, LinearLayout.LayoutParams(-1, -2))
        val running = NativeEngine.nativeFxChain().joinToString(" > ") { blockTitle(it) }.ifEmpty { L10n.t("(dry)") }
        chain.body.addView(Ui.text(a, L10n.f("running: %s", running), 12f, Ui.DIM, tr = false),
            LinearLayout.LayoutParams(-1, -2).apply { topMargin = d(6) })
        content.addView(chain, Hw.sectionLp(a))

        panel("Tempo / macros", group("FX Chain").filter { !it.name.startsWith("fx.slot") && !it.name.startsWith("looper.") })

        val shown = mutableSetOf<Int>()
        for (s in slots.indices) {
            val b = blocks[s]
            if (b <= 0 || !shown.add(b)) continue
            val key = blockKeys[b]
            val extra: View? = when (key) {
                "cab", "convo" -> {
                    val target = if (key == "cab") 0 else 1
                    sampleRow(L10n.f("User IR: %s", NativeEngine.nativeImpulseName(target).ifEmpty { L10n.t("(none)") }),
                        "Load IR…" to { a.libraryPanel.pickImpulse(target) },
                        "Unload" to { NativeEngine.nativeUnloadImpulse(target); buildPage() })
                }
                "vocoder" -> Ui.text(a, "The vocoder needs an audio input (microphone). Input capture is " +
                    "not wired up in the Android app yet; it works in the desktop build.", 12f, Ui.WARN)
                else -> null
            }
            panel("${s + 1}. ${blockTitle(b)}", group("FX / ${blockTitle(b)}").filter { !it.name.endsWith(".on") }, tr = false, extra = extra)
        }
        for (g in listOf("Send FX", "Sends", "FX Mod", "Part FX")) group(g).takeIf { it.isNotEmpty() }?.let { panel(g, it) }

        // looper
        val status = Hw.lcd(a, "", 13f)
        looperText = status
        val buttons = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            for ((t, cmd) in listOf("Rec / Play / Dub" to NativeEngine.Looper.TAP, "Stop" to NativeEngine.Looper.STOP,
                    "Undo" to NativeEngine.Looper.UNDO, "Clear" to NativeEngine.Looper.CLEAR))
                addView(LedButton(a, t, led = false).apply { setOnClickListener { NativeEngine.nativeLooperCommand(cmd) } },
                    LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(6) })
        }
        val box = Ui.column(a).apply {
            addView(status, LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = d(6) })
            addView(buttons)
        }
        panel("Looper", Params.meta.filter { it.name.startsWith("looper.") }, extra = box)
        a.ui.removeCallbacks(looperTick)
        a.ui.post(looperTick)
    }
}
