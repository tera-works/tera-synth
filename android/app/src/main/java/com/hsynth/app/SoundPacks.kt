package com.hsynth.app

import android.app.AlertDialog
import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.drawable.ColorDrawable
import android.net.ConnectivityManager
import android.net.Uri
import android.os.StatFs
import android.os.SystemClock
import android.text.TextUtils
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.AbsListView
import android.widget.AdapterView
import android.widget.BaseAdapter
import android.widget.FrameLayout
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.TextView
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.FilterInputStream
import java.io.IOException
import java.io.InputStream
import java.net.HttpURLConnection
import java.net.URL
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.security.MessageDigest
import java.text.SimpleDateFormat
import java.util.Date
import java.util.LinkedHashMap
import java.util.Locale
import java.util.TimeZone
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import java.util.zip.ZipException
import java.util.zip.ZipFile
import java.util.zip.ZipInputStream
import kotlin.math.max

/*
 * Sound packs — extensible JSON catalog, safe downloader / extractor, user catalogs, licenses viewer.
 *
 * Contents: [SoundPack] · [SoundCatalog] (alias [SoundPackCatalog]) · [InstalledInfo] · [SoundPackDownloader] ·
 * [SoundPackSheet] (store / manager, CENTER 840 × 366 dp) · [SoundLicensesSheet] (CENTER 760 × 340 dp).
 *
 * Integration wave (nothing here is wired yet — no existing file was touched):
 *  - build `val soundPacks = SoundPackSheet(this) { pack, dir -> ... }` lazily in MainActivity and open it with
 *    `openSheet(soundPacks)` from the Library tab / Monitor tab; `onOpenPack` receives the install folder
 *    (`files/library/user/<root>/<installSubdir>/`) and `soundPacks.catalog.installedInfo(pack)?.primaryFiles()`
 *    lists the loadable files that were discovered after extraction;
 *  - built-in catalog: `assets/catalog/sound_packs.json` (a missing asset is fine: the list is then user catalogs only);
 *  - user catalogs: every `*.json` in `files/catalog/` (alphabetical); the same `id` as an earlier pack overrides it
 *    field by field (typically `url` = mirror). Request codes 172 / 173 are registered by the sheet itself;
 *  - needs INTERNET + ACCESS_NETWORK_STATE (both already in the manifest).
 */

// ====================================================================================================== model

/** One catalog entry. Immutable; a user catalog that repeats an id produces a modified [copy]. */
data class SoundPack(
    val id: String,
    val name: String,
    /** Free text, normalised to lower case (`piano`, `orch`, `drums`, `guitar`, `bass`, `gm`, `synth`, `wt`, `custom` …). */
    val category: String,
    /** `sf2|sf3|dls|gig|sfz|dslibrary|dspreset|syx|wt|wav` */
    val format: String,
    val author: String,
    val license: String,
    val licenseUrl: String,
    val licenseText: String,
    val sizeMB: Double,
    val unpackedMB: Double,
    val description: String,
    val url: String,
    val webUrl: String,
    val directDownload: Boolean,
    /** `none|zip` */
    val archive: String,
    /** Optional lower-case hex SHA-256 of the downloaded file (empty = not verified). */
    val sha256: String,
    /** Single folder name below the format root (never contains a path separator). */
    val installSubdir: String,
    /** File name used for a non-archive download. */
    val filename: String,
    /** [SoundCatalog.BUILTIN] or the user JSON file name this entry (last) came from. */
    val sourceCatalog: String,
) {
    val isBuiltIn: Boolean get() = sourceCatalog == SoundCatalog.BUILTIN

    /** Folder below `files/library/user/`: sf2 · sfz · ds · dx7 · wt. */
    val rootFolder: String get() = SoundCatalog.rootFolderOf(format)

    fun installRoot(ctx: Context) = File(ctx.filesDir, "library/user/$rootFolder")

    fun installDir(ctx: Context) = File(installRoot(ctx), installSubdir)
}

/** Result of reading `.hsynth_installed.json`. */
class InstalledInfo(
    val packId: String,
    val dir: File,
    val installedAt: Long,
    val bytes: Long,
    /** Loadable files found after extraction, relative to [dir] (`/` separated, sorted, at most 500). */
    val files: List<String>,
) {
    fun primaryFiles(): List<File> = files.map { File(dir, it) }.filter { it.isFile }
    val primary: File? get() = primaryFiles().firstOrNull()
}

/** Name used by [SoundLicensesSheet] (the data model is [SoundCatalog]). */
typealias SoundPackCatalog = SoundCatalog

/**
 * Built-in catalog + every `*.json` in `files/catalog/`, merged by pack id. Call [reload] after anything changes
 * (the sheet does it when it opens). UI thread, except where a method says otherwise.
 */
class SoundCatalog(private val a: MainActivity) {
    companion object {
        const val FORMAT = "hsynth-catalog"
        const val VERSION = 1
        const val BUILTIN = "built-in"
        const val ASSET = "catalog/sound_packs.json"
        const val MARKER = ".hsynth_installed.json"
        const val REQ_IMPORT_JSON = 172
        const val REQ_EXPORT_TEMPLATE = 173
        const val TEMPLATE_NAME = "my_custom_packs.json"
        private const val MAX_JSON_BYTES = 2 * 1024 * 1024
        private const val MAX_PACKS = 2000

        val FORMATS = setOf("sf2", "sf3", "dls", "gig", "sfz", "dslibrary", "dspreset", "syx", "wt", "wav")
        private val ID_RE = Regex("[A-Za-z0-9][A-Za-z0-9._-]{0,63}")

        /** `files/library/user/<this>/<installSubdir>/` */
        fun rootFolderOf(format: String): String = when (format) {
            "sf2", "sf3", "dls", "gig" -> "sf2"
            "sfz" -> "sfz"
            "dslibrary", "dspreset" -> "ds"
            "syx" -> "dx7"
            else -> "wt"   // wt, wav
        }

        /** File extensions that count as "loadable" for [format] when the pack is scanned after extraction. */
        fun extsOf(format: String): Set<String> = when (format) {
            "sf2", "sf3", "dls", "gig" -> setOf("sf2", "sf3", "dls", "gig")
            "sfz" -> setOf("sfz")
            "dslibrary", "dspreset" -> setOf("dspreset")
            "syx" -> setOf("syx")
            else -> setOf("wav", "wt", "vitaltable")
        }

        fun guessFormat(name: String): String {
            val ext = name.substringAfterLast('.', "").lowercase(Locale.ROOT)
            return if (ext in FORMATS) ext else ""
        }

        /** Loadable files below [dir] (relative paths). */
        fun findLoadable(dir: File, format: String): List<String> {
            val exts = extsOf(format)
            return dir.walkTopDown()
                .filter { it.isFile && !it.name.startsWith("._") && it.extension.lowercase(Locale.ROOT) in exts }
                .map { it.relativeTo(dir).path.replace('\\', '/') }
                .sorted().take(500).toList()
        }

        fun dirSize(dir: File): Long = dir.walkTopDown().filter { it.isFile }.sumOf { it.length() }

        /** Tiny marker so installed state survives restarts; the pack's license data travels with the files. */
        fun writeMarker(dir: File, pack: SoundPack, files: List<String>, bytes: Long) {
            val now = System.currentTimeMillis()
            val iso = SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss'Z'", Locale.US).apply { timeZone = TimeZone.getTimeZone("UTC") }
            val j = JSONObject()
                .put("format", "hsynth-installed").put("version", 1)
                .put("packId", pack.id).put("name", pack.name)
                .put("installedAt", now).put("installedDate", iso.format(Date(now)))
                .put("bytes", bytes)
                .put("author", pack.author).put("license", pack.license)
                .put("licenseUrl", pack.licenseUrl).put("licenseText", pack.licenseText)
                .put("url", pack.url).put("sha256", pack.sha256).put("sourceCatalog", pack.sourceCatalog)
                .put("files", JSONArray(files))
            File(dir, MARKER).writeText(j.toString(2))
        }

        private fun jStr(o: JSONObject, k: String, def: String): String =
            if (!o.has(k) || o.isNull(k)) def else o.optString(k, def)

        private fun jNum(o: JSONObject, k: String, def: Double): Double {
            if (!o.has(k) || o.isNull(k)) return def
            val v = o.optDouble(k, def)
            return if (v.isNaN() || v < 0.0) def else v
        }

        private fun isHttp(u: String) = u.startsWith("https://", true) || u.startsWith("http://", true)

        /** `TEMPLATE_JSON` is a valid catalog: the `_help` fields are ignored by the parser. */
        const val TEMPLATE_JSON = """{
  "format": "hsynth-catalog",
  "version": 1,
  "name": "My custom sound packs",
  "_help": [
    "Import this file with [+ ADD JSON] in SOUND PACKS. Every pack below appears under the CUSTOM filter.",
    "Required per pack: id (letters, digits, . _ -; max 64) and name. url (direct download) or webUrl (a web page) tells HSynth where the sounds are.",
    "format: sf2 | sf3 | dls | gig | sfz | dslibrary | dspreset | syx | wt | wav.",
    "archive: none = a single file (saved as filename) | zip = extracted into the install folder (zip-slip safe).",
    "url must be https://. directDownload defaults to true when url is set; without it the card shows [OPEN SITE] so you can download in the browser and import the file with + IMPORT.",
    "sha256 (optional): lower-case hex SHA-256 of the downloaded file. A mismatch deletes the download.",
    "installSubdir (optional): folder name below the format root. Default: the id.",
    "sizeMB / unpackedMB: shown on the card and used for the free-space check (needs 1.35 x the larger value + 200 MB).",
    "A pack that repeats the id of a built-in pack (or of an earlier file, sorted by name) replaces the fields you give, typically url = a mirror. sha256 is cleared when url changes unless you give a new one.",
    "Only add sounds you are allowed to use. author, license, licenseUrl and licenseText are shown on the LICENSES page; fill them in honestly."
  ],
  "packs": [
    {
      "id": "example-sf2-zip",
      "name": "EXAMPLE: SoundFont inside a ZIP",
      "category": "gm",
      "format": "sf2",
      "author": "Your Name",
      "license": "CC0",
      "licenseUrl": "https://creativecommons.org/publicdomain/zero/1.0/",
      "licenseText": "Released into the public domain.",
      "sizeMB": 120,
      "unpackedMB": 150,
      "description": "Replace url with a real https link, or delete this pack.",
      "url": "https://example.com/downloads/my_soundfont.zip",
      "webUrl": "https://example.com/my-soundfont",
      "directDownload": true,
      "archive": "zip",
      "sha256": "",
      "installSubdir": "my_soundfont"
    },
    {
      "id": "example-web-only",
      "name": "EXAMPLE: download from a web page",
      "category": "synth",
      "format": "sfz",
      "author": "Some Author",
      "license": "CC BY 4.0",
      "licenseUrl": "https://creativecommons.org/licenses/by/4.0/",
      "sizeMB": 40,
      "description": "No direct link: the card shows [OPEN SITE], you download it yourself and import it.",
      "webUrl": "https://example.com/pack-page",
      "directDownload": false
    }
  ]
}
"""
    }

    @Volatile
    var packs: List<SoundPack> = emptyList()
        private set

    /** Problems found by the last [reload] (bad JSON, rejected packs …). */
    var warnings: List<String> = emptyList()
        private set

