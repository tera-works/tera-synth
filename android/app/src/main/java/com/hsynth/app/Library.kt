package com.hsynth.app

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import java.io.File
import java.io.IOException

/**
 * App-private sample & preset library.
 *
 *   files/library/factory/...  demo content copied from assets
 *   files/library/user/...     imported WAV files and SFZ folders
 *   files/presets/factory/ and files/presets/user/ (JSON presets)
 *
 * The engine reads plain file paths (portable C++ I/O), so content picked via
 * the Storage Access Framework is copied into the library first.
 */
class Library(private val context: Context) {
    val libraryRoot = File(context.filesDir, "library")
    val factoryPresets = File(context.filesDir, "presets/factory")
    val userPresets = File(context.filesDir, "presets/user")
    private val userSamples = File(libraryRoot, "user")

    data class Entry(val file: File, val title: String, val user: Boolean,
                     val category: String = "", val tags: List<String> = emptyList(), val comment: String = "")

    /** Copy bundled assets when the app version changes. Blocking. */
    fun installFactoryContent(versionCode: Long) {
        val marker = File(context.filesDir, "factory.version")
        if (marker.exists() && marker.readText().trim() == versionCode.toString()) return
        File(libraryRoot, "factory").deleteRecursively()
        factoryPresets.deleteRecursively()
        copyAssetDir("library/factory", File(libraryRoot, "factory"))
        copyAssetDir("presets", factoryPresets)
        userPresets.mkdirs()
        userSamples.mkdirs()
        marker.writeText(versionCode.toString())
    }

    private fun copyAssetDir(assetPath: String, dest: File) {
        val list = context.assets.list(assetPath) ?: return
        if (list.isEmpty()) {
            dest.parentFile?.mkdirs()
            context.assets.open(assetPath).use { input -> dest.outputStream().use { input.copyTo(it) } }
            return
        }
        dest.mkdirs()
        for (name in list) copyAssetDir("$assetPath/$name", File(dest, name))
    }

    fun presets(): List<Entry> {
        val f = factoryPresets.listFiles { x -> x.extension == "json" }?.sortedBy { it.name }?.map { presetEntry(it, false) } ?: emptyList()
        // user presets: flat files plus (imported banks / installed packs) up to two folders below presets/user
        val u = ArrayList<File>()
        fun walk(dir: File, depth: Int) {
            val kids = dir.listFiles() ?: return
            for (x in kids) {
                if (x.isFile && x.extension.equals("json", true) && !x.name.startsWith(".")) u += x
                else if (x.isDirectory && depth < 2) walk(x, depth + 1)
            }
        }
        walk(userPresets, 0)
        return f + u.sortedBy { it.path.lowercase() }.map { presetEntry(it, true) }
    }

    // preset metadata by path + modification time (parsing every JSON on each refresh was slow)
    private val metaCache = java.util.concurrent.ConcurrentHashMap<String, Pair<Long, Entry>>()

    private fun presetEntry(f: File, user: Boolean): Entry {
        val mod = f.lastModified()
        metaCache[f.path]?.let { if (it.first == mod) return it.second }
        val e = try {
            val o = org.json.JSONObject(f.readText())
            val tags = o.optJSONArray("tags")?.let { t -> List(t.length()) { t.getString(it) } } ?: emptyList()
            Entry(f, o.optString("name", f.nameWithoutExtension), user, o.optString("category", ""), tags, o.optString("comment", ""))
        } catch (e: Exception) {
            Entry(f, f.nameWithoutExtension + if (L10n.ja) "（読み込めません）" else " (invalid)", user)
        }
        metaCache[f.path] = mod to e
        return e
    }

    /** Extensions of the Phase 6 formats that are listed next to .sfz / .wav (opened through [FormatImporter]). */
    private val formatExts = setOf("sf2", "sf3", "dls", "gig", "dspreset", "dslibrary", "syx", "wt", "vitaltable")

    /**
     * All loadable sample sources: .sfz and stand-alone .wav / .flac, plus the Phase 6 formats found under
     * `library/user/{sf2,sfz,ds,dx7,wt}/` (.sf2 .sf3 .dls .gig .dspreset .dslibrary .syx .wt .vitaltable), up to
     * six folder levels below those roots, so installed sound packs show up at once.
     */
    fun sampleSources(): List<Entry> {
        // One pass per directory. A folder with an .sfz is an instrument: its WAVs and
        // its sub-folders' WAVs belong to it and are not listed (and not walked).
        // Decent Sampler libraries (user/ds) are instruments as a whole: their samples are never listed on their own.
        val out = mutableListOf<Entry>()
        val dsRoot = File(userSamples, "ds").path
        fun walk(dir: File, insideInstrument: Boolean) {
            val files = dir.listFiles() ?: return
            val sfz = files.filter { it.isFile && it.extension.equals("sfz", true) }
            val user = dir.path.startsWith(userSamples.path)
            // folders below library/user: user/sf2 = 1, user/sf2/<pack> = 2, ... (listed down to 6)
            val level = if (!user || dir == userSamples) 0 else dir.relativeTo(userSamples).path.split(File.separatorChar).size
            val inDs = dir.path == dsRoot || dir.path.startsWith(dsRoot + File.separator)
            for (f in sfz) out += Entry(f, f.relativeTo(libraryRoot).path, user)
            if (level in 1..6) {
                for (f in files) if (f.isFile && f.extension.lowercase() in formatExts && !f.name.startsWith("."))
                    out += Entry(f, f.relativeTo(libraryRoot).path, true)
            }
            val instrument = insideInstrument || sfz.isNotEmpty() || inDs
            if (!instrument)
                for (f in files) if (f.isFile && (f.extension.equals("wav", true) || f.extension.equals("flac", true)))
                    out += Entry(f, f.relativeTo(libraryRoot).path, user)
            if (sfz.isNotEmpty()) return  // sample folders below an .sfz
            for (f in files) if (f.isDirectory && !f.name.startsWith(".")) walk(f, instrument)
        }
        walk(libraryRoot, false)
        return out.sortedWith(compareBy({ it.user }, { it.title.lowercase() }))
    }

