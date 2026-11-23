package com.hsynth.app

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.drawable.StateListDrawable
import android.os.SystemClock
import android.text.TextUtils
import android.util.TypedValue
import android.view.GestureDetector
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import org.json.JSONArray
import java.io.File
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

/**
 * STAGE (tab 0): the Obsidian Instrument performance home.
 *
 * MainActivity already draws the 26dp telemetry strip (MIDI / BLE / DSP / meter / battery /
 * clock) and the 52dp rail, so every pixel here goes to performance:
 *  - left (~60%): hero preset / set card (PREV / NEXT, swipe, category pill, 2×8 part pills),
 *    8 colour-coded set-list slots (tap = recall, long press = store) and an 8-scene strip;
 *  - right (~40%): four segmented LED faders (long press label = reassign) and a hold-safe PANIC.
 */
@SuppressLint("ViewConstructor", "ClickableViewAccessibility")
class LivePanel(private val a: MainActivity) {
    companion object {
        const val SLOTS = 8
        const val SCENES = 8
        val FADER_CHOICES = listOf("master.volume", "filter.cutoff", "filter.reso", "amp.attack", "amp.release",
            "macro.1", "macro.2", "macro.3", "macro.4", "fx.macro1", "fx.macro2", "arp.rate", "fx.tempo", "perf.super",
            "perf.morph", "piano.lid", "piano.softrelease")
        val FADER_DEFAULT = listOf("master.volume", "filter.cutoff", "macro.1", "fx.macro1")
        /** Stage names for the performance macros (Live presets: MACRO = tone, FX = effect amount). */
        val FADER_LABEL = mapOf("macro.1" to "MACRO", "fx.macro1" to "FX", "master.volume" to "VOLUME", "filter.cutoff" to "CUTOFF",
            "perf.super" to "SUPER", "perf.morph" to "MORPH", "piano.lid" to "LID", "piano.softrelease" to "SOFT REL")
        /** Fader slot colours: Cyan volume, Amber cutoff, Violet macro, Emerald FX. */
        val FADER_ACCENTS = intArrayOf(Hw.MOD_OSC, Hw.MOD_FILTER, Hw.MOD_ENV, Hw.MOD_FX)
        const val ALL = "ALL"
        const val SETS = "#SETS#"
        private const val FADER_SYNC_MS = 200L
        private const val USER_HOLD_MS = 450L
    }

    private val d = { v: Int -> Ui.dp(a, v) }
    val view = FrameLayout(a)
    private val portrait = Ui.portrait(a)

    // ------------------------------------------------------------ hero card
    private val presetIndex = label("", 14f, Hw.MOD_OSC, mono = true)
    private val kindBg = Hw.lcdBg(a, stroke = Hw.withAlpha(Hw.MOD_OSC, 150), radiusDp = 15f)
    private val presetKind = label("", 12.5f, Hw.MOD_OSC).apply {
        background = kindBg
        letterSpacing = 0.06f
        gravity = Gravity.CENTER_VERTICAL
        setPadding(d(12), 0, d(12), 0)
        setOnClickListener { chooseCategory() }
    }
    private val presetName = TextView(a).apply {
        setTextColor(Hw.SILK)
        typeface = Hw.BOLD
        maxLines = 2
        ellipsize = TextUtils.TruncateAt.END
        includeFontPadding = false
        setAutoSizeTextTypeUniformWithConfiguration(24, 42, 1, TypedValue.COMPLEX_UNIT_SP)
        gravity = Gravity.CENTER_VERTICAL
    }

    // 16 channels of the current set: tap = the part the faders control
    private val partCells = mutableListOf<PartPill>()
    private val partGrid = LinearLayout(a).apply { orientation = LinearLayout.VERTICAL }

    // S4: 8 banks x 8 slots, chain mode, memo + slot editor sheets
    val store = SetlistStore(a)
    private val bankStrip = BankStrip(a)
    private val memoSheet = MemoSheet(a, store) { delta -> walkChain(delta) }
    private val slotEditSheet = SlotEditSheet(a, store, { i -> storeCurrentInto(i) }, { onStoreChanged() })
    private val cueText = label("", 10f, Hw.INK_DIM, mono = true)
    private val memoChip = TextView(a).apply {
        text = "MEMO"
        textSize = 11f
        typeface = Hw.BOLD
        letterSpacing = 0.06f
        setTextColor(Hw.MOD_FILTER)
        gravity = Gravity.CENTER
        background = Hw.lcdBg(a, stroke = Hw.withAlpha(Hw.MOD_FILTER, 150), radiusDp = 11f)
        setPadding(d(9), 0, d(9), 0)
        visibility = View.GONE
        setOnClickListener { openMemoForActive() }
    }
    /** A4: shown only while the current preset is a piano; tap opens the Piano Realism sheet. */
    private val pianoChip = TextView(a).apply {
        text = "PIANO"
        textSize = 11f
        typeface = Hw.BOLD
        letterSpacing = 0.06f
        setTextColor(Hw.MOD_OSC)
        gravity = Gravity.CENTER
        background = Hw.lcdBg(a, stroke = Hw.withAlpha(Hw.MOD_OSC, 150), radiusDp = 11f)
        setPadding(d(9), 0, d(9), 0)
        visibility = View.GONE
        setOnClickListener { a.openSheet(a.pianoSheet) }
    }
    private var soundKind = SetlistSlot.PRESET
    private var seenPreset: String? = null
    private var pendingScene = -1
    private var pendingSceneAfter: (() -> Unit)? = null
    private var pendingSceneUntil = 0L

    // set list, scenes, faders
    private val slotViews = mutableListOf<Pad>()
    private val sceneButtons = mutableListOf<LedButton>()
    private val faders = mutableListOf<LiveFader>()
    private val faderPart = label("", 11f, Hw.MOD_OSC, mono = true)
    private var lastFaderSync = 0L

