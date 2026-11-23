package com.hsynth.app

import android.os.Looper
import android.os.SystemClock
import org.json.JSONArray
import org.json.JSONObject
import org.json.JSONTokener
import java.io.BufferedInputStream
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.nio.file.Files
import java.security.MessageDigest
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone
import java.util.concurrent.CancellationException
import java.util.zip.Deflater
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream

/*
 * B6 — one-tap ZIP backup / restore (Phase 4 Wave 1B). Pure `java.util.zip` + `MessageDigest("SHA-256")`, no deps.
 * Standalone: nothing here touches the engine. Wired in Wave 2 (SYS > SYSTEM, ActivityResults REQ_BACKUP_*, WebEditorServer
 * `/backup.zip`). All heavy methods are blocking: call them from `a.loader` (never the UI thread).
 *
 * Archive layout (paths are relative, '/' separated):
 *   presets/user/...  scenes/...  setups/...  pads/...  scales/...      user content, mirrors `files/<same path>`
 *   setlist.json  master_stage.json  metronome.json  pedal_config.json  rhythm.json  midi_map.json  globals.json
 *   prefs.json                      whitelisted SharedPreferences keys (typed)
 *   library/samples/user/...         optional user samples — mirrors `files/library/user/...` (Library.userSamples)
 *   manifest.json                   LAST entry: format, app version, timestamps, counts, per-file size + SHA-256
 */

data class BackupSummary(
    val files: Int,
    val bytes: Long,          // uncompressed payload (without the manifest)
    val zipBytes: Long,       // bytes written to the stream
    val presets: Int,
    val scenes: Int,
    val includesSamples: Boolean,
    val createdMs: Long,
)

data class RestoreResult(
    val ok: Boolean,
    val error: String? = null,
    val files: Int = 0,               // files placed (replace: every staged file; merge: only the ones that were newer)
    val bytes: Long = 0L,
    val presets: Int = 0,
    val scenes: Int = 0,
    val skipped: Int = 0,             // merge: kept because the existing file was newer; unknown entries are not counted
    val merged: Boolean = false,
    val prefsApplied: Int = 0,
    val createdMs: Long = 0L,         // timestamp stored in the backup's manifest
)

class BackupManager(private val a: MainActivity) {
    companion object {
        const val FORMAT = 1
        const val MAX_ENTRIES = 50_000
        const val MAX_TOTAL_BYTES = 12L * 1024 * 1024 * 1024
        private const val MAX_JSON_BYTES = 64L * 1024 * 1024
        private const val MIN_FREE_BYTES = 32L * 1024 * 1024
        private const val BUF = 8 * 1024
        private const val PROGRESS_MIN_MS = 100L
        const val TMP_DIR = ".restore_tmp"
        private const val MANIFEST = "manifest.json"
        private const val PREFS = "prefs.json"
        private const val SAMPLES_ARCHIVE = "library/samples/user"
        private const val SAMPLES_REAL = "library/user"

        private val SINGLE_FILES = listOf(
            "setlist.json", "master_stage.json", "metronome.json", "pedal_config.json",
            "rhythm.json", "midi_map.json", "globals.json",
        )
        /** Archive dir == `files/` dir. */
        private val DIRS = listOf("presets/user", "scenes", "setups", "pads", "scales")

        private val PREF_KEYS = setOf(
            "voices", "quality", "mpe", "sstOn", "sstMs", "lang", "transpose", "firstNote", "nightKeys", "slideBend",
            "liveMode", "liveSets", "liveCategory", "tuningRoot", "phraseQuant", "phraseQStrength", "midiOutName", "midiOutCh",
            "blePeripheral", "mutateScope", "memoFontSp", "memoMono", "tunerA4", "tunerMute", "multiZoneDensity", "favorites",
            "powerScreenOffMin", "powerStandbyMin", "powerDeepSleepMin", "powerShutdownMin", "powerShutdownBattOnly",
        )
        private val PREF_PREFIXES = listOf("liveFader", "footswitch", "extPort")

        private fun prefAllowed(k: String) = k in PREF_KEYS || PREF_PREFIXES.any { k.startsWith(it) }

        private val lock = Any()
        private var restoring = false
        private var backups = 0

        private fun hex(b: ByteArray): String {
            val c = "0123456789abcdef"
            val sb = StringBuilder(b.size * 2)
            for (x in b) { sb.append(c[(x.toInt() shr 4) and 15]); sb.append(c[x.toInt() and 15]) }
            return sb.toString()
        }

        /**
         * Call once at startup, before anything reads `files/` (also done at the start of every restore): a restore
         * that was killed in the middle of its directory swap is rolled back, then the leftover `.restore_tmp` is removed.
         */
        fun recoverInterrupted(filesDir: File) {
            val tmp = File(filesDir, TMP_DIR)
            if (!tmp.exists()) return
            try {
                val journal = File(tmp, "journal.json")
                if (journal.isFile && !File(tmp, "COMMITTED").exists()) {
                    val ops = JSONArray(journal.readText())
                    val old = File(tmp, "old")
                    for (i in ops.length() - 1 downTo 0) {
                        val o = ops.getJSONObject(i)
                        val rel = o.getString("rel")
                        val dest = File(filesDir, rel)
                        val saved = File(old, rel)
                        if (saved.exists()) {
                            dest.deleteRecursively()
                            dest.parentFile?.mkdirs()
                            saved.renameTo(dest)
                        } else if (!o.optBoolean("hadOld")) {
                            dest.deleteRecursively()
                        }
                    }
                }
            } catch (_: Exception) {
                return                       // unreadable journal: keep everything for manual inspection
            }
            tmp.deleteRecursively()
        }
    }

