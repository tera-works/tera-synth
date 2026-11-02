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
    private val presetList = Ui.column(a)
    private val sampleList = Ui.column(a)
    private val logView = Ui.text(a, "", 12f, Ui.DIM).apply { setTextIsSelectable(true) }
    private val logScroll = ScrollView(a)

    init {
        val d = { v: Int -> Ui.dp(a, v) }
        val presetsCol = Ui.column(a).apply {
            addView(Ui.text(a, "Presets", 16f, Ui.ACCENT, bold = true))
            addView(Ui.row(a,
                Ui.button(a, "Save as…") { promptSave() },
                Ui.button(a, "Import…") { open(REQ_PRESET_IMPORT, "application/json", "*/*") },
                Ui.button(a, "Export…") { export() },
            ))
            addView(ScrollView(a).apply { addView(presetList) }, Ui.lp(weight = 1f, height = 0).apply { width = -1 })
        }
        val samplesCol = Ui.column(a).apply {
            addView(Ui.text(a, "Samples (WAV / SFZ)", 16f, Ui.ACCENT, bold = true))
            addView(Ui.row(a,
                Ui.button(a, "Import WAV…") { open(REQ_WAV, "audio/*", "audio/wav", "audio/x-wav") },
                Ui.button(a, "Import SFZ folder…") {
                    @Suppress("DEPRECATION")
                    a.startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQ_SFZ_TREE)
                },
                Ui.button(a, "SFZ info") { showSfzInfo() },
            ))
            addView(ScrollView(a).apply { addView(sampleList) }, Ui.lp(weight = 1f, height = 0).apply { width = -1 })
        }
        val logCol = Ui.column(a).apply {
            addView(Ui.row(a, Ui.text(a, "Messages", 16f, Ui.ACCENT, bold = true), Ui.button(a, "Clear view") { logView.text = "" }))
            logScroll.addView(logView)
            addView(logScroll, Ui.lp(weight = 1f, height = 0).apply { width = -1 })
        }
        view = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(d(12), d(8), d(12), d(8))
            addView(presetsCol, LinearLayout.LayoutParams(0, -1, 1f).apply { marginEnd = d(12) })
            addView(samplesCol, LinearLayout.LayoutParams(0, -1, 1.2f).apply { marginEnd = d(12) })
            addView(logCol, LinearLayout.LayoutParams(0, -1, 1f))
        }
        a.addLogListener { logView.text = a.logText(); logScroll.post { logScroll.fullScroll(View.FOCUS_DOWN) } }
        a.addPresetListener { refreshPresets() }
    }

    private fun item(title: String, sub: String, highlight: Boolean, onClick: () -> Unit, onLong: (() -> Unit)? = null): View {
        val d = { v: Int -> Ui.dp(a, v) }
        return Ui.column(a).apply {
            setPadding(d(10), d(8), d(10), d(8))
            background = Ui.rounded(if (highlight) android.graphics.Color.rgb(30, 60, 80) else Ui.PANEL, d(6))
            addView(Ui.text(a, title, 15f, if (highlight) Ui.ACCENT else Ui.TEXT))
            if (sub.isNotEmpty()) addView(Ui.text(a, sub, 12f, Ui.DIM))
            setOnClickListener { onClick() }
            if (onLong != null) setOnLongClickListener { onLong(); true }
            layoutParams = LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = d(4) }
        }
    }

    fun refresh() {
        refreshPresets()
        refreshSamples()
    }

    private fun refreshPresets() {
        presetList.removeAllViews()
        for (e in a.library.presets()) {
            presetList.addView(item(e.title, if (e.user) "user · long-press to delete" else "factory", e.file == a.currentPreset,
                { a.loadPreset(e.file) },
                if (e.user) ({ confirmDelete(e.file) }) else null))
        }
    }

    private fun refreshSamples() {
        sampleList.removeAllViews()
        val sources = a.library.sampleSources()
        if (sources.isEmpty()) sampleList.addView(Ui.text(a, "(no samples yet)", 13f, Ui.DIM))
        for (e in sources) {
            sampleList.addView(item(e.file.name, e.title + if (e.user) "  · long-press to delete" else "", false,
                { chooseLayer(e.file) },
                if (e.user) ({ confirmDelete(e.file) }) else null))
        }
    }

    private fun chooseLayer(file: File) {
        AlertDialog.Builder(a)
            .setTitle("Load ${file.name} into…")
            .setItems(arrayOf("Layer A", "Layer B", "Layer C")) { _, which -> a.loadSamples(which, file) }
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
        val sources = a.library.sampleSources()
        if (sources.isEmpty()) {
            a.log("no samples in the library yet — import WAV or SFZ on the Library tab", Ui.WARN)
            return
        }
        AlertDialog.Builder(a)
            .setTitle("Samples for layer ${"ABC"[layer]}")
            .setItems(sources.map { it.title }.toTypedArray()) { _, which -> a.loadSamples(layer, sources[which].file) }
            .show()
    }

    private fun confirmDelete(file: File) {
        AlertDialog.Builder(a)
            .setTitle("Delete ${file.name}?")
            .setMessage(if (file.extension == "sfz") "The .sfz file is removed; its sample folder is kept." else "This removes the file from the app library.")
            .setPositiveButton("Delete") { _, _ ->
                if (file.delete()) a.log("deleted ${file.name}") else a.log("could not delete ${file.name}", Ui.ERR)
                refresh()
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun promptSave() {
        val input = EditText(a).apply {
            inputType = InputType.TYPE_CLASS_TEXT
            setText(a.currentPresetName)
            setSelectAllOnFocus(true)
        }
        AlertDialog.Builder(a)
            .setTitle("Save preset")
            .setView(input)
            .setPositiveButton("Save") { _, _ ->
                val name = input.text.toString().trim().ifEmpty { "Untitled" }
                a.savePreset(name) { refreshPresets() }
            }
            .setNegativeButton("Cancel", null)
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
            .setTitle("SFZ support")
            .setMessage("Supported headers: <control> <global> <master> <group> <region>, #define, #include.\n\n" +
                "Supported opcodes:\n" + NativeEngine.nativeSupportedSfzOpcodes() +
                "\n\nAll other opcodes are ignored and listed as warnings in the message log when an instrument is loaded. " +
                "Samples must be WAV (8/16/24/32-bit PCM or float). FLAC is planned.")
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
                        a.ui.post { a.log("imported preset ${f.name}"); refreshPresets(); a.loadPreset(f) }
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