    // U1: dedicated Super Knob strip (36 dp) and S3: Master EQ pill in the PERFORM header
    private val superBar = HSegBar(a, Hw.MOD_ENV, 24, 40f)
    private var superEditing = false
    private var superUsed = true
    private var superSrcIndex = -1
    private val morphChip = ChipButton(a, "A↔B", widthDp = 40f, accent = Hw.MOD_ENV).apply {
        setOnClickListener { MorphUi.toggleSuperSource() }
        setOnLongClickListener { a.openSheet(a.morphSheet); true }
    }
    /** Phase 5: Edge TPU 2D patch-morph pad, one tap from Stage / Live. */
    private val aiChip = ChipButton(a, L10n.t("AI 2D"), widthDp = 46f, accent = Hw.MOD_FX).apply {
        setOnClickListener { a.openSheet(a.aiMorphSheet) }
    }
    private val eqPill = ChipButton(a, "EQ", widthDp = 34f, accent = Hw.MOD_FX).apply {
        setOnClickListener { a.toggleSheet(a.masterEqSheet) }
    }

    /** Live sets: 16-part multi setups (one sound per MIDI channel) in files/setups/live. */
    private val setsDir = File(a.filesDir, "setups/live")
    private fun sets(): List<File> = setsDir.listFiles { f -> f.isFile && f.extension == "json" }?.sortedBy { it.name.lowercase() } ?: emptyList()
    private var setsMode = a.prefs.getBoolean("liveSets", true) && sets().isNotEmpty()
    private var currentSet: File? = a.prefs.getString("liveSet", null)?.let { File(it) }?.takeIf { it.isFile }

    /** "LIVE 03 Jazz and Soul.json" -> "Jazz & Soul" */
    private fun setName(f: File) = f.nameWithoutExtension.replace(Regex("^LIVE \\d+ "), "").replace(" and ", " & ")

    /** "SC PRESET A 001 Rich Grand" -> "Rich Grand" */
    private fun shortName(n: String) = n.replace(Regex("^SC (PRESET [A-G] |PRESET Rhythm |GM2 Rhythm Sets |GM2 \\S+ )\\d+ "), "")
        .replace(Regex("^SC GM2 Capital \\d+ "), "").ifEmpty { "—" }

    private fun recallSet(f: File, after: (() -> Unit)? = null) {
        currentSet = f
        soundKind = SetlistSlot.SET
        a.prefs.edit().putString("liveSet", f.absolutePath).apply()
        presetName.text = setName(f)
        SstSweep.start()
        a.loader.execute {
            a.multi.applySetup(f.readText())
            // queued behind applySetup's own UI block, so the sound is in place when [after] runs
            if (after != null) a.runOnUiThread { after() }
        }
    }

    /** Category PREV / NEXT step through ("ALL" = every preset) in single-sound mode. */
    private var category: String = a.prefs.getString("liveCategory", null) ?: ALL

    private fun browseList() = a.library.presets().let { all -> if (category == ALL) all else all.filter { it.category == category }.ifEmpty { all } }

    private fun step(delta: Int) {
        if (store.chainMode) { walkChain(delta); return }
        if (setsMode) {
            val l = sets()
            if (l.isEmpty()) return
            val i = l.indexOfFirst { it == currentSet }
            recallSet(l[if (i < 0) (if (delta > 0) 0 else l.size - 1) else ((i + delta) % l.size + l.size) % l.size])
            return
        }
        val list = browseList()
        if (list.isEmpty()) return
        val idx = list.indexOfFirst { it.file == a.currentPreset }
        val next = if (idx < 0) (if (delta > 0) 0 else list.size - 1) else ((idx + delta) % list.size + list.size) % list.size
        a.loadPreset(list[next].file)
    }

    private fun chooseCategory() {
        val all = a.library.presets()
        val nSets = sets().size
        val cats = (if (nSets > 0) listOf(SETS) else emptyList()) + listOf(ALL) +
            all.map { it.category.ifEmpty { "-" } }.distinct().sortedBy { it.lowercase() }
        val labels = cats.map { c ->
            when (c) {
                SETS -> L10n.t("LIVE SETS — 16 ch") + "  ($nSets)"
                ALL -> L10n.t("Single sounds: all") + "  (${all.size})"
                else -> "$c  (${all.count { it.category.ifEmpty { "-" } == c }})"
            }
        }
        android.app.AlertDialog.Builder(a)
            .setTitle(L10n.t("Category"))
            .setItems(labels.toTypedArray()) { _, w ->
                if (store.chainMode) { store.chainMode = false; bankStrip.sync() }   // picking a category leaves chain mode
                setsMode = cats[w] == SETS
                a.prefs.edit().putBoolean("liveSets", setsMode).apply()
                if (setsMode) { step(if (currentSet == null) 1 else 0); refresh(); return@setItems }
                category = cats[w]
                a.prefs.edit().putString("liveCategory", category).apply()
                if (browseList().none { it.file == a.currentPreset }) step(1) else refresh()
            }.show()
    }