    private class Item(val path: String, val file: File?, val bytes: ByteArray?, val sample: Boolean) {
        val size: Long get() = file?.length() ?: bytes!!.size.toLong()
        fun open(): InputStream = if (file != null) FileInputStream(file) else bytes!!.inputStream()
    }

    private val root: File get() = a.filesDir
    private val sampleRoot: File get() = File(a.library.libraryRoot, "user")

    // ====================================================================================== estimate

    /**
     * Payload of a backup: `(bytes, fileCount)`. Walks the folders — blocking, call off the UI thread
     * (or use [estimateSizesAsync]).
     */
    fun estimateSizes(includeSamples: Boolean): Pair<Long, Int> {
        val items = collect(includeSamples, withPrefs = false)
        var n = 0L
        for (it in items) n += it.size
        return n to items.size + 1                                    // + prefs.json
    }

    fun estimateSizesAsync(includeSamples: Boolean, onDone: (Pair<Long, Int>) -> Unit) {
        a.loader.execute {
            val r = try { estimateSizes(includeSamples) } catch (_: Exception) { 0L to 0 }
            a.ui.post { onDone(r) }
        }
    }

    // ====================================================================================== backup

    /**
     * Stream a backup ZIP into [outStream] (not closed here; it is flushed). 8 KB buffer, one pass: every file's SHA-256
     * is computed from the bytes actually written, and `manifest.json` is the last entry.
     * [onProgress] gets (0..1, current path) at most 10 times a second from the calling thread.
     * @throws IOException on I/O failure or when a restore is running, [CancellationException] when [isCancelled] turns true.
     */
    @Throws(IOException::class, CancellationException::class)
    fun createBackup(
        outStream: OutputStream,
        includeSamples: Boolean,
        onProgress: (Float, String) -> Unit,
        isCancelled: () -> Boolean = { false },
    ): BackupSummary {
        checkWorker()
        synchronized(lock) {
            if (restoring) throw IOException("a restore is running")
            backups++
        }
        try {
            val items = collect(includeSamples, withPrefs = true)
            val total = items.sumOf { it.size }.coerceAtLeast(1L)
            val counting = CountingOut(outStream)
            val zip = ZipOutputStream(counting)
            val files = JSONArray()
            val buf = ByteArray(BUF)
            var done = 0L
            var last = 0L
            var presets = 0
            var scenes = 0
            var bytes = 0L
            for (it in items) {
                if (isCancelled()) throw CancellationException("cancelled")
                val ze = ZipEntry(it.path)
                ze.time = it.file?.lastModified() ?: System.currentTimeMillis()
                zip.setLevel(if (it.sample) 1 else Deflater.DEFAULT_COMPRESSION)
                zip.putNextEntry(ze)
                val md = MessageDigest.getInstance("SHA-256")
                var size = 0L
                it.open().use { input ->
                    while (true) {
                        val n = input.read(buf)
                        if (n < 0) break
                        zip.write(buf, 0, n)
                        md.update(buf, 0, n)
                        size += n
                        done += n
                        val now = SystemClock.elapsedRealtime()
                        if (now - last >= PROGRESS_MIN_MS) {
                            last = now
                            if (isCancelled()) throw CancellationException("cancelled")
                            onProgress((done.toFloat() / total * 0.98f).coerceIn(0f, 0.98f), it.path)
                        }
                    }
                }
                zip.closeEntry()
                files.put(JSONObject().put("path", it.path).put("size", size).put("sha256", hex(md.digest())))
                bytes += size
                if (it.path.startsWith("presets/user/") && it.path.endsWith(".json")) presets++
                if (isScene(it.path)) scenes++
            }
            val created = System.currentTimeMillis()
            val manifest = JSONObject()
                .put("format", FORMAT)
                .put("app", "HSynth")
                .put("appVersion", appVersion())
                .put("created", created)
                .put("createdIso", isoUtc(created))
                .put("includesSamples", includeSamples)
                .put("fileCount", files.length())
                .put("counts", JSONObject().put("presets", presets).put("scenes", scenes))
                .put("files", files)
            val mb = manifest.toString(1).toByteArray(Charsets.UTF_8)
            zip.setLevel(Deflater.DEFAULT_COMPRESSION)
            zip.putNextEntry(ZipEntry(MANIFEST).apply { time = created })
            zip.write(mb)
            zip.closeEntry()
            zip.finish()
            zip.close()                                              // CountingOut.close() only flushes
            onProgress(1f, MANIFEST)
            return BackupSummary(files.length(), bytes, counting.count, presets, scenes, includeSamples, created)
        } finally {
            synchronized(lock) { backups-- }
        }
    }

