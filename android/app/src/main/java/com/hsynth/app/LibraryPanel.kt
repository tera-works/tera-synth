package com.hsynth.app

import android.app.AlertDialog
import android.content.Intent
import android.text.InputType
import android.view.View
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import org.json.JSONObject
import java.io.File

/**
 * Library screen: presets (load / save / import / export), sample sources
 * (WAV / SFZ import via the Storage Access Framework) and the message log.
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
    }

    val view: LinearLayout
    private val presetList = FlowLayout(a)
    private val sampleList = Ui.column(a)
    /** MIDI files: app-specific external storage, so they can be copied in over USB (adb push) without permissions. Played on the Player tab. */
    val songDir: File = (a.getExternalFilesDir("midi") ?: File(a.filesDir, "midi")).apply { mkdirs() }
    private var presetFilter = 0  // 0 all, 1 factory, 2 user, 3 favourites
    private var category = ""     // "" = every category
    private var search = ""
    private val categoryRow = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL }
    private val favorites: MutableSet<String> = (a.prefs.getStringSet("favorites", emptySet()) ?: emptySet()).toMutableSet()

    init {
        val d = { v: Int -> Ui.dp(a, v) }
        // presets: filter + actions, then a grid of preset cards
        val presets = Hw.Section(a, "Presets")
        val filters = mutableListOf<LedButton>()
        // filters and actions; they wrap onto two lines on narrow screens
        val head: android.view.ViewGroup = if (Ui.portrait(a)) FlowLayout(a, Ui.dp(a, 6), Ui.dp(a, 6))
            else LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = android.view.Gravity.CENTER_VERTICAL }
        listOf("All", "Factory", "User", "★").forEachIndexed { i, t ->
            val b = LedButton(a, t).apply { textSize = 13.5f; setOnClickListener { presetFilter = i; filters.forEachIndexed { k, x -> x.lit = k == i }; refreshPresets() } }
            b.lit = i == 0
            filters += b
            head.addView(b, if (head is LinearLayout) LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(6) } else android.view.ViewGroup.LayoutParams(-2, -2))
        }
        if (head is LinearLayout) head.addView(View(a), LinearLayout.LayoutParams(0, 1, 1f))
        for ((t, f) in listOf<Pair<String, () -> Unit>>("Save as…" to { promptSave() },
                "Import…" to { open(REQ_PRESET_IMPORT, "application/json", "*/*") }, "Export…" to { export() }))
            head.addView(LedButton(a, t, led = false).apply { textSize = 13.5f; setOnClickListener { f() } },
                if (head is LinearLayout) LinearLayout.LayoutParams(-2, -2).apply { marginStart = d(6) } else android.view.ViewGroup.LayoutParams(-2, -2))
        presets.body.addView(head)
        // search + categories
        val searchBox = android.widget.EditText(a).apply {
            hint = L10n.t("Search name / category / tag")
            textSize = 14f
            setTextColor(Hw.LCD_FG)
            setHintTextColor(Hw.LCD_DIM)
            typeface = Hw.MONO
            background = Hw.lcdBg(a)
            setPadding(d(10), d(6), d(10), d(6))
            isSingleLine = true
            imeOptions = android.view.inputmethod.EditorInfo.IME_ACTION_DONE
            addTextChangedListener(object : android.text.TextWatcher {
                override fun beforeTextChanged(s: CharSequence?, st: Int, c: Int, af: Int) {}
                override fun onTextChanged(s: CharSequence?, st: Int, b: Int, c: Int) {}
                override fun afterTextChanged(s: android.text.Editable?) { search = s?.toString()?.trim() ?: ""; refreshPresets() }
            })
        }
        presets.body.addView(searchBox, LinearLayout.LayoutParams(-1, -2).apply { topMargin = d(8) })
        presets.body.addView(android.widget.HorizontalScrollView(a).apply {
            isHorizontalScrollBarEnabled = false
            addView(categoryRow)
        }, LinearLayout.LayoutParams(-1, -2).apply { topMargin = d(6) })
        presets.body.addView(ScrollView(a).apply { addView(presetList) }, LinearLayout.LayoutParams(-1, 0, 1f).apply { topMargin = d(8) })

        // samples
        val samples = Hw.Section(a, "Samples (WAV / SFZ)")
        samples.body.addView(Ui.buttonRow(a,
            Ui.button(a, "+ WAV…") { open(REQ_WAV, "audio/*", "audio/wav", "audio/x-wav") },
            Ui.button(a, "+ SFZ…") {
                @Suppress("DEPRECATION")
                a.startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQ_SFZ_TREE)
            },
            Ui.button(a, "SFZ info") { showSfzInfo() },
        ))
        samples.body.addView(ScrollView(a).apply { addView(sampleList) }, LinearLayout.LayoutParams(-1, 0, 1f).apply { topMargin = d(8) })

        view = if (Ui.portrait(a)) LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(d(10), d(8), d(10), d(8))
            addView(presets, LinearLayout.LayoutParams(-1, 0, 2f).apply { bottomMargin = d(10) })
            addView(samples, LinearLayout.LayoutParams(-1, 0, 1f))
        } else LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(d(10), d(8), d(10), d(8))
            addView(presets, LinearLayout.LayoutParams(0, -1, 2f).apply { marginEnd = d(10) })
            addView(samples, LinearLayout.LayoutParams(0, -1, 1f))
        }
        // the section bodies stretch so their lists can scroll
        presets.body.layoutParams = LinearLayout.LayoutParams(-1, 0, 1f)
        samples.body.layoutParams = LinearLayout.LayoutParams(-1, 0, 1f)
        a.addPresetListener { refreshPresets() }
    }

    /** Song list now lives on the Player tab. */
    fun refreshSongs() {}

    private fun playSong(f: File) {
        a.selectTab(4)
        a.player.playFile(f)
    }

    /** A preset card (grid) or a list row. */
    private fun item(title: String, sub: String, highlight: Boolean, onClick: () -> Unit, onLong: (() -> Unit)? = null, width: Int = -1): View {
        val d = { v: Int -> Ui.dp(a, v) }
        return Ui.column(a).apply {
            setPadding(d(10), d(8), d(10), d(8))
            background = if (highlight) Hw.lcdBg(a) else Hw.keyBg(a)
            addView(Ui.text(a, title, 14f, if (highlight) Hw.LCD_FG else Ui.TEXT, bold = highlight, tr = false).apply { maxLines = 1 })
            if (sub.isNotEmpty()) addView(Ui.text(a, sub, 11f, if (highlight) Hw.LCD_DIM else Ui.DIM, tr = false).apply { maxLines = 2 })
            setOnClickListener { onClick() }
            if (onLong != null) setOnLongClickListener { onLong(); true }
            layoutParams = if (width > 0) android.view.ViewGroup.LayoutParams(width, -2)
                           else LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = d(6) }
        }
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

    /** Category chips with counts (from the presets the other filters leave). */
    private fun refreshCategories() {
        val d = { v: Int -> Ui.dp(a, v) }
        categoryRow.removeAllViews()
        val counts = presetEntries.filter { e ->
            !(presetFilter == 1 && e.user || presetFilter == 2 && !e.user || presetFilter == 3 && e.file.path !in favorites)
        }.groupingBy { it.category.ifEmpty { "—" } }.eachCount().toSortedMap()
        fun chip(label: String, key: String) = LedButton(a, label, tr = false).apply {
            textSize = 12f
            minHeight = d(32)
            lit = category == key
            setOnClickListener { category = if (category == key) "" else key; refreshPresets() }
        }
        categoryRow.addView(chip(L10n.t("All") + " ${counts.values.sum()}", ""), LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(4) })
        for ((c, n) in counts) categoryRow.addView(chip("$c $n", if (c == "—") "" else c), LinearLayout.LayoutParams(-2, -2).apply { marginEnd = d(4) })
    }

    private fun refreshPresets() {
        refreshCategories()
        presetList.removeAllViews()
        for (e in presetEntries) {
            if (!matches(e)) continue
            val fav = e.file.path in favorites
            val sub = listOfNotNull(e.category.ifEmpty { null }, if (e.user) "User" else "Factory",
                e.tags.takeIf { it.isNotEmpty() }?.joinToString(" ") { "#$it" }).joinToString(" · ")
            presetList.addView(item((if (fav) "★ " else "") + e.title, sub, e.file == a.currentPreset,
                { a.loadPreset(e.file) }, { presetMenu(e) }, width = cardWidth()))
        }
    }

    /** Cards fill the list's width: as many ~180 dp columns as fit. */
    private fun cardWidth(): Int {
        val w = presetList.width
        if (w <= 0) { presetList.post { if (presetList.width > 0) refreshPresets() }; return Ui.dp(a, 172) }
        val gap = Ui.dp(a, 10)
        val cols = maxOf(1, (w + gap) / (Ui.dp(a, 180) + gap))
        return (w - (cols - 1) * gap) / cols
    }

    /** Long press on a preset: favourite on / off, delete (user presets). */
    private fun presetMenu(e: Library.Entry) {
        val fav = e.file.path in favorites
        val items = mutableListOf(L10n.t(if (fav) "Remove from favourites" else "Add to favourites"))
        if (e.user) items += L10n.t("Delete")
        AlertDialog.Builder(a).setTitle(e.title).setItems(items.toTypedArray()) { _, w ->
            if (w == 0) {
                if (fav) favorites.remove(e.file.path) else favorites.add(e.file.path)
                a.prefs.edit().putStringSet("favorites", favorites.toSet()).apply()
                refreshPresets()
            } else confirmDelete(e.file)
        }.show()
    }

    private fun refreshSamples() = refresh()

    private fun buildSamples() {
        sampleList.removeAllViews()
        val sources = sampleEntries ?: return
        if (sources.isEmpty()) sampleList.addView(Ui.text(a, "(no samples yet)", 13f, Ui.DIM))
        for (e in sources) {
            sampleList.addView(item(e.file.name, e.title + if (e.user) L10n.t("  · long-press to delete") else "", false,
                { chooseLayer(e.file) },
                if (e.user) ({ confirmDelete(e.file) }) else null))
        }
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
        val sources = sampleEntries ?: a.library.sampleSources()
        if (sources.isEmpty()) {
            a.log("no samples in the library yet — import WAV or SFZ on the Library tab", Ui.WARN)
            return
        }
        AlertDialog.Builder(a)
            .setTitle(L10n.f("Samples for layer %s", "ABC"[layer].toString()))
            .setItems(sources.map { it.title }.toTypedArray()) { _, which -> a.loadSamples(layer, sources[which].file) }
            .show()
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

    fun onActivityResult(req: Int, data: Intent?) {
        data ?: return
        when (req) {
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
                val target = if (req == REQ_IR_CAB) 0 else 1
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
