package com.hsynth.app

import android.annotation.SuppressLint
import android.app.AlertDialog
import android.content.Context
import android.content.Intent
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.drawable.ColorDrawable
import android.graphics.drawable.GradientDrawable
import android.text.InputType
import android.text.TextUtils
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.BaseAdapter
import android.widget.EditText
import android.widget.GridView
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.TextView
import org.json.JSONObject
import java.io.File

/**
 * Library screen: presets (load / save / import / export), sample sources
 * (WAV / SFZ import via the Storage Access Framework). Obsidian browser:
 * one compact filter + search + actions row, category chips in their own
 * colors, a 3-column grid of color-coded preset cards and a collapsible
 * samples column on the right.
 */
class LibraryPanel(private val a: MainActivity) {
    companion object {
        const val REQ_WAV = 101
        const val REQ_SFZ_TREE = 102
        const val REQ_PRESET_IMPORT = 103
        const val REQ_PRESET_EXPORT = 104
        const val REQ_IR_CAB = 105
        const val REQ_IR_REVERB = 106
        const val REQ_WAVETABLE = 107
        /** Phase 6: "+ IMPORT" - any supported file, routed by extension. */
        const val REQ_IMPORT_ANY = 174
        private val SOUNDBANK_EXTS = setOf("sf2", "sf3", "dls", "gig")
    }

    val view: LinearLayout
    private val presetGrid: GridView
    private val presetEmpty: TextView
    private val sampleListView: ListView
    private val sampleEmpty: TextView
    private var filteredPresets: List<Library.Entry> = emptyList()
    private var displayedSamples: List<Library.Entry> = emptyList()
    private lateinit var presetAdapter: BaseAdapter
    private lateinit var sampleAdapter: BaseAdapter
    /** MIDI files: app-specific external storage, so they can be copied in over USB (adb push) without permissions. Played on the Player tab. */
    val songDir: File = (a.getExternalFilesDir("midi") ?: File(a.filesDir, "midi")).apply { mkdirs() }
    private var presetFilter = 0  // 0 all, 1 factory, 2 user, 3 favourites
    private var category = ""     // "" = every category
    private var search = ""
    private val categoryRow = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
    private val favorites: MutableSet<String> = (a.prefs.getStringSet("favorites", emptySet()) ?: emptySet()).toMutableSet()
    private val samplesColumn: LinearLayout
    private var samplesShown = a.prefs.getBoolean("libSamplesShown", true)
    private val samplesToggle: LedButton

    /** Compact Obsidian button for the dense landscape rows. */
    private fun compact(b: LedButton, size: Float = 12.5f): LedButton {
        val d = { v: Int -> Ui.dp(a, v) }
        b.textSize = size
        b.minHeight = d(32)
        b.minimumHeight = d(32)
        b.setPadding(b.paddingLeft, d(4), d(10), d(4))
        return b
    }

