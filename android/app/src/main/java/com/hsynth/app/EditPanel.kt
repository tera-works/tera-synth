package com.hsynth.app

import android.view.View
import android.widget.CompoundButton
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.SeekBar
import android.widget.Spinner
import android.widget.Switch
import android.widget.TextView
import kotlin.math.roundToInt

/**
 * Generic sound editor generated from the engine's parameter metadata, so new
 * engine parameters appear here without UI code changes.
 */
class EditPanel(private val a: MainActivity) {
    val view: LinearLayout
    private val content = Ui.column(a)
    private val pageButtons = mutableListOf<TextView>()
    private var page = 0
    private var layer = 0
    private val rows = mutableListOf<Row>()
    private val layerBar: LinearLayout
    private val layerButtons = mutableListOf<TextView>()

    private data class Page(val title: String, val perLayer: Boolean, val groups: (String) -> List<String>)

    private val pages = listOf(
        Page("Layers", false) { listOf("Layer A", "Layer B", "Layer C", "Master") },
        Page("VA", true) { l -> listOf("$l / VA") },
        Page("FM", true) { l -> listOf("$l / FM") },
        Page("PCM", true) { l -> listOf("$l / PCM") },
        Page("WT", true) { l -> listOf("$l / WT") },
        Page("Filter", false) { listOf("Filter") },
        Page("Env", false) { listOf("Amp Env", "Mod Env") },
        Page("LFO", false) { listOf("LFO") },
        Page("FX", false) { emptyList() },  // chain editor, built by buildFx()
        Page("FX Mod", false) { listOf("FX Mod") },
        Page("Mod", false) { listOf("Mod Matrix") },
    )

    /** One editable parameter row; keeps its widgets in sync with the engine. */
    private inner class Row(val meta: ParamMeta) {
        val valueText = Ui.text(a, "", 13f, Ui.ACCENT)
        var seek: SeekBar? = null
        var spinner: Spinner? = null
        var switch: Switch? = null
        var updating = false
        val view: View

        init {
            val d = { v: Int -> Ui.dp(a, v) }
            val label = Ui.text(a, meta.label, 14f)
            val line = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = android.view.Gravity.CENTER_VERTICAL }
            line.addView(label, LinearLayout.LayoutParams(d(150), -2))
            when (meta.kind) {
                "enum" -> {
                    // Spinners report their initial selection on first layout: ignore no-op changes
                    spinner = Ui.spinner(a, meta.options, 0) { pos ->
                        if (!updating && pos != NativeEngine.nativeGetParam(meta.id).roundToInt()) set(pos.toFloat())
                    }
                    line.addView(spinner, LinearLayout.LayoutParams(0, -2, 1f))
                }
                "bool" -> {
                    switch = Switch(a).apply {
                        setOnCheckedChangeListener { _: CompoundButton, c: Boolean -> if (!updating) set(if (c) 1f else 0f) }
                    }
                    line.addView(switch, LinearLayout.LayoutParams(0, -2, 1f))
                }
                else -> {
                    val steps = if (meta.kind == "int") (meta.max - meta.min).roundToInt().coerceAtLeast(1) else 1000
                    seek = Ui.seek(a, steps, 0) { p, user ->
                        if (user && !updating) set(if (meta.kind == "int") meta.min + p else meta.fromNormalized(p / 1000f))
                    }
                    line.addView(seek, LinearLayout.LayoutParams(0, d(40), 1f))
                    // double tap the value to reset to default
                    valueText.setOnClickListener { set(meta.def); refresh() }
                }
            }
            line.addView(valueText, LinearLayout.LayoutParams(d(96), -2).apply { marginStart = d(8) })
            view = line
            refresh()
        }

        fun set(v: Float) {
            NativeEngine.setParam(meta.id, v)
            valueText.text = meta.format(NativeEngine.nativeGetParam(meta.id))
            if (meta.name.endsWith(".type")) ui { buildPage() }
        }