    private val installed = HashMap<String, InstalledInfo>()

    fun userDir() = File(a.filesDir, "catalog")

    fun find(id: String): SoundPack? = packs.firstOrNull { it.id == id }

    // ------------------------------------------------------------------ load / merge

    fun listUserCatalogs(): List<File> =
        userDir().listFiles { f -> f.isFile && f.extension.equals("json", ignoreCase = true) && !f.name.startsWith(".") }
            ?.sortedBy { it.name.lowercase(Locale.ROOT) } ?: emptyList()

    /** Re-read the built-in asset and every user catalog, then the installed markers. */
    fun reload() {
        val errors = ArrayList<String>()
        val merged = LinkedHashMap<String, SoundPack>()

        runCatching { a.assets.open(ASSET).bufferedReader().use { it.readText() } }.getOrNull()?.let { text ->
            parseCatalog(text, BUILTIN, merged, errors)?.forEach { merged[it.id] = it }
        }
        for (f in listUserCatalogs()) {
            try {
                if (f.length() > MAX_JSON_BYTES) { errors += "${f.name}: file is too large"; continue }
                parseCatalog(f.readText(), f.name, merged, errors)?.forEach { merged[it.id] = it }
            } catch (e: Exception) {
                errors += "${f.name}: ${e.message ?: e.javaClass.simpleName}"
            }
        }

        // two packs must never share one install folder (uninstall deletes the folder)
        val used = HashMap<String, String>()
        val out = ArrayList<SoundPack>(merged.size)
        for (p in merged.values) {
            var q = p
            var key = q.rootFolder + "/" + q.installSubdir.lowercase(Locale.ROOT)
            if (used[key] != null && used[key] != q.id) {
                q = q.copy(installSubdir = spSafeSegment(q.installSubdir + "-" + q.id).ifEmpty { "pack-" + q.id })
                key = q.rootFolder + "/" + q.installSubdir.lowercase(Locale.ROOT)
            }
            used[key] = q.id
            out += q
        }
        packs = out
        warnings = errors
        refreshInstalled()
    }

    /**
     * Parse one catalog text. [base] holds the packs merged so far: an entry with a known id inherits every field
     * it does not set itself. @return null when the document is not an `hsynth-catalog` v1 file.
     */
    private fun parseCatalog(text: String, source: String, base: Map<String, SoundPack>, errors: MutableList<String>): List<SoundPack>? {
        val root = try { JSONObject(text) } catch (e: Exception) {
            errors += "$source: not valid JSON (${e.message})"
            return null
        }
        if (root.optString("format") != FORMAT) { errors += "$source: \"format\" must be \"$FORMAT\""; return null }
        if (root.optInt("version", -1) != VERSION) { errors += "$source: \"version\" must be $VERSION"; return null }
        val arr = root.optJSONArray("packs") ?: run { errors += "$source: no \"packs\" list"; return null }
        val out = ArrayList<SoundPack>()
        for (i in 0 until minOf(arr.length(), MAX_PACKS)) {
            val o = arr.optJSONObject(i) ?: continue
            val id = jStr(o, "id", "").trim()
            try {
                out += parsePack(o, base[id] ?: out.lastOrNull { it.id == id }, source)
            } catch (e: IllegalArgumentException) {
                errors += "$source: pack #${i + 1}: ${e.message}"
            }
        }
        return out
    }

    private fun parsePack(o: JSONObject, b: SoundPack?, source: String): SoundPack {
        val id = jStr(o, "id", b?.id ?: "").trim()
        require(ID_RE.matches(id)) { "bad id \"$id\" (letters, digits, . _ - ; max 64)" }
        val url = jStr(o, "url", b?.url ?: "").trim()
        val webUrl = jStr(o, "webUrl", b?.webUrl ?: "").trim()
        val licenseUrl = jStr(o, "licenseUrl", b?.licenseUrl ?: "").trim()
        require(url.isEmpty() || isHttp(url)) { "$id: url must start with https://" }
        require(webUrl.isEmpty() || isHttp(webUrl)) { "$id: webUrl must start with https://" }
        require(licenseUrl.isEmpty() || isHttp(licenseUrl)) { "$id: licenseUrl must start with https://" }

        val filename0 = jStr(o, "filename", b?.filename ?: "").trim()
        var format = jStr(o, "format", b?.format ?: "").trim().lowercase(Locale.ROOT)
        if (format.isEmpty()) format = guessFormat(filename0.ifEmpty { spUrlLastSegment(url) })
        require(format in FORMATS) { "$id: unknown format \"$format\"" }

        val archive = jStr(o, "archive", b?.archive ?: if (spUrlPath(url).endsWith(".zip")) "zip" else "none")
            .trim().lowercase(Locale.ROOT)
        require(archive == "none" || archive == "zip") { "$id: archive must be none or zip" }

        val urlChanged = b != null && url != b.url
        val sha = when {
            o.has("sha256") && !o.isNull("sha256") -> o.optString("sha256").trim().lowercase(Locale.ROOT)
            b != null && !urlChanged -> b.sha256   // a mirror / override must give its own checksum
            else -> ""
        }
        val sub = spSafeSegment(jStr(o, "installSubdir", b?.installSubdir ?: ""))
            .ifEmpty { spSafeSegment(id).ifEmpty { "pack-$id" } }
        val filename = spSafeSegment(filename0).ifEmpty { defaultFilename(id, format, url) }
        val direct = (if (o.has("directDownload") && !o.isNull("directDownload")) o.optBoolean("directDownload") else true) && url.isNotEmpty()

        return SoundPack(
            id = id,
            name = jStr(o, "name", b?.name ?: id).trim().ifEmpty { id },
            category = jStr(o, "category", b?.category ?: "custom").trim().lowercase(Locale.ROOT).ifEmpty { "custom" },
            format = format,
            author = jStr(o, "author", b?.author ?: "").trim(),
            license = jStr(o, "license", b?.license ?: "").trim(),
            licenseUrl = licenseUrl,
            licenseText = jStr(o, "licenseText", b?.licenseText ?: "").trim(),
            sizeMB = jNum(o, "sizeMB", b?.sizeMB ?: 0.0),
            unpackedMB = jNum(o, "unpackedMB", b?.unpackedMB ?: 0.0),
            description = jStr(o, "description", b?.description ?: "").trim(),
            url = url,
            webUrl = webUrl,
            directDownload = direct,
            archive = archive,
            sha256 = sha,
            installSubdir = sub,
            filename = filename,
            sourceCatalog = source,
        )
    }

    private fun defaultFilename(id: String, format: String, url: String): String {
        val seg = spSafeSegment(spUrlLastSegment(url))
        return if (seg.contains('.')) seg else "${spSafeSegment(id).ifEmpty { "pack" }}.$format"
    }

    // ------------------------------------------------------------------ installed state

    fun installedInfo(p: SoundPack): InstalledInfo? = installed[p.id]

    fun isInstalled(p: SoundPack) = installed.containsKey(p.id)

    fun refreshInstalled() {
        installed.clear()
        for (p in packs) readMarker(p)?.let { installed[p.id] = it }
    }

    private fun readMarker(p: SoundPack): InstalledInfo? {
        val dir = p.installDir(a)
        val m = File(dir, MARKER)
        if (!m.isFile) return null
        return try {
            val j = JSONObject(m.readText())
            if (j.optString("packId") != p.id) return null
            val fa = j.optJSONArray("files")
            val files = ArrayList<String>()
            for (i in 0 until (fa?.length() ?: 0)) fa!!.optString(i).takeIf { it.isNotEmpty() }?.let { files += it }
            InstalledInfo(p.id, dir, j.optLong("installedAt"), j.optLong("bytes"), files)
        } catch (e: Exception) {
            null
        }
    }

    // ------------------------------------------------------------------ user catalogs

    fun deleteUserCatalog(file: File): Boolean {
        val dir = userDir().canonicalFile
        val f = file.canonicalFile
        if (f.parentFile != dir) return false
        val ok = f.delete()
        reload()
        return ok
    }

    /**
     * Validate a picked JSON (`hsynth-catalog` v1, at least one usable pack), copy it to `files/catalog/<name>.json`
     * (same name = update) and reload the merged catalog. Reads on `a.loader`; [onDone] runs on the UI thread.
     */
    fun importCatalogJson(uri: Uri, onDone: (ok: Boolean, message: String) -> Unit = { _, m -> FormatFiles.toast(a, m) }) {
        val known = packs.associateBy { it.id }
        a.loader.execute {
            var ok = false
            var msg: String
            try {
                val text = a.contentResolver.openInputStream(uri)?.use { spReadLimited(it, MAX_JSON_BYTES) }
                    ?: throw IOException("cannot read the file")
                val name = spSafeSegment(FormatFiles.displayName(a, uri).substringBeforeLast('.', "")).ifEmpty { "catalog" } + ".json"
                val errors = ArrayList<String>()
                val parsed = parseCatalog(text, name, known, errors)
                if (parsed == null) throw IOException(errors.firstOrNull() ?: "not an $FORMAT file")
                if (parsed.isEmpty()) throw IOException(errors.firstOrNull() ?: "the catalog contains no usable pack")
                val dir = userDir().apply { mkdirs() }
                val tmp = File(dir, ".$name.tmp")
                tmp.writeText(text)
                Files.move(tmp.toPath(), File(dir, name).toPath(), StandardCopyOption.REPLACE_EXISTING)
                ok = true
                msg = L10n.f("Added %s: %s packs", name, parsed.size.toString()) +
                    if (errors.isNotEmpty()) L10n.f(" (%s skipped)", errors.size.toString()) else ""
            } catch (e: Exception) {
                msg = L10n.t("Catalog not added:") + " " + (e.message ?: e.javaClass.simpleName)
            }
            a.ui.post {
                if (a.isDestroyed) return@post
                if (ok) reload()
                a.log(msg, if (ok) Ui.TEXT else Ui.WARN)
                onDone(ok, msg)
            }
        }
    }

    /** Write the documented example catalog ([TEMPLATE_JSON]) to a SAF `CREATE_DOCUMENT` [uri]. */
    fun exportTemplateJson(uri: Uri, onDone: (ok: Boolean, message: String) -> Unit = { _, m -> FormatFiles.toast(a, m) }) {
        a.loader.execute {
            var ok = false
            val msg = try {
                a.contentResolver.openOutputStream(uri, "wt")?.use { it.write(TEMPLATE_JSON.toByteArray(Charsets.UTF_8)) }
                    ?: throw IOException("cannot write the file")
                ok = true
                L10n.t("Template saved. Edit it, then add it with + ADD JSON.")
            } catch (e: Exception) {
                L10n.t("Template not saved:") + " " + (e.message ?: e.javaClass.simpleName)
            }
            a.ui.post { if (!a.isDestroyed) { a.log(msg, if (ok) Ui.TEXT else Ui.WARN); onDone(ok, msg) } }
        }
    }
}

// ====================================================================================================== downloader

private class SpCancelled : IOException("cancelled")

/** Counts the bytes that were read from the archive (extraction progress). */
private class SpCountingStream(ins: InputStream) : FilterInputStream(ins) {
    @Volatile var count = 0L
    override fun read(): Int = super.read().also { if (it >= 0) count++ }
    override fun read(b: ByteArray, off: Int, len: Int): Int = super.read(b, off, len).also { if (it > 0) count += it }
}