    private fun collect(includeSamples: Boolean, withPrefs: Boolean): List<Item> {
        val out = ArrayList<Item>()
        for (n in SINGLE_FILES) File(root, n).takeIf { it.isFile }?.let { out += Item(n, it, null, false) }
        for (d in DIRS) walk(File(root, d), d, out, false, 0)
        if (includeSamples) walk(sampleRoot, SAMPLES_ARCHIVE, out, true, 0)
        if (withPrefs) out += Item(PREFS, null, prefsJson().toByteArray(Charsets.UTF_8), false)
        return out
    }

    private fun walk(dir: File, arc: String, out: MutableList<Item>, sample: Boolean, depth: Int) {
        if (depth > 16) return
        val list = dir.listFiles() ?: return
        list.sortBy { it.name }
        for (f in list) {
            val name = f.name
            if (name.startsWith(".") || name.endsWith(".tmp") || name.endsWith(".bad")) continue
            try { if (Files.isSymbolicLink(f.toPath())) continue } catch (_: Exception) { continue }
            if (f.isDirectory) walk(f, "$arc/$name", out, sample, depth + 1)
            else if (f.isFile) out += Item("$arc/$name", f, null, sample)
        }
    }

    private fun isScene(path: String) = path.startsWith("scenes/") && path.endsWith(".json") && !path.endsWith(".names.json")

    private fun appVersion(): String = try {
        val pi = a.packageManager.getPackageInfo(a.packageName, 0)
        "${pi.versionName ?: ""} (${pi.longVersionCode})"
    } catch (_: Exception) { "" }

    private fun isoUtc(ms: Long): String =
        SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss'Z'", Locale.US).apply { timeZone = TimeZone.getTimeZone("UTC") }.format(Date(ms))

    /** `{"v":1,"prefs":{key:{"t":"i|l|f|b|s|set","v":...}}}` — only whitelisted keys. */
    private fun prefsJson(): String {
        val p = JSONObject()
        for ((k, v) in a.prefs.all) {
            if (!prefAllowed(k) || v == null) continue
            val e = JSONObject()
            when (v) {
                is Boolean -> e.put("t", "b").put("v", v)
                is Int -> e.put("t", "i").put("v", v)
                is Long -> e.put("t", "l").put("v", v)
                is Float -> e.put("t", "f").put("v", v.toDouble())
                is String -> e.put("t", "s").put("v", v)
                is Set<*> -> e.put("t", "set").put("v", JSONArray().also { arr -> v.forEach { s -> if (s is String) arr.put(s) } })
                else -> continue
            }
            p.put(k, e)
        }
        return JSONObject().put("v", 1).put("prefs", p).toString()
    }