        fun refresh() {
            val v = NativeEngine.nativeGetParam(meta.id)
            updating = true
            seek?.progress = if (meta.kind == "int") (v - meta.min).roundToInt() else (meta.toNormalized(v) * 1000).roundToInt()
            spinner?.setSelection(v.roundToInt(), false)
            switch?.isChecked = v > 0.5f
            valueText.text = meta.format(v)
            updating = false
        }
    }

    private fun ui(f: () -> Unit) = a.ui.post(f)

    init {
        val d = { v: Int -> Ui.dp(a, v) }
        val bar = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; setPadding(d(8), d(4), d(8), d(4)) }
        pages.forEachIndexed { i, p ->
            val b = Ui.text(a, p.title, 15f, Ui.DIM, bold = true).apply {
                setPadding(d(14), d(8), d(14), d(8))
                setOnClickListener { page = i; buildPage() }
            }
            pageButtons += b
            bar.addView(b)
        }
        layerBar = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; setPadding(d(8), 0, d(8), d(4)) }
        layerBar.addView(Ui.text(a, "Layer:", 14f, Ui.DIM))
        for (l in 0..2) {
            val b = Ui.text(a, "ABC"[l].toString(), 15f, Ui.DIM, bold = true).apply {
                setPadding(d(16), d(6), d(16), d(6))
                setOnClickListener { layer = l; buildPage() }
            }
            layerButtons += b
            layerBar.addView(b)
        }
        val scroll = ScrollView(a).apply { addView(content) }
        content.setPadding(d(16), d(4), d(16), d(24))
        view = Ui.column(a).apply {
            addView(HorizontalScrollView(a).apply { addView(bar) }, Ui.lp())
            addView(layerBar, Ui.lp())
            addView(scroll, Ui.lp(weight = 1f, height = 0).apply { width = -1 })
        }
        Params.addListener { buildPage() }  // layer types / sample names may have changed
        buildPage()
    }

    private fun header(text: String) = Ui.text(a, text, 15f, Ui.ACCENT, bold = true).apply {
        setPadding(0, Ui.dp(a, 14), 0, Ui.dp(a, 4))
    }

    fun buildPage() {
        val p = pages[page]
        pageButtons.forEachIndexed { i, b -> b.setTextColor(if (i == page) Ui.ACCENT else Ui.DIM) }
        layerBar.visibility = if (p.perLayer) View.VISIBLE else View.GONE
        layerButtons.forEachIndexed { i, b -> b.setTextColor(if (i == layer) Ui.ACCENT else Ui.DIM) }
        content.removeAllViews()
        rows.clear()
        val layerName = "Layer " + "ABC"[layer]
        if (p.title == "FX") {
            buildFx()
            return
        }
        if (p.perLayer) {
            val typeMeta = Params.meta.first { it.name == "${"abc"[layer]}.type" }
            val type = typeMeta.options.getOrElse(NativeEngine.nativeGetParam(typeMeta.id).roundToInt()) { "?" }
            val expected = p.title.lowercase()
            val note = if (type != expected) "  (layer $layerName is currently '$type' — set Type below to '$expected' to hear these)" else ""
            content.addView(Ui.text(a, "$layerName engine: $type$note", 13f, if (note.isEmpty()) Ui.DIM else Ui.WARN))
            val r = Row(typeMeta)
            rows += r
            content.addView(r.view)
            if (p.title == "WT") {
                val name = NativeEngine.nativeSampleSetName(layer).ifEmpty { "(none)" }
                content.addView(Ui.row(a,
                    Ui.text(a, "User table: $name", 14f),
                    Ui.button(a, "Load .wav…") { a.libraryPanel.pickWavetable(layer) },
                    Ui.button(a, "Unload") { NativeEngine.nativeUnloadSamples(layer); buildPage() },
                ))
            }
            if (p.title == "PCM") {
                val name = NativeEngine.nativeSampleSetName(layer).ifEmpty { "(none)" }
                content.addView(Ui.row(a,
                    Ui.text(a, "Samples: $name", 14f),
                    Ui.button(a, "Choose…") { a.libraryPanel.pickSamplesFor(layer) },
                    Ui.button(a, "Unload") { NativeEngine.nativeUnloadSamples(layer); buildPage() },
                ))
            }
        }
        for (g in p.groups(layerName)) {
            content.addView(header(g))
            val metas = Params.meta.filter { it.group == g }
            if (g == "Mod Matrix") {
                content.addView(Ui.text(a, "out += source × via × amount   (via 'none' = 1)", 12f, Ui.DIM))
            }
            for (m in metas) {
                if (p.perLayer && m.name.endsWith(".type")) continue
                val r = Row(m)
                rows += r
                content.addView(r.view)
                if (g == "Mod Matrix" && m.name.endsWith(".amt")) content.addView(View(a), LinearLayout.LayoutParams(-1, Ui.dp(a, 10)))
            }
        }
    }

    fun refreshValues() {
        if (rows.isEmpty()) return
        rows.forEach { it.refresh() }
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
            t.text = "Looper: ${names.getOrElse(s[0].toInt()) { "?" }}   %.1f / %.1f s".format(s[2], s[1])
            a.ui.postDelayed(this, 250)
        }
    }

    private fun buildFx() {
        val d = { v: Int -> Ui.dp(a, v) }
        val slots = (1..12).map { Params.id("fx.slot$it") }
        val blocks = slots.map { NativeEngine.nativeGetParam(it).roundToInt() }
        val titles = blockKeys.indices.map { blockTitle(it) }
        content.addView(header("Effect chain (top to bottom)"))
        val seen = mutableSetOf<Int>()
        for (s in slots.indices) {
            val b = blocks[s]
            val line = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = android.view.Gravity.CENTER_VERTICAL }
            line.addView(Ui.text(a, "${s + 1}", 14f, Ui.DIM), LinearLayout.LayoutParams(d(28), -2))
            val sp = Ui.spinner(a, titles, b) { pos ->
                if (pos != NativeEngine.nativeGetParam(slots[s]).roundToInt()) {
                    NativeEngine.setParam(slots[s], pos.toFloat())
                    if (pos > 0) Params.set("fx.${blockKeys[pos]}.on", 1f)
                    ui { buildPage() }
                }
            }
            line.addView(sp, LinearLayout.LayoutParams(0, -2, 1f))
            if (b > 0) {
                val onId = Params.id("fx.${blockKeys[b]}.on")
                line.addView(Switch(a).apply {
                    isChecked = NativeEngine.nativeGetParam(onId) > 0.5f
                    setOnCheckedChangeListener { _: CompoundButton, c: Boolean -> NativeEngine.setParam(onId, if (c) 1f else 0f) }
                })
                if (!seen.add(b)) line.addView(Ui.text(a, " dup", 12f, Ui.WARN))
            }
            fun swap(o: Int) {
                NativeEngine.setParam(slots[s], blocks[o].toFloat())
                NativeEngine.setParam(slots[o], blocks[s].toFloat())
                buildPage()
            }
            line.addView(Ui.button(a, "▲") { if (s > 0) swap(s - 1) })
            line.addView(Ui.button(a, "▼") { if (s < slots.size - 1) swap(s + 1) })
            content.addView(line)
        }
        val running = NativeEngine.nativeFxChain().joinToString(" > ") { blockTitle(it) }.ifEmpty { "(dry)" }
        content.addView(Ui.text(a, "running: $running", 12f, Ui.DIM))

        content.addView(header("Tempo / macros"))
        for (n in listOf("fx.tempo", "fx.retrig", "fx.macro1", "fx.macro2")) {
            val r = Row(Params.meta.first { it.name == n })
            rows += r
            content.addView(r.view)
        }

        val shown = mutableSetOf<Int>()
        for (s in slots.indices) {
            val b = blocks[s]
            if (b <= 0 || !shown.add(b)) continue
            content.addView(header("${s + 1}. ${blockTitle(b)}"))
            val group = "FX / ${blockTitle(b)}"
            for (m in Params.meta.filter { it.group == group }) {
                val r = Row(m)
                rows += r
                content.addView(r.view)
            }
            val key = blockKeys[b]
            if (key == "cab" || key == "convo") {
                val target = if (key == "cab") 0 else 1
                val name = NativeEngine.nativeImpulseName(target).ifEmpty { "(none)" }
                content.addView(Ui.row(a,
                    Ui.text(a, "User IR: $name", 14f),
                    Ui.button(a, "Load IR…") { a.libraryPanel.pickImpulse(target) },
                    Ui.button(a, "Unload") { NativeEngine.nativeUnloadImpulse(target); buildPage() },
                ))
            }
            if (key == "vocoder") {
                content.addView(Ui.text(a, "The vocoder needs an audio input (microphone). Input capture is " +
                    "not wired up in the Android app yet; it works in the desktop build.", 12f, Ui.WARN))
            }
        }

        content.addView(header("Looper"))
        val status = Ui.text(a, "", 14f)
        looperText = status
        content.addView(status)
        content.addView(Ui.row(a,
            Ui.button(a, "Rec / Play / Dub") { NativeEngine.nativeLooperCommand(NativeEngine.Looper.TAP) },
            Ui.button(a, "Stop") { NativeEngine.nativeLooperCommand(NativeEngine.Looper.STOP) },
            Ui.button(a, "Undo") { NativeEngine.nativeLooperCommand(NativeEngine.Looper.UNDO) },
            Ui.button(a, "Clear") { NativeEngine.nativeLooperCommand(NativeEngine.Looper.CLEAR) },
        ))
        val lvl = Row(Params.meta.first { it.name == "looper.level" })
        rows += lvl
        content.addView(lvl.view)
        a.ui.removeCallbacks(looperTick)
        a.ui.post(looperTick)
    }
}