/**
 * Sliding-window transfer speed in bytes per second: `bytesDelta / (elapsedMs / 1000)` between the newest sample and
 * the oldest one that is still inside [windowMs] (one older sample is kept as the window's left edge). Never negative.
 */
private class SpSpeedMeter(private val windowMs: Long) {
    private val times = ArrayList<Long>()
    private val bytes = ArrayList<Long>()

    fun add(nowMs: Long, total: Long): Double {
        times += nowMs; bytes += total
        while (times.size > 2 && nowMs - times[1] >= windowMs) { times.removeAt(0); bytes.removeAt(0) }
        val dt = nowMs - times[0]
        if (dt <= 0L) return 0.0
        return max(0.0, (total - bytes[0]).toDouble() / (dt / 1000.0))
    }
}

/**
 * Installs / removes packs. One active download (executor `hsynth-dl`); [cancel] stops it cleanly and removes the
 * partial `cache/dl/<id>.tmp`. Listeners are called on the UI thread. Downloads run in this process only: they
 * stop when Android kills the app in the background.
 */
class SoundPackDownloader(private val a: MainActivity, private val catalog: SoundCatalog) {
    enum class Phase { PREPARING, DOWNLOADING, EXTRACTING, FINISHING }

    /**
     * [exactTotal] = false when [total] is only the catalog's size estimate (the server sent no Content-Length, e.g. a
     * chunked GitHub archive): the fraction then never reaches 100 % while the download is still running.
     */
    class Progress(
        val pack: SoundPack, val phase: Phase, val bytes: Long, val total: Long, val bytesPerSec: Double,
        val exactTotal: Boolean = true,
    ) {
        val fraction: Float get() {
            if (total <= 0) return 0f
            val f = (bytes.toDouble() / total).toFloat().coerceIn(0f, 1f)
            return if (exactTotal) f else minOf(f, 0.99f)
        }
        /** Whole percent for display; capped at 99 while the total is an estimate. */
        val percent: Int get() = (fraction * 100f).toInt().let { if (exactTotal) it else minOf(it, 99) }
    }

    interface Listener {
        /** Download state changed; null = idle. */
        fun onProgress(p: Progress?) {}
        /** One pack finished (installed, failed or cancelled); the queue then advances by itself. */
        fun onResult(pack: SoundPack, ok: Boolean, message: String) {}
        /** The waiting queue changed (add / remove / advance / clear). */
        fun onQueueChanged() {}
    }

    companion object {
        private const val MB = 1024.0 * 1024.0
        private const val MAX_ENTRIES = 200_000
        private const val MAX_UNPACKED_BYTES = 16L * 1024 * 1024 * 1024
        private const val MAX_DOWNLOAD_BYTES = 16L * 1024 * 1024 * 1024
        private const val MAX_REDIRECTS = 5
        private const val USER_AGENT = "HSynth-SoundPacks/1.0"

        private val exec: ExecutorService by lazy {
            Executors.newSingleThreadExecutor { r ->
                Thread({
                    android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_BACKGROUND)
                    r.run()
                }, "hsynth-dl")
            }
        }

        fun freeBytes(ctx: Context): Long = runCatching { StatFs(ctx.filesDir.absolutePath).availableBytes }.getOrDefault(0L)

        /** Free space a pack needs: max(unpacked, size) x 1.35 + 200 MB. */
        fun requiredBytes(p: SoundPack): Long = (max(p.unpackedMB, p.sizeMB) * 1.35 * MB + 200.0 * MB).toLong()