    private class CountingOut(private val o: OutputStream) : OutputStream() {
        var count = 0L
            private set
        override fun write(b: Int) { o.write(b); count++ }
        override fun write(b: ByteArray, off: Int, len: Int) { o.write(b, off, len); count += len }
        override fun flush() = o.flush()
        override fun close() = o.flush()                   // the caller owns the underlying stream
    }

    // ====================================================================================== restore

    private class Fail(msg: String) : Exception(msg)

    private class Staged(val size: Long, val sha: String, val time: Long)

    private class Op(val rel: String, val hadOld: Boolean, val place: File?)

    /**
     * Restore [inStream] (not closed here). Everything is extracted to `files/.restore_tmp/stage/`, verified
     * (zip-slip, caps, manifest, SHA-256, every JSON parses) and only then swapped into `files/`; a journal makes the
     * swap roll back on any error or process death ([recoverInterrupted]), so the existing data stays 100 % untouched on failure.
     *
     * [merge] = false: replace — the user-content folders in the backup scope (`presets/user`, `scenes`, `setups`, `pads`,
     * `scales`, and the user samples when the backup contains them) become exactly the backup's; single files / prefs are
     * replaced when the backup has them. [merge] = true: file by file, the backup wins only when its entry is newer than
     * the existing file (ties keep the existing one); prefs only fill keys that are not set yet.
     * [compressedSize] (the zip's length, when known) makes the extraction progress real; otherwise it reports -1 (indeterminate).
     * The caller reloads the app state afterwards (library refresh, native learn / settings apply, `a.recreate()`).
     */
    fun restoreBackup(
        inStream: InputStream,
        merge: Boolean,
        onProgress: (Float, String) -> Unit,
        compressedSize: Long = -1L,
        isCancelled: () -> Boolean = { false },
    ): RestoreResult {
        checkWorker()
        synchronized(lock) {
            if (restoring || backups > 0) return RestoreResult(false, "a backup / restore is already running")
            restoring = true
        }
        val filesDir = try { root.canonicalFile } catch (e: IOException) { synchronized(lock) { restoring = false }; return RestoreResult(false, e.message) }
        val tmp = File(filesDir, TMP_DIR)
        var ops: List<Op> = emptyList()
        var prefsSnapshot: Map<String, Any?>? = null
        try {
            recoverInterrupted(filesDir)
            val stage = File(tmp, "stage").apply { mkdirs() }
            val stageCanon = stage.canonicalFile
            val old = File(tmp, "old").apply { mkdirs() }

            // ---------------------------------------------------------------- 1) extract + hash
            val staged = LinkedHashMap<String, Staged>()
            extract(inStream, stageCanon, staged, filesDir, compressedSize, onProgress, isCancelled)

            // ---------------------------------------------------------------- 2) verify manifest + hashes
            onProgress(0.82f, "verify")
            val mf = staged[MANIFEST] ?: throw Fail("manifest.json is missing — not an HSynth backup")
            if (mf.size > MAX_JSON_BYTES) throw Fail("manifest.json is too large")
            val manifest = try { JSONObject(File(stageCanon, MANIFEST).readText(Charsets.UTF_8)) } catch (e: Exception) { throw Fail("manifest.json is invalid") }
            if (manifest.optInt("format", 0) !in 1..FORMAT) throw Fail("unsupported backup format ${manifest.opt("format")}")
            val listed = manifest.optJSONArray("files") ?: throw Fail("manifest.json has no file list")
            val expected = HashMap<String, JSONObject>()
            for (i in 0 until listed.length()) {
                val e = listed.optJSONObject(i) ?: throw Fail("manifest.json: bad file entry")
                val p = e.optString("path")
                validateName(p)
                if (!restorable(p)) continue                         // written by a newer app: ignored, never extracted
                if (expected.put(p, e) != null) throw Fail("manifest.json lists $p twice")
            }
            for ((p, e) in expected) {
                val s = staged[p] ?: throw Fail("missing in archive: $p")
                if (s.size != e.optLong("size", -1L)) throw Fail("size mismatch: $p")
                if (!s.sha.equals(e.optString("sha256"), ignoreCase = true)) throw Fail("checksum mismatch: $p")
            }
            for (p in staged.keys) if (p != MANIFEST && p !in expected) throw Fail("not listed in manifest: $p")
            val includesSamples = manifest.optBoolean("includesSamples", false)

            // ---------------------------------------------------------------- 3) every JSON must parse
            onProgress(0.86f, "validate")
            for ((p, _) in staged) {
                if (p == MANIFEST || !p.endsWith(".json")) continue
                val f = File(stageCanon, p)
                if (f.length() > MAX_JSON_BYTES) throw Fail("JSON too large: $p")
                val v = try { JSONTokener(f.readText(Charsets.UTF_8)).nextValue() } catch (e: Exception) { throw Fail("invalid JSON: $p") }
                if (v !is JSONObject && v !is JSONArray) throw Fail("invalid JSON: $p")
            }
            val newPrefs = if (staged.containsKey(PREFS)) parsePrefs(File(stageCanon, PREFS)) else emptyList()

            // ---------------------------------------------------------------- 4) plan the swap
            onProgress(0.9f, "swap")
            val plan = ArrayList<Op>()
            var placed = 0
            var placedBytes = 0L
            var skipped = 0
            if (!merge) {
                val dirs = ArrayList<Pair<String, String>>()               // (archive dir, real dir)
                for (d in DIRS) dirs += d to d
                if (includesSamples) dirs += SAMPLES_ARCHIVE to SAMPLES_REAL
                for ((arc, real) in dirs) {
                    val src = File(stageCanon, arc)
                    val dest = File(filesDir, real)
                    if (!src.exists() && !dest.exists()) continue
                    plan += Op(real, dest.exists(), if (src.exists()) src else null)
                }
                for (n in SINGLE_FILES) {
                    val src = File(stageCanon, n)
                    if (!src.isFile) continue
                    plan += Op(n, File(filesDir, n).exists(), src)
                }
                for ((p, s) in staged) if (p != MANIFEST && p != PREFS) { placed++; placedBytes += s.size }
            } else {
                for ((p, s) in staged) {
                    if (p == MANIFEST || p == PREFS) continue
                    val real = realPath(p)
                    val dest = File(filesDir, real)
                    if (dest.exists() && dest.lastModified() >= s.time) { skipped++; continue }
                    plan += Op(real, dest.exists(), File(stageCanon, p))
                    placed++; placedBytes += s.size
                }
            }
            val presets = staged.keys.count { it.startsWith("presets/user/") && it.endsWith(".json") }
            val scenes = staged.keys.count { isScene(it) }

            // journal first, then swap
            writeAtomic(File(tmp, "journal.json"), JSONArray().also { arr ->
                for (o in plan) arr.put(JSONObject().put("rel", o.rel).put("hadOld", o.hadOld))
            }.toString())
            if (isCancelled()) throw CancellationException("cancelled")

            ops = plan
            for (o in plan) {
                val dest = File(filesDir, o.rel)
                if (o.hadOld) {
                    val saved = File(old, o.rel)
                    saved.parentFile?.mkdirs()
                    if (!dest.renameTo(saved)) throw Fail("cannot move aside ${o.rel}")
                }
                if (o.place != null) {
                    dest.parentFile?.mkdirs()
                    if (!o.place.renameTo(dest)) throw Fail("cannot place ${o.rel}")
                }
            }

            // ---------------------------------------------------------------- 5) prefs, then commit
            var prefsApplied = 0
            if (newPrefs.isNotEmpty()) {
                val cur = a.prefs.all
                val ed = a.prefs.edit()
                val snap = HashMap<String, Any?>()
                for (p in newPrefs) {
                    if (merge && cur.containsKey(p.key)) continue
                    snap[p.key] = cur[p.key]
                    when (val v = p.value) {
                        is Boolean -> ed.putBoolean(p.key, v)
                        is Int -> ed.putInt(p.key, v)
                        is Long -> ed.putLong(p.key, v)
                        is Float -> ed.putFloat(p.key, v)
                        is String -> ed.putString(p.key, v)
                        is Set<*> -> ed.putStringSet(p.key, v.filterIsInstance<String>().toSet())
                    }
                    prefsApplied++
                }
                prefsSnapshot = snap
                if (!ed.commit()) throw Fail("cannot write settings")
            }
            if (!writeMarker(File(tmp, "COMMITTED"))) throw Fail("cannot commit")
            tmp.deleteRecursively()
            onProgress(1f, "done")
            return RestoreResult(true, null, placed, placedBytes, presets, scenes, skipped, merge, prefsApplied, manifest.optLong("created", 0L))
        } catch (e: CancellationException) {
            rollback(filesDir, tmp, ops, prefsSnapshot)
            return RestoreResult(false, "cancelled")
        } catch (e: Fail) {
            rollback(filesDir, tmp, ops, prefsSnapshot)
            return RestoreResult(false, e.message)
        } catch (e: Throwable) {
            rollback(filesDir, tmp, ops, prefsSnapshot)
            return RestoreResult(false, e.message ?: e.javaClass.simpleName)
        } finally {
            synchronized(lock) { restoring = false }
        }
    }