    init {
        val d = { v: Int -> Ui.dp(a, v) }
        val portrait = Ui.portrait(a)

        // ---- filter pills + search + actions (one row in landscape)
        val filters = mutableListOf<LedButton>()
        val pills = listOf("All" to Hw.MOD_OSC, "Factory" to Hw.MOD_OSC, "User" to Hw.MOD_OSC, "★" to Hw.MOD_FILTER)
        val pillViews = pills.mapIndexed { i, (t, c) ->
            compact(LedButton(a, t, tr = t != "★", led = false, accentColor = c)).apply {
                lit = i == presetFilter
                // a color span survives LedButton's touch / lit text color resets
                if (t == "★") text = android.text.SpannableString("★").apply {
                    setSpan(android.text.style.ForegroundColorSpan(Hw.MOD_FILTER), 0, length, android.text.Spannable.SPAN_EXCLUSIVE_EXCLUSIVE)
                }
                setOnClickListener {
                    presetFilter = i
                    filters.forEachIndexed { k, x -> x.lit = k == i }
                    refreshPresets()
                }
            }.also { filters += it }
        }
        val searchBox = EditText(a).apply {
            hint = L10n.t("Search name / category / tag")
            textSize = 13f
            setTextColor(Hw.LCD_FG)
            setHintTextColor(Hw.INK_DIM)
            typeface = Hw.MONO
            background = Hw.lcdBg(a)
            setPadding(d(10), d(5), d(10), d(5))
            minHeight = d(32)
            minimumHeight = d(32)
            isSingleLine = true
            imeOptions = android.view.inputmethod.EditorInfo.IME_ACTION_DONE or android.view.inputmethod.EditorInfo.IME_FLAG_NO_EXTRACT_UI
            addTextChangedListener(object : android.text.TextWatcher {
                override fun beforeTextChanged(s: CharSequence?, st: Int, c: Int, af: Int) {}
                override fun onTextChanged(s: CharSequence?, st: Int, b: Int, c: Int) {}
                override fun afterTextChanged(s: android.text.Editable?) { search = s?.toString()?.trim() ?: ""; refreshPresets() }
            })
        }
        // "+ IMPORT" takes presets (.json) AND every sound format (.sf2 .sf3 .dls .gig .dspreset .dslibrary .syx .wt .vitaltable .wav)
        val actions = listOf<Triple<String, Boolean, () -> Unit>>(
            Triple("Save", true) { promptSave() },
            Triple("Export", true) { export() },
            Triple(L10n.t("+ IMPORT"), false) { importAny() },
            Triple(L10n.t("⬇ PACKS"), false) { a.openSheet(a.soundPackSheet) },
        ).map { (t, tr, f) ->
            compact(LedButton(a, t, tr = tr, led = false, accentColor = if (tr) Hw.MOD_OSC else Hw.MOD_FX), 12f).apply {
                setPadding(d(8), d(4), d(8), d(4))
                setOnClickListener { f() }
            }
        }

        val head: View = if (!portrait) LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            for (b in pillViews) addView(b, LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(4) })
            addView(searchBox, LinearLayout.LayoutParams(0, -2, 1f).apply { marginStart = d(4); marginEnd = d(4) })
            for (b in actions) addView(b, LinearLayout.LayoutParams(-2, -2).apply { marginStart = d(4) })
        } else Ui.column(a).apply {
            addView(FlowLayout(a, d(4), d(4)).apply {
                for (b in pillViews) addView(b, ViewGroup.LayoutParams(-2, -2))
                for (b in actions) addView(b, ViewGroup.LayoutParams(-2, -2))
            }, LinearLayout.LayoutParams(-1, -2))
            addView(searchBox, LinearLayout.LayoutParams(-1, -2).apply { topMargin = d(6) })
        }

        // ---- category chips + samples column toggle
        samplesToggle = compact(LedButton(a, "Samples", accentColor = Hw.MOD_OSC), 12f).apply {
            lit = samplesShown
            setOnClickListener { setSamplesShown(!samplesShown) }
        }
        val catLine = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(HorizontalScrollView(a).apply {
                isHorizontalScrollBarEnabled = false
                addView(categoryRow)
            }, LinearLayout.LayoutParams(0, -2, 1f))
            addView(samplesToggle, LinearLayout.LayoutParams(-2, -2).apply { marginStart = d(6) })
        }

        presetEmpty = Ui.text(a, "(no presets match)", 13f, Hw.INK_DIM).apply {
            visibility = View.GONE
            setPadding(d(4), d(12), d(4), d(4))
        }
        presetAdapter = object : BaseAdapter() {
            override fun getCount() = filteredPresets.size
            override fun getItem(pos: Int) = filteredPresets[pos]
            override fun getItemId(pos: Int) = pos.toLong()
            override fun getView(pos: Int, convertView: View?, parent: ViewGroup): View {
                val h = (convertView?.tag as? PresetHolder) ?: createPresetHolder()
                bindPresetHolder(h, filteredPresets[pos])
                return h.card
            }
        }
        presetGrid = GridView(a).apply {
            numColumns = gridColumns()
            horizontalSpacing = d(6)
            verticalSpacing = d(6)
            stretchMode = GridView.STRETCH_COLUMN_WIDTH
            isVerticalScrollBarEnabled = false
            selector = ColorDrawable(Color.TRANSPARENT)
            adapter = presetAdapter
        }

        val presetsColumn = Ui.column(a).apply {
            addView(head, LinearLayout.LayoutParams(-1, -2))
            addView(catLine, LinearLayout.LayoutParams(-1, -2).apply { topMargin = d(6) })
            addView(presetEmpty, LinearLayout.LayoutParams(-1, -2))
            addView(presetGrid, LinearLayout.LayoutParams(-1, 0, 1f).apply { topMargin = d(6) })
        }

        // ---- samples column (recycled ListView so 100s of SFZ/WAV never stall UI)
        val samples = Hw.Section(a, "Samples (WAV / SFZ)", accent = Hw.MOD_OSC)
        samples.body.addView(LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            val bs = listOf<Pair<String, () -> Unit>>(
                "+ WAV" to { open(REQ_WAV, "audio/*", "audio/wav", "audio/x-wav") },
                "● SAMPLE" to { a.openSampleWizard() },   // B2 live sampling wizard
                "+ SFZ" to {
                    @Suppress("DEPRECATION")
                    a.startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQ_SFZ_TREE)
                },
                "SFZ info" to { showSfzInfo() },
            )
            bs.forEachIndexed { i, (t, f) ->
                addView(compact(LedButton(a, t, led = false), 12f).apply {
                    setPadding(d(4), d(4), d(4), d(4))
                    setOnClickListener { f() }
                }, LinearLayout.LayoutParams(0, -2, 1f).apply { if (i < bs.size - 1) marginEnd = d(4) })
            }
        }, LinearLayout.LayoutParams(-1, -2))
        sampleEmpty = Ui.text(a, "(no samples yet)", 13f, Ui.DIM).apply {
            visibility = View.GONE
            setPadding(d(4), d(8), d(4), d(4))
        }
        sampleAdapter = object : BaseAdapter() {
            override fun getCount() = displayedSamples.size
            override fun getItem(pos: Int) = displayedSamples[pos]
            override fun getItemId(pos: Int) = pos.toLong()
            override fun getView(pos: Int, convertView: View?, parent: ViewGroup): View {
                val h = (convertView?.tag as? SampleHolder) ?: createSampleHolder()
                bindSampleHolder(h, displayedSamples[pos])
                return h.card
            }
        }
        sampleListView = ListView(a).apply {
            divider = ColorDrawable(Color.TRANSPARENT)
            dividerHeight = d(4)
            isVerticalScrollBarEnabled = false
            selector = ColorDrawable(Color.TRANSPARENT)
            adapter = sampleAdapter
        }
        samples.body.addView(sampleEmpty, LinearLayout.LayoutParams(-1, -2))
        samples.body.addView(sampleListView, LinearLayout.LayoutParams(-1, 0, 1f).apply { topMargin = d(6) })
        samplesColumn = samples

        view = if (portrait) LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Hw.CHASSIS)
            setPadding(d(8), d(6), d(8), d(6))
            addView(presetsColumn, LinearLayout.LayoutParams(-1, 0, 2f).apply { bottomMargin = d(8) })
            addView(samples, LinearLayout.LayoutParams(-1, 0, 1f))
        } else LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            setBackgroundColor(Hw.CHASSIS)
            setPadding(d(8), d(6), d(8), d(6))
            addView(presetsColumn, LinearLayout.LayoutParams(0, -1, 7f))
            addView(samples, LinearLayout.LayoutParams(0, -1, 3f).apply { marginStart = d(8) })
        }
        // the section body stretches so its list can scroll
        samples.body.layoutParams = LinearLayout.LayoutParams(-1, 0, 1f)
        samples.visibility = if (samplesShown) View.VISIBLE else View.GONE
        a.addPresetListener { presetAdapter.notifyDataSetChanged() }
    }

    private fun gridColumns(): Int = if (Ui.portrait(a)) 2 else if (samplesShown) 3 else 4

    private fun setSamplesShown(on: Boolean) {
        samplesShown = on
        samplesToggle.lit = on
        samplesColumn.visibility = if (on) View.VISIBLE else View.GONE
        a.prefs.edit().putBoolean("libSamplesShown", on).apply()
        presetGrid.numColumns = gridColumns()
    }

    /** Song list now lives on the Player tab. */
    fun refreshSongs() {}

    @Suppress("unused")
    private fun playSong(f: File) {
        a.selectTab(5)
        a.player.playFile(f)
    }

    private var presetEntries: List<Library.Entry> = emptyList()
    private var sampleEntries: List<Library.Entry>? = null
    private var scanning = false

    /** Presets from the last scan (scans now if there was none). */
    fun presetEntries(): List<Library.Entry> = presetEntries.ifEmpty { a.library.presets().also { presetEntries = it } }

    /** Rescans in the background (thousands of sample files); the lists update when done. */
    fun refresh() {
        if (scanning) return
        scanning = true
        a.loader.execute {
            val p = a.library.presets()
            val s = a.library.sampleSources()
            a.ui.post {
                scanning = false
                if (p != presetEntries) { presetEntries = p; refreshPresets() }
                if (s != sampleEntries) { sampleEntries = s; buildSamples() }
            }
        }
    }

    /** From the last scan (no file access): filter / highlight changes are instant. */
    private fun matches(e: Library.Entry): Boolean {
        if (presetFilter == 1 && e.user || presetFilter == 2 && !e.user) return false
        if (presetFilter == 3 && e.file.path !in favorites) return false
        if (category.isNotEmpty() && e.category != category) return false
        if (search.isEmpty()) return true
        val hay = (e.title + " " + e.category + " " + e.tags.joinToString(" ") + " " + e.comment).lowercase()
        return search.lowercase().split(Regex("\\s+")).all { it in hay }
    }

    /** Category chips in their category colors, with counts (from the presets the other filters leave). */
    private fun refreshCategories() {
        val d = { v: Int -> Ui.dp(a, v) }
        categoryRow.removeAllViews()
        val counts = presetEntries.filter { e ->
            !(presetFilter == 1 && e.user || presetFilter == 2 && !e.user || presetFilter == 3 && e.file.path !in favorites)
        }.groupingBy { it.category.ifEmpty { "—" } }.eachCount().toSortedMap()
        fun chip(label: String, key: String, color: Int) = compact(LedButton(a, label, tr = false, accentColor = color), 11.5f).apply {
            minHeight = d(28)
            minimumHeight = d(28)
            lit = category == key
            setOnClickListener { category = if (category == key) "" else key; refreshPresets() }
        }
        categoryRow.addView(chip(L10n.t("All") + " ${counts.values.sum()}", "", Hw.MOD_OSC), LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(4) })
        for ((c, n) in counts) {
            val key = if (c == "—") "" else c
            categoryRow.addView(chip("$c $n", key, if (key.isEmpty()) Hw.INK_DIM else Hw.categoryColor(c)),
                LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(4) })
        }
    }

    private fun refreshPresets() {
        refreshCategories()
        filteredPresets = presetEntries.filter { matches(it) }
        presetEmpty.visibility = if (filteredPresets.isEmpty() && presetEntries.isNotEmpty()) View.VISIBLE else View.GONE
        presetAdapter.notifyDataSetChanged()
    }

    private fun toggleFavorite(e: Library.Entry): Boolean {
        val fav = e.file.path in favorites
        if (fav) favorites.remove(e.file.path) else favorites.add(e.file.path)
        a.prefs.edit().putStringSet("favorites", favorites.toSet()).apply()
        return !fav
    }

    private class PresetHolder(
        val card: LibraryCard,
        val title: TextView,
        val cat: TextView,
        val badge: TextView,
        val badgeBg: GradientDrawable,
        val tags: TextView,
        val star: TextView
    )

    private fun createPresetHolder(): PresetHolder {
        val d = { v: Int -> Ui.dp(a, v) }
        val card = LibraryCard(a, Hw.MOD_OSC, false)
        val texts = Ui.column(a)
        val title = TextView(a).apply {
            textSize = 13.5f
            typeface = Hw.BOLD
            maxLines = 1
            ellipsize = TextUtils.TruncateAt.END
        }
        texts.addView(title)
        val sub = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        val cat = TextView(a).apply {
            textSize = 10.5f
            typeface = Hw.BOLD
            maxLines = 1
        }
        sub.addView(cat, LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(5) })
        val badgeBg = GradientDrawable().apply { cornerRadius = Hw.dp(a, 3f) }
        val badge = TextView(a).apply {
            textSize = 8.5f
            typeface = Hw.BOLD
            letterSpacing = 0.08f
            background = badgeBg
            setPadding(d(4), 0, d(4), 0)
            maxLines = 1
        }
        sub.addView(badge, LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(5) })
        val tags = TextView(a).apply {
            textSize = 10f
            typeface = Hw.LABEL
            setTextColor(Hw.INK_DIM)
            maxLines = 1
            ellipsize = TextUtils.TruncateAt.END
        }
        sub.addView(tags, LinearLayout.LayoutParams(0, -2, 1f))
        texts.addView(sub, LinearLayout.LayoutParams(-1, -2).apply { topMargin = d(2) })
        card.addView(texts, LinearLayout.LayoutParams(0, -2, 1f))

        val star = TextView(a).apply {
            textSize = 17f
            gravity = Gravity.CENTER
            isClickable = true
        }
        card.addView(star, LinearLayout.LayoutParams(d(30), d(40)))
        val holder = PresetHolder(card, title, cat, badge, badgeBg, tags, star)
        card.tag = holder
        return holder
    }

    private fun bindPresetHolder(h: PresetHolder, e: Library.Entry) {
        val catColor = if (e.category.isEmpty()) Hw.INK_DIM else Hw.categoryColor(e.category)
        val active = e.file == a.currentPreset
        h.card.setStyle(catColor, active)
        h.title.text = e.title
        h.title.setTextColor(if (active) Color.WHITE else Hw.SILK)
        if (e.category.isNotEmpty()) {
            h.cat.visibility = View.VISIBLE
            h.cat.text = e.category
            h.cat.setTextColor(catColor)
        } else {
            h.cat.visibility = View.GONE
        }
        h.badge.text = L10n.t(if (e.user) "User" else "Factory").let { if (it.any { c -> c.code > 0x2FF }) it else it.uppercase() }
        h.badge.setTextColor(if (e.user) Hw.MOD_GOOD else Hw.ENGRAVE)
        h.badgeBg.setStroke(1, if (e.user) Hw.withAlpha(Hw.MOD_GOOD, 140) else Hw.PANEL_EDGE)
        if (e.tags.isNotEmpty()) {
            h.tags.visibility = View.VISIBLE
            h.tags.text = e.tags.joinToString(" ") { "#$it" }
        } else {
            h.tags.visibility = View.GONE
        }
        val fav = e.file.path in favorites
        h.star.text = if (fav) "★" else "☆"
        h.star.setTextColor(if (fav) Hw.MOD_FILTER else Hw.INK_DIM)
        h.star.setOnClickListener {
            val on = toggleFavorite(e)
            if (presetFilter == 3) refreshPresets()
            else {
                h.star.text = if (on) "★" else "☆"
                h.star.setTextColor(if (on) Hw.MOD_FILTER else Hw.INK_DIM)
            }
        }
        h.card.setOnClickListener { a.loadPreset(e.file) }
        h.card.setOnLongClickListener { presetMenu(e); true }
    }

    /** Long press on a preset: favourite on / off, delete (user presets). */
    private fun presetMenu(e: Library.Entry) {
        val fav = e.file.path in favorites
        val items = mutableListOf(L10n.t(if (fav) "Remove from favourites" else "Add to favourites"))
        if (e.user) items += L10n.t("Delete")
        AlertDialog.Builder(a).setTitle(e.title).setItems(items.toTypedArray()) { _, w ->
            if (w == 0) {
                toggleFavorite(e)
                refreshPresets()
            } else confirmDelete(e.file)
        }.show()
    }

    private fun refreshSamples() = refresh()

    private class SampleHolder(val card: LibraryCard, val name: TextView, val sub: TextView, val ext: TextView)

    private fun createSampleHolder(): SampleHolder {
        val d = { v: Int -> Ui.dp(a, v) }
        val row = LibraryCard(a, Hw.MOD_OSC, false, compact = true)
        val name = TextView(a).apply {
            textSize = 12.5f
            typeface = Hw.BOLD
            setTextColor(Hw.SILK)
            maxLines = 1
            ellipsize = TextUtils.TruncateAt.MIDDLE
        }
        val sub = TextView(a).apply {
            textSize = 10f
            typeface = Hw.LABEL
            setTextColor(Hw.INK_DIM)
            maxLines = 1
            ellipsize = TextUtils.TruncateAt.END
        }
        row.addView(Ui.column(a).apply { addView(name); addView(sub) }, LinearLayout.LayoutParams(0, -2, 1f))
        val ext = TextView(a).apply {
            textSize = 9f
            typeface = Hw.BOLD
            letterSpacing = 0.08f
        }
        row.addView(ext, LinearLayout.LayoutParams(-2, -2).apply { marginStart = d(4); marginEnd = d(4) })
        val holder = SampleHolder(row, name, sub, ext)
        row.tag = holder
        return holder
    }

    /** Badge text + colour of a sample-column row by file type. */
    private fun kindOf(f: File): Pair<String, Int> {
        val ext = f.extension.lowercase()
        return when {
            ext == "sfz" -> "SFZ" to Hw.MOD_OSC
            ext in SOUNDBANK_EXTS -> ext.uppercase() to Hw.MOD_ENV
            ext == "dspreset" || ext == "dslibrary" -> "DS" to Hw.MOD_FILTER
            ext == "syx" -> "DX7" to Hw.MOD_FILTER
            ext == "wt" || ext == "vitaltable" || isWavetableWav(f) -> "WT" to Hw.MOD_GOOD
            ext == "flac" -> "FLAC" to Hw.MOD_FX
            else -> "WAV" to Hw.MOD_FX
        }
    }

    private fun isWavetableWav(f: File): Boolean =
        f.extension.equals("wav", true) && f.path.replace('\\', '/').contains("/library/user/wt/")

    private fun bindSampleHolder(h: SampleHolder, e: Library.Entry) {
        val (label, col) = kindOf(e.file)
        h.card.setStyle(col, false)
        h.name.text = e.file.name
        h.sub.text = e.title + if (e.user) L10n.t("  · long-press to delete") else ""
        h.ext.text = label
        h.ext.setTextColor(col)
        h.card.setOnClickListener { openSampleEntry(e.file) }
        if (e.user) h.card.setOnLongClickListener { confirmDelete(e.file); true }
        else h.card.setOnLongClickListener(null)
    }

    /** Tap on a sample row: plain samples pick a layer, every other format opens its importer sheet. */
    private fun openSampleEntry(f: File) {
        val ext = f.extension.lowercase()
        val formatted = ext in SOUNDBANK_EXTS || ext == "dspreset" || ext == "dslibrary" || ext == "syx" ||
            ext == "wt" || ext == "vitaltable" || isWavetableWav(f)
        if (formatted && a.formatImporter.open(f)) return
        chooseLayer(f)
    }

    private fun buildSamples() {
        displayedSamples = sampleEntries ?: emptyList()
        sampleEmpty.visibility = if (displayedSamples.isEmpty()) View.VISIBLE else View.GONE
        sampleAdapter.notifyDataSetChanged()
    }

    private fun chooseLayer(file: File) {
        AlertDialog.Builder(a)
            .setTitle(L10n.f("Load %s into…", file.name))
            .setItems(arrayOf("Layer A", "Layer B", "Layer C").map { L10n.t(it) }.toTypedArray()) { _, which -> a.loadSamples(which, file) }
            .show()
    }

    private var wavetableLayer = 0

    /** Pick a WAV wavetable for a layer (becomes its "user" table). */
    fun pickWavetable(layer: Int) {
        wavetableLayer = layer
        open(REQ_WAVETABLE, "audio/*", "audio/wav", "audio/x-wav")
    }

    /** Pick a WAV impulse response for the cabinet (0) or IR reverb (1). */
    fun pickImpulse(target: Int) = open(if (target == 0) REQ_IR_CAB else REQ_IR_REVERB, "audio/*", "audio/wav", "audio/x-wav")

    fun pickSamplesFor(layer: Int) {
        val sources = (sampleEntries ?: a.library.sampleSources()).filter { e ->
            val x = e.file.extension.lowercase()
            (x == "sfz" || x == "wav" || x == "flac") && !isWavetableWav(e.file)
        }
        if (sources.isEmpty()) {
            a.log("no samples in the library yet — import WAV or SFZ on the Library tab", Ui.WARN)
            return
        }
        AlertDialog.Builder(a)
            .setTitle(L10n.f("Samples for layer %s", "ABC"[layer].toString()))
            .setItems(sources.map { it.title }.toTypedArray()) { _, which -> a.loadSamples(layer, sources[which].file) }
            .show()
    }

    // ------------------------------------------------------------------ Phase 6: "+ IMPORT" and installed packs

    private fun importAny() {
        ActivityResults.register(REQ_IMPORT_ANY) { data -> data?.data?.let { routeImport(it) } }
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"   // .sf2 .sf3 .dls .gig .dspreset .dslibrary .syx .wt .vitaltable have no registered MIME type
        }
        @Suppress("DEPRECATION")
        a.startActivityForResult(intent, REQ_IMPORT_ANY)
    }

    private fun routeImport(uri: android.net.Uri) {
        val name = FormatFiles.displayName(a, uri)
        when (name.substringAfterLast('.', "").lowercase()) {
            "json" -> onActivityResult(REQ_PRESET_IMPORT, Intent().setData(uri))
            "wav" -> onActivityResult(REQ_WAV, Intent().setData(uri))   // plain sample (wavetables: EDIT > OSC > WT)
            "sfz" -> a.log("An .sfz needs its samples: use + SFZ to import the whole folder", Ui.WARN)
            else -> a.formatImporter.handle(uri)   // sf2 sf3 dls gig dspreset dslibrary syx wt vitaltable
        }
    }

    /**
     * SoundPackSheet OPEN: open the first loadable file of an installed pack
     * (SF2/SF3/DLS/GIG -> bank picker, .syx -> DX7 browser, .dspreset/.dslibrary/.wt/.vitaltable -> [FormatImporter],
     * .sfz -> Layer A PCM + back to Stage, plain WAV/FLAC -> the LIB sample list).
     */
    fun openInstalledPack(pack: SoundPack, dir: File) {
        val order = listOf("sf2", "sf3", "dls", "gig", "dspreset", "dslibrary", "sfz", "syx", "wt", "vitaltable", "wav", "flac")
        val listed = a.soundPackSheet.catalog.installedInfo(pack)?.primaryFiles().orEmpty()
        val candidates = listed.ifEmpty {
            dir.walkTopDown().maxDepth(4).filter { it.isFile && !it.name.startsWith(".") }.toList()
        }.filter { it.extension.lowercase() in order }
        val first = candidates.minWithOrNull(
            compareBy<File> { if (pack.format.equals(it.extension, true)) -1 else order.indexOf(it.extension.lowercase()) }
                .thenBy { it.path.lowercase() })
        if (first == null) {
            a.log("${pack.name}: no loadable file found in ${dir.name}", Ui.WARN)
            return
        }
        refresh()
        when (first.extension.lowercase()) {
            "sfz" -> {
                a.selectTab(0)
                a.loadSamples(0, first) { a.log("${pack.name}: ${first.name} -> layer A") }
            }
            "wav", "flac" -> {
                if (isWavetableWav(first)) a.formatImporter.open(first)
                else {
                    a.selectTab(3)
                    a.log("${pack.name}: samples are listed under Samples in LIB")
                }
            }
            else -> if (!a.formatImporter.open(first)) a.log("${pack.name}: cannot open ${first.name}", Ui.WARN)
        }
    }

    private fun confirmDelete(file: File) {
        AlertDialog.Builder(a)
            .setTitle(L10n.f("Delete %s?", file.name))
            .setMessage(L10n.t(if (file.extension == "sfz") "The .sfz file is removed; its sample folder is kept." else "This removes the file from the app library."))
            .setPositiveButton(L10n.t("Delete")) { _, _ ->
                if (file.delete()) a.log("deleted ${file.name}") else a.log("could not delete ${file.name}", Ui.ERR)
                refresh()
            }
            .setNegativeButton(L10n.t("Cancel"), null)
            .show()
    }

    private fun promptSave() {
        val input = EditText(a).apply {
            inputType = InputType.TYPE_CLASS_TEXT
            setText(a.currentPresetName)
            setSelectAllOnFocus(true)
        }
        AlertDialog.Builder(a)
            .setTitle(L10n.t("Save preset"))
            .setView(input)
            .setPositiveButton(L10n.t("Save")) { _, _ ->
                val name = input.text.toString().trim().ifEmpty { "Untitled" }
                a.savePreset(name) { refresh() }
            }
            .setNegativeButton(L10n.t("Cancel"), null)
            .show()
    }

    private fun open(req: Int, vararg mimes: String) {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = mimes.first()
            putExtra(Intent.EXTRA_MIME_TYPES, mimes)
            putExtra(Intent.EXTRA_ALLOW_MULTIPLE, req == REQ_WAV)
        }
        @Suppress("DEPRECATION")
        a.startActivityForResult(intent, req)
    }

    private fun export() {
        val intent = Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "application/json"
            putExtra(Intent.EXTRA_TITLE, a.library.safeName(a.currentPresetName) + ".json")
        }
        @Suppress("DEPRECATION")
        a.startActivityForResult(intent, REQ_PRESET_EXPORT)
    }

    private fun showSfzInfo() {
        AlertDialog.Builder(a)
            .setTitle(L10n.t("SFZ support"))
            .setMessage(L10n.f("Supported headers: <control> <global> <master> <group> <region>, #define, #include.\n\n" +
                "Supported opcodes:\n%s\n\nAll other opcodes are ignored and listed as warnings in the message log when an instrument is loaded. " +
                "Samples must be WAV (8/16/24/32-bit PCM or float). FLAC is planned.", NativeEngine.nativeSupportedSfzOpcodes()))
            .setPositiveButton("OK", null)
            .show()
    }

    fun onActivityResult(requestCode: Int, data: Intent?) {
        data ?: return
        when (requestCode) {
            REQ_WAVETABLE -> {
                val uri = data.data ?: return
                val layer = wavetableLayer
                a.loader.execute {
                    try {
                        val f = a.library.importWav(uri)
                        val r = JSONObject(NativeEngine.nativeLoadWavetable(layer, f.absolutePath))
                        if (r.optBoolean("ok")) {
                            val l = "abc"[layer]
                            Params.set("$l.type", 4f)  // wt
                            val tableMeta = Params.meta.first { it.name == "$l.wt.table" }
                            Params.set("$l.wt.table", (tableMeta.options.size - 1).toFloat())  // "user"
                        }
                        a.ui.post {
                            if (r.optBoolean("ok")) a.log("wavetable loaded: ${f.name}")
                            else a.log("wavetable load failed: ${r.optString("error")}", Ui.ERR)
                            a.edit.buildPage()
                        }
                    } catch (e: Exception) {
                        a.ui.post { a.log("wavetable import failed: ${e.message}", Ui.ERR) }
                    }
                }
            }
            REQ_IR_CAB, REQ_IR_REVERB -> {
                val uri = data.data ?: return
                val target = if (requestCode == REQ_IR_CAB) 0 else 1
                a.loader.execute {
                    try {
                        val f = a.library.importIr(uri)
                        val r = JSONObject(NativeEngine.nativeLoadImpulse(target, f.absolutePath))
                        if (r.optBoolean("ok")) {
                            // switch the block to its user-IR mode
                            if (target == 0) Params.set("fx.cab.model", 8f) else Params.set("fx.convo.ir", 3f)
                        }
                        a.ui.post {
                            if (r.optBoolean("ok")) a.log("IR loaded: ${f.name} (${r.optInt("length")} samples)")
                            else a.log("IR load failed: ${r.optString("error")}", Ui.ERR)
                            a.edit.buildPage()
                        }
                    } catch (e: Exception) {
                        a.ui.post { a.log("IR import failed: ${e.message}", Ui.ERR) }
                    }
                }
            }
            REQ_WAV -> {
                val uris = buildList {
                    data.clipData?.let { c -> for (i in 0 until c.itemCount) add(c.getItemAt(i).uri) }
                    if (isEmpty()) data.data?.let { add(it) }
                }
                a.loader.execute {
                    for (u in uris) {
                        try {
                            val f = a.library.importWav(u)
                            val info = JSONObject(NativeEngine.nativeInspectSamples(f.absolutePath))
                            a.ui.post {
                                if (info.optBoolean("ok")) a.log("imported ${f.name}")
                                else {
                                    a.log("${f.name}: ${info.optString("error")}", Ui.ERR)
                                    f.delete()
                                }
                            }
                        } catch (e: Exception) {
                            a.ui.post { a.log("import failed: ${e.message}", Ui.ERR) }
                        }
                    }
                    a.ui.post { refreshSamples() }
                }
            }
            REQ_SFZ_TREE -> {
                val tree = data.data ?: return
                a.log("importing folder…")
                a.loader.execute {
                    try {
                        val sfz = a.library.importTree(tree) { msg -> a.ui.post { a.log(msg) } }
                        if (sfz.isEmpty()) a.ui.post { a.log("no .sfz file found in the selected folder", Ui.WARN) }
                        for (f in sfz) {
                            val info = JSONObject(NativeEngine.nativeInspectSamples(f.absolutePath))
                            val w = info.optJSONArray("warnings")
                            a.ui.post {
                                if (info.optBoolean("ok")) {
                                    a.log("${f.name}: ${info.optInt("regions")} regions, ${info.optInt("samples")} samples, " +
                                        "${info.optLong("bytes") / 1024 / 1024} MB")
                                } else {
                                    a.log("${f.name}: ${info.optString("error")}", Ui.ERR)
                                }
                                for (i in 0 until (w?.length() ?: 0)) a.log("  ${w!!.getString(i)}", Ui.WARN)
                            }
                        }
                    } catch (e: Exception) {
                        a.ui.post { a.log("folder import failed: ${e.message}", Ui.ERR) }
                    }
                    a.ui.post { refreshSamples() }
                }
            }
            REQ_PRESET_IMPORT -> {
                val u = data.data ?: return
                a.loader.execute {
                    try {
                        val f = a.library.importPreset(u)
                        a.ui.post { a.log("imported preset ${f.name}"); refresh(); a.loadPreset(f) }
                    } catch (e: Exception) {
                        a.ui.post { a.log("preset import failed: ${e.message}", Ui.ERR) }
                    }
                }
            }
            REQ_PRESET_EXPORT -> {
                val u = data.data ?: return
                val json = NativeEngine.nativePresetJson(a.currentPresetName, a.library.libraryRoot.absolutePath)
                a.loader.execute {
                    try {
                        a.library.exportText(u, json)
                        a.ui.post { a.log("preset exported (sample paths are relative to the app library)") }
                    } catch (e: Exception) {
                        a.ui.post { a.log("export failed: ${e.message}", Ui.ERR) }
                    }
                }
            }
        }
    }
}

