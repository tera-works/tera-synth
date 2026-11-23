package com.hsynth.app

import android.annotation.SuppressLint
import android.app.AlertDialog
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.view.GestureDetector
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.widget.FrameLayout
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import kotlin.math.abs
import kotlin.math.roundToInt

/**
 * Sound editor laid out like a synth's edit panels: module tabs along the top
 * (one compact row with the layer selector and undo / redo / compare), each page
 * a stack of framed panels with knobs, LCD selectors and switches, colored by
 * its module (cyan OSC, amber Filter, violet Env / Mod, emerald FX), and a
 * persistent quick-edit strip along the bottom. Generated from the engine's
 * parameter metadata, so new engine parameters appear without UI code changes.
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
    private var tuning: TuningPanel? = null
    private var vintage: VintagePanel? = null
    private var pianoPanel: PianoPanel? = null
    private var physicalView: PhysicalModelView? = null
    private var strumView: StrumView? = null
    private var arpPattern: ArpPatternView? = null
    private var wtBar: WavetableBrowserBar? = null
    private lateinit var mutateBar: MutateBar
    private lateinit var morphBtn: LedButton
    private val learnLineTop = View(a).apply { setBackgroundColor(Hw.MOD_ENV); visibility = View.GONE }
    private val learnLineBottom = View(a).apply { setBackgroundColor(Hw.MOD_ENV); visibility = View.GONE }
    private var seenLearning = false
    private var fmOp = 0
    private val quickCells = mutableListOf<QuickEditCell>()

    private data class Page(val title: String, val groups: List<String>, val accent: Int)

    private val pages = listOf(
        Page("OSC", emptyList(), Hw.MOD_OSC),  // layer common + the engine of the layer's type, built by buildOsc()
        Page("Filter", listOf("Filter", "Filter 2", "Voice FX"), Hw.MOD_FILTER),
        Page("Env", listOf("Amp Env", "Mod Env", "Env 3"), Hw.MOD_ENV),
        Page("LFO", listOf("LFO"), Hw.MOD_ENV),
        Page("Mod", listOf("Mod Matrix", "Macros", "Controllers"), Hw.MOD_ENV),
        Page("Arp / Seq", listOf("Arpeggiator", "Strum", "Sequencer", "#Rhythm", "#Phrase", "Performance"), Hw.MOD_OSC),
        Page("Motion", listOf("MSEG", "Motion 1", "Motion 2", "Motion 3", "Motion 4"), Hw.MOD_ENV),
        Page("FX", emptyList(), Hw.MOD_FX),   // chain editor, built by buildFx()
        Page("Voice", listOf("Voice", "Piano", "Tuning", "Master"), Hw.MOD_OSC),
    )

    /** Semantic module color of the page being shown. */
    private val pageAccent: Int get() = pages[page].accent

    private fun ui(f: () -> Unit) = a.ui.post(f)

    /** Compact top-bar button (keeps all 9 tabs + Layer A/B/C + Undo/Redo/Compare unclipped on 862dp). */
    private fun compact(b: LedButton, size: Float = 11.5f): LedButton {
        val d = { v: Int -> Ui.dp(a, v) }
        b.textSize = size
        b.minHeight = d(30)
        b.minimumHeight = d(30)
        val lp = if (b.paddingLeft > d(14)) d(15) else d(8)
        b.setPadding(lp, d(3), d(8), d(3))
        return b
    }

    init {
        val d = { v: Int -> Ui.dp(a, v) }
        val portrait = Ui.portrait(a)
        mutateBar = MutateBar(a)

        // module tabs
        val tabs = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        pages.forEachIndexed { i, p ->
            val b = compact(LedButton(a, p.title, accentColor = p.accent)).apply { setOnClickListener { page = i; mutateBar.dismiss(); buildPage() } }
            pageButtons += b
            tabs.addView(b, LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(4) })
        }
        val tabScroll = HorizontalScrollView(a).apply {
            isHorizontalScrollBarEnabled = false
            isFillViewport = false
            addView(tabs)
        }

        // layer selector (OSC page only)
        layerBox = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(Hw.engraved(a, "Layer", 10f), LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(4) })
        }
        for (l in 0..2) {
            val b = compact(LedButton(a, "ABC"[l].toString(), tr = false, accentColor = Hw.MOD_OSC)).apply {
                setOnClickListener { layer = l; mutateBar.dismiss(); buildPage() }
            }
            layerButtons += b
            layerBox.addView(b, LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(3) })
        }

        // undo / redo / compare
        val undoB = compact(LedButton(a, "Undo", led = false), 12f).apply { setOnClickListener { History.undo() } }
        val redoB = compact(LedButton(a, "Redo", led = false), 12f).apply { setOnClickListener { History.redo() } }
        val cmpB = compact(LedButton(a, "Compare", accentColor = Hw.MOD_FILTER), 12f).apply { setOnClickListener { History.toggleCompare() } }
        // A3: MORPH LEARN toggle (long-press = morph drawer) and B5: mutate / init popup
        morphBtn = compact(LedButton(a, "MORPH", tr = false, accentColor = Hw.MOD_ENV), 12f).apply {
            setOnClickListener { MorphUi.setLearning(!MorphUi.learning) }
            setOnLongClickListener { a.openSheet(a.morphSheet); true }
        }
        val moreB = compact(LedButton(a, "⋯", tr = false, led = false), 14f).apply {
            setOnClickListener { openMutate(this) }
        }
        val history = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            for (b in listOf(undoB, redoB, cmpB, morphBtn, moreB)) addView(b, LinearLayout.LayoutParams(-2, -2).apply { marginStart = d(4) })
        }
        MorphUi.addListener { syncLearnUi() }
        History.onChange = {
            undoB.alpha = if (History.canUndo()) 1f else 0.45f
            redoB.alpha = if (History.canRedo()) 1f else 0.45f
            cmpB.lit = History.comparing
        }
        History.onChange?.invoke()

        val topBar: View = if (!portrait) {
            ChassisBar(a).apply {
                setPadding(d(6), d(4), d(6), d(4))
                addView(tabScroll, LinearLayout.LayoutParams(0, -2, 1f))
                addView(View(a).apply { setBackgroundColor(Hw.PANEL_EDGE) }, LinearLayout.LayoutParams(d(1), d(22)).apply { marginStart = d(6); marginEnd = d(6) })
                addView(layerBox, LinearLayout.LayoutParams(-2, -2))
                addView(history, LinearLayout.LayoutParams(-2, -2))
            }
        } else {
            Ui.column(a).apply {
                setBackgroundColor(Hw.LCD_BG)
                setPadding(d(6), d(4), d(6), d(4))
                addView(tabScroll, LinearLayout.LayoutParams(-1, -2))
                addView(HorizontalScrollView(a).apply {
                    isHorizontalScrollBarEnabled = false
                    addView(LinearLayout(a).apply {
                        orientation = LinearLayout.HORIZONTAL
                        gravity = Gravity.CENTER_VERTICAL
                        addView(layerBox)
                        addView(history)
                    })
                }, LinearLayout.LayoutParams(-1, -2).apply { topMargin = d(4) })
            }
        }

        // bottom quick-edit strip (always visible)
        val strip = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            setBackgroundColor(Hw.LCD_BG)
        }
        val quick = listOf(
            Triple("filter.cutoff", "CUTOFF", Hw.MOD_FILTER),
            Triple("filter.reso", "RESO", Hw.MOD_FILTER),
            Triple("amp.attack", "ATTACK", Hw.MOD_ENV),
            Triple("amp.release", "RELEASE", Hw.MOD_ENV),
            Triple("macro.1", "MACRO", Hw.MOD_ENV),
            Triple("fx.macro1", "FX", Hw.MOD_FX),
        )
        quick.forEachIndexed { i, (name, label, color) ->
            val cell = QuickEditCell(a, name, label, color, divider = i > 0)
            cell.onChanged = { id -> onQuickEdit(id) }
            quickCells += cell
            strip.addView(cell, LinearLayout.LayoutParams(0, -1, 1f))
        }

        val scroll = ScrollView(a).apply { isVerticalScrollBarEnabled = false; addView(content) }
        content.setPadding(d(8), d(6), d(8), d(12))
        // B5: the MutateBar floats above the quick strip (bottom of the scroll area)
        val scrollFrame = FrameLayout(a).apply {
            addView(scroll, FrameLayout.LayoutParams(-1, -1))
            addView(mutateBar, FrameLayout.LayoutParams(-2, d(28), Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL).apply { bottomMargin = d(6) })
        }
        view = Ui.column(a).apply {
            setBackgroundColor(Hw.CHASSIS)
            addView(topBar, LinearLayout.LayoutParams(-1, -2))
            addView(learnLineTop, LinearLayout.LayoutParams(-1, d(2)))
            addView(scrollFrame, LinearLayout.LayoutParams(-1, 0, 1f))
            addView(strip, LinearLayout.LayoutParams(-1, d(44)))
            addView(learnLineBottom, LinearLayout.LayoutParams(-1, d(2)))
        }
        a.addPresetListener { mutateBar.dismiss() }
        Params.addListener { buildPage() }  // layer types / sample names may have changed
        buildPage()
    }

    /** B5: the `⋯` popup — scope PAGE = the groups of the current page (OSC: the selected layer), PATCH = everything. */
    private fun openMutate(anchor: View) {
        val p = pages[page]
        val groups: Set<String> =
            if (p.title == "OSC") setOf("Layer " + "ABC"[layer]) else p.groups.filter { !it.startsWith("#") }.toSet()
        showMutatePopup(a, anchor, groups, null) { o -> mutateBar.show(o) }
    }

    /** MORPH LEARN indicator: lit button + 2 dp violet underline on the top bar and the quick strip. */
    private fun syncLearnUi() {
        val on = MorphUi.learning
        morphBtn.lit = on
        val vis = if (on) View.VISIBLE else View.GONE
        if (learnLineTop.visibility != vis) { learnLineTop.visibility = vis; learnLineBottom.visibility = vis }
        if (on != seenLearning) {
            seenLearning = on
            if (on) Hw.onParamTouched?.invoke("MORPH LEARN", L10n.t("move a knob"))
            refreshValues()   // turning learn on forces the morph position to A
        }
    }

    /** A quick-edit cell moved a parameter: follow it on the page's knobs and graphs. */
    private fun onQuickEdit(id: Int) {
        for (c in controls) if (c.meta.id == id) c.refresh()
        graphs.forEach { it.invalidate() }
    }

    private fun refreshQuick() = quickCells.forEach { it.refresh() }

    /** A framed panel with the controls of the given parameters. */
    private fun panel(title: String, metas: List<ParamMeta>, tr: Boolean = true, extra: View? = null, top: View? = null): Hw.Section {
        val accent = pageAccent
        val s = Hw.Section(a, title, tr, accent)
        if (top != null) s.body.addView(top, LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = Ui.dp(a, 8) })
        val flow = FlowLayout(a, Ui.dp(a, 6), Ui.dp(a, 6))
        for (m in metas) {
            val c = ParamControl(a, m, accent = accent) { pc ->
                if (pc.meta.name.endsWith(".type")) ui { buildPage() }
                graphs.forEach { it.invalidate() }
                if (pc.meta.name.endsWith(".fm.algo")) ui { buildPage() }
                refreshQuick()
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
        layerBox.visibility = if (p.title == "OSC") View.VISIBLE else View.GONE
        layerButtons.forEachIndexed { i, b -> b.lit = i == layer }
        content.removeAllViews()
        controls.clear()
        graphs.clear()
        modMatrix = null
        tuning = null
        vintage = null
        pianoPanel = null
        physicalView = null
        strumView = null
        arpPattern = null
        wtBar = null
        // A5: collapsible live scope as the first section of the sound pages (default collapsed: zero cost while closed)
        if (p.title == "OSC" || p.title == "Filter" || p.title == "FX") addScopeSection()
        // A2: Vintage / Spread sits at the top of the Voice page
        if (p.title == "Voice") addVintageSection()
        when (p.title) {
            "OSC" -> buildOsc()
            "FX" -> buildFx()
            else -> {
            for (g in p.groups) {
                // synthetic groups (not engine params): rhythm bar, phrase recorder
                if (g == "#Rhythm") { addRhythmSection(); continue }
                if (g == "#Phrase") { addPhraseSection(); continue }
                val metas = group(g)
                if (metas.isEmpty()) continue
                when (g) {
                    // A6: 16-step pattern grid + toolbar on top of the arpeggiator knobs
                    "Arpeggiator" -> {
                        val apv = ArpPatternView(a)
                        graph(apv)
                        arpPattern = apv
                        panel(g, metas, top = ArpPatternView.withToolbar(a, apv))
                    }
                    "Strum" -> panel(g, metas, top = StrumView(a).also { strumView = it })
                    // A2: the vintage / spread params live in the VintagePanel, not in the generic flow
                    "Voice" -> metas.filter { it.name !in VintagePanel.NAMES }.takeIf { it.isNotEmpty() }?.let { panel(g, it) }
                    // A4: Piano Realism knobs + lid
                    "Piano" -> {
                        val s = Hw.Section(a, "Piano Realism", accent = pageAccent)
                        val pp = PianoPanel(a, largeKnobs = false)
                        pp.onEdited = { refreshQuick() }
                        pianoPanel = pp
                        s.body.addView(pp, LinearLayout.LayoutParams(-1, -2))
                        content.addView(s, Hw.sectionLp(a))
                    }
                    "Amp Env" -> panel(g, metas, top = graph(EnvGraphView(a, "amp")))
                    "Mod Env" -> panel(g, metas, top = graph(EnvGraphView(a, "env2")))
                    "Env 3" -> panel(g, metas, top = graph(EnvGraphView(a, "env3")))
                    "Filter" -> panel(g, metas, top = graph(FilterCurveView(a)))
                    "LFO" -> for (n in 1..3) panel("LFO $n", metas.filter { it.name.startsWith("lfo$n.") }, tr = false, top = graph(LfoView(a, n)))
                    "Sequencer" -> panel(g, metas.filter { !Regex("seq\\.s\\d+\\..*").matches(it.name) }, top = graph(StepSeqView(a)))
                    "MSEG" -> panel(g, metas.filter { !Regex("mseg\\.p\\d+\\..*").matches(it.name) }, top = graph(MsegView(a)))
                    "Mod Matrix" -> {
                        val s = Hw.Section(a, g, accent = pageAccent)
                        s.body.addView(Ui.text(a, "out += source × via × amount   (via 'none' = 1)", 12f, Ui.DIM),
                            LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = Ui.dp(a, 6) })
                        val mm = ModMatrixView(a) { graphs.forEach { it.invalidate() }; refreshQuick() }
                        modMatrix = mm
                        s.body.addView(mm, LinearLayout.LayoutParams(-1, -2))
                        content.addView(s, Hw.sectionLp(a))
                    }
                    // U3: temperament picker + Scala + cents bars (the 12 stock tune.* knobs sit in a collapsed drawer)
                    "Tuning" -> {
                        val s = Hw.Section(a, g, accent = pageAccent)
                        val tp = TuningPanel(a, pageAccent)
                        tuning = tp
                        s.body.addView(tp, LinearLayout.LayoutParams(-1, -2))
                        content.addView(s, Hw.sectionLp(a))
                    }
                    else -> panel(g, metas)
                }
            }
            }
        }
        refreshQuick()
    }

    /** A2: Vintage / Spread block (skipped when the engine does not offer the params). */
    private fun addVintageSection() {
        val vp = VintagePanel(a)
        if (!vp.isAvailable) return
        vp.onEdited = { refreshQuick() }
        vintage = vp
        val s = Hw.Section(a, "Vintage / Spread", accent = pageAccent)
        s.body.addView(vp, LinearLayout.LayoutParams(-1, -2))
        content.addView(s, Hw.sectionLp(a))
    }

    /** A6: wide drum-groove bar. */
    private fun addRhythmSection() {
        val s = Hw.Section(a, "Rhythm Pattern", accent = pageAccent)
        s.body.addView(RhythmBar(a, compact = false), LinearLayout.LayoutParams(-1, -2))
        content.addView(s, Hw.sectionLp(a))
    }

    /** U2: the phrase recorder sits with the arpeggiator / sequencer (full-width variant). */
    private fun addPhraseSection() {
        val s = Hw.Section(a, "Phrase Recorder", accent = pageAccent)
        s.body.addView(PhrasePanelView(a, wide = true), LinearLayout.LayoutParams(-1, -2))
        content.addView(s, Hw.sectionLp(a))
    }

    /** Collapsible "Scope" section (pref `scopeOpen`); expanding / collapsing acquires / releases the shared scope tap. */
    private fun addScopeSection() {
        val s = Hw.collapsibleSection(a, "Scope", accent = pageAccent, collapsed = !a.prefs.getBoolean("scopeOpen", false))
        val scope = AudioScopeView(a, 110f)
        scope.allowed = !s.collapsed
        s.onToggle = { collapsed ->
            a.prefs.edit().putBoolean("scopeOpen", !collapsed).apply()
            scope.allowed = !collapsed
        }
        s.body.addView(scope, LinearLayout.LayoutParams(-1, -2))
        content.addView(s, Hw.sectionLp(a))
    }

    fun refreshValues() {
        controls.forEach { it.refresh() }
        graphs.forEach { it.invalidate() }
        modMatrix?.refresh()
        tuning?.refresh()
        vintage?.refresh()
        pianoPanel?.refresh()
        physicalView?.refresh()
        strumView?.refresh()
        arpPattern?.refresh()
        wtBar?.refresh()
        refreshQuick()
    }

    /** A graph in the page's module color that refreshes the page's knobs after it is dragged. */
    private fun graph(g: GraphView): GraphView {
        g.accent = pageAccent
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
        val extra: View? = when (type.lowercase()) {
            "pcm", "granular" -> sampleRow(L10n.f("Samples: %s", NativeEngine.nativeSampleSetName(layer).ifEmpty { L10n.t("(none)") }),
                "Choose…" to { a.libraryPanel.pickSamplesFor(layer) },
                "Record…" to { a.openSampleWizard(defaultLayer = layer) },   // B2 live sampling wizard
                "Unload" to { NativeEngine.nativeUnloadSamples(layer); buildPage() })
            "wt" -> sampleRow(L10n.f("User table: %s", NativeEngine.nativeSampleSetName(layer).ifEmpty { L10n.t("(none)") }),
                "Load .wav…" to { a.libraryPanel.pickWavetable(layer) },
                "Unload" to { NativeEngine.nativeUnloadSamples(layer); buildPage() })
            // A2: vintage drift / voice spread next to the analog engine
            "va" -> VintagePanel(a).takeIf { it.isAvailable }?.also { vp ->
                vp.onEdited = { refreshQuick() }
                vintage = vp
            }
            else -> null
        }
        // A1: physical-model chips + resonance view above the engine's knobs
        val top: View? = if (type.equals("wt", ignoreCase = true)) {
            // Phase 6: 2.5D wavetable ribbon + built-in / user table chips + "+ IMPORT" (WT_POS / WT_TABLE of this layer)
            WavetableBrowserBar(a, layer, a.formatBridge).also { bar ->
                bar.onEdited = { refreshValues() }
                wtBar = bar
            }
        } else if (type.equals("physical", ignoreCase = true)) {
            PhysicalModelView(a, layer, NativeEngine::nativePhysModes).also { pv ->
                pv.onModelChanged = { ui { buildPage() } }
                pv.resonance.onEdited = { refreshValues() }
                pv.resonance.accent = pageAccent
                physicalView = pv
            }
        } else null
        panel(engine, group(engine), extra = extra, top = top)
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
            ops.addView(compact(LedButton(a, "OP ${i + 1}", tr = false, accentColor = pageAccent)).apply {
                lit = i == fmOp
                alpha = if (i < numOps) 1f else 0.4f
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
            addView(Hw.lcd(a, label, 13f).apply { background = Hw.lcdBg(a, stroke = Hw.withAlpha(pageAccent, 110)) },
                LinearLayout.LayoutParams(0, -2, 1f).apply { marginEnd = d(8) })
            for ((t, f) in buttons) addView(compact(LedButton(a, t, led = false, accentColor = pageAccent)).apply { setOnClickListener { f() } },
                LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(6) })
        }
    }

    // ------------------------------------------------------------------ effects

    /** Block keys in engine order (slot option labels), e.g. "reverb". */
    private val blockKeys: List<String> by lazy { Params.meta.first { it.name == "fx.slot1" }.options }
    private fun blockTitle(i: Int): String =
        if (i <= 0) "–" else Params.meta.firstOrNull { it.name == "fx.${blockKeys[i]}.on" }?.group?.removePrefix("FX / ") ?: blockKeys[i]

    private var looperText: TextView? = null

    /**
     * Looper read-out through the shared [FrameTicker] (4 fps): it only runs while the LCD is really shown, so a
     * hidden EDIT tab no longer polls the engine every 250 ms. Registered / dropped with the LCD's attachment.
     */
    private val looperClient = object : FrameTicker.Client {
        override val intervalMs: Int get() = 250
        override val tickView: View? get() = looperText
        override fun onFrame(nowMs: Long) {
            val t = looperText ?: return
            val s = NativeEngine.nativeLooperStatus()
            val names = arrayOf("empty", "recording", "playing", "overdubbing", "stopped")
            t.text = L10n.f("Looper: %s   %.1f / %.1f s", L10n.t(names.getOrElse(s[0].toInt()) { "?" }), s[2], s[1])
        }
    }

    private fun buildFx() {
        val d = { v: Int -> Ui.dp(a, v) }
        val accent = pageAccent
        val slots = (1..12).map { Params.id("fx.slot$it") }
        val blocks = slots.map { NativeEngine.nativeGetParam(it).roundToInt() }
        val titles = blockKeys.indices.map { blockTitle(it) }

        // chain: 12 slots, flowing into columns
        val chain = Hw.Section(a, "Effect chain (top to bottom)", accent = accent)
        val flow = FlowLayout(a, d(10), d(4))
        val seen = mutableSetOf<Int>()
        for (s in slots.indices) {
            val b = blocks[s]
            val line = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
            line.addView(Hw.engraved(a, "${s + 1}", 12f, tr = false, color = if (b > 0) accent else Hw.INK_DIM), LinearLayout.LayoutParams(d(22), -2))
            val sel = Hw.lcd(a, titles.getOrElse(b) { "?" } + if (b > 0 && !seen.add(b)) L10n.t(" dup") else "", 12.5f).apply {
                background = Hw.lcdBg(a, stroke = if (b > 0) Hw.withAlpha(accent, 110) else Hw.PANEL_EDGE)
                if (b <= 0) setTextColor(Hw.INK_DIM)
                setOnClickListener {
                    AlertDialog.Builder(a).setTitle(L10n.f("Slot %d", s + 1))
                        .setSingleChoiceItems(titles.toTypedArray(), b) { dlg, pos ->
                            dlg.dismiss()
                            History.checkpoint()
                            NativeEngine.setParam(slots[s], pos.toFloat())
                            if (pos > 0) Params.set("fx.${blockKeys[pos]}.on", 1f)
                            buildPage()
                        }.show()
                }
            }
            line.addView(sel, LinearLayout.LayoutParams(d(132), -2).apply { marginEnd = d(6) })
            if (b > 0) {
                val onId = Params.id("fx.${blockKeys[b]}.on")
                line.addView(ToggleSwitch(a, "On", vertical = false, accent = accent).apply {
                    on = NativeEngine.nativeGetParam(onId) > 0.5f
                    onToggle = { v -> History.checkpoint(); NativeEngine.setParam(onId, if (v) 1f else 0f) }
                }, LinearLayout.LayoutParams(d(80), -2).apply { marginEnd = d(4) })
            } else {
                line.addView(View(a), LinearLayout.LayoutParams(d(84), 1))
            }
            fun swap(o: Int) {
                History.checkpoint()
                NativeEngine.setParam(slots[s], blocks[o].toFloat())
                NativeEngine.setParam(slots[o], blocks[s].toFloat())
                buildPage()
            }
            line.addView(compact(LedButton(a, "▲", tr = false, led = false)).apply { setOnClickListener { if (s > 0) swap(s - 1) } },
                LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(4) })
            line.addView(compact(LedButton(a, "▼", tr = false, led = false)).apply { setOnClickListener { if (s < slots.size - 1) swap(s + 1) } })
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
                // U4: 1-tap "Enable Audio Input" with live state + level (replaces the stale "not wired up" warning)
                "vocoder" -> Ui.column(a).apply {
                    addView(Ui.text(a, "Vocoder carrier = this synth · modulator = Audio Input (mic / USB)", 12f, Ui.DIM))
                    addView(InputStatusRow(a), LinearLayout.LayoutParams(-1, d(30)).apply { topMargin = d(6) })
                }
                else -> null
            }
            panel("${s + 1}. ${blockTitle(b)}", group("FX / ${blockTitle(b)}").filter { !it.name.endsWith(".on") }, tr = false, extra = extra)
        }
        for (g in listOf("Send FX", "Sends", "FX Mod", "Part FX")) group(g).takeIf { it.isNotEmpty() }?.let { panel(g, it) }

        // looper
        val status = Hw.lcd(a, "", 13f).apply { background = Hw.lcdBg(a, stroke = Hw.withAlpha(accent, 110)) }
        looperText = status
        val buttons = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            for ((t, cmd) in listOf("Rec / Play / Dub" to NativeEngine.Looper.TAP, "Stop" to NativeEngine.Looper.STOP,
                    "Undo" to NativeEngine.Looper.UNDO, "Clear" to NativeEngine.Looper.CLEAR))
                addView(compact(LedButton(a, t, led = false, accentColor = accent)).apply { setOnClickListener { NativeEngine.nativeLooperCommand(cmd) } },
                    LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(6) })
        }
        status.addOnAttachStateChangeListener(object : View.OnAttachStateChangeListener {
            override fun onViewAttachedToWindow(v: View) { FrameTicker.register(looperClient) }
            override fun onViewDetachedFromWindow(v: View) { FrameTicker.unregister(looperClient) }
        })
        val box = Ui.column(a).apply {
            addView(InputStatusRow(a), LinearLayout.LayoutParams(-1, d(30)).apply { bottomMargin = d(6) })
            addView(status, LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = d(6) })
            addView(buttons)
        }
        panel("Looper", Params.meta.filter { it.name.startsWith("looper.") }, extra = box)
    }
}