        /**
         * Free space for a whole batch: every pack's unpacked size, plus the largest single archive kept next to it
         * while it is unpacked (0.35 x the biggest pack), plus ONE 200 MB margin. For one pack this equals [requiredBytes].
         */
        fun requiredBytesBatch(list: List<SoundPack>): Long {
            if (list.isEmpty()) return 0L
            val sizes = list.map { max(it.unpackedMB, it.sizeMB) }
            return ((sizes.sum() + 0.35 * (sizes.maxOrNull() ?: 0.0)) * MB + 200.0 * MB).toLong()
        }
    }

    @Volatile
    var active: Progress? = null
        private set

    private val listeners = CopyOnWriteArrayList<Listener>()
    private val cancelFlag = AtomicBoolean(false)
    @Volatile private var conn: HttpURLConnection? = null

    /** Waiting packs in the exact order they were added (head = next to download). Guarded by [lock]. */
    private val queue = ArrayList<SoundPack>()
    private val lock = Any()
    /** The "metered network" question was answered with Continue for the batch that is still running. */
    private var meteredApproved = false

    fun addListener(l: Listener) { listeners.addIfAbsent(l) }
    fun removeListener(l: Listener) { listeners.remove(l) }

    fun isActive(pack: SoundPack) = active?.pack?.id == pack.id

    // ------------------------------------------------------------------ queue queries

    fun queueCount(): Int = synchronized(lock) { queue.size }

    /** Ids of the waiting packs, next-to-download first. */
    fun queuedIds(): List<String> = synchronized(lock) { queue.map { it.id } }

    fun queuedPacks(): List<SoundPack> = synchronized(lock) { ArrayList(queue) }

    fun isQueued(packId: String): Boolean = synchronized(lock) { queue.any { it.id == packId } }

    /** 0 = downloading now · 1, 2, … = waiting position · -1 = not queued. */
    fun queuePosition(packId: String): Int {
        if (active?.pack?.id == packId) return 0
        return synchronized(lock) {
            val i = queue.indexOfFirst { it.id == packId }
            if (i >= 0) i + 1 else -1
        }
    }

    // ------------------------------------------------------------------ enqueue (UI thread, preflight)

    /** Same as [enqueue]; kept for older callers. */
    fun start(pack: SoundPack) = enqueue(pack)

    fun enqueue(pack: SoundPack, onAccepted: (() -> Unit)? = null) = enqueueAll(listOf(pack), onAccepted)

    /**
     * Add [packs] to the back of the queue in the given order and start downloading if nothing runs. Packs that are
     * active, already queued, installed or without a direct download are skipped. One free-space check and one
     * metered / http confirmation cover the whole batch; [onAccepted] runs once the packs are really queued
     * (not when the user declines a dialog).
     */
    fun enqueueAll(packs: List<SoundPack>, onAccepted: (() -> Unit)? = null) {
        if (a.isDestroyed) return
        val fresh = ArrayList<SoundPack>()
        val seen = HashSet<String>()
        var reason = ""
        for (p in packs) {
            if (!seen.add(p.id)) continue
            if (isActive(p) || isQueued(p.id) || catalog.isInstalled(p)) {
                if (reason.isEmpty()) reason = L10n.t("Already queued or installed.")
                continue
            }
            when {
                p.url.isEmpty() || !p.directDownload -> reason = L10n.t("This pack has no direct download. Use OPEN SITE.")
                !isHttpUrl(p.url) -> reason = L10n.t("Only http(s) downloads are supported.")
                else -> fresh += p
            }
        }
        if (fresh.isEmpty()) {
            if (reason.isNotEmpty()) FormatFiles.toast(a, reason)
            return
        }

        // free space for everything that is (or is about to be) on its way
        val all = ArrayList<SoundPack>()
        active?.pack?.let { all += it }
        all += queuedPacks()
        all += fresh
        val need = requiredBytesBatch(all)
        val free = freeBytes(a)
        if (free < need) {
            val msg = if (all.size == 1) {
                L10n.f("\"%s\" needs about %s free (download + unpacking), but only %s is available.", all[0].name, spFmtBytes(need), spFmtBytes(free))
            } else {
                L10n.f("%s packs need about %s free (download + unpacking), but only %s is available.", all.size.toString(), spFmtBytes(need), spFmtBytes(free))
            }
            AlertDialog.Builder(a)
                .setTitle(L10n.t("Not enough free space"))
                .setMessage(msg)
                .setPositiveButton(L10n.t("OK"), null)
                .show()
            return
        }

        val steps = ArrayList<Pair<String, String>>()
        val plain = fresh.filter { it.url.startsWith("http://", true) }
        if (plain.isNotEmpty()) {
            steps += L10n.t("Unencrypted download") to
                L10n.t("This pack is served over http:// (not encrypted): the file could be altered on the way.") +
                (if (plain.any { it.sha256.isEmpty() }) "\n" + L10n.t("The catalog gives no SHA-256 checksum either.") else "") +
                "\n" + L10n.t("Continue only if you trust the source.")
        }
        val totalMB = fresh.sumOf { it.sizeMB }
        val askMetered = !meteredApproved && totalMB >= 50.0 && isMetered()
        if (askMetered) {
            steps += L10n.f("Wi-Fi recommended (%s MB)", Math.round(totalMB).toString()) to
                L10n.t("You are on a metered connection (mobile data). Download anyway?")
        }
        confirmSteps(steps, 0) {
            if (askMetered) meteredApproved = true
            commit(fresh, onAccepted)
        }
    }

    private fun isHttpUrl(u: String) = u.startsWith("https://", true) || u.startsWith("http://", true)

    private fun isMetered(): Boolean = runCatching {
        (a.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager).isActiveNetworkMetered
    }.getOrDefault(false)

    private fun confirmSteps(steps: List<Pair<String, String>>, i: Int, done: () -> Unit) {
        if (i >= steps.size) { done(); return }
        AlertDialog.Builder(a)
            .setTitle(steps[i].first)
            .setMessage(steps[i].second)
            .setPositiveButton(L10n.t("Continue")) { _, _ -> confirmSteps(steps, i + 1, done) }
            .setNegativeButton(L10n.t("Cancel"), null)
            .show()
    }

    /** Append the confirmed packs (state may have changed while a dialog was open) and start the head if idle. */
    private fun commit(fresh: List<SoundPack>, onAccepted: (() -> Unit)?) {
        if (a.isDestroyed) return
        var added = 0
        synchronized(lock) {
            for (p in fresh) {
                if (isActive(p) || queue.any { it.id == p.id } || catalog.isInstalled(p)) continue
                queue += p
                added++
            }
        }
        onAccepted?.invoke()
        if (added == 0) return
        pump()
        notifyQueue()
    }

    private fun notifyQueue() { for (l in listeners) l.onQueueChanged() }

    /** Start the next waiting pack when nothing runs (UI thread). */
    private fun pump() {
        if (a.isDestroyed || active != null) return
        val next = synchronized(lock) { if (queue.isEmpty()) null else queue.removeAt(0) }
        if (next == null) { meteredApproved = false; return }
        begin(next)
    }

    // ------------------------------------------------------------------ cancel / remove (UI thread)

    /** Stop ONLY the running download; the queue then advances to the next pack by itself. */
    fun cancelCurrent() {
        if (active == null) return
        cancelFlag.set(true)
        val c = conn
        if (c != null) Thread { runCatching { c.disconnect() } }.start()   // unblocks a pending read; never on the UI thread
    }

    /** Older name of [cancelCurrent]. */
    fun cancel() = cancelCurrent()

    /** Stop the running download AND drop every waiting pack. */
    fun cancelAll() {
        val had = synchronized(lock) { val h = queue.isNotEmpty(); queue.clear(); h }
        cancelCurrent()
        if (had) notifyQueue()
    }

    /** Remove a waiting pack; the running download is not touched. */
    fun removeFromQueue(packId: String): Boolean {
        val removed = synchronized(lock) {
            val i = queue.indexOfFirst { it.id == packId }
            if (i >= 0) { queue.removeAt(i); true } else false
        }
        if (removed) notifyQueue()
        return removed
    }

    private fun begin(pack: SoundPack) {
        cancelFlag.set(false)
        publish(Progress(pack, Phase.PREPARING, 0, 0, 0.0))
        exec.execute {
            var ok = false
            var msg: String
            try {
                install(pack)
                ok = true
                msg = L10n.f("Installed \"%s\"", pack.name)
            } catch (e: SpCancelled) {
                msg = L10n.t("Download cancelled.")
            } catch (e: Exception) {
                val m = e.message ?: e.javaClass.simpleName
                msg = if (m.contains("Cleartext", ignoreCase = true)) L10n.t("Android blocks plain http:// downloads. Use an https:// URL.")
                else L10n.f("Download failed: %s", m)
            }
            a.ui.post {
                active = null
                conn = null
                if (a.isDestroyed) { synchronized(lock) { queue.clear() }; return@post }
                if (ok) catalog.refreshInstalled()
                a.log("sound pack ${pack.id}: $msg", if (ok) Ui.TEXT else Ui.WARN)
                for (l in listeners) l.onResult(pack, ok, msg)
                if (ok) runCatching { a.libraryPanel.refresh() }
                // fault isolation: whatever happened to this pack, go on with the next one
                pump()
                if (active == null) for (l in listeners) l.onProgress(null)
                notifyQueue()
            }
        }
    }

    private fun publish(p: Progress) {
        active = p
        a.ui.post { if (!a.isDestroyed) for (l in listeners) l.onProgress(p) }
    }

    private fun checkCancel() { if (cancelFlag.get()) throw SpCancelled() }

    // ------------------------------------------------------------------ install (executor thread)

    private fun install(pack: SoundPack) {
        // earlier packs of a batch may have used the space: fail only this pack, the queue goes on
        val need = requiredBytes(pack)
        val free = freeBytes(a)
        if (free < need) throw IOException(L10n.f("Not enough free space (need %s, free %s)", spFmtBytes(need), spFmtBytes(free)))
        val tmpDir = File(a.cacheDir, "dl").apply { mkdirs() }
        val tmp = File(tmpDir, spSafeSegment(pack.id).ifEmpty { "pack" } + ".tmp")
        tmp.delete()
        var staging: File? = null
        try {
            download(pack, tmp)
            checkCancel()

            val target = pack.installDir(a)
            val root = pack.installRoot(a)
            guardInside(target, root)
            val parent = root.apply { mkdirs() }
            if (target.exists() && !File(target, SoundCatalog.MARKER).isFile) {
                throw IOException(L10n.f("The folder \"%s\" already exists and was not created by Sound Packs. Rename or remove it first.", target.name))
            }
            val st = File(parent, target.name + ".staging")
            st.deleteRecursively()
            if (!st.mkdirs()) throw IOException("cannot create $st")
            staging = st

            publish(Progress(pack, Phase.EXTRACTING, 0, 1, 0.0))
            if (isArchive(pack)) {
                extractZip(pack, tmp, st)
            } else {
                val dest = File(st, pack.filename)
                guardInside(dest, st)
                if (!tmp.renameTo(dest)) { tmp.copyTo(dest, overwrite = true); tmp.delete() }
            }
            checkCancel()

            publish(Progress(pack, Phase.FINISHING, 1, 1, 0.0))
            val files = SoundCatalog.findLoadable(st, pack.format)
            if (files.isEmpty()) {
                throw IOException(L10n.f("No .%s files were found in the downloaded pack.", SoundCatalog.extsOf(pack.format).first()))
            }
            SoundCatalog.writeMarker(st, pack, files, SoundCatalog.dirSize(st))

            swap(st, target)
            staging = null
        } finally {
            tmp.delete()
            staging?.deleteRecursively()
        }
    }

    private fun isArchive(p: SoundPack): Boolean {
        fun arch(n: String) = n.endsWith(".zip") || n.endsWith(".dslibrary")
        return p.archive == "zip" || p.format == "dslibrary" || arch(spUrlPath(p.url)) || arch(p.filename.lowercase(Locale.ROOT))
    }

    /** Replace [target] by [staging] (rename on one file system; the old install is kept until the new one is in place). */
    private fun swap(staging: File, target: File) {
        val parent = target.parentFile ?: throw IOException("bad install path")
        if (target.exists()) {
            val old = File(parent, target.name + ".old")
            old.deleteRecursively()
            if (!target.renameTo(old)) throw IOException("cannot replace the existing install")
            if (!staging.renameTo(target)) {
                old.renameTo(target)
                throw IOException("cannot finish the install")
            }
            old.deleteRecursively()
        } else if (!staging.renameTo(target)) {
            throw IOException("cannot finish the install")
        }
    }

    /** [f] must be a strict descendant of [root] (canonical paths). */
    private fun guardInside(f: File, root: File) {
        val r = root.canonicalFile.path + File.separator
        val c = f.canonicalFile.path
        if (!c.startsWith(r)) throw IOException("unsafe install path: $f")
    }

    // ------------------------------------------------------------------ HTTP

    private fun connect(pack: SoundPack): HttpURLConnection {
        var url = URL(pack.url)
        var hops = 0
        while (true) {
            checkCancel()
            val c = url.openConnection() as HttpURLConnection
            c.instanceFollowRedirects = false
            c.connectTimeout = 15_000
            c.readTimeout = 30_000
            c.setRequestProperty("User-Agent", USER_AGENT)
            c.setRequestProperty("Accept-Encoding", "identity")
            conn = c
            val code = try { c.responseCode } catch (e: IOException) {
                if (cancelFlag.get()) throw SpCancelled()
                throw e
            }
            if (code in 300..399 && code != 304) {
                val loc = c.getHeaderField("Location") ?: run { c.disconnect(); throw IOException("redirect without a Location header") }
                c.disconnect()
                if (++hops > MAX_REDIRECTS) throw IOException("too many redirects")
                val next = URL(url, loc)
                val proto = next.protocol.lowercase(Locale.ROOT)
                if (proto != "https" && proto != "http") throw IOException("redirect to an unsupported address")
                if (url.protocol.equals("https", true) && proto == "http") throw IOException("redirect from https to http refused")
                url = next
                continue
            }
            if (code != 200) {
                val m = c.responseMessage ?: ""
                c.disconnect()
                throw IOException("HTTP $code $m".trim())
            }
            return c
        }
    }

    private fun download(pack: SoundPack, out: File) {
        val c = connect(pack)
        try {
            val declared = c.getHeaderField("Content-Length")?.trim()?.toLongOrNull() ?: -1L
            val total = if (declared > 0) declared else (pack.sizeMB * MB).toLong()
            val md = if (pack.sha256.isNotEmpty()) MessageDigest.getInstance("SHA-256") else null
            val buf = ByteArray(32 * 1024)
            var done = 0L
            var lastT = 0L
            var speed = 0.0
            // The window starts when the FIRST byte arrives (connect / TLS / server latency must not count as slow
            // transfer) and always spans the last ~1 s of samples, so a burst never reads as 0.2 MB/s.
            val meter = SpSpeedMeter(1000L)
            var started = false
            try {
                c.inputStream.use { ins ->
                    out.outputStream().use { os ->
                        while (true) {
                            if (cancelFlag.get()) throw SpCancelled()
                            val n = ins.read(buf)
                            if (n < 0) break
                            os.write(buf, 0, n)
                            md?.update(buf, 0, n)
                            done += n
                            if (done > MAX_DOWNLOAD_BYTES) throw IOException("download is larger than the limit")
                            val now = SystemClock.elapsedRealtime()
                            if (!started) {
                                started = true
                                lastT = now
                                meter.add(now, done - n)   // baseline just before the first chunk
                            }
                            if (now - lastT >= 300) {
                                lastT = now
                                speed = meter.add(now, done)
                                publish(Progress(pack, Phase.DOWNLOADING, done, total, speed, exactTotal = declared > 0))
                            }
                        }
                    }
                }
            } catch (e: IOException) {
                if (cancelFlag.get()) throw SpCancelled()
                throw e
            }
            if (declared > 0 && done != declared) throw IOException("download incomplete ($done of $declared bytes)")
            publish(Progress(pack, Phase.DOWNLOADING, done, max(done, 1L), speed))
            if (md != null) {
                val got = md.digest().joinToString("") { "%02x".format(it) }
                if (!got.equals(pack.sha256, ignoreCase = true)) {
                    out.delete()
                    throw IOException(L10n.f("Checksum mismatch (expected %s…, got %s…). The download was deleted.", pack.sha256.take(10), got.take(10)))
                }
            }
        } finally {
            runCatching { c.disconnect() }
        }
    }

    // ------------------------------------------------------------------ ZIP

    /**
     * Unpacks [zip] into [staging] with the same rules as [DsLibraryExtractor] (no absolute paths, no `..`, canonical
     * path must stay inside [staging], Finder junk skipped) and caps of 200,000 entries / 16 GB.
     * Reads with [ZipFile] (ZIP64-safe central directory, exact total for the progress bar). Only when [ZipFile]
     * rejects the archive with a [ZipException] does it retry once with the streaming [ZipInputStream]; if both fail the
     * error names both reasons.
     */
    private fun extractZip(pack: SoundPack, zip: File, staging: File) {
        val dest = staging.canonicalFile
        val prefix = dest.path + File.separator
        val buf = ByteArray(64 * 1024)
        var entries = 0
        var written = 0L
        var lastT = 0L

        /** null = skip this entry; throws for an unsafe name. */
        fun targetOf(name: String): File? {
            val raw = name.replace('\\', '/')
            if (raw.startsWith("__MACOSX/") || raw.substringAfterLast('/').startsWith("._")) return null
            if (raw.startsWith("/") || raw.split('/').any { it == ".." } || (raw.length > 1 && raw[1] == ':')) {
                throw IOException("unsafe path in archive (zip-slip): $name")
            }
            val out = File(dest, raw).canonicalFile
            if (out.path != dest.path && !out.path.startsWith(prefix)) throw IOException("unsafe path in archive (zip-slip): $name")
            return out
        }

        fun copyEntry(ins: InputStream, out: File, done: () -> Long, total: Long) {
            out.parentFile?.mkdirs()
            out.outputStream().use { os ->
                while (true) {
                    if (cancelFlag.get()) throw SpCancelled()
                    val n = ins.read(buf)
                    if (n < 0) break
                    written += n
                    if (written > MAX_UNPACKED_BYTES) throw IOException("archive is too large when unpacked")
                    os.write(buf, 0, n)
                    val now = SystemClock.elapsedRealtime()
                    if (now - lastT >= 300) {
                        lastT = now
                        publish(Progress(pack, Phase.EXTRACTING, done(), total, 0.0))
                    }
                }
            }
        }

        fun viaZipFile() {
            ZipFile(zip).use { zf ->
                if (zf.size() > MAX_ENTRIES) throw IOException("archive has too many entries")
                var totalBytes = 0L
                val scan = zf.entries()
                while (scan.hasMoreElements()) { val sz = scan.nextElement().size; if (sz > 0) totalBytes += sz }
                totalBytes = max(totalBytes, 1L)
                val en = zf.entries()
                while (en.hasMoreElements()) {
                    checkCancel()
                    val e = en.nextElement()
                    if (++entries > MAX_ENTRIES) throw IOException("archive has too many entries")
                    val out = targetOf(e.name) ?: continue
                    if (e.isDirectory) { out.mkdirs(); continue }
                    if (out.path == dest.path) continue
                    zf.getInputStream(e).use { ins -> copyEntry(ins, out, { written }, totalBytes) }
                }
            }
        }

        fun viaStream() {
            val len = max(zip.length(), 1L)
            val counter = SpCountingStream(zip.inputStream().buffered(64 * 1024))
            ZipInputStream(counter).use { zin ->
                while (true) {
                    checkCancel()
                    val e = zin.nextEntry ?: break
                    if (++entries > MAX_ENTRIES) throw IOException("archive has too many entries")
                    val out = targetOf(e.name) ?: continue
                    if (e.isDirectory) { out.mkdirs(); continue }
                    if (out.path == dest.path) continue
                    copyEntry(zin, out, { counter.count }, len)
                }
            }
        }

        try {
            viaZipFile()
        } catch (first: ZipException) {
            // non-standard header: start over with the streaming reader
            dest.listFiles()?.forEach { it.deleteRecursively() }
            entries = 0; written = 0L; lastT = 0L
            try {
                viaStream()
            } catch (c: SpCancelled) {
                throw c
            } catch (second: Exception) {
                throw IOException("ZIP unreadable: ${first.message ?: first.javaClass.simpleName}; stream fallback: ${second.message ?: second.javaClass.simpleName}")
            }
        }
    }

    // ------------------------------------------------------------------ uninstall

    /**
     * Delete the pack's own install folder (on `a.loader`). Refuses anything that is not a direct child of the
     * pack's format root inside `files/library/user/`, and any folder without the `.hsynth_installed.json` marker
     * (so hand-imported files with the same folder name survive). [onDone] runs on the UI thread.
     */
    fun uninstall(pack: SoundPack, onDone: (ok: Boolean, message: String) -> Unit = { _, _ -> }) {
        if (isActive(pack)) { onDone(false, L10n.t("Cancel the download first.")); return }
        a.loader.execute {
            var ok = false
            val msg = try {
                val userRoot = File(a.filesDir, "library/user").canonicalFile
                val fmtRoot = pack.installRoot(a).canonicalFile
                val dir = pack.installDir(a).canonicalFile
                if (!fmtRoot.path.startsWith(userRoot.path + File.separator) || dir.parentFile != fmtRoot) {
                    throw IOException("refusing to delete outside the pack folder")
                }
                if (!dir.exists()) {
                    ok = true
                    L10n.t("Not installed.")
                } else if (!File(dir, SoundCatalog.MARKER).isFile) {
                    throw IOException("this folder was not installed by Sound Packs")
                } else {
                    val bytes = SoundCatalog.dirSize(dir)
                    if (!dir.deleteRecursively()) throw IOException("some files could not be deleted")
                    ok = true
                    L10n.f("Deleted \"%s\" (%s freed)", pack.name, spFmtBytes(bytes))
                }
            } catch (e: Exception) {
                L10n.f("Delete failed: %s", e.message ?: e.javaClass.simpleName)
            }
            a.ui.post {
                if (a.isDestroyed) return@post
                catalog.refreshInstalled()
                a.log("sound pack ${pack.id}: $msg", if (ok) Ui.TEXT else Ui.WARN)
                onDone(ok, msg)
                if (ok) runCatching { a.libraryPanel.refresh() }
            }
        }
    }
}