    /** Undo every op (reverse order), put the old prefs values back, delete the temp folder. Best effort, never throws. */
    private fun rollback(filesDir: File, tmp: File, ops: List<Op>, prefs: Map<String, Any?>?) {
        try {
            if (prefs != null) {
                val ed = a.prefs.edit()
                for ((k, v) in prefs) when (v) {
                    null -> ed.remove(k)
                    is Boolean -> ed.putBoolean(k, v)
                    is Int -> ed.putInt(k, v)
                    is Long -> ed.putLong(k, v)
                    is Float -> ed.putFloat(k, v)
                    is String -> ed.putString(k, v)
                    is Set<*> -> ed.putStringSet(k, v.filterIsInstance<String>().toSet())
                }
                ed.commit()
            }
            val old = File(tmp, "old")
            for (i in ops.indices.reversed()) {
                val o = ops[i]
                val dest = File(filesDir, o.rel)
                val saved = File(old, o.rel)
                if (saved.exists()) {
                    dest.deleteRecursively()
                    dest.parentFile?.mkdirs()
                    saved.renameTo(dest)
                } else if (!o.hadOld) {
                    dest.deleteRecursively()                   // did not exist before: whatever is there is ours
                }
            }
            tmp.deleteRecursively()
        } catch (_: Throwable) {
            // leave .restore_tmp + journal in place: recoverInterrupted() retries at the next start
        }
    }