    init {
        view.setBackgroundColor(Hw.CHASSIS)
        view.isClickable = true  // nothing underneath gets touches
        bankStrip.bind(store)
        bankStrip.onSelectBank = { b -> store.viewedBank = b; refresh() }
        bankStrip.onToggleChain = { store.chainMode = !store.chainMode; refresh() }

        val hero = heroCard()
        val slotGrid = slotGrid()
        val sceneStrip = sceneStrip()
        val faderCard = faderCard()
        val superStrip = superStrip()
        val panic = HoldPanicButton(a, "HOLD · PANIC") {
            a.play.keyboard.allNotesOff()
            NativeEngine.control(NativeEngine.Control.ALL_SOUND_OFF, 0f)
            a.log("panic: all sound off")
        }

        val root = LinearLayout(a).apply { setPadding(d(8), d(8), d(8), d(8)) }
        if (portrait) {
            root.orientation = LinearLayout.VERTICAL
            root.addView(hero, LinearLayout.LayoutParams(-1, 0, 1f))
            root.addView(bankStrip, LinearLayout.LayoutParams(-1, d(26)).apply { topMargin = d(6) })
            root.addView(slotGrid, LinearLayout.LayoutParams(-1, d(112)).apply { topMargin = d(4) })
            root.addView(sceneStrip, LinearLayout.LayoutParams(-1, d(32)).apply { topMargin = d(8) })
            root.addView(faderCard, LinearLayout.LayoutParams(-1, d(260)).apply { topMargin = d(8) })
            root.addView(superStrip, LinearLayout.LayoutParams(-1, d(36)).apply { topMargin = d(6) })
            root.addView(panic, LinearLayout.LayoutParams(-1, d(42)).apply { topMargin = d(6) })
        } else {
            root.orientation = LinearLayout.HORIZONTAL
            val left = LinearLayout(a).apply { orientation = LinearLayout.VERTICAL }
            left.addView(hero, LinearLayout.LayoutParams(-1, 0, 1f))
            left.addView(bankStrip, LinearLayout.LayoutParams(-1, d(26)).apply { topMargin = d(6) })
            left.addView(slotGrid, LinearLayout.LayoutParams(-1, d(112)).apply { topMargin = d(4) })
            left.addView(sceneStrip, LinearLayout.LayoutParams(-1, d(32)).apply { topMargin = d(6) })
            val right = LinearLayout(a).apply { orientation = LinearLayout.VERTICAL }
            right.addView(faderCard, LinearLayout.LayoutParams(-1, 0, 1f))
            right.addView(superStrip, LinearLayout.LayoutParams(-1, d(36)).apply { topMargin = d(6) })
            right.addView(panic, LinearLayout.LayoutParams(-1, d(42)).apply { topMargin = d(6) })
            root.addView(left, LinearLayout.LayoutParams(0, -1, 1.5f))
            root.addView(right, LinearLayout.LayoutParams(0, -1, 1f).apply { marginStart = d(8) })
        }
        view.addView(root, FrameLayout.LayoutParams(-1, -1))

        a.addPresetListener { refresh() }
        Params.addListener { faders.forEach { it.sync(true) } }
        refresh()
    }

    // ------------------------------------------------------------ building blocks

    private fun label(s: String, size: Float, color: Int, mono: Boolean = false, bold: Boolean = true) = TextView(a).apply {
        text = s
        textSize = size
        setTextColor(color)
        typeface = if (mono) Hw.MONO else if (bold) Hw.BOLD else Hw.LABEL
        maxLines = 1
        ellipsize = TextUtils.TruncateAt.END
    }

    private fun multiOrNull(): MultiPanel? = try { a.multi } catch (_: UninitializedPropertyAccessException) { null }

    private fun heroCard(): View {
        val c = HeroCard(a).apply {
            orientation = LinearLayout.VERTICAL
            background = Hw.panelBg(a, radiusDp = 10f)
            setPadding(d(12), d(8), d(8), d(8))
        }
        val top = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        top.addView(presetIndex, LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(10) })
        top.addView(presetKind, LinearLayout.LayoutParams(-2, d(30)))
        top.addView(View(a), LinearLayout.LayoutParams(0, 1, 1f))
        val bw = d(if (portrait) 84 else 92)
        top.addView(stepButton("◀ PREV") { step(-1) }, LinearLayout.LayoutParams(bw, d(48)).apply { marginEnd = d(6) })
        top.addView(stepButton("NEXT ▶") { step(1) }, LinearLayout.LayoutParams(bw, d(48)))
        c.addView(top, LinearLayout.LayoutParams(-1, -2))
        // NameArea: the name + (chain / bpm) cue bottom-left + MEMO chip bottom-right
        val nameArea = FrameLayout(a)
        nameArea.addView(presetName, FrameLayout.LayoutParams(-1, -1))
        nameArea.addView(cueText, FrameLayout.LayoutParams(-2, -2, Gravity.BOTTOM or Gravity.START))
        nameArea.addView(memoChip, FrameLayout.LayoutParams(-2, d(22), Gravity.BOTTOM or Gravity.END))
        nameArea.addView(pianoChip, FrameLayout.LayoutParams(-2, d(22), Gravity.BOTTOM or Gravity.END))
        c.addView(nameArea, LinearLayout.LayoutParams(-1, 0, 1f).apply { topMargin = d(2); bottomMargin = d(4) })