// ====================================================================================================== shared helpers

/** One folder name: no separators, no leading / trailing dots (so `..` can never survive). */
private fun spSafeSegment(s: String): String =
    s.replace(Regex("[^A-Za-z0-9._ -]"), "_").trim().trim('.').trim().take(80)

private fun spUrlPath(url: String): String = url.substringBefore('#').substringBefore('?').lowercase(Locale.ROOT)

private fun spUrlLastSegment(url: String): String = url.substringBefore('#').substringBefore('?').substringAfterLast('/')

private fun spFmtBytes(b: Long): String = when {
    b >= 1_073_741_824L -> String.format(Locale.US, "%.1f GB", b / 1_073_741_824.0)
    b >= 1_048_576L -> String.format(Locale.US, "%.0f MB", b / 1_048_576.0)
    else -> String.format(Locale.US, "%d KB", b / 1024)
}

private fun spFmtSizeMB(mb: Double): String = when {
    mb <= 0.0 -> "—"
    mb >= 1024.0 -> String.format(Locale.US, "~%.1f GB", mb / 1024.0)
    else -> String.format(Locale.US, "~%d MB", Math.round(mb))
}

private fun spReadLimited(ins: InputStream, limit: Int): String {
    val bos = java.io.ByteArrayOutputStream()
    val buf = ByteArray(8192)
    while (true) {
        val n = ins.read(buf)
        if (n < 0) break
        bos.write(buf, 0, n)
        if (bos.size() > limit) throw IOException("file is larger than ${limit / 1024 / 1024} MB")
    }
    return bos.toString("UTF-8")
}

private fun spOpenUrl(a: Context, url: String) {
    if (!url.startsWith("https://", true) && !url.startsWith("http://", true)) return
    try {
        a.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))
    } catch (e: ActivityNotFoundException) {
        FormatFiles.toast(a, L10n.t("No browser available."))
    }
}

private enum class SpLic { OPEN, ATTRIBUTION, NON_COMMERCIAL, UNKNOWN }

private fun spLicKind(license: String): SpLic {
    val s = license.lowercase(Locale.ROOT).trim()
    return when {
        s.isEmpty() || s.contains("unknown") -> SpLic.UNKNOWN
        s.contains("-nc") || s.contains(" nc") || s.contains("noncommercial") || s.contains("non-commercial") ||
            s.contains("proprietary") -> SpLic.NON_COMMERCIAL
        s.contains("cc0") || s.contains("public domain") || s == "pd" || s.contains("unlicense") -> SpLic.OPEN
        else -> SpLic.ATTRIBUTION
    }
}

/** CC0 green · MIT / CC BY / other attribution licenses amber · non-commercial red · unknown grey. */
private fun spLicColor(license: String): Int = when (spLicKind(license)) {
    SpLic.OPEN -> Hw.MOD_GOOD
    SpLic.ATTRIBUTION -> Hw.MOD_FILTER
    SpLic.NON_COMMERCIAL -> Hw.MOD_RED
    SpLic.UNKNOWN -> Hw.ENGRAVE
}

private fun spFormatColor(format: String): Int = when (format) {
    "sfz" -> Hw.MOD_OSC                                   // cyan
    "sf2", "sf3", "dls", "gig" -> Hw.MOD_FX               // emerald
    "dslibrary", "dspreset" -> Hw.MOD_ENV                 // violet
    "syx" -> Hw.MOD_RED                                   // red
    else -> Hw.MOD_FILTER                                 // wt / wav: amber
}

private fun spFormatLabel(format: String): String = if (format == "syx") "DX7" else if (format == "dslibrary" || format == "dspreset") "DS" else format.uppercase(Locale.ROOT)

private fun spCopyright(p: SoundPack): String {
    p.licenseText.lineSequence().map { it.trim() }.firstOrNull { it.contains("copyright", true) || it.contains("©") }?.let { return it }
    val lic = p.license.ifEmpty { L10n.t("the terms at the license link") }
    return if (p.author.isNotEmpty()) "© ${p.author}. " + L10n.f("Licensed under %s.", lic) else L10n.f("Licensed under %s.", lic)
}

/** Full license text of a pack with an `Open License URL` button. */
private fun spShowLicense(a: Context, p: SoundPack) {
    val sb = StringBuilder()
    sb.append(L10n.t("Author")).append(": ").append(p.author.ifEmpty { "—" }).append('\n')
    sb.append(L10n.t("License")).append(": ").append(p.license.ifEmpty { "—" }).append('\n')
    if (p.licenseUrl.isNotEmpty()) sb.append(p.licenseUrl).append('\n')
    if (spLicKind(p.license) == SpLic.NON_COMMERCIAL) sb.append('\n').append(L10n.t("Not cleared for commercial use.")).append('\n')
    sb.append('\n').append(p.licenseText.ifEmpty { L10n.t("(The catalog gives no license text. Open the license link.)") })
    val b = AlertDialog.Builder(a).setTitle(p.name).setMessage(sb.toString()).setNegativeButton(L10n.t("Close"), null)
    if (p.licenseUrl.isNotEmpty()) b.setPositiveButton(L10n.t("Open License URL")) { _, _ -> spOpenUrl(a, p.licenseUrl) }
    b.show()
}

/** Rounded progress bar (no allocation while drawing). */
private class SpBarView(ctx: Context) : View(ctx) {
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)
    private val r = RectF()
    var fraction = 0f
        set(v) { if (v != field) { field = v; invalidate() } }

    override fun onDraw(c: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        p.color = Hw.PANEL_EDGE
        r.set(0f, 0f, w, h)
        c.drawRoundRect(r, h / 2f, h / 2f, p)
        if (fraction > 0f) {
            p.color = Hw.MOD_OSC
            r.set(0f, 0f, max(h, w * fraction), h)
            c.drawRoundRect(r, h / 2f, h / 2f, p)
        }
    }
}

// ====================================================================================================== store / manager sheet

/**
 * SOUND PACKS store (CENTER 840 × 366 dp). Top bar: title · category pills · `+ ADD JSON` (long-press: manage imported
 * catalogs) · `TEMPLATE` · `LICENSES` · ✕. Sub-bar: free storage and, while a download runs, name + bar + MB/s + CANCEL.
 * Recycled list of pack cards (format colour bar · name · author · LICENSE badge · size · format · description ·
 * OPEN / DELETE / DOWNLOAD / OPEN SITE ↗). Built lazily; [catalog] and [downloader] are public so the owner can reuse them.
 *
 * @param onOpenPack called with the pack and its install folder when OPEN is tapped.
 */
class SoundPackSheet(private val a: MainActivity, private val onOpenPack: (SoundPack, File) -> Unit) : Sheet(Edge.CENTER, 840, 366) {
    override val view: View by lazy { build() }

    private val d = a.resources.displayMetrics.density
    private fun dp(v: Int) = (v * d).toInt()

    val catalog = SoundCatalog(a)
    val downloader = SoundPackDownloader(a, catalog)
    val licensesSheet: SoundLicensesSheet by lazy { SoundLicensesSheet(a, catalog).also { it.returnTo = this } }

    private val filters = listOf("ALL", "PIANO", "ORCH", "DRUMS", "GUITAR/BASS", "GM", "SYNTH/WT", "CUSTOM")
    private var filter = 0
    private var shown: List<SoundPack> = emptyList()
    private var built = false
    private var lastActiveId: String? = null
    private var lastWarn = ""
    /** packId → why its last download failed; cleared when the pack starts again or installs. */
    private val packErrors = HashMap<String, String>()