    private fun extract(
        inStream: InputStream, stage: File, staged: MutableMap<String, Staged>, filesDir: File,
        compressedSize: Long, onProgress: (Float, String) -> Unit, isCancelled: () -> Boolean,
    ) {
        val counting = CountingIn(BufferedInputStream(inStream, 64 * 1024))
        val zin = ZipInputStream(counting)
        val seen = HashSet<String>()
        val prefix = stage.path + File.separator
        val buf = ByteArray(BUF)
        var entries = 0
        var total = 0L
        var lastReport = 0L
        var lastSpace = 0L
        try {
            while (true) {
                if (isCancelled()) throw CancellationException("cancelled")
                val ze = zin.nextEntry ?: break
                if (++entries > MAX_ENTRIES) throw Fail("too many entries (> $MAX_ENTRIES)")
                val name = ze.name
                validateName(name)
                if (ze.isDirectory) continue
                if (!seen.add(name)) throw Fail("duplicate entry: $name")
                val keep = name == MANIFEST || restorable(name)
                var dest: File? = null
                var fos: FileOutputStream? = null
                if (keep) {
                    val d = File(stage, name).canonicalFile
                    if (!d.path.startsWith(prefix)) throw Fail("illegal path: $name")        // zip-slip, 2nd line of defence
                    d.parentFile!!.mkdirs()
                    dest = d
                    fos = FileOutputStream(d)
                }
                val md = MessageDigest.getInstance("SHA-256")
                var size = 0L
                try {
                    while (true) {
                        val n = zin.read(buf)
                        if (n < 0) break
                        total += n
                        if (total > MAX_TOTAL_BYTES) throw Fail("backup is larger than 12 GB")
                        if (keep) {
                            fos!!.write(buf, 0, n)
                            md.update(buf, 0, n)
                            size += n
                        }
                        val now = SystemClock.elapsedRealtime()
                        if (now - lastSpace >= 1000L) {
                            lastSpace = now
                            if (filesDir.usableSpace < MIN_FREE_BYTES) throw Fail("not enough storage")
                            if (isCancelled()) throw CancellationException("cancelled")
                        }
                        if (now - lastReport >= PROGRESS_MIN_MS) {
                            lastReport = now
                            val p = if (compressedSize > 0) (counting.count.toFloat() / compressedSize * 0.8f).coerceIn(0f, 0.8f) else -1f
                            onProgress(p, name)
                        }
                    }
                } finally {
                    fos?.close()
                }
                if (keep) {
                    val t = ze.time
                    if (t > 0) dest!!.setLastModified(t)
                    staged[name] = Staged(size, hex(md.digest()), if (t > 0) t else 0L)
                }
            }
        } finally {
            try { zin.close() } catch (_: Exception) {}                // CountingIn.close() does not close the caller's stream
        }
    }