/**
 * One cell of the Edit page's bottom quick-edit strip (MONTAGE M / FANTOM style):
 * label, live mono value and a colored progress bar. Drag horizontally (relative,
 * no jump), tap to jump to the touched position, double-tap to reset to default.
 * Zero allocations in onDraw().
 */
@SuppressLint("ViewConstructor")
class QuickEditCell(
    ctx: Context,
    paramName: String,
    label: String,
    private val accent: Int,
    private val divider: Boolean = true
) : View(ctx) {
    /** Called with the parameter id after the cell changed the engine value. */
    var onChanged: ((Int) -> Unit)? = null

    private val meta: ParamMeta? = Params.metaOf(paramName)
    private val labelText = label.uppercase()
    private var valueText = "—"
    private var norm = 0f
    private var dragging = false
    private var downX = 0f
    private var downNorm = 0f
    /** Engine value at touch-down (A of a morph route being learned). */
    private var downValue = 0f

    private val d = resources.displayMetrics.density
    private val p = Paint()
    private val lblP = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Hw.BOLD; textSize = 9.5f * d; letterSpacing = 0.1f }
    private val valP = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Hw.MONO; textSize = 13.5f * d }
    private val r = RectF()
    private val trackCol = Color.rgb(20, 25, 36)

    private val gestures = GestureDetector(ctx, object : GestureDetector.SimpleOnGestureListener() {
        override fun onDoubleTap(e: MotionEvent): Boolean {
            val m = meta ?: return true
            History.checkpoint()
            commit(m.def)
            return true
        }
        override fun onSingleTapConfirmed(e: MotionEvent): Boolean {
            val m = meta ?: return true
            if (dragging) return true
            History.checkpoint()
            val t = (e.x / width.coerceAtLeast(1)).coerceIn(0f, 1f)
            commit(m.fromNormalized(t))
            return true
        }
    })

    init {
        isClickable = true
        if (meta == null) alpha = 0.4f
        refresh()
    }

    private fun normOf(m: ParamMeta, v: Float): Float =
        if (m.kind == "int" && m.max > m.min) ((v - m.min) / (m.max - m.min)).coerceIn(0f, 1f) else m.toNormalized(v)

    /** Re-read the engine value (cheap: one native call). */
    fun refresh() {
        val m = meta ?: return
        val v = NativeEngine.nativeGetParam(m.id)
        val n = normOf(m, v)
        val s = m.format(v)
        if (n != norm || s != valueText) {
            norm = n
            valueText = s
            invalidate()
        }
    }

    private fun commit(v: Float) {
        val m = meta ?: return
        if (MorphUi.learning && MorphUi.learnable(m)) {
            // MORPH LEARN: move the route's end-point B; the sound (base value) stays untouched
            MorphUi.learnSet(m, MorphUi.startOf(m, downValue), v)
            norm = normOf(m, v)
            valueText = m.format(v)
            Hw.onParamTouched?.invoke(m.label, valueText)
            invalidate()
            onChanged?.invoke(m.id)
            return
        }
        NativeEngine.setParam(m.id, v)
        val now = NativeEngine.nativeGetParam(m.id)
        norm = normOf(m, now)
        valueText = m.format(now)
        Hw.onParamTouched?.invoke(m.label, valueText)
        invalidate()
        onChanged?.invoke(m.id)
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        val m = meta ?: return false
        gestures.onTouchEvent(e)
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                parent?.requestDisallowInterceptTouchEvent(true)
                downX = e.x
                refresh()
                downNorm = norm
                downValue = NativeEngine.nativeGetParam(m.id)
                dragging = false
                Hw.onParamTouched?.invoke(m.label, valueText)
                invalidate()
            }
            MotionEvent.ACTION_MOVE -> {
                if (!dragging && abs(e.x - downX) > 6f * d) {
                    dragging = true
                    downX = e.x
                    History.checkpoint()
                }
                if (dragging) {
                    val t = (downNorm + (e.x - downX) / (width.coerceAtLeast(1) * 1.25f)).coerceIn(0f, 1f)
                    val v = m.fromNormalized(t)
                    if (v != NativeEngine.nativeGetParam(m.id)) commit(v)
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                dragging = false
                if (MorphUi.learning && MorphUi.learnable(m)) {
                    // gesture over: snap back to the base value and pick up the engine's routes once
                    MorphUi.refresh()
                    refresh()
                    onChanged?.invoke(m.id)
                }
                invalidate()
            }
        }
        return true
    }

    override fun onDraw(c: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        // dark well + hairlines
        p.color = if (dragging) Hw.withAlpha(accent, 26) else Hw.LCD_BG
        c.drawRect(0f, 0f, w, h, p)
        p.color = Hw.PANEL_EDGE
        c.drawRect(0f, 0f, w, d, p)
        if (divider) c.drawRect(0f, 0f, d, h, p)

        val x = 10f * d
        c.save()
        c.clipRect(x, 0f, w - 4f * d, h)
        lblP.color = if (dragging) accent else Hw.ENGRAVE
        c.drawText(labelText, x, 15f * d, lblP)
        valP.color = if (dragging) Color.WHITE else Hw.SILK
        c.drawText(valueText, x, 32f * d, valP)
        c.restore()

        // progress bar
        val by = h - 7f * d
        val bh = 3f * d
        r.set(x, by, w - 10f * d, by + bh)
        p.color = trackCol
        c.drawRoundRect(r, bh / 2f, bh / 2f, p)
        if (norm > 0.001f) {
            r.right = r.left + (r.width()) * norm
            p.color = accent
            c.drawRoundRect(r, bh / 2f, bh / 2f, p)
        }
    }
}