    private lateinit var pills: Array<LedButton>
    private lateinit var freeTv: TextView
    private lateinit var strip: LinearLayout
    private lateinit var stripTv: TextView
    private lateinit var stripBar: SpBarView
    private lateinit var list: ListView
    private lateinit var emptyTv: TextView
    private lateinit var batchChip: LedButton
    private lateinit var batchBar: LinearLayout
    private lateinit var batchHint: TextView
    private lateinit var allBtn: LedButton
    private lateinit var startBtn: LedButton
    private lateinit var skipBtn: LedButton
    private lateinit var stopAllBtn: LedButton

    /** BATCH mode: pack ids in the exact order they were tapped (index + 1 = the number shown on the card). */
    private var batchMode = false
    private val batchSel = ArrayList<String>()

    private enum class Act { NONE, DOWNLOAD, CANCEL, OPEN, DELETE, SITE, UNQUEUE, PICK }

    private val adapter = object : BaseAdapter() {
        override fun getCount() = shown.size
        override fun getItem(position: Int): Any = shown[position]
        override fun getItemId(position: Int) = position.toLong()
        override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
            val r = (convertView as? PackRow) ?: PackRow(a)
            r.bind(shown[position])
            return r
        }
    }

    private val dlListener = object : SoundPackDownloader.Listener {
        override fun onProgress(p: SoundPackDownloader.Progress?) {
            if (!built) return
            updateStrip(p)
            val id = p?.pack?.id
            if (id != null) packErrors.remove(id)
            if (id != lastActiveId) {
                lastActiveId = id
                adapter.notifyDataSetChanged()
            }
        }

        override fun onQueueChanged() {
            if (!built) return
            updateStrip(downloader.active)
            refreshBatchUi()
        }

        override fun onResult(pack: SoundPack, ok: Boolean, message: String) {
            FormatFiles.toast(a, message)
            if (ok || message == L10n.t("Download cancelled.")) packErrors.remove(pack.id) else packErrors[pack.id] = message
            // the finished pack turns green (INSTALLED / OPEN) right away while the next one downloads
            if (built) { updateFree(); refreshBatchUi() }
        }
    }

    init {
        downloader.addListener(dlListener)
    }

    override fun onShow() {
        view
        registerResults()
        catalog.reload()
        val w = catalog.warnings.take(4).joinToString("; ")
        if (w.isNotEmpty() && w != lastWarn) a.log("sound pack catalog: $w", Ui.WARN)
        lastWarn = w
        batchMode = false
        batchSel.clear()
        updateFree()
        updateStrip(downloader.active)
        lastActiveId = downloader.active?.pack?.id
        applyFilter()
    }

    // ------------------------------------------------------------------ build

    private fun smallBtn(label: String, accent: Int = Hw.MOD_OSC, onClick: () -> Unit) =
        LedButton(a, label, tr = false, led = false, accentColor = accent).apply {
            textSize = 11f
            minHeight = 0; minimumHeight = 0
            setPadding(dp(9), 0, dp(9), 0)
            setOnClickListener { onClick() }
        }

    private fun build(): View {
        val root = LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(10), dp(2), dp(10), dp(8))
        }

        // top bar
        val top = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        top.addView(Hw.engraved(a, "SOUND PACKS", 12f, tr = false, color = Hw.MOD_OSC), LinearLayout.LayoutParams(-2, -2).apply { marginEnd = dp(10) })
        val pillRow = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        pills = Array(filters.size) { i ->
            smallBtn(filters[i]) { setFilter(i) }.also {
                pillRow.addView(it, LinearLayout.LayoutParams(-2, dp(28)).apply { marginEnd = dp(3) })
            }
        }
        val scroll = HorizontalScrollView(a).apply {
            isHorizontalScrollBarEnabled = false
            overScrollMode = View.OVER_SCROLL_NEVER
            addView(pillRow, FrameLayout.LayoutParams(-2, -2))
        }
        top.addView(scroll, LinearLayout.LayoutParams(0, -2, 1f))
        batchChip = smallBtn("BATCH", Hw.MOD_FILTER) { setBatchMode(!batchMode) }
        top.addView(batchChip, LinearLayout.LayoutParams(-2, dp(28)).apply { marginStart = dp(8) })
        val add = smallBtn("+ ADD JSON") { pickCatalogJson() }
        add.setOnLongClickListener { manageCatalogs(); true }
        top.addView(add, LinearLayout.LayoutParams(-2, dp(28)).apply { marginStart = dp(8) })
        top.addView(smallBtn("TEMPLATE") { pickTemplateTarget() }, LinearLayout.LayoutParams(-2, dp(28)).apply { marginStart = dp(4) })
        top.addView(smallBtn("LICENSES") { a.openSheet(licensesSheet) }, LinearLayout.LayoutParams(-2, dp(28)).apply { marginStart = dp(4) })
        top.addView(
            LedButton(a, "✕", tr = false, led = false).apply {
                minHeight = 0; minimumHeight = 0; setPadding(0, 0, 0, 0)
                setOnClickListener { dismiss() }
            }, LinearLayout.LayoutParams(dp(36), dp(28)).apply { marginStart = dp(6) }
        )
        root.addView(top, LinearLayout.LayoutParams(-1, dp(32)))

        // sub-bar: free storage + download strip
        val sub = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        freeTv = TextView(a).apply { typeface = Hw.MONO; textSize = 11.5f; setTextColor(Hw.ENGRAVE); maxLines = 1 }
        sub.addView(freeTv, LinearLayout.LayoutParams(-2, -2))
        strip = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL; visibility = View.GONE }
        stripTv = TextView(a).apply {
            typeface = Hw.MONO; textSize = 10.5f; setTextColor(Hw.SILK); maxLines = 1; ellipsize = TextUtils.TruncateAt.END
        }
        stripBar = SpBarView(a)
        strip.addView(stripTv, LinearLayout.LayoutParams(0, -2, 1f))
        strip.addView(stripBar, LinearLayout.LayoutParams(dp(80), dp(8)).apply { marginStart = dp(6); marginEnd = dp(6) })
        skipBtn = smallBtn("CANCEL", Hw.MOD_RED) { downloader.cancelCurrent() }
        stopAllBtn = smallBtn("STOP ALL", Hw.MOD_RED) { downloader.cancelAll() }.also { it.visibility = View.GONE }
        strip.addView(skipBtn, LinearLayout.LayoutParams(-2, dp(26)))
        strip.addView(stopAllBtn, LinearLayout.LayoutParams(-2, dp(26)).apply { marginStart = dp(4) })
        sub.addView(strip, LinearLayout.LayoutParams(0, -2, 1f).apply { marginStart = dp(10) })
        root.addView(sub, LinearLayout.LayoutParams(-1, dp(30)))

        // batch bar (only in BATCH mode): hint · ALL (N) · START (N · X.X GB) · CLEAR
        batchBar = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL; visibility = View.GONE }
        batchHint = TextView(a).apply {
            typeface = Hw.LABEL; textSize = 11f; setTextColor(Hw.MOD_FILTER); maxLines = 1; ellipsize = TextUtils.TruncateAt.END
            text = L10n.t("BATCH: tap packs in the order you want them downloaded")
        }
        allBtn = smallBtn("ALL", Hw.MOD_OSC) { batchSelectAll() }
        startBtn = smallBtn("START", Hw.MOD_GOOD) { batchStart() }
        batchBar.addView(batchHint, LinearLayout.LayoutParams(0, -2, 1f))
        batchBar.addView(allBtn, LinearLayout.LayoutParams(-2, dp(26)).apply { marginStart = dp(6) })
        batchBar.addView(startBtn, LinearLayout.LayoutParams(-2, dp(26)).apply { marginStart = dp(4) })
        batchBar.addView(smallBtn("CLEAR", Hw.MOD_RED) { batchClear() }, LinearLayout.LayoutParams(-2, dp(26)).apply { marginStart = dp(4) })
        root.addView(batchBar, LinearLayout.LayoutParams(-1, dp(28)))

        // list (+ empty state)
        list = ListView(a).apply {
            adapter = this@SoundPackSheet.adapter
            background = Hw.lcdBg(a)
            divider = ColorDrawable(Hw.PANEL_EDGE)
            dividerHeight = 1
            selector = ColorDrawable(0)
            cacheColorHint = 0
            isVerticalScrollBarEnabled = false
            overScrollMode = View.OVER_SCROLL_NEVER
            onItemClickListener = AdapterView.OnItemClickListener { _, _, pos, _ ->
                if (batchMode) shown.getOrNull(pos)?.let { toggleSelect(it) }
            }
        }
        emptyTv = Ui.text(a, "No sound packs here. Add a catalog with + ADD JSON.", 13f, Hw.INK_DIM).apply {
            gravity = Gravity.CENTER
            visibility = View.GONE
        }
        val listBox = FrameLayout(a)
        listBox.addView(list, FrameLayout.LayoutParams(-1, -1))
        listBox.addView(emptyTv, FrameLayout.LayoutParams(-1, -1))
        list.emptyView = emptyTv
        root.addView(listBox, LinearLayout.LayoutParams(-1, 0, 1f).apply { topMargin = dp(2) })

        built = true
        updatePills()
        return root
    }

    private fun setFilter(i: Int) {
        filter = i
        updatePills()
        applyFilter()
    }

    private fun updatePills() {
        if (!::pills.isInitialized) return
        for (i in pills.indices) pills[i].lit = i == filter
    }

    private fun matches(p: SoundPack, f: Int): Boolean {
        val c = p.category
        return when (f) {
            1 -> c.contains("piano")
            2 -> c.contains("orch") || c.contains("string") || c.contains("brass") || c.contains("wind") || c.contains("choir")
            3 -> c.contains("drum") || c.contains("perc")
            4 -> c.contains("guitar") || c.contains("bass")
            5 -> c == "gm" || c.contains("general midi") || c.startsWith("gm ") || c.startsWith("gm/")
            6 -> c.contains("synth") || c.contains("wt") || c.contains("wavetable")
            7 -> !p.isBuiltIn || c == "custom"
            else -> true
        }
    }

    private fun applyFilter() {
        if (!built) return
        shown = catalog.packs.filter { matches(it, filter) }
        refreshBatchUi()
    }

    // ------------------------------------------------------------------ batch selection

    /** Direct-download pack that is neither installed nor active / waiting: the only kind a batch can contain. */
    private fun batchSelectable(p: SoundPack): Boolean =
        p.directDownload && p.url.isNotEmpty() && !catalog.isInstalled(p) && downloader.queuePosition(p.id) < 0

    private fun setBatchMode(on: Boolean) {
        batchMode = on
        if (!on) batchSel.clear()
        refreshBatchUi()
    }

    private fun toggleSelect(p: SoundPack) {
        if (!batchSelectable(p)) return
        if (!batchSel.remove(p.id)) batchSel.add(p.id)   // un-selecting closes the gap: the rest re-number
        refreshBatchUi()
    }

    private fun batchSelectAll() {
        val cand = shown.filter { batchSelectable(it) }
        if (cand.isEmpty()) return
        if (cand.all { it.id in batchSel }) batchSel.removeAll(cand.map { it.id }.toSet())
        else for (p in cand) if (p.id !in batchSel) batchSel.add(p.id)   // keeps the order already tapped, then displayed order
        refreshBatchUi()
    }

    private fun batchClear() = setBatchMode(false)

    private fun batchStart() {
        val picks = batchSel.mapNotNull { id -> catalog.find(id) }.filter { batchSelectable(it) }
        if (picks.isEmpty()) { FormatFiles.toast(a, L10n.t("Select packs first.")); return }
        downloader.enqueueAll(picks) { setBatchMode(false) }   // exits BATCH only once the packs are really queued
    }

    /** Re-evaluates selection, batch bar, header chip and the card list. */
    private fun refreshBatchUi() {
        if (!built) return
        batchSel.retainAll { id -> catalog.find(id)?.let { batchSelectable(it) } == true }
        batchChip.lit = batchMode
        batchBar.visibility = if (batchMode) View.VISIBLE else View.GONE
        if (batchMode) {
            val cand = shown.count { batchSelectable(it) }
            val mb = batchSel.sumOf { id -> catalog.find(id)?.sizeMB ?: 0.0 }
            allBtn.text = "ALL ($cand)"
            startBtn.text = if (mb < 1000.0) String.format(Locale.US, "START (%d · %d MB)", batchSel.size, Math.round(mb))
            else String.format(Locale.US, "START (%d · %.1f GB)", batchSel.size, mb / 1024.0)
            startBtn.isEnabled = batchSel.isNotEmpty()
            startBtn.alpha = if (batchSel.isNotEmpty()) 1f else 0.4f
        }
        adapter.notifyDataSetChanged()
    }

    private fun updateFree() {
        freeTv.text = L10n.t("Free:") + " " + spFmtBytes(SoundPackDownloader.freeBytes(a))
    }

    private fun updateStrip(p: SoundPackDownloader.Progress?) {
        if (!built) return
        if (p == null) { strip.visibility = View.GONE; stripBar.fraction = 0f; return }
        strip.visibility = View.VISIBLE
        val pct = p.percent
        val tail = when (p.phase) {
            SoundPackDownloader.Phase.PREPARING -> L10n.t("preparing…")
            SoundPackDownloader.Phase.DOWNLOADING ->
                if (!p.exactTotal && (p.bytes > p.total || p.total <= 0)) {
                    // size unknown and the estimate is already exceeded: no fake percentage
                    String.format(Locale.US, "%.0f MB · %.1f MB/s", p.bytes / 1048576.0, p.bytesPerSec / 1048576.0)
                } else {
                    String.format(
                        Locale.US, if (p.exactTotal) "%d%%  %.0f/%.0f MB  %.1f MB/s" else "%d%%  %.0f/~%.0f MB  %.1f MB/s",
                        pct, p.bytes / 1048576.0, p.total / 1048576.0, p.bytesPerSec / 1048576.0
                    )
                }
            SoundPackDownloader.Phase.EXTRACTING -> L10n.t("Extracting...") + " $pct%"
            SoundPackDownloader.Phase.FINISHING -> L10n.t("Finishing...")
        }
        val waiting = downloader.queueCount()
        stripTv.text = "${p.pack.name}  ·  $tail" + if (waiting > 0) "  [" + L10n.f("+%s queued", waiting.toString()) + "]" else ""
        skipBtn.text = if (waiting > 0) "SKIP" else "CANCEL"
        stopAllBtn.visibility = if (waiting > 0) View.VISIBLE else View.GONE
        stripBar.fraction = if (p.phase == SoundPackDownloader.Phase.PREPARING) 0f else p.fraction
    }

    // ------------------------------------------------------------------ actions

    private fun onAction(p: SoundPack, act: Act) {
        when (act) {
            Act.DOWNLOAD -> downloader.enqueue(p)   // starts at once when idle, otherwise joins the back of the queue
            Act.CANCEL -> downloader.cancelCurrent()
            Act.UNQUEUE -> downloader.removeFromQueue(p.id)
            Act.PICK -> toggleSelect(p)
            Act.OPEN -> catalog.installedInfo(p)?.let { onOpenPack(p, it.dir) }
            Act.DELETE -> confirmDelete(p)
            Act.SITE -> spOpenUrl(a, p.webUrl.ifEmpty { p.url })
            Act.NONE -> {}
        }
    }

    private fun confirmDelete(p: SoundPack) {
        val size = catalog.installedInfo(p)?.bytes?.let { spFmtBytes(it) } ?: ""
        AlertDialog.Builder(a)
            .setTitle(L10n.f("Delete \"%s\"?", p.name))
            .setMessage(L10n.f("The installed files (%s) are removed from this device. You can download the pack again later.", size))
            .setPositiveButton(L10n.t("Delete")) { _, _ ->
                downloader.uninstall(p) { _, msg ->
                    FormatFiles.toast(a, msg)
                    updateFree()
                    adapter.notifyDataSetChanged()
                }
            }
            .setNegativeButton(L10n.t("Cancel"), null)
            .show()
    }

    // ------------------------------------------------------------------ catalog JSON import / template

    private fun registerResults() {
        ActivityResults.register(SoundCatalog.REQ_IMPORT_JSON) { data ->
            data?.data?.let { uri ->
                catalog.importCatalogJson(uri) { _, msg ->
                    FormatFiles.toast(a, msg)
                    applyFilter()
                }
            }
        }
        ActivityResults.register(SoundCatalog.REQ_EXPORT_TEMPLATE) { data ->
            data?.data?.let { uri -> catalog.exportTemplateJson(uri) { _, msg -> FormatFiles.toast(a, msg) } }
        }
    }

    private fun pickCatalogJson() {
        registerResults()
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
        }
        @Suppress("DEPRECATION")
        a.startActivityForResult(intent, SoundCatalog.REQ_IMPORT_JSON)
    }

    private fun pickTemplateTarget() {
        registerResults()
        val intent = Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "application/json"
            putExtra(Intent.EXTRA_TITLE, SoundCatalog.TEMPLATE_NAME)
        }
        @Suppress("DEPRECATION")
        a.startActivityForResult(intent, SoundCatalog.REQ_EXPORT_TEMPLATE)
    }

    /** Long-press on + ADD JSON: list the imported catalogs; a tap removes one (its packs vanish, installed files stay). */
    private fun manageCatalogs() {
        val files = catalog.listUserCatalogs()
        if (files.isEmpty()) { FormatFiles.toast(a, L10n.t("No custom catalogs imported yet.")); return }
        AlertDialog.Builder(a)
            .setTitle(L10n.t("Custom catalogs (tap to remove)"))
            .setItems(files.map { it.name }.toTypedArray()) { _, i ->
                val f = files[i]
                AlertDialog.Builder(a)
                    .setTitle(L10n.f("Remove \"%s\"?", f.name))
                    .setMessage(L10n.t("The catalog file is deleted. Packs that are already installed stay on the device."))
                    .setPositiveButton(L10n.t("Remove")) { _, _ ->
                        catalog.deleteUserCatalog(f)
                        applyFilter()
                    }
                    .setNegativeButton(L10n.t("Cancel"), null)
                    .show()
            }
            .setNegativeButton(L10n.t("Close"), null)
            .show()
    }

    // ------------------------------------------------------------------ one card

    private inner class PackRow(ctx: Context) : LinearLayout(ctx) {
        private val bar = View(ctx)
        private val nameTv = TextView(ctx)
        private val authorTv = TextView(ctx)
        private val licTv = TextView(ctx)
        private val fmtTv = TextView(ctx)
        private val sizeTv = TextView(ctx)
        private val stateTv = TextView(ctx)
        private val descTv = TextView(ctx)
        private val errTv = TextView(ctx)
        private val btn1: LedButton
        private val btn2: LedButton
        private var pack: SoundPack? = null
        private var act1 = Act.NONE
        private var act2 = Act.NONE

        init {
            orientation = HORIZONTAL
            layoutParams = AbsListView.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            setPadding(0, dp(5), dp(8), dp(5))
            gravity = Gravity.CENTER_VERTICAL

            addView(bar, LinearLayout.LayoutParams(dp(4), ViewGroup.LayoutParams.MATCH_PARENT))

            val mid = LinearLayout(ctx).apply { orientation = VERTICAL }
            nameTv.apply { typeface = Hw.BOLD; textSize = 14.5f; setTextColor(Hw.SILK); maxLines = 1; ellipsize = TextUtils.TruncateAt.END }
            authorTv.apply { typeface = Hw.LABEL; textSize = 11.5f; setTextColor(Hw.ENGRAVE); maxLines = 1; ellipsize = TextUtils.TruncateAt.END }
            val l1 = LinearLayout(ctx).apply { orientation = HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
            l1.addView(nameTv, LinearLayout.LayoutParams(-2, -2))
            l1.addView(authorTv, LinearLayout.LayoutParams(0, -2, 1f).apply { marginStart = dp(8) })
            mid.addView(l1, LinearLayout.LayoutParams(-1, -2))

            licTv.apply {
                typeface = Hw.MONO; textSize = 10.5f; maxLines = 1; ellipsize = TextUtils.TruncateAt.END
                setPadding(dp(6), dp(1), dp(6), dp(1)); isClickable = true; maxWidth = dp(130)
                setOnClickListener { pack?.let { spShowLicense(a, it) } }
            }
            fmtTv.apply { typeface = Hw.MONO; textSize = 10.5f; setPadding(dp(6), dp(1), dp(6), dp(1)); maxLines = 1 }
            sizeTv.apply { typeface = Hw.MONO; textSize = 11.5f; setTextColor(Hw.LCD_DIM); maxLines = 1 }
            stateTv.apply { typeface = Hw.MONO; textSize = 10.5f; maxLines = 1; ellipsize = TextUtils.TruncateAt.END }
            val l2 = LinearLayout(ctx).apply { orientation = HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
            l2.addView(licTv, LinearLayout.LayoutParams(-2, -2))
            l2.addView(fmtTv, LinearLayout.LayoutParams(-2, -2).apply { marginStart = dp(5) })
            l2.addView(sizeTv, LinearLayout.LayoutParams(-2, -2).apply { marginStart = dp(8) })
            l2.addView(stateTv, LinearLayout.LayoutParams(0, -2, 1f).apply { marginStart = dp(8) })
            mid.addView(l2, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(3) })

            descTv.apply { typeface = Hw.LABEL; textSize = 11.5f; setTextColor(Hw.ENGRAVE); maxLines = 2; ellipsize = TextUtils.TruncateAt.END }
            mid.addView(descTv, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(3) })
            errTv.apply {
                typeface = Hw.MONO; textSize = 10.5f; setTextColor(Hw.MOD_RED); maxLines = 3; ellipsize = TextUtils.TruncateAt.END
                visibility = View.GONE
            }
            mid.addView(errTv, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(3) })
            addView(mid, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply { marginStart = dp(10) })

            btn1 = smallBtn("", Hw.MOD_OSC) { pack?.let { onAction(it, act1) } }
            btn2 = smallBtn("", Hw.MOD_RED) { pack?.let { onAction(it, act2) } }
            val right = LinearLayout(ctx).apply { orientation = HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
            right.addView(btn1, LinearLayout.LayoutParams(-2, dp(34)))
            right.addView(btn2, LinearLayout.LayoutParams(-2, dp(34)).apply { marginStart = dp(5) })
            addView(right, LinearLayout.LayoutParams(-2, -2).apply { marginStart = dp(8) })
        }

        private fun badge(tv: TextView, color: Int) {
            tv.setTextColor(color)
            tv.background = Ui.rounded(Hw.withAlpha(color, 38), dp(3))
        }

        private fun setBtn(b: LedButton, label: String, act: Act, enabled: Boolean = true) {
            b.visibility = if (act == Act.NONE) View.GONE else View.VISIBLE
            b.text = label
            b.isEnabled = enabled
            b.alpha = if (enabled) 1f else 0.4f
        }

        fun bind(p: SoundPack) {
            pack = p
            val fc = spFormatColor(p.format)
            bar.setBackgroundColor(fc)
            nameTv.text = p.name
            authorTv.text = if (p.author.isEmpty()) "" else "· ${p.author}"
            licTv.text = p.license.ifEmpty { "LICENSE ?" }
            badge(licTv, spLicColor(p.license))
            fmtTv.text = spFormatLabel(p.format)
            badge(fmtTv, fc)
            sizeTv.text = spFmtSizeMB(p.sizeMB)
            descTv.text = p.description
            descTv.visibility = if (p.description.isEmpty()) View.GONE else View.VISIBLE

            val err = packErrors[p.id]
            errTv.visibility = if (err.isNullOrEmpty()) View.GONE else View.VISIBLE
            errTv.text = if (err.isNullOrEmpty()) "" else "⚠ $err"

            val inst = catalog.installedInfo(p)
            val qpos = downloader.queuePosition(p.id)   // 0 = active · n = waiting #n · -1 = idle
            val selIdx = if (batchMode) batchSel.indexOf(p.id) else -1
            var pickLit = false
            var accent1 = Hw.MOD_OSC
            when {
                qpos == 0 -> {
                    act1 = Act.CANCEL; act2 = Act.NONE
                    setBtn(btn1, if (downloader.queueCount() > 0) "SKIP" else "CANCEL", act1); setBtn(btn2, "", act2)
                    accent1 = Hw.MOD_RED
                }
                qpos > 0 -> {
                    act1 = Act.UNQUEUE; act2 = Act.NONE
                    setBtn(btn1, "#$qpos ✕", act1); setBtn(btn2, "", act2)
                    accent1 = Hw.MOD_FILTER; pickLit = true
                }
                inst != null -> {
                    act1 = Act.OPEN; act2 = Act.DELETE
                    setBtn(btn1, "OPEN", act1); setBtn(btn2, "DELETE", act2)
                }
                p.directDownload && p.url.isNotEmpty() && batchMode -> {
                    act1 = Act.PICK; act2 = Act.NONE
                    setBtn(btn1, if (selIdx >= 0) (selIdx + 1).toString() else "SELECT", act1); setBtn(btn2, "", act2)
                    accent1 = Hw.MOD_FILTER; pickLit = selIdx >= 0
                }
                p.directDownload && p.url.isNotEmpty() -> {
                    act1 = Act.DOWNLOAD; act2 = Act.NONE
                    setBtn(btn1, "DOWNLOAD", act1); setBtn(btn2, "", act2)
                }
                p.webUrl.isNotEmpty() || p.url.isNotEmpty() -> {
                    act1 = Act.SITE; act2 = Act.NONE
                    setBtn(btn1, "OPEN SITE ↗", act1); setBtn(btn2, "", act2)
                }
                else -> {
                    act1 = Act.NONE; act2 = Act.NONE
                    setBtn(btn1, "", act1); setBtn(btn2, "", act2)
                }
            }
            btn1.accent = accent1
            btn1.lit = pickLit   // numbered pill / queue badge glows in the accent colour
            setBackgroundColor(if (selIdx >= 0) Hw.withAlpha(Hw.MOD_FILTER, 30) else 0)

            val src = if (p.isBuiltIn) "" else "  ·  " + p.sourceCatalog
            if (inst != null) {
                stateTv.setTextColor(Hw.MOD_GOOD)
                stateTv.text = "● " + L10n.t("INSTALLED") + "  " + spFmtBytes(inst.bytes) + src
            } else {
                stateTv.setTextColor(Hw.INK_DIM)
                stateTv.text = src.removePrefix("  ·  ")
            }
        }
    }
}