/**
 * Obsidian library card: dark surface (raised + 1 dp cyan border when [active]),
 * crisp 3 dp left accent bar in the category color. Zero allocations in drawing.
 */
@SuppressLint("ViewConstructor")
class LibraryCard(ctx: Context, accent: Int, active: Boolean, compact: Boolean = false) : LinearLayout(ctx) {
    private val d = resources.displayMetrics.density
    private val bar = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = accent }
    private val bg = Hw.panelBg(ctx,
        color = if (active) Hw.PANEL_RAISED else Hw.PANEL,
        stroke = if (active) Hw.MOD_OSC else Hw.PANEL_EDGE,
        radiusDp = 6f)
    private val strokePx = Hw.dp(ctx, 1f).toInt().coerceAtLeast(1)

    init {
        orientation = HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        isClickable = true
        isLongClickable = true
        background = bg
        val v = ((if (compact) 5f else 7f) * d).toInt()
        setPadding((12f * d).toInt(), v, (4f * d).toInt(), v)
        minimumHeight = ((if (compact) 36f else 48f) * d).toInt()
        setWillNotDraw(false)
    }

    fun setStyle(accent: Int, active: Boolean) {
        bar.color = accent
        bg.setColor(if (active) Hw.PANEL_RAISED else Hw.PANEL)
        bg.setStroke(strokePx, if (active) Hw.MOD_OSC else Hw.PANEL_EDGE)
        invalidate()
    }

    override fun dispatchDraw(c: Canvas) {
        super.dispatchDraw(c)
        val inset = 6f * d
        c.drawRoundRect(2f * d, inset, 5f * d, height - inset, 1.5f * d, 1.5f * d, bar)
    }
}