        for (r in 0 until 2) {
            val row = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL }
            for (k in 0 until 8) {
                val part = r * 8 + k
                val cell = PartPill(a, part).apply {
                    // setEditPart notifies the preset + param listeners -> refresh() and fader sync
                    setOnClickListener { a.setEditPart(part) }
                }
                partCells += cell
                row.addView(cell, LinearLayout.LayoutParams(0, -1, 1f).apply { if (k < 7) marginEnd = d(3) })
            }
            partGrid.addView(row, LinearLayout.LayoutParams(-1, d(26)).apply { if (r == 0) bottomMargin = d(3) })
        }
        c.addView(partGrid, LinearLayout.LayoutParams(-1, -2))

        // swipe the name area to step presets / sets
        val swipe = GestureDetector(a, object : GestureDetector.SimpleOnGestureListener() {
            override fun onDown(e: MotionEvent) = true
            override fun onFling(e1: MotionEvent?, e2: MotionEvent, vx: Float, vy: Float): Boolean {
                if (abs(vx) < 600 || abs(vx) < abs(vy)) return false
                step(if (vx < 0) 1 else -1)
                return true
            }
        })
        presetName.setOnTouchListener { _, e -> swipe.onTouchEvent(e) }
        return c
    }

    private fun stepButton(text: String, onClick: () -> Unit) = TextView(a).apply {
        this.text = text
        textSize = 15f
        typeface = Hw.BOLD
        letterSpacing = 0.08f
        gravity = Gravity.CENTER
        maxLines = 1
        setTextColor(Hw.SILK)
        background = StateListDrawable().apply {
            addState(intArrayOf(android.R.attr.state_pressed), Hw.keyBg(a, accent = Hw.MOD_OSC))
            addState(intArrayOf(), Hw.keyBg(a))
        }
        isHapticFeedbackEnabled = false
        setOnClickListener { onClick() }
    }

    private fun slotGrid(): View {
        val grid = LinearLayout(a).apply { orientation = LinearLayout.VERTICAL }
        val cols = 4
        val rows = SLOTS / cols
        for (r in 0 until rows) {
            val row = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL }
            for (c in 0 until cols) {
                val i = r * cols + c
                val p = Pad(a, i)
                // pads are bound to the viewed bank: slot index = bank * 8 + pad position
                p.setOnClickListener { recallSlot(bankBase() + i) }
                p.setOnLongClickListener { slotLongPress(bankBase() + i); true }
                slotViews += p
                row.addView(p, LinearLayout.LayoutParams(0, -1, 1f).apply { if (c < cols - 1) marginEnd = d(6) })
            }
            grid.addView(row, LinearLayout.LayoutParams(-1, 0, 1f).apply { if (r < rows - 1) bottomMargin = d(6) })
        }
        return grid
    }

    private fun sceneStrip(): View {
        val row = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        for (i in 0 until SCENES) {
            val b = LedButton(a, "SC ${i + 1}", tr = false, accentColor = Hw.MOD_FX).apply {
                textSize = 12f
                minHeight = 0
                minimumHeight = 0
                setPadding(d(16), 0, d(4), 0)
                setOnClickListener { multiOrNull()?.recallScene(i) }
                setOnLongClickListener {
                    multiOrNull()?.let { m -> Hw.onParamTouched?.invoke("SC ${i + 1}", m.sceneLabel(i)) }
                    true
                }
            }
            sceneButtons += b
            row.addView(b, LinearLayout.LayoutParams(0, -1, 1f).apply { if (i < SCENES - 1) marginEnd = d(4) })
        }
        return row
    }

    private fun faderCard(): View {
        val card = LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            background = Hw.panelBg(a, radiusDp = 10f)
            setPadding(d(8), d(6), d(8), d(4))
        }
        val head = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        head.addView(Hw.engraved(a, "PERFORM", 10f, tr = false), LinearLayout.LayoutParams(0, -2, 1f))
        head.addView(eqPill, LinearLayout.LayoutParams(d(34), d(22)).apply { marginEnd = d(6) })
        head.addView(faderPart, LinearLayout.LayoutParams(-2, -2))
        card.addView(head, LinearLayout.LayoutParams(-1, d(22)))
        val row = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL }
        for (i in 0 until 4) {
            val name = a.prefs.getString("liveFader$i", null) ?: FADER_DEFAULT[i]
            val f = LiveFader(i, name)
            faders += f
            row.addView(f.view, LinearLayout.LayoutParams(0, -1, 1f).apply { if (i < 3) marginEnd = d(4) })
        }
        card.addView(row, LinearLayout.LayoutParams(-1, 0, 1f))
        return card
    }

    /** U1: SUPER label + 24-segment violet [HSegBar] (relative drag, double-tap = 0) — 36 dp, between faders and PANIC. */
    private fun superStrip(): View {
        val row = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            background = Hw.panelBg(a, radiusDp = 10f)
            setPadding(d(8), d(4), d(6), d(4))
        }
        row.addView(Hw.engraved(a, "SUPER", 9.5f, tr = false, color = Hw.MOD_ENV), LinearLayout.LayoutParams(d(44), -2))
        row.addView(superBar, LinearLayout.LayoutParams(0, d(24), 1f))
        row.addView(morphChip, LinearLayout.LayoutParams(d(40), d(22)).apply { marginStart = d(6) })
        row.addView(aiChip, LinearLayout.LayoutParams(d(46), d(22)).apply { marginStart = d(4) })
        MorphUi.addListener { syncMorphChip() }
        superBar.onDragStart = {
            superEditing = true
            History.checkpoint()
            if (!superUsed) Hw.onParamTouched?.invoke("SUPER", L10n.t("no destinations"))
        }
        superBar.onDragEnd = { superEditing = false }
        superBar.onChange = { v ->
            Params.set("perf.super", v)
            superBar.valueText = "%.2f".format(v)
            if (superUsed) Hw.onParamTouched?.invoke("SUPER", superBar.valueText)
        }
        superBar.onReset = {
            History.checkpoint()
            Params.set("perf.super", 0f)
            superBar.value = 0f
            superBar.valueText = "0.00"
        }
        return row
    }

    /** Any of the 16 mod-matrix slots using the Super Knob as source / via with a non-zero amount? (16 reads, on preset change). */
    private fun scanSuperUsed() {
        if (superSrcIndex < 0) superSrcIndex = Params.metaOf("mod1.src")?.options?.indexOf("super") ?: -1
        if (superSrcIndex < 0) { superUsed = true; return }
        var used = false
        for (s in 1..16) {
            val src = Params.metaOf("mod$s.src") ?: break
            val amt = Params.get("mod$s.amt")
            if (amt == 0f) continue
            if (NativeEngine.nativeGetParam(src.id).roundToInt() == superSrcIndex ||
                Params.get("mod$s.via").roundToInt() == superSrcIndex) { used = true; break }
        }
        superUsed = used
        superBar.alpha = if (used) 1f else 0.55f
    }

    private fun syncSuper() {
        if (superEditing) return
        val v = Params.get("perf.super")
        if (abs(v - superBar.value) > 1e-4f) superBar.value = v
        val t = "%.2f".format(v)
        if (t != superBar.valueText) superBar.valueText = t
    }

    /** `[A↔B]` is lit (violet) while the Super Knob is the morph source. */
    private fun syncMorphChip() {
        val on = MorphUi.sourceIsSuper()
        morphChip.active = on
        morphChip.ledColor = if (on) Hw.MOD_ENV else 0
    }

    private fun syncEqPill() {
        val st = MasterEqStore
        eqPill.ledColor = if (st.active) Hw.MOD_FX else if (st.bypassedConfigured) Hw.MOD_FILTER else 0
        eqPill.active = a.sheets.current === a.masterEqSheet
    }

    // ------------------------------------------------------------ set list (S4)

    private fun bankBase() = store.viewedBank * SetlistStore.PER_BANK

    /**
     * Recall slot [i] (0..63): SST sweep → load the preset / set / scene → performance offsets (transpose, trim), tempo,
     * then the memo sheet when the slot asks for it. Used by the pads, the chain (PREV / NEXT / swipe / footswitch) and
     * the memo sheet.
     */
    fun recallSlot(i: Int) {
        if (i !in 0 until SetlistStore.SLOTS) return
        val s = store.slot(i)
        if (s.isEmpty) {
            a.log(L10n.t("empty slot — long press to store the current sound"), Ui.WARN)
            flashHint(i)
            return
        }
        val after: () -> Unit = { afterRecall(i) }
        when (s.kind) {
            SetlistSlot.SCENE -> {
                val m = multiOrNull()
                val n = s.ref.toIntOrNull() ?: -1
                if (m == null || !m.sceneExists(n)) { a.log("scene ${n + 1} is empty", Ui.WARN); flashHint(i); return }
                SstSweep.start()
                pendingScene = n
                pendingSceneAfter = after
                pendingSceneUntil = SystemClock.elapsedRealtime() + 8000L
                m.recallScene(n)
            }
            else -> {
                val f = File(s.ref)
                if (!f.exists()) { a.log("${f.name}: file not found", Ui.WARN); flashHint(i); return }
                SstSweep.start()
                if (s.kind == SetlistSlot.SET) {
                    setsMode = true
                    a.prefs.edit().putBoolean("liveSets", true).apply()
                    recallSet(f, after)
                    refresh()
                } else {
                    setsMode = false
                    a.prefs.edit().putBoolean("liveSets", false).apply()
                    soundKind = SetlistSlot.PRESET
                    a.loadPreset(f, after)
                }
            }
        }
    }

    /** MultiPanel.onSceneChanged (scene recalled or stored). */
    fun onSceneChanged() {
        val m = multiOrNull()
        val cur = m?.currentSceneIndex ?: -1
        if (cur >= 0) soundKind = SetlistSlot.SCENE
        val after = pendingSceneAfter
        if (after != null) {
            if (SystemClock.elapsedRealtime() > pendingSceneUntil) { pendingSceneAfter = null }
            else if (cur == pendingScene) { pendingSceneAfter = null; pendingScene = -1; after() ; return }
        }
        refresh()
    }

    private fun afterRecall(i: Int) {
        val s = store.slot(i)
        NativeEngine.nativeSetPerfOffsets(s.trn, s.trimDb)
        if (s.bpm > 0) Params.set("fx.tempo", s.bpm.toFloat())
        store.activeSlot = i
        store.viewedBank = i / SetlistStore.PER_BANK   // the viewed bank follows the playing slot
        refresh()
        if (s.memoShow && s.memo.isNotBlank()) memoSheet.open(i)
        else if (memoSheet.isShowing) memoSheet.showSlot(i)
    }

    /**
     * Footswitch / host action: step through the chain (non-empty slots A1 → H8) from the slot recalled last.
     * The first press also turns CHAIN mode on.
     */
    fun chainStep(delta: Int) {
        if (!store.chainMode) { store.chainMode = true; refresh() }
        walkChain(delta)
    }

    /** One step along [SetlistStore.chainOrder] (no mode change). */
    private fun walkChain(delta: Int) {
        if (store.chainOrder().isEmpty()) {
            a.log(L10n.t("set list is empty — long press a slot to store the current sound"), Ui.WARN)
            return
        }
        val next = store.nextInChain(store.activeSlot, delta)
        if (next < 0) { a.log(L10n.t("end of set list")); return }
        recallSlot(next)
    }

    private fun slotLongPress(i: Int) {
        if (store.slot(i).isEmpty) storeCurrentInto(i)   // old muscle memory: instant store, no dialog
        else slotEditSheet.edit(i)
    }

    /** Kind + reference of the sound that is playing now (scene → set → preset). */
    private fun currentRef(): Pair<String, String>? {
        val m = multiOrNull()
        if (soundKind == SetlistSlot.SCENE && m != null && m.currentSceneIndex >= 0 && m.sceneExists(m.currentSceneIndex))
            return SetlistSlot.SCENE to m.currentSceneIndex.toString()
        if (setsMode) currentSet?.let { return SetlistSlot.SET to it.absolutePath }
        a.currentPreset?.let { return SetlistSlot.PRESET to it.absolutePath }
        return null
    }

    private fun storeCurrentInto(i: Int) {
        val (kind, ref) = currentRef() ?: run {
            a.log(L10n.t("nothing to store — load a preset first"), Ui.WARN)
            return
        }
        store.storeCurrent(i, kind, ref, "")
        store.activeSlot = i
        a.log("slot ${SetlistStore.codeOf(i)} = ${store.displayTitle(i)}")
        refresh()
    }

    private fun onStoreChanged() { refresh() }

    private fun openMemoForActive() {
        val i = memoSlot()
        if (i >= 0) memoSheet.open(i)
    }

    /** Active slot when the playing sound is still that slot and it has a memo, else -1. */
    private fun memoSlot(): Int {
        val i = store.activeSlot
        if (i < 0 || !matches(i)) return -1
        return if (store.slot(i).memo.isNotBlank()) i else -1
    }

    private fun matches(i: Int): Boolean {
        val s = store.slot(i)
        if (s.isEmpty) return false
        return when (s.kind) {
            SetlistSlot.SET -> setsMode && currentSet?.absolutePath == s.ref
            SetlistSlot.SCENE -> multiOrNull()?.currentSceneIndex == (s.ref.toIntOrNull() ?: -2) && soundKind == SetlistSlot.SCENE
            else -> !setsMode && a.currentPreset?.absolutePath == s.ref
        }
    }

    private fun slotName(i: Int, all: List<Library.Entry>): String {
        val s = store.slot(i)
        if (s.isEmpty) return ""
        if (s.title.isNotBlank()) return s.title
        val raw = when (s.kind) {
            SetlistSlot.SCENE -> multiOrNull()?.sceneLabel(s.ref.toIntOrNull() ?: 0) ?: store.displayTitle(i)
            SetlistSlot.SET -> setName(File(s.ref))
            else -> all.firstOrNull { it.file.absolutePath == s.ref }?.title ?: File(s.ref).nameWithoutExtension
        }
        return raw.replace(Regex("^LIVE \\d+ "), "").replace(" and ", " & ")
    }

    private fun flashHint(i: Int) {
        val n = i - bankBase()
        if (n !in slotViews.indices) return
        slotViews[n].hint = true
        view.postDelayed({ slotViews[n].hint = false }, 1200)
    }

    // ------------------------------------------------------------ updates

    fun refresh() {
        val all = a.library.presets()
        val edit = NativeEngine.nativeEditPart()
        val accent: Int
        if (setsMode) {
            val l = sets()
            val i = l.indexOfFirst { it == currentSet }
            presetIndex.text = if (i >= 0) "SET %02d/%02d".format(i + 1, l.size) else "SET --/%02d".format(l.size)
            presetKind.text = "▾  LIVE SETS · 16 CH"
            presetName.text = currentSet?.let { setName(it) } ?: L10n.t("choose a set")
            accent = Hw.MOD_OSC
        } else {
            val list = browseList()
            val idx = list.indexOfFirst { it.file == a.currentPreset }
            presetIndex.text = if (idx >= 0) "%03d/%03d".format(idx + 1, list.size) else "---/%03d".format(list.size)
            presetKind.text = "▾  " + (if (category == ALL) "ALL" else category.uppercase())
            presetName.text = a.currentPresetName
            val cat = if (category != ALL) category else all.firstOrNull { it.file == a.currentPreset }?.category.orEmpty()
            accent = if (cat.isEmpty()) Hw.MOD_OSC else Hw.categoryColor(cat)
        }
        // a preset loaded from elsewhere (library, browse) is the "current sound" again, not the last scene
        val curPath = a.currentPreset?.absolutePath
        if (curPath != seenPreset) {
            seenPreset = curPath
            if (soundKind == SetlistSlot.SCENE) soundKind = SetlistSlot.PRESET
        }
        if (store.chainMode) {
            val act = store.activeSlot
            val nx = store.nextInChain(act, 1)
            presetIndex.text = if (act >= 0) SetlistStore.codeOf(act) + (if (nx >= 0 && nx != act) "▸" + SetlistStore.codeOf(nx) else "") else "CHAIN"
            presetKind.text = "▾  CHAIN · BANK " + SetlistStore.letterOf(if (act >= 0) act / SetlistStore.PER_BANK else store.viewedBank)
        }
        presetKind.setTextColor(accent)
        kindBg.setStroke(max(1, d(1)), Hw.withAlpha(accent, 150))

        // channel grid (sets mode, or any multi)
        val multi = NativeEngine.nativeIsMulti()
        val showParts = setsMode || multi
        partGrid.visibility = if (showParts) View.VISIBLE else View.GONE
        partCells.forEachIndexed { k, c -> c.set(shortName(a.partNames[k]), k == edit, a.partNames[k].isEmpty()) }
        faderPart.text = if (showParts) "→ P%02d".format(edit + 1) else ""
        faderPart.setTextColor(Hw.partColor(edit))

        // slot pads of the viewed bank: "A1".."H8", amber ≡ when the slot has a memo
        val base = bankBase()
        val act = store.activeSlot
        val primary = act in base until base + SLOTS && matches(act)
        slotViews.forEachIndexed { n, p ->
            val i = base + n
            val s = store.slot(i)
            p.code = SetlistStore.codeOf(i)
            p.title = slotName(i, all)
            p.memo = s.memo.isNotBlank()
            p.accent = if (s.color != -1 && !s.isEmpty) s.color or (0xFF shl 24) else Hw.partColor(n)
            p.active = if (primary) i == act else matches(i)
        }
        bankStrip.sync()
        syncNameArea()
        refreshScenes()
        faders.forEach { it.sync(true) }
        scanSuperUsed()
        syncSuper()
        syncMorphChip()
        syncEqPill()
    }

    /** NameArea overlays: MEMO chip (slot has a memo) and the "♩120 · NEXT ▸ title" cue (chain / bpm only). */
    private fun syncNameArea() {
        val mi = memoSlot()
        val chip = mi >= 0
        memoChip.visibility = if (chip) View.VISIBLE else View.GONE
        val piano = PianoPanel.isPianoPreset(a)
        pianoChip.visibility = if (piano) View.VISIBLE else View.GONE
        (pianoChip.layoutParams as? FrameLayout.LayoutParams)?.let { lp ->
            val m = if (chip) d(64) else 0
            if (lp.marginEnd != m) { lp.marginEnd = m; pianoChip.layoutParams = lp }
        }
        presetName.setPadding(0, 0, (if (chip) d(64) else 0) + (if (piano) d(68) else 0), 0)
        val act = store.activeSlot
        val s = if (act >= 0 && matches(act)) store.slot(act) else null
        val sb = StringBuilder(48)
        if (s != null && s.bpm > 0) sb.append('♩').append(s.bpm)
        if (store.chainMode) {
            val nx = store.nextInChain(act, 1)
            if (nx >= 0 && nx != act) {
                if (sb.isNotEmpty()) sb.append(" · ")
                sb.append("NEXT ▸ ").append(SetlistStore.codeOf(nx))
                val t = store.displayTitle(nx)
                if (t.isNotEmpty()) sb.append(' ').append(t)
            }
        }
        if (sb.isEmpty()) cueText.visibility = View.GONE
        else { cueText.text = sb; cueText.visibility = View.VISIBLE }
    }

    private fun refreshScenes() {
        val m = multiOrNull() ?: return
        val cur = m.currentSceneIndex
        sceneButtons.forEachIndexed { i, b ->
            val lit = i == cur
            if (b.lit != lit) b.lit = lit
            b.alpha = if (m.sceneExists(i)) 1f else 0.38f
        }
    }

    /** Called from the stats timer while Stage is shown: cheap periodic fader resync (MIDI learn / CC moves). */
    @Suppress("UNUSED_PARAMETER")
    fun updateStats(s: FloatArray) {
        val now = SystemClock.elapsedRealtime()
        if (now - lastFaderSync < FADER_SYNC_MS) return
        lastFaderSync = now
        faders.forEach { it.sync(false) }
        syncSuper()
        syncEqPill()
    }

    // ------------------------------------------------------------ faders

    /** Binds a [SegmentedFader] to an engine parameter (relative drag, double tap = default, long press label = assign). */
    private inner class LiveFader(private val index: Int, var param: String) {
        val view = SegmentedFader(a, "", FADER_ACCENTS[index])
        private var meta: ParamMeta? = null
        private var lastUser = 0L
        private var editing = false

        init {
            view.onChange = { n ->
                val m = meta
                if (m != null) {
                    if (!editing) { editing = true; History.checkpoint() }
                    lastUser = SystemClock.elapsedRealtime()
                    Params.set(param, m.fromNormalized(n))
                    view.valueText = m.format(Params.get(param))
                    Hw.onParamTouched?.invoke(caption(), view.valueText)
                }
            }
            view.onDragEnd = { editing = false; lastUser = SystemClock.elapsedRealtime() }
            view.onReset = {
                meta?.let { m ->
                    History.checkpoint()
                    Params.set(param, m.def)
                    sync(true)
                    Hw.onParamTouched?.invoke(caption(), view.valueText)
                }
            }
            view.onLongPressLabel = { chooseParam() }
            sync(true)
        }

        fun caption() = FADER_LABEL[param] ?: (meta?.label ?: param).uppercase()

        fun sync(force: Boolean) {
            if (!force && (editing || SystemClock.elapsedRealtime() - lastUser < USER_HOLD_MS)) return
            meta = Params.metaOf(param)
            val cap = caption()
            if (view.label != cap) { view.label = cap; view.invalidate() }
            val m = meta
            if (m == null) {
                if (view.valueText != "—") view.valueText = "—"
                return
            }
            val v = Params.get(param)
            val n = m.toNormalized(v)
            if (abs(n - view.value) > 1e-4f) view.value = n
            val vt = m.format(v)
            if (vt != view.valueText) view.valueText = vt
        }

        private fun chooseParam() {
            val opts = FADER_CHOICES.mapNotNull { Params.metaOf(it) }
            android.app.AlertDialog.Builder(a)
                .setTitle(L10n.t("Fader") + " ${index + 1}")
                .setItems(opts.map { it.label }.toTypedArray()) { _, w ->
                    param = opts[w].name
                    a.prefs.edit().putString("liveFader$index", param).apply()
                    sync(true)
                }.show()
        }
    }

    // ------------------------------------------------------------ views

    /** Hero card that draws the S1 3 dp emerald SST sweep on its top edge during a sound transition (idle: nothing). */
    private class HeroCard(ctx: Context) : LinearLayout(ctx) {
        private val dd = resources.displayMetrics.density

        init { setWillNotDraw(false) }

        override fun dispatchDraw(c: Canvas) {
            super.dispatchDraw(c)
            SstSweep.drawTopSweep(c, width.toFloat(), height.toFloat(), 10f * dd, dd)
        }

        override fun onAttachedToWindow() { super.onAttachedToWindow(); SstSweep.attach(this) }
        override fun onDetachedFromWindow() { SstSweep.detach(this); super.onDetachedFromWindow() }
    }

    /** One of the 16 part pills in the hero card (tap = edit part). Zero allocations per frame. */
    private class PartPill(ctx: Context, part: Int) : View(ctx) {
        private var name = ""
        private var sel = false
        private var empty = true
        private val accent = Hw.partColor(part)
        private val numStr = (part + 1).toString()
        private val dd = resources.displayMetrics.density
        private val p = Paint(Paint.ANTI_ALIAS_FLAG)
        private val t = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Hw.BOLD }
        private val n = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Hw.MONO }
        private val r = RectF()
        private var shown = ""
        private var shownW = -1

        fun set(name: String, selected: Boolean, empty: Boolean) {
            if (name == this.name && selected == this.sel && empty == this.empty) return
            this.name = name
            this.sel = selected
            this.empty = empty
            shownW = -1
            invalidate()
        }

        override fun onDraw(c: Canvas) {
            val w = width.toFloat()
            val h = height.toFloat()
            val rad = 5f * dd
            r.set(0f, 0f, w, h)
            p.style = Paint.Style.FILL
            p.color = if (isPressed) Hw.PANEL_RAISED else Hw.KEY
            c.drawRoundRect(r, rad, rad, p)
            if (sel) {
                p.color = Hw.withAlpha(accent, 48)
                c.drawRoundRect(r, rad, rad, p)
            }
            // left part-colour edge
            c.save()
            c.clipRect(0f, 0f, 3f * dd, h)
            p.color = if (sel) accent else Hw.withAlpha(accent, if (empty) 60 else 150)
            c.drawRoundRect(r, rad, rad, p)
            c.restore()
            if (sel) {
                p.style = Paint.Style.STROKE
                p.strokeWidth = 1.2f * dd
                p.color = accent
                r.inset(0.6f * dd, 0.6f * dd)
                c.drawRoundRect(r, rad, rad, p)
                p.style = Paint.Style.FILL
            }
            val baseline = h / 2f + 3.6f * dd
            val x0 = 6f * dd
            n.textSize = 10f * dd
            n.color = if (sel) Color.WHITE else accent
            c.drawText(numStr, x0, baseline, n)
            val nx = x0 + n.measureText(numStr) + 4f * dd
            t.textSize = 10.5f * dd
            t.color = if (sel) Color.WHITE else if (empty) Hw.INK_DIM else Hw.SILK
            if (shownW != width) { shownW = width; shown = fit(name, w - nx - 3f * dd) }
            c.drawText(shown, nx, baseline, t)
        }

        private fun fit(s: String, maxW: Float): String {
            if (maxW <= 0f) return ""
            if (t.measureText(s) <= maxW) return s
            var e = s
            while (e.length > 1 && t.measureText("$e…") > maxW) e = e.dropLast(1)
            return "$e…"
        }

        override fun drawableStateChanged() { super.drawableStateChanged(); invalidate() }
    }

    /** Set-list slot: dark card, 3dp part-colour edge, mono slot number, 2-line title, glow when current. */
    private inner class Pad(ctx: Context, index: Int) : View(ctx) {
        var title = ""
            set(v) { if (field != v) { field = v; dirty = true; invalidate() } }
        var active = false
            set(v) { if (field != v) { field = v; invalidate() } }
        var hint = false
            set(v) { if (field != v) { field = v; invalidate() } }
        /** Preformatted slot code ("A1".."H8"); set on bind, never formatted in onDraw. */
        var code = SetlistStore.codeOf(index)
            set(v) { if (field != v) { field = v; invalidate() } }
        /** The slot has a memo: amber ≡ glyph. */
        var memo = false
            set(v) { if (field != v) { field = v; invalidate() } }
        var accent = Hw.partColor(index)
            set(v) { if (field != v) { field = v; invalidate() } }
        private val memoP = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Hw.BOLD; color = Hw.MOD_FILTER; textAlign = Paint.Align.RIGHT }
        private val hintText = L10n.t("hold to store")
        private val dd = resources.displayMetrics.density
        private val bg = Paint(Paint.ANTI_ALIAS_FLAG)
        private val txt = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Hw.BOLD }
        private val num = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Hw.MONO }
        private val r = RectF()
        private var dirty = true
        private var line1 = ""
        private var line2 = ""
        private var lineSize = 12f

        init { isHapticFeedbackEnabled = true }

        override fun onSizeChanged(w: Int, h: Int, ow: Int, oh: Int) {
            super.onSizeChanged(w, h, ow, oh)
            dirty = true
        }

        /** Wraps / shrinks the title once per change (not per frame). */
        private fun layoutText() {
            dirty = false
            val label = title.ifEmpty { hintText }
            val maxW = width - 18f * dd
            txt.textSize = (if (title.isEmpty()) 11f else 15f) * dd
            lineSize = txt.textSize
            if (maxW <= 0f) { line1 = ""; line2 = ""; return }
            while (txt.textSize > 11f * dd && txt.measureText(label) > maxW * 1.9f) txt.textSize -= 0.5f * dd
            lineSize = txt.textSize
            if (txt.measureText(label) <= maxW) { line1 = label; line2 = ""; return }
            var cut = label.length
            while (cut > 1 && txt.measureText(label, 0, cut) > maxW) cut--
            val sp = label.lastIndexOf(' ', cut).takeIf { it > 0 } ?: cut
            line1 = label.substring(0, sp).trimEnd()
            var second = label.substring(sp).trim()
            if (txt.measureText(second) > maxW) {
                var k = second.length
                while (k > 1 && txt.measureText(second, 0, k) + txt.measureText("…") > maxW) k--
                second = second.substring(0, k) + "…"
            }
            line2 = second
        }

        override fun onDraw(c: Canvas) {
            if (dirty) layoutText()
            val w = width.toFloat()
            val h = height.toFloat()
            val rad = 8f * dd
            val empty = title.isEmpty()
            r.set(0f, 0f, w, h)
            bg.style = Paint.Style.FILL
            bg.color = if (isPressed) Hw.PANEL_RAISED else Hw.PANEL
            c.drawRoundRect(r, rad, rad, bg)
            if (active) {
                bg.color = Hw.withAlpha(accent, 30)
                c.drawRoundRect(r, rad, rad, bg)
            }
            // 3dp left accent edge (follows the card's rounded corners)
            c.save()
            c.clipRect(0f, 0f, 3f * dd, h)
            bg.color = if (empty) Hw.withAlpha(accent, 90) else accent
            c.drawRoundRect(r, rad, rad, bg)
            c.restore()
            // glow + hairline
            bg.style = Paint.Style.STROKE
            if (active && !hint) {
                bg.strokeWidth = 4f * dd
                bg.color = Hw.withAlpha(accent, 50)
                r.set(2f * dd, 2f * dd, w - 2f * dd, h - 2f * dd)
                c.drawRoundRect(r, rad, rad, bg)
            }
            bg.strokeWidth = (if (hint) 2f else if (active) 1.5f else 1f) * dd
            bg.color = if (hint) Hw.MOD_FILTER else if (active) accent else Hw.PANEL_EDGE
            r.set(0f, 0f, w, h)
            r.inset(bg.strokeWidth / 2f, bg.strokeWidth / 2f)
            c.drawRoundRect(r, rad, rad, bg)
            bg.style = Paint.Style.FILL
            // slot number + live dot
            val padL = 10f * dd
            num.textSize = 10.5f * dd
            num.color = if (empty) Hw.INK_DIM else accent
            c.drawText(code, padL, 6f * dd + num.textSize * 0.85f, num)
            if (active) {
                bg.color = accent
                c.drawCircle(w - 9f * dd, 10f * dd, 3f * dd, bg)
            }
            if (memo) {
                memoP.textSize = 11f * dd
                c.drawText("≡", w - (if (active) 17f else 9f) * dd, 6f * dd + 11f * dd * 0.85f + dd, memoP)
            }
            // title (bottom aligned, up to two lines)
            txt.textSize = lineSize
            txt.color = if (empty) Hw.INK_DIM else Hw.SILK
            val bottom = h - 7f * dd
            if (line2.isEmpty()) {
                c.drawText(line1, padL, bottom, txt)
            } else {
                c.drawText(line1, padL, bottom - lineSize * 1.12f, txt)
                c.drawText(line2, padL, bottom, txt)
            }
        }

        override fun drawableStateChanged() { super.drawableStateChanged(); invalidate() }
    }
}