// ====================================================================================================== licenses sheet

/**
 * LICENSES (CENTER 760 × 340 dp): author, license, copyright notice and a tappable license link for every cataloged
 * pack (installed ones first), under a notice that third-party packs stay under their own open licenses.
 * Tap a card for the full license text; tap the link to open it in the browser. [returnTo] (set by [SoundPackSheet])
 * adds a ◀ BACK button.
 */
class SoundLicensesSheet(private val a: MainActivity, private val catalog: SoundPackCatalog) : Sheet(Edge.CENTER, 760, 340) {
    override val view: View by lazy { build() }

    /** Sheet to reopen with ◀ BACK (null = no back button). */
    var returnTo: Sheet? = null

    private val d = a.resources.displayMetrics.density
    private fun dp(v: Int) = (v * d).toInt()

    private var items: List<SoundPack> = emptyList()
    private var built = false
    private lateinit var countTv: TextView
    private lateinit var list: ListView
    private lateinit var backBtn: LedButton

    private val adapter = object : BaseAdapter() {
        override fun getCount() = items.size
        override fun getItem(position: Int): Any = items[position]
        override fun getItemId(position: Int) = position.toLong()
        override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
            val r = (convertView as? Row) ?: Row(a)
            r.bind(items[position], catalog.isInstalled(items[position]))
            return r
        }
    }

    override fun onShow() {
        view
        if (catalog.packs.isEmpty()) catalog.reload() else catalog.refreshInstalled()
        items = catalog.packs.sortedWith(
            compareByDescending<SoundPack> { catalog.isInstalled(it) }.thenBy { it.name.lowercase(Locale.ROOT) }
        )
        val n = items.count { catalog.isInstalled(it) }
        countTv.text = L10n.f("%s installed / %s cataloged", n.toString(), items.size.toString())
        backBtn.visibility = if (returnTo != null) View.VISIBLE else View.GONE
        adapter.notifyDataSetChanged()
        list.setSelection(0)
    }

    private fun build(): View {
        val root = LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(10), dp(2), dp(10), dp(8))
        }
        val head = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        backBtn = LedButton(a, "◀ BACK", tr = false, led = false).apply {
            textSize = 11f; minHeight = 0; minimumHeight = 0; setPadding(dp(9), 0, dp(9), 0)
            visibility = View.GONE
            setOnClickListener { returnTo?.let { a.openSheet(it) } }
        }
        head.addView(backBtn, LinearLayout.LayoutParams(-2, dp(28)).apply { marginEnd = dp(8) })
        head.addView(Hw.engraved(a, "SOUND LICENSES", 12f, tr = false, color = Hw.MOD_OSC), LinearLayout.LayoutParams(0, -2, 1f))
        countTv = Hw.lcd(a, "", 11f, Hw.ENGRAVE).apply { setPadding(dp(8), dp(2), dp(8), dp(2)) }
        head.addView(countTv, LinearLayout.LayoutParams(-2, -2).apply { marginStart = dp(8); marginEnd = dp(8) })
        head.addView(
            LedButton(a, "✕", tr = false, led = false).apply {
                minHeight = 0; minimumHeight = 0; setPadding(0, 0, 0, 0)
                setOnClickListener { dismiss() }
            }, LinearLayout.LayoutParams(dp(36), dp(28))
        )
        root.addView(head, LinearLayout.LayoutParams(-1, dp(32)))

        val notice = TextView(a).apply {
            typeface = Hw.LABEL
            textSize = 11.5f
            setTextColor(Hw.ENGRAVE)
            setPadding(dp(10), dp(8), dp(10), dp(8))
            text = L10n.t(
                "Third-party sound packs are provided by their authors under the open licenses shown below. " +
                    "HSynth does not bundle them: they are downloaded at your request from the catalog links, and every pack " +
                    "stays © its author. Keep the author credit and the license text with anything you redistribute, and follow " +
                    "each license's attribution terms (for example CC BY: name the author). CC0 packs need no credit. " +
                    "Packs marked in red are not cleared for commercial use; check a license before you publish music made with it."
            )
        }

        list = ListView(a).apply {
            addHeaderView(notice, null, false)
            adapter = this@SoundLicensesSheet.adapter
            background = Hw.lcdBg(a)
            divider = ColorDrawable(Hw.PANEL_EDGE)
            dividerHeight = 1
            selector = ColorDrawable(Hw.withAlpha(Hw.MOD_OSC, 30))
            cacheColorHint = 0
            isVerticalScrollBarEnabled = false
            overScrollMode = View.OVER_SCROLL_NEVER
            onItemClickListener = AdapterView.OnItemClickListener { _, _, pos, _ ->
                items.getOrNull(pos - headerViewsCount)?.let { spShowLicense(a, it) }
            }
        }
        root.addView(list, LinearLayout.LayoutParams(-1, 0, 1f).apply { topMargin = dp(4) })
        built = true
        return root
    }

    private inner class Row(ctx: Context) : LinearLayout(ctx) {
        private val nameTv = TextView(ctx)
        private val stateTv = TextView(ctx)
        private val metaTv = TextView(ctx)
        private val noteTv = TextView(ctx)
        private val linkTv = TextView(ctx)
        private var pack: SoundPack? = null

        init {
            orientation = VERTICAL
            layoutParams = AbsListView.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            setPadding(dp(10), dp(6), dp(10), dp(6))
            nameTv.apply { typeface = Hw.BOLD; textSize = 14f; setTextColor(Hw.SILK); maxLines = 1; ellipsize = TextUtils.TruncateAt.END }
            stateTv.apply { typeface = Hw.MONO; textSize = 10.5f; setTextColor(Hw.MOD_GOOD); maxLines = 1 }
            val l1 = LinearLayout(ctx).apply { orientation = HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
            l1.addView(nameTv, LinearLayout.LayoutParams(0, -2, 1f))
            l1.addView(stateTv, LinearLayout.LayoutParams(-2, -2).apply { marginStart = dp(8) })
            addView(l1, LinearLayout.LayoutParams(-1, -2))
            metaTv.apply { typeface = Hw.MONO; textSize = 11.5f; maxLines = 1; ellipsize = TextUtils.TruncateAt.END }
            addView(metaTv, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(2) })
            noteTv.apply { typeface = Hw.LABEL; textSize = 11.5f; setTextColor(Hw.ENGRAVE); maxLines = 2; ellipsize = TextUtils.TruncateAt.END }
            addView(noteTv, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(2) })
            linkTv.apply {
                typeface = Hw.MONO; textSize = 11f; setTextColor(Hw.MOD_OSC); maxLines = 1; ellipsize = TextUtils.TruncateAt.MIDDLE
                paint.isUnderlineText = true
                isClickable = true
                setPadding(0, dp(4), 0, dp(4))
                setOnClickListener { pack?.let { spOpenUrl(a, it.licenseUrl) } }
            }
            addView(linkTv, LinearLayout.LayoutParams(-2, -2))
        }

        fun bind(p: SoundPack, installed: Boolean) {
            pack = p
            nameTv.text = p.name
            stateTv.text = if (installed) "● " + L10n.t("INSTALLED") else ""
            metaTv.text = L10n.t("Author") + ": " + p.author.ifEmpty { "—" } + "   " + L10n.t("License") + ": " + p.license.ifEmpty { "—" }
            metaTv.setTextColor(spLicColor(p.license))
            noteTv.text = spCopyright(p)
            linkTv.text = p.licenseUrl
            linkTv.visibility = if (p.licenseUrl.isEmpty()) View.GONE else View.VISIBLE
        }
    }
}