    fun safeName(name: String): String = name.replace(Regex("[^A-Za-z0-9._ -]"), "_").trim().ifEmpty { "untitled" }

    fun displayName(uri: Uri): String {
        context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
            if (c.moveToFirst()) return c.getString(0)
        }
        return uri.lastPathSegment ?: "file"
    }

    /** Import single WAV files. Returns imported files. */
    fun importWav(uri: Uri): File {
        val name = safeName(displayName(uri))
        if (!name.lowercase().endsWith(".wav")) throw IOException("$name is not a .wav file")
        val dir = File(userSamples, "wav").apply { mkdirs() }
        val dest = uniqueFile(dir, name)
        context.contentResolver.openInputStream(uri)?.use { input -> dest.outputStream().use { input.copyTo(it) } }
            ?: throw IOException("cannot read $name")
        return dest
    }

    /** Import an impulse response (cabinet / reverb) into library/user/ir. */
    fun importIr(uri: Uri): File {
        val name = safeName(displayName(uri))
        if (!name.lowercase().endsWith(".wav")) throw IOException("$name is not a .wav file")
        val dir = File(userSamples, "ir").apply { mkdirs() }
        val dest = uniqueFile(dir, name)
        context.contentResolver.openInputStream(uri)?.use { input -> dest.outputStream().use { input.copyTo(it) } }
            ?: throw IOException("cannot read $name")
        return dest
    }

    /**
     * Import a folder picked with ACTION_OPEN_DOCUMENT_TREE (SFZ instrument with
     * its samples). Copies .sfz/.wav/.txt files preserving relative paths.
     * Returns the .sfz files found.
     */
    fun importTree(treeUri: Uri, progress: (String) -> Unit): List<File> {
        val rootId = DocumentsContract.getTreeDocumentId(treeUri)
        val rootName = safeName(displayNameOfDoc(treeUri, rootId) ?: "import")
        val destRoot = uniqueFile(userSamples, rootName).apply { mkdirs() }
        val sfz = mutableListOf<File>()
        var count = 0
        fun walk(docId: String, dest: File, depth: Int) {
            if (depth > 8) return
            val children = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, docId)
            context.contentResolver.query(children, arrayOf(
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE), null, null, null)?.use { c ->
                while (c.moveToNext()) {
                    val id = c.getString(0)
                    val name = c.getString(1) ?: continue
                    val mime = c.getString(2)
                    if (mime == DocumentsContract.Document.MIME_TYPE_DIR) {
                        walk(id, File(dest, name), depth + 1)
                        continue
                    }
                    val ext = name.substringAfterLast('.', "").lowercase()
                    if (ext !in setOf("sfz", "wav", "txt", "sfzh")) continue
                    val out = File(dest, name)
                    out.parentFile?.mkdirs()
                    val docUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, id)
                    context.contentResolver.openInputStream(docUri)?.use { input -> out.outputStream().use { input.copyTo(it) } }
                    count++
                    if (count % 10 == 0) progress("copied $count files…")
                    if (ext == "sfz") sfz += out
                }
            }
        }
        walk(rootId, destRoot, 0)
        if (count == 0) destRoot.deleteRecursively()
        progress("copied $count files from $rootName")
        return sfz
    }

    private fun displayNameOfDoc(treeUri: Uri, docId: String): String? {
        val uri = DocumentsContract.buildDocumentUriUsingTree(treeUri, docId)
        context.contentResolver.query(uri, arrayOf(DocumentsContract.Document.COLUMN_DISPLAY_NAME), null, null, null)?.use { c ->
            if (c.moveToFirst()) return c.getString(0)
        }
        return null
    }

    fun importPreset(uri: Uri): File {
        val name = safeName(displayName(uri)).let { if (it.endsWith(".json")) it else "$it.json" }
        val text = context.contentResolver.openInputStream(uri)?.use { it.readBytes().decodeToString() } ?: throw IOException("cannot read $name")
        org.json.JSONObject(text) // validate syntax early
        userPresets.mkdirs()
        val dest = uniqueFile(userPresets, name)
        dest.writeText(text)
        return dest
    }

    fun exportText(uri: Uri, text: String) {
        context.contentResolver.openOutputStream(uri, "wt")?.use { it.write(text.toByteArray()) } ?: throw IOException("cannot write")
    }

    fun userPresetFile(name: String) = File(userPresets.apply { mkdirs() }, safeName(name) + ".json")

    private fun uniqueFile(dir: File, name: String): File {
        var f = File(dir, name)
        if (!f.exists()) return f
        val base = name.substringBeforeLast('.')
        val ext = name.substringAfterLast('.', "")
        var i = 2
        while (f.exists()) {
            f = File(dir, if (ext.isEmpty()) "$base ($i)" else "$base ($i).$ext")
            i++
        }
        return f
    }
}