    private class CountingIn(private val i: InputStream) : InputStream() {
        var count = 0L
            private set
        override fun read(): Int { val r = i.read(); if (r >= 0) count++; return r }
        override fun read(b: ByteArray, off: Int, len: Int): Int { val r = i.read(b, off, len); if (r > 0) count += r; return r }
        override fun available(): Int = i.available()
        override fun close() { /* caller owns the stream */ }
    }

    /** Reject absolute paths, drive letters, backslashes, NUL, empty / `.` / `..` components, over-long names. */
    private fun validateName(n: String) {
        if (n.isEmpty() || n.length > 512) throw Fail("illegal entry name")
        if (n.startsWith("/") || n.contains('\\') || n.contains('\u0000') || n.contains(':')) throw Fail("illegal entry name: $n")
        val parts = n.removeSuffix("/").split('/')
        for (p in parts) if (p.isEmpty() || p == "." || p == "..") throw Fail("illegal entry name: $n")
    }

    /** Entries this version knows how to restore (everything else is skipped without being extracted). */
    private fun restorable(p: String): Boolean {
        if (p == PREFS || p in SINGLE_FILES) return true
        if (p.startsWith("$SAMPLES_ARCHIVE/")) return true
        for (d in DIRS) if (p.startsWith("$d/")) return true
        return false
    }

    private fun realPath(archive: String): String =
        if (archive.startsWith("$SAMPLES_ARCHIVE/")) SAMPLES_REAL + archive.substring(SAMPLES_ARCHIVE.length) else archive

    private class Pref(val key: String, val value: Any)

    private fun parsePrefs(f: File): List<Pref> {
        val o = try { JSONObject(f.readText(Charsets.UTF_8)).getJSONObject("prefs") } catch (e: Exception) { throw Fail("prefs.json is invalid") }
        val out = ArrayList<Pref>()
        val it = o.keys()
        while (it.hasNext()) {
            val k = it.next()
            if (!prefAllowed(k)) continue
            val e = o.optJSONObject(k) ?: throw Fail("prefs.json: bad entry '$k'")
            val v: Any = try {
                when (e.getString("t")) {
                    "b" -> e.getBoolean("v")
                    "i" -> e.getInt("v")
                    "l" -> e.getLong("v")
                    "f" -> e.getDouble("v").toFloat()
                    "s" -> e.getString("v")
                    "set" -> { val arr = e.getJSONArray("v"); HashSet<String>().also { s -> for (i in 0 until arr.length()) s.add(arr.getString(i)) } }
                    else -> throw Fail("prefs.json: bad type for '$k'")
                }
            } catch (x: Fail) { throw x } catch (x: Exception) { throw Fail("prefs.json: bad value for '$k'") }
            out += Pref(k, v)
        }
        return out
    }

    private fun writeAtomic(f: File, text: String) {
        val t = File(f.parentFile, f.name + ".tmp")
        FileOutputStream(t).use { it.write(text.toByteArray(Charsets.UTF_8)); it.fd.sync() }
        if (!t.renameTo(f)) { f.delete(); if (!t.renameTo(f)) throw IOException("cannot write ${f.name}") }
    }

    private fun writeMarker(f: File): Boolean = try { FileOutputStream(f).use { it.fd.sync() }; true } catch (_: Exception) { false }

    private fun checkWorker() {
        if (Looper.myLooper() == Looper.getMainLooper()) throw IllegalStateException("BackupManager must run off the UI thread")
    }
}
