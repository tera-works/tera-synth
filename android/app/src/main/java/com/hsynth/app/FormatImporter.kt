package com.hsynth.app

import android.app.AlertDialog
import android.content.Context
import android.content.Intent
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.drawable.ColorDrawable
import android.media.AudioFormat
import android.media.MediaCodec
import android.media.MediaDataSource
import android.media.MediaExtractor
import android.media.MediaFormat
import android.net.Uri
import android.os.SystemClock
import android.provider.OpenableColumns
import android.text.Editable
import android.text.InputType
import android.text.TextPaint
import android.text.TextUtils
import android.text.TextWatcher
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.widget.AbsListView
import android.widget.AdapterView
import android.widget.BaseAdapter
import android.widget.EditText
import android.widget.GridView
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.TextView
import android.widget.Toast
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.nio.ByteOrder
import java.util.Locale
import java.util.zip.ZipInputStream

/*
 * Phase 6 — format importers (SF2 / SF3, Decent Sampler, DX7 SysEx, wavetables).
 *
 * This file has NO compile dependency on new NativeEngine methods: every engine call goes through [FormatBridge],
 * which MainActivity / NativeEngine implement in the integration wave. Until then [NoFormatBridge] answers every
 * call with a JSON error, so the UI can be instantiated and tested on its own.
 *
 * Contents: [FormatBridge] · [NoFormatBridge] · [OggVorbisHelper] · [DsLibraryExtractor] · [FormatKind] ·
 * [FormatFiles] (shared helpers) · [FormatImporter] (picker glue) · [SoundFontPickerSheet] · [Dx7BankSheet].
 */

// ====================================================================================================== bridge

/**
 * Engine side of the importers. Every method may block (file I/O, decoding): call from `a.loader`, never from the UI
 * thread. All results are JSON strings so the JNI layer stays trivial.
 */
interface FormatBridge {
    /**
     * `{"ok":true,"presets":[{"bank":0,"program":0,"isDrum":false,"name":"Acoustic Grand","index":0}, ...]}` of an
     * `.sf2` / `.sf3` / `.dls` / `.gig` bank (a bare JSON array is accepted too).
     */
    fun listSoundBankPresets(path: String): String

    /** `{"ok":true,"warnings":[]}` or `{"ok":false,"error":"..."}`. [isDrum] selects the percussion bank of the file. */
    fun loadSoundBankPreset(layer: Int, path: String, bank: Int, program: Int, isDrum: Boolean): String

    /** Load a `.dspreset` (its samples sit next to it) into [layer]. Same result shape as [loadSf2Preset]. */
    fun loadDecentSampler(layer: Int, dspresetPath: String): String

    /** `{"ok":true,"voices":[{"index":0,"name":"E.PIANO 1","algo":5}, ...]}` */
    fun inspectDx7Sysex(path: String): String

    /** Loads voice [index] into the CURRENT EDIT PART (audition). `{"ok":true,"name":"..."}` */
    fun loadDx7Voice(path: String, index: Int): String

    /** Saves every voice as a flat user preset `<prefix>_NN_<name>.json` in [outDir]. `{"ok":true,"count":32,"names":[...]}` */
    fun importDx7Bank(path: String, outDir: String, prefix: String): String

    /** Load a `.wt` / `.vitaltable` / `.wav` wavetable into [layer] as its "user" table. */
    fun loadAnyWavetable(layer: Int, path: String): String

    /** Fill [outSamples] with one cycle of [layer]'s table at [pos] (0..1). @return false when there is no table. */
    fun wavetablePreview(layer: Int, pos: Float, outSamples: FloatArray): Boolean
}

/** Placeholder until the engine is wired: every call fails with a readable message (never throws). */
object NoFormatBridge : FormatBridge {
    private const val NA = """{"ok":false,"error":"format engine not available"}"""
    override fun listSoundBankPresets(path: String) = "[]"
    override fun loadSoundBankPreset(layer: Int, path: String, bank: Int, program: Int, isDrum: Boolean) = NA
    override fun loadDecentSampler(layer: Int, dspresetPath: String) = NA
    override fun inspectDx7Sysex(path: String) = NA
    override fun loadDx7Voice(path: String, index: Int) = NA
    override fun importDx7Bank(path: String, outDir: String, prefix: String) = NA
    override fun loadAnyWavetable(layer: Int, path: String) = NA
    override fun wavetablePreview(layer: Int, pos: Float, outSamples: FloatArray) = false
}

/** [FormatBridge] over the Phase 6 JNI calls. Never throws: native failures become `{"ok":false,"error":…}`. */
class NativeFormatBridge(private val eng: NativeEngine) : FormatBridge {
    private fun err(t: Throwable) = JSONObject().put("ok", false).put("error", t.message ?: t.javaClass.simpleName).toString()

    override fun listSoundBankPresets(path: String) = try { eng.nativeListSoundBankPresets(path) } catch (t: Throwable) { err(t) }
    override fun loadSoundBankPreset(layer: Int, path: String, bank: Int, program: Int, isDrum: Boolean) =
        try { eng.nativeLoadSoundBankPreset(layer, path, bank, program, isDrum) } catch (t: Throwable) { err(t) }
    override fun loadDecentSampler(layer: Int, dspresetPath: String) =
        try { eng.nativeLoadDecentSampler(layer, dspresetPath) } catch (t: Throwable) { err(t) }
    override fun inspectDx7Sysex(path: String) = try { eng.nativeInspectDx7Sysex(path) } catch (t: Throwable) { err(t) }
    override fun loadDx7Voice(path: String, index: Int) = try { eng.nativeLoadDx7Voice(path, index) } catch (t: Throwable) { err(t) }
    override fun importDx7Bank(path: String, outDir: String, prefix: String) =
        try { eng.nativeImportDx7Bank(path, outDir, prefix) } catch (t: Throwable) { err(t) }
    override fun loadAnyWavetable(layer: Int, path: String) = try { eng.nativeLoadAnyWavetable(layer, path) } catch (t: Throwable) { err(t) }
    override fun wavetablePreview(layer: Int, pos: Float, outSamples: FloatArray) =
        try { eng.nativeWavetablePreview(layer, pos, outSamples) } catch (_: Throwable) { false }
}

// ====================================================================================================== Ogg Vorbis

/**
 * Decodes Ogg Vorbis (SF3 sample chunks, Decent Sampler `.ogg` samples) with the platform codec
 * (`MediaExtractor` + `MediaCodec`, no extra native library). Blocking: call from a background thread.
 * Thread-safe (no shared state); every call creates and releases its own extractor / codec.
 */
object OggVorbisHelper {
    /** Interleaved float32 PCM in -1..1; [channels] is 1 or 2 (more channels are cut down to the first two). */
    class Decoded(val samples: FloatArray, val sampleRate: Int, val channels: Int)

    private const val DEQUEUE_US = 10_000L
    private const val DEADLINE_MS = 120_000L

    /** @return (interleaved samples, sampleRate), or null when the data cannot be decoded. */
    fun decode(ogg: ByteArray): Pair<FloatArray, Int>? = decodeEx(ogg)?.let { it.samples to it.sampleRate }

    fun decodeEx(ogg: ByteArray): Decoded? = decodeSource { it.setDataSource(BytesSource(ogg)) }

    fun decodeFile(file: File): Decoded? = decodeSource { it.setDataSource(file.absolutePath) }

    /** Raw bytes served to [MediaExtractor] without a temp file. */
    private class BytesSource(private val b: ByteArray) : MediaDataSource() {
        override fun readAt(position: Long, buffer: ByteArray, offset: Int, size: Int): Int {
            if (position < 0 || position >= b.size) return -1
            val n = minOf(size, b.size - position.toInt())
            System.arraycopy(b, position.toInt(), buffer, offset, n)
            return n
        }

        override fun getSize(): Long = b.size.toLong()
        override fun close() {}
    }

    private class Acc(cap: Int) {
        var buf = FloatArray(cap.coerceAtLeast(4096))
        var n = 0
        fun ensure(extra: Int) {
            if (n + extra > buf.size) buf = buf.copyOf(maxOf(buf.size * 2, n + extra))
        }
    }

    private fun decodeSource(setSource: (MediaExtractor) -> Unit): Decoded? {
        val ex = MediaExtractor()
        var codec: MediaCodec? = null
        try {
            setSource(ex)
            var track = -1
            var fmt: MediaFormat? = null
            for (i in 0 until ex.trackCount) {
                val f = ex.getTrackFormat(i)
                if (f.getString(MediaFormat.KEY_MIME)?.startsWith("audio/") == true) { track = i; fmt = f; break }
            }
            if (track < 0 || fmt == null) return null
            ex.selectTrack(track)
            val mime = fmt.getString(MediaFormat.KEY_MIME) ?: return null

            // ask for float output; decoders that cannot do it fall back to 16-bit (the real format is read below)
            var dec = MediaCodec.createDecoderByType(mime)
            codec = dec
            try {
                fmt.setInteger(MediaFormat.KEY_PCM_ENCODING, AudioFormat.ENCODING_PCM_FLOAT)
                dec.configure(fmt, null, null, 0)
            } catch (_: Exception) {
                dec.release()
                dec = MediaCodec.createDecoderByType(mime)
                codec = dec
                fmt.setInteger(MediaFormat.KEY_PCM_ENCODING, AudioFormat.ENCODING_PCM_16BIT)
                dec.configure(fmt, null, null, 0)
            }
            dec.start()

            var channels = if (fmt.containsKey(MediaFormat.KEY_CHANNEL_COUNT)) fmt.getInteger(MediaFormat.KEY_CHANNEL_COUNT) else 2
            var rate = if (fmt.containsKey(MediaFormat.KEY_SAMPLE_RATE)) fmt.getInteger(MediaFormat.KEY_SAMPLE_RATE) else 44100
            var enc = AudioFormat.ENCODING_PCM_16BIT
            val durUs = if (fmt.containsKey(MediaFormat.KEY_DURATION)) fmt.getLong(MediaFormat.KEY_DURATION) else 0L
            val acc = Acc(if (durUs > 0) (durUs * rate / 1_000_000L * channels).coerceAtMost(64L * 1024 * 1024).toInt() else 1 shl 16)

            val info = MediaCodec.BufferInfo()
            var inDone = false
            var outDone = false
            val deadline = SystemClock.elapsedRealtime() + DEADLINE_MS
            while (!outDone) {
                if (SystemClock.elapsedRealtime() > deadline) return null
                if (!inDone) {
                    val ii = dec.dequeueInputBuffer(DEQUEUE_US)
                    if (ii >= 0) {
                        val ib = dec.getInputBuffer(ii)!!
                        val sz = ex.readSampleData(ib, 0)
                        if (sz < 0) {
                            dec.queueInputBuffer(ii, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                            inDone = true
                        } else {
                            dec.queueInputBuffer(ii, 0, sz, ex.sampleTime, 0)
                            ex.advance()
                        }
                    }
                }
                val oi = dec.dequeueOutputBuffer(info, DEQUEUE_US)
                when {
                    oi >= 0 -> {
                        if (info.size > 0) {
                            val ob = dec.getOutputBuffer(oi)!!
                            ob.position(info.offset)
                            ob.limit(info.offset + info.size)
                            ob.order(ByteOrder.nativeOrder())
                            if (enc == AudioFormat.ENCODING_PCM_FLOAT) {
                                val fb = ob.asFloatBuffer()
                                val cnt = fb.remaining()
                                acc.ensure(cnt)
                                fb.get(acc.buf, acc.n, cnt)
                                acc.n += cnt
                            } else {
                                val sb = ob.asShortBuffer()
                                val cnt = sb.remaining()
                                acc.ensure(cnt)
                                for (i in 0 until cnt) acc.buf[acc.n + i] = sb.get() / 32768f
                                acc.n += cnt
                            }
                        }
                        dec.releaseOutputBuffer(oi, false)
                        if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) outDone = true
                    }
                    oi == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                        val f = dec.outputFormat
                        if (f.containsKey(MediaFormat.KEY_CHANNEL_COUNT)) channels = f.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
                        if (f.containsKey(MediaFormat.KEY_SAMPLE_RATE)) rate = f.getInteger(MediaFormat.KEY_SAMPLE_RATE)
                        enc = if (f.containsKey(MediaFormat.KEY_PCM_ENCODING)) f.getInteger(MediaFormat.KEY_PCM_ENCODING) else AudioFormat.ENCODING_PCM_16BIT
                    }
                }
            }
            if (acc.n == 0 || channels < 1) return null
            if (channels <= 2) return Decoded(acc.buf.copyOf(acc.n), rate, channels)
            // > 2 channels: keep the first two
            val frames = acc.n / channels
            val st = FloatArray(frames * 2)
            for (i in 0 until frames) { st[2 * i] = acc.buf[i * channels]; st[2 * i + 1] = acc.buf[i * channels + 1] }
            return Decoded(st, rate, 2)
        } catch (_: Exception) {
            return null
        } finally {
            runCatching { codec?.stop() }
            runCatching { codec?.release() }
            runCatching { ex.release() }
        }
    }
}

// ====================================================================================================== file kinds / shared helpers

/** User library folders: `files/library/user/<dir>/`. SF2 = every sound bank (`sf2 sf3 dls gig`). `.wav` is treated as a wavetable by this importer. */
enum class FormatKind(val dir: String, val exts: Set<String>) {
    SF2("sf2", setOf("sf2", "sf3", "dls", "gig")),
    DS("ds", setOf("dspreset")),
    DX7("dx7", setOf("syx")),
    WT("wt", setOf("wt", "vitaltable", "wav"));

    companion object {
        fun of(fileName: String): FormatKind? {
            val ext = fileName.substringAfterLast('.', "").lowercase(Locale.ROOT)
            return values().firstOrNull { ext in it.exts }
        }
    }
}

/** Small shared helpers (names are all UI-thread safe unless noted). */
object FormatFiles {
    const val REQ_FORMAT_IMPORT = 170
    const val REQ_WT_IMPORT = 171

    private const val AUDITION_ID = 0x7E000010
    private const val AUDITION_KEY = 60

    fun userDir(ctx: Context, kind: FormatKind) = File(ctx.filesDir, "library/user/${kind.dir}")

    fun safeName(name: String): String = name.replace(Regex("[^A-Za-z0-9._ -]"), "_").trim().ifEmpty { "untitled" }

    fun displayName(ctx: Context, uri: Uri): String {
        runCatching {
            ctx.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
                if (c.moveToFirst()) c.getString(0)?.let { return it }
            }
        }
        return uri.lastPathSegment?.substringAfterLast('/') ?: "file"
    }

    fun uniqueFile(dir: File, name: String): File {
        var f = File(dir, name)
        if (!f.exists()) return f
        val base = name.substringBeforeLast('.')
        val ext = name.substringAfterLast('.', "")
        var i = 2
        while (f.exists()) {
            f = File(dir, if (ext.isEmpty() || base == name) "$base ($i)" else "$base ($i).$ext")
            i++
        }
        return f
    }

    /** Copy one picked file into `library/user/<kind>/` (blocking). Throws [IOException] on a wrong extension / read error. */
    fun importSingle(ctx: Context, uri: Uri, kind: FormatKind? = null): File {
        val name = safeName(displayName(ctx, uri))
        val k = kind ?: FormatKind.of(name) ?: throw IOException("unsupported file: $name")
        checkExt(name, k)
        val dest = uniqueFile(userDir(ctx, k).apply { mkdirs() }, name)
        try {
            ctx.contentResolver.openInputStream(uri)?.use { i -> dest.outputStream().use { i.copyTo(it) } }
                ?: throw IOException("cannot read $name")
        } catch (e: Exception) {
            dest.delete()
            throw if (e is IOException) e else IOException(e.message ?: "copy failed")
        }
        return dest
    }

    /** Same for a local [File] (already readable by the app). */
    fun importSingle(ctx: Context, file: File, kind: FormatKind? = null): File {
        val name = safeName(file.name)
        val k = kind ?: FormatKind.of(name) ?: throw IOException("unsupported file: $name")
        checkExt(name, k)
        val dest = uniqueFile(userDir(ctx, k).apply { mkdirs() }, name)
        try {
            file.inputStream().use { i -> dest.outputStream().use { i.copyTo(it) } }
        } catch (e: Exception) {
            dest.delete()
            throw if (e is IOException) e else IOException(e.message ?: "copy failed")
        }
        return dest
    }

    private fun checkExt(name: String, k: FormatKind) {
        val ext = name.substringAfterLast('.', "").lowercase(Locale.ROOT)
        if (ext !in k.exts) throw IOException("$name is not a .${k.exts.joinToString("/.")} file")
    }

    /** Runs a bridge call and always returns a JSON object (errors and non-JSON answers become `{"ok":false,...}`). */
    inline fun json(call: () -> String): JSONObject = try {
        val s = call().trim()
        if (s.startsWith("{")) JSONObject(s) else errorJson("unexpected answer: " + s.take(80))
    } catch (e: Exception) {
        errorJson(e.message ?: e.javaClass.simpleName)
    } catch (e: LinkageError) {   // JNI not wired yet
        errorJson(e.message ?: "native method missing")
    }

    fun errorJson(msg: String): JSONObject = JSONObject().put("ok", false).put("error", msg)

    /** Index of the option [name] of enum param `<l>.<param>`, or [fallback]. */
    fun optionIndex(l: Char, param: String, name: String, fallback: Int): Int {
        val m = Params.metaOf("$l.$param") ?: return fallback
        val i = m.options.indexOfFirst { it.equals(name, ignoreCase = true) }
        return if (i >= 0) i else fallback
    }

    /**
     * UI-thread follow-up of a layer load: switch the layer's engine [typeName] (`pcm` / `wt` / `fm`), log engine
     * warnings, tell every listener and (optionally) rebuild the EDIT page. @return true when [r] was a success.
     */
    fun finishLayerLoad(a: MainActivity, layer: Int, r: JSONObject, what: String, typeName: String?, rebuild: Boolean): Boolean {
        val ok = r.optBoolean("ok")
        val letter = "abc"[layer.coerceIn(0, 2)]
        if (ok) {
            if (typeName != null) {
                val ti = optionIndex(letter, "type", typeName, if (typeName == "pcm") 3 else if (typeName == "wt") 4 else -1)
                if (ti >= 0) Params.set("$letter.type", ti.toFloat())
            }
            a.log("layer ${letter.uppercaseChar()}: $what loaded")
        } else {
            a.log("load failed: ${r.optString("error", "unknown error")}", Ui.ERR)
        }
        val w = r.optJSONArray("warnings")
        for (i in 0 until (w?.length() ?: 0)) a.log("  ${w!!.optString(i)}", Ui.WARN)
        Params.notifyChanged()
        if (rebuild) runCatching { a.edit.buildPage() }
        return ok
    }

    /** Load [file] as a wavetable into [layer]. Checkpoints for undo, runs on `a.loader`, calls [onDone] on the UI thread. */
    fun loadWavetable(a: MainActivity, bridge: FormatBridge, layer: Int, file: File, rebuildPage: Boolean, onDone: (Boolean) -> Unit = {}) {
        History.checkpoint()
        a.log("loading wavetable ${file.name}…")
        a.loader.execute {
            val r = json { bridge.loadAnyWavetable(layer, file.absolutePath) }
            a.ui.post {
                if (a.isDestroyed) return@post
                val ok = r.optBoolean("ok")
                if (ok) {
                    val l = "abc"[layer.coerceIn(0, 2)]
                    Params.set("$l.type", optionIndex(l, "type", "wt", 4).toFloat())
                    Params.metaOf("$l.wt.table")?.let { Params.set("$l.wt.table", (it.options.size - 1).toFloat()) }  // "user"
                    a.log("wavetable loaded: ${file.name}")
                    val w = r.optJSONArray("warnings")
                    for (i in 0 until (w?.length() ?: 0)) a.log("  ${w!!.optString(i)}", Ui.WARN)
                } else {
                    a.log("wavetable load failed: ${r.optString("error", "unknown error")}", Ui.ERR)
                }
                Params.notifyChanged()
                if (rebuildPage) runCatching { a.edit.buildPage() }
                onDone(ok)
            }
        }
    }

    fun toast(a: Context, msg: String) = Toast.makeText(a, msg, Toast.LENGTH_SHORT).show()

    fun hideKeyboard(a: Context, v: View) {
        (a.getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager)?.hideSoftInputFromWindow(v.windowToken, 0)
    }

    // ---- audition: one short C4 note on the edit part (UI thread only)

    private var auditionOff: Runnable? = null
    private var auditionOn = false

    fun auditionPlay(a: MainActivity, ms: Long = 900L) {
        auditionStop(a)
        NativeEngine.noteOn(AUDITION_ID, AUDITION_KEY, 0.8f)
        auditionOn = true
        val r = Runnable { auditionStop(a) }
        auditionOff = r
        a.ui.postDelayed(r, ms)
    }

    fun auditionStop(a: MainActivity) {
        auditionOff?.let { a.ui.removeCallbacks(it) }
        auditionOff = null
        if (auditionOn) {
            auditionOn = false
            NativeEngine.noteOff(AUDITION_ID, AUDITION_KEY)
        }
    }
}

// ====================================================================================================== .dslibrary

/** Extracts Decent Sampler libraries (ZIP) with zip-slip and zip-bomb protection. Blocking: use `a.loader`. */
object DsLibraryExtractor {
    private const val MAX_ENTRIES = 20_000
    private const val MAX_TOTAL_BYTES = 2L * 1024 * 1024 * 1024

    fun root(ctx: Context) = FormatFiles.userDir(ctx, FormatKind.DS)

    /** Extract a SAF [uri]. @return every `.dspreset` found (sorted by path). */
    fun extract(ctx: Context, uri: Uri): List<File> {
        val name = FormatFiles.displayName(ctx, uri)
        val input = ctx.contentResolver.openInputStream(uri) ?: throw IOException("cannot read $name")
        return input.use { extract(ctx, it, name) }
    }

    /** Extract a local `.dslibrary` file. */
    fun extract(ctx: Context, file: File): List<File> = file.inputStream().buffered().use { extract(ctx, it, file.name) }

    /**
     * Into `files/library/user/ds/<safeName>/` (a numbered sibling when it exists). Rejects absolute paths, `..`
     * segments and anything whose canonical path leaves the target folder; the half-written folder is removed on any
     * failure. Throws [IOException] when the archive holds no `.dspreset`.
     */
    private fun extract(ctx: Context, input: InputStream, archiveName: String): List<File> {
        val base = FormatFiles.safeName(archiveName.substringBeforeLast('.'))
        val dest = FormatFiles.uniqueFile(root(ctx).apply { mkdirs() }, base)
        dest.mkdirs()
        val destCanon = dest.canonicalFile
        val prefix = destCanon.path + File.separator
        val presets = ArrayList<File>()
        var ok = false
        try {
            val zin = ZipInputStream(input)
            val buf = ByteArray(64 * 1024)
            var entries = 0
            var total = 0L
            while (true) {
                val e = zin.nextEntry ?: break
                if (++entries > MAX_ENTRIES) throw IOException("archive has too many entries")
                val raw = e.name.replace('\\', '/')
                if (raw.startsWith("__MACOSX/") || raw.substringAfterLast('/').startsWith("._")) continue   // Finder junk
                if (raw.startsWith("/") || raw.split('/').any { it == ".." } || (raw.length > 1 && raw[1] == ':'))
                    throw IOException("unsafe path in archive (zip-slip): ${e.name}")
                val out = File(destCanon, raw).canonicalFile
                if (out.path != destCanon.path && !out.path.startsWith(prefix))
                    throw IOException("unsafe path in archive (zip-slip): ${e.name}")
                if (e.isDirectory) { out.mkdirs(); continue }
                out.parentFile?.mkdirs()
                out.outputStream().use { os ->
                    while (true) {
                        val n = zin.read(buf)
                        if (n < 0) break
                        total += n
                        if (total > MAX_TOTAL_BYTES) throw IOException("archive is too large when extracted")
                        os.write(buf, 0, n)
                    }
                }
                if (out.extension.equals("dspreset", ignoreCase = true)) presets += out
            }
            if (presets.isEmpty()) throw IOException("no .dspreset inside $archiveName")
            ok = true
        } finally {
            if (!ok) dest.deleteRecursively()
        }
        return presets.sortedBy { it.path.lowercase(Locale.ROOT) }
    }
}

// ====================================================================================================== picker glue

/**
 * One "import a sound-format file" entry point for the Library tab (wire in the integration wave):
 * `FormatImporter(a, bridge).pickFile()` opens the system picker; the result is routed by extension:
 * `.sf2/.sf3/.dls/.gig` → [SoundFontPickerSheet], `.syx` → [Dx7BankSheet], `.dslibrary/.dspreset` → layer chooser →
 * `bridge.loadDecentSampler`, `.wt/.vitaltable/.wav` → layer chooser → `bridge.loadAnyWavetable`.
 * Create once and cache in the owner (the sheets are cached here).
 */
class FormatImporter(private val a: MainActivity, private val bridge: FormatBridge) {
    val sf2Sheet: SoundFontPickerSheet by lazy { SoundFontPickerSheet(a, bridge) }
    val dx7Sheet: Dx7BankSheet by lazy { Dx7BankSheet(a, bridge) }

    fun pickFile() {
        ActivityResults.register(FormatFiles.REQ_FORMAT_IMPORT) { data -> data?.data?.let { handle(it) } }
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
        }
        @Suppress("DEPRECATION")
        a.startActivityForResult(intent, FormatFiles.REQ_FORMAT_IMPORT)
    }

    /** Copy / extract [uri] into the user library on `a.loader`, then continue on the UI thread. */
    fun handle(uri: Uri) {
        val name = FormatFiles.displayName(a, uri)
        val ext = name.substringAfterLast('.', "").lowercase(Locale.ROOT)
        a.log("importing $name…")
        a.loader.execute {
            try {
                when (ext) {
                    "dslibrary" -> {
                        val presets = DsLibraryExtractor.extract(a, uri)
                        a.ui.post { if (!a.isDestroyed) { runCatching { a.libraryPanel.refresh() }; chooseDsPreset(presets) } }
                    }
                    else -> {
                        val kind = FormatKind.of(name) ?: throw IOException("unsupported file type: .$ext")
                        val f = FormatFiles.importSingle(a, uri, kind)
                        a.ui.post {
                            if (a.isDestroyed) return@post
                            runCatching { a.libraryPanel.refresh() }   // the new file shows up in LIB at once
                            when (kind) {
                                FormatKind.SF2 -> sf2Sheet.show(f)
                                FormatKind.DX7 -> dx7Sheet.show(f)
                                FormatKind.DS -> chooseDsPreset(listOf(f))
                                FormatKind.WT -> chooseLayer(f.name) { l -> FormatFiles.loadWavetable(a, bridge, l, f, rebuildPage = true) }
                            }
                        }
                    }
                }
            } catch (e: Exception) {
                a.ui.post { a.log("import failed: ${e.message}", Ui.ERR) }
            }
        }
    }

    /**
     * Route a file that already lives in the app (library / installed pack) by extension, on the UI thread:
     * `.sf2/.sf3/.dls/.gig` → [SoundFontPickerSheet], `.syx` → [Dx7BankSheet], `.dspreset` → layer chooser,
     * `.dslibrary` → extract then choose, `.wt/.vitaltable/.wav` → layer chooser → wavetable. @return false = not a format file.
     */
    fun open(f: File): Boolean {
        val ext = f.extension.lowercase(Locale.ROOT)
        when (ext) {
            "sf2", "sf3", "dls", "gig" -> sf2Sheet.show(f)
            "syx" -> dx7Sheet.show(f)
            "dspreset" -> chooseDsPreset(listOf(f))
            "dslibrary" -> {
                a.log("extracting ${f.name}…")
                a.loader.execute {
                    try {
                        val presets = DsLibraryExtractor.extract(a, f)
                        a.ui.post { if (!a.isDestroyed) chooseDsPreset(presets) }
                    } catch (e: Exception) {
                        a.ui.post { a.log("extract failed: ${e.message}", Ui.ERR) }
                    }
                }
            }
            "wt", "vitaltable", "wav" -> chooseLayer(f.name) { l -> FormatFiles.loadWavetable(a, bridge, l, f, rebuildPage = true) }
            else -> return false
        }
        return true
    }

    private fun chooseDsPreset(presets: List<File>) {
        when (presets.size) {
            0 -> a.log("no .dspreset found", Ui.WARN)
            1 -> chooseLayer(presets[0].name) { l -> loadDs(l, presets[0]) }
            else -> {
                val names = presets.map { it.nameWithoutExtension }.toTypedArray()
                AlertDialog.Builder(a)
                    .setTitle(L10n.f("%s presets in library", presets.size.toString()))
                    .setItems(names) { _, i -> chooseLayer(presets[i].name) { l -> loadDs(l, presets[i]) } }
                    .show()
            }
        }
    }

    private fun loadDs(layer: Int, f: File) {
        History.checkpoint()
        a.log("loading ${f.name} into layer ${"ABC"[layer]}…")
        a.loader.execute {
            val r = FormatFiles.json { bridge.loadDecentSampler(layer, f.absolutePath) }
            a.ui.post { if (!a.isDestroyed) FormatFiles.finishLayerLoad(a, layer, r, f.name, "pcm", rebuild = true) }
        }
    }

    private fun chooseLayer(what: String, onLayer: (Int) -> Unit) {
        AlertDialog.Builder(a)
            .setTitle(L10n.f("Load %s into…", what))
            .setItems(arrayOf("Layer A", "Layer B", "Layer C").map { L10n.t(it) }.toTypedArray()) { _, which -> onLayer(which) }
            .show()
    }
}

// ====================================================================================================== SoundFont picker sheet

/**
 * SF2 / SF3 / DLS / GIG preset picker (CENTER 700 × 350 dp). Drum kits carry a `[DRUM]` badge. Header: file name · `N PRESETS` · LAYER A|B|C pills · ✕.
 * Live search, recycled list of `[BBB:PPP]` (cyan mono) + preset name; a tap loads the preset into the chosen layer
 * on `a.loader` and auditions it (C4). `SAVE AS PRESET…` stores the edit part as a user preset.
 * Open with [show]; built once (lazily) and cached by the owner.
 */
class SoundFontPickerSheet(private val a: MainActivity, private val bridge: FormatBridge) : Sheet(Edge.CENTER, 700, 350) {
    override val view: View by lazy { build() }

    private val d = a.resources.displayMetrics.density
    private fun dp(v: Int) = (v * d).toInt()

    private class Preset(val bank: Int, val program: Int, val isDrum: Boolean, val name: String, val index: Int) {
        val label = String.format(Locale.US, "[%03d:%03d]", bank, program)
        val key = name.lowercase(Locale.ROOT) + if (isDrum) " drum" else ""
    }

    private var file: File? = null
    private var layer = 0
    private var all: List<Preset> = emptyList()
    private var shown: List<Preset> = emptyList()
    private var loadedIndex = -1
    private var busy = false
    private var pending: Preset? = null
    private var touched = false   // a preset was loaded: EDIT page must be rebuilt when the sheet closes
    private var lastName = ""

    private lateinit var titleTv: TextView
    private lateinit var countTv: TextView
    private lateinit var statusTv: TextView
    private lateinit var search: EditText
    private lateinit var list: ListView
    private lateinit var pills: Array<LedButton>

    private val adapter = object : BaseAdapter() {
        override fun getCount() = shown.size
        override fun getItem(position: Int): Any = shown[position]
        override fun getItemId(position: Int) = shown[position].index.toLong()
        override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
            val r = (convertView as? Row) ?: Row(a)
            val s = shown[position]
            r.bankTv.text = s.label
            r.nameTv.text = s.name
            r.drumTv.visibility = if (s.isDrum) View.VISIBLE else View.GONE
            r.nameTv.setTextColor(if (s.index == loadedIndex) Hw.MOD_OSC else Hw.SILK)
            return r
        }
    }

    private class Row(ctx: Context) : LinearLayout(ctx) {
        val bankTv = TextView(ctx)
        val nameTv = TextView(ctx)
        val drumTv = TextView(ctx)

        init {
            val d = ctx.resources.displayMetrics.density
            orientation = HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding((10 * d).toInt(), 0, (10 * d).toInt(), 0)
            layoutParams = AbsListView.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, (34 * d).toInt())
            bankTv.apply { textSize = 12.5f; typeface = Hw.MONO; setTextColor(Hw.MOD_OSC); maxLines = 1 }
            nameTv.apply { textSize = 14f; typeface = Hw.LABEL; maxLines = 1; ellipsize = TextUtils.TruncateAt.END }
            addView(bankTv, LayoutParams((84 * d).toInt(), ViewGroup.LayoutParams.WRAP_CONTENT))
            addView(nameTv, LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            drumTv.apply {
                text = "[DRUM]"; textSize = 10.5f; typeface = Hw.MONO; setTextColor(Hw.MOD_FX); maxLines = 1
                visibility = View.GONE
            }
            addView(drumTv, LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { marginStart = (6 * d).toInt() })
        }
    }

    /** Open the picker for [f] (an .sf2 / .sf3 / .dls / .gig already inside the app library). */
    fun show(f: File, layer: Int = this.layer) {
        view
        file = f
        this.layer = layer.coerceIn(0, 2)
        all = emptyList(); shown = emptyList(); loadedIndex = -1; pending = null; touched = false
        titleTv.text = f.name
        countTv.text = L10n.t("LOADING…")
        statusTv.text = ""
        search.setText("")
        adapter.notifyDataSetChanged()
        updatePills()
        a.openSheet(this)
        a.loader.execute {
            val presets = parse(runCatching { bridge.listSoundBankPresets(f.absolutePath) }.getOrDefault("[]"))
            a.ui.post {
                if (file !== f) return@post
                all = presets.first
                applyFilter()
                statusTv.text = presets.second
                if (all.isEmpty() && presets.second.isEmpty()) statusTv.text = L10n.t("No presets found in this file.")
            }
        }
    }

    override fun onHide() {
        FormatFiles.hideKeyboard(a, search)
        FormatFiles.auditionStop(a)
        if (touched) {
            touched = false
            a.ui.post { runCatching { a.edit.buildPage() } }
        }
    }

    // ------------------------------------------------------------------ build

    private fun build(): View {
        val root = LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(10), dp(2), dp(10), dp(8))
        }

        val head = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        titleTv = Hw.engraved(a, "", 12f, tr = false, color = Hw.MOD_OSC)
        head.addView(titleTv, LinearLayout.LayoutParams(0, -2, 1f))
        countTv = Hw.lcd(a, "", 11f, Hw.ENGRAVE).apply { setPadding(dp(8), dp(2), dp(8), dp(2)) }
        head.addView(countTv, LinearLayout.LayoutParams(-2, -2).apply { marginStart = dp(8); marginEnd = dp(10) })
        head.addView(Hw.engraved(a, "LAYER", 9.5f, tr = false, color = Hw.INK_DIM), LinearLayout.LayoutParams(-2, -2).apply { marginEnd = dp(5) })
        pills = Array(3) { i ->
            LedButton(a, "ABC"[i].toString(), tr = false, led = false).apply {
                minHeight = 0; minimumHeight = 0; setPadding(0, 0, 0, 0)
                setOnClickListener { layer = i; updatePills() }
            }
        }
        pills.forEach { head.addView(it, LinearLayout.LayoutParams(dp(34), dp(28)).apply { marginEnd = dp(3) }) }
        head.addView(
            LedButton(a, "✕", tr = false, led = false).apply {
                minHeight = 0; minimumHeight = 0; setPadding(0, 0, 0, 0)
                setOnClickListener { dismiss() }
            }, LinearLayout.LayoutParams(dp(36), dp(28)).apply { marginStart = dp(6) }
        )
        root.addView(head, LinearLayout.LayoutParams(-1, dp(32)))

        search = EditText(a).apply {
            hint = L10n.t("Search presets…")
            setHintTextColor(Hw.INK_DIM)
            setTextColor(Hw.SILK)
            typeface = Hw.MONO
            textSize = 13.5f
            isSingleLine = true
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI or EditorInfo.IME_ACTION_SEARCH
            background = Hw.lcdBg(a)
            setPadding(dp(10), 0, dp(10), 0)
            addTextChangedListener(object : TextWatcher {
                override fun beforeTextChanged(s: CharSequence?, st: Int, c: Int, af: Int) {}
                override fun onTextChanged(s: CharSequence?, st: Int, b: Int, c: Int) {}
                override fun afterTextChanged(s: Editable?) = applyFilter()
            })
            setOnEditorActionListener { v, _, _ -> FormatFiles.hideKeyboard(a, v); true }
        }
        root.addView(search, LinearLayout.LayoutParams(-1, dp(36)).apply { topMargin = dp(4) })

        list = ListView(a).apply {
            adapter = this@SoundFontPickerSheet.adapter
            background = Hw.lcdBg(a)
            divider = ColorDrawable(Hw.PANEL_EDGE)
            dividerHeight = 1
            selector = ColorDrawable(Hw.withAlpha(Hw.MOD_OSC, 40))
            cacheColorHint = 0
            isVerticalScrollBarEnabled = false
            overScrollMode = View.OVER_SCROLL_NEVER
            onItemClickListener = AdapterView.OnItemClickListener { _, _, pos, _ -> shown.getOrNull(pos)?.let { loadPreset(it) } }
        }
        root.addView(list, LinearLayout.LayoutParams(-1, 0, 1f).apply { topMargin = dp(5) })

        val bottom = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        statusTv = Ui.text(a, "", 11.5f, Hw.ENGRAVE, tr = false).apply { typeface = Hw.MONO; maxLines = 1 }
        bottom.addView(statusTv, LinearLayout.LayoutParams(0, -2, 1f))
        bottom.addView(
            LedButton(a, "SAVE AS PRESET…", tr = false, led = false, accentColor = Hw.MOD_GOOD).apply {
                textSize = 11.5f; minHeight = 0; minimumHeight = 0; setPadding(dp(12), 0, dp(12), 0)
                setOnClickListener { promptSave() }
            }, LinearLayout.LayoutParams(-2, dp(34))
        )
        root.addView(bottom, LinearLayout.LayoutParams(-1, dp(34)).apply { topMargin = dp(5) })
        return root
    }

    private fun updatePills() {
        for (i in pills.indices) pills[i].lit = i == layer
    }

    // ------------------------------------------------------------------ data

    /** @return (presets, error text — empty when fine). Runs on the loader thread. */
    private fun parse(raw: String): Pair<List<Preset>, String> {
        val t = raw.trim()
        var err = ""
        val arr: JSONArray? = try {
            if (t.startsWith("[")) JSONArray(t) else JSONObject(t).also { err = it.optString("error", "") }.optJSONArray("presets")
        } catch (e: Exception) {
            err = e.message ?: "bad preset list"
            null
        }
        val out = ArrayList<Preset>(arr?.length() ?: 0)
        for (i in 0 until (arr?.length() ?: 0)) {
            val o = arr!!.optJSONObject(i) ?: continue
            out += Preset(o.optInt("bank"), o.optInt("program"), o.optBoolean("isDrum", false), o.optString("name", "Preset $i"), o.optInt("index", i))
        }
        out.sortWith(compareBy<Preset>({ if (it.isDrum) 1 else 0 }, { it.bank }, { it.program }, { it.name.lowercase(Locale.ROOT) }))
        return out to err
    }

    private fun applyFilter() {
        val tokens = search.text.toString().trim().lowercase(Locale.ROOT).split(' ').filter { it.isNotEmpty() }
        shown = if (tokens.isEmpty()) all else all.filter { p -> tokens.all { p.key.contains(it) || p.label.contains(it) } }
        countTv.text = if (tokens.isEmpty()) "${all.size} " + L10n.t("PRESETS")
        else "${shown.size} / ${all.size}"
        adapter.notifyDataSetChanged()
    }

    // ------------------------------------------------------------------ load / save

    private fun loadPreset(p: Preset) {
        val f = file ?: return
        if (busy) { pending = p; return }   // latest tap wins while a load is running
        busy = true
        pending = null
        statusTv.text = L10n.t("loading…") + " " + p.name
        val l = layer
        History.checkpoint()
        a.log("SoundFont: ${p.name} → layer ${"ABC"[l]}…")
        a.loader.execute {
            val r = FormatFiles.json { bridge.loadSoundBankPreset(l, f.absolutePath, p.bank, p.program, p.isDrum) }
            a.ui.post {
                busy = false
                if (a.isDestroyed || file !== f) return@post
                val ok = FormatFiles.finishLayerLoad(a, l, r, "${p.name} (${f.name})", "pcm", rebuild = false)
                if (ok) {
                    touched = true
                    loadedIndex = p.index
                    lastName = p.name
                    statusTv.text = "▶ ${p.name}  →  " + L10n.t("Layer") + " ${"ABC"[l]}"
                    adapter.notifyDataSetChanged()
                    FormatFiles.auditionPlay(a)
                } else {
                    statusTv.text = r.optString("error", "load failed")
                }
                pending?.let { next -> pending = null; if (next !== p) loadPreset(next) }
            }
        }
    }

    private fun promptSave() {
        val input = EditText(a).apply {
            inputType = InputType.TYPE_CLASS_TEXT
            imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI
            setText(lastName.ifEmpty { file?.nameWithoutExtension ?: "SoundFont" })
            setSelectAllOnFocus(true)
        }
        AlertDialog.Builder(a)
            .setTitle(L10n.t("Save as preset"))
            .setView(input)
            .setPositiveButton(L10n.t("Save")) { _, _ ->
                val name = input.text.toString().trim().ifEmpty { "SoundFont" }
                a.savePreset(name) { f -> if (f != null) FormatFiles.toast(a, L10n.f("Saved \"%s\"", name)) }
            }
            .setNegativeButton(L10n.t("Cancel"), null)
            .show()
    }
}

// ====================================================================================================== DX7 bank sheet

/**
 * DX7 SysEx cartridge browser (CENTER 760 × 354 dp). Header: file name · `N VOICES` · `IMPORT ALL TO LIBRARY` · ✕;
 * a 4-column scrollable grid of patch cards (`01`, name, `ALG xx`). A tap loads the voice into the current edit part
 * (`bridge.loadDx7Voice`, undoable) and auditions it. `IMPORT ALL` writes every voice as a user preset
 * `files/presets/user/dx7_<bank>_NN_<name>.json`. Open with [show]; built once (lazily) and cached by the owner.
 */
class Dx7BankSheet(private val a: MainActivity, private val bridge: FormatBridge) : Sheet(Edge.CENTER, 760, 354) {
    override val view: View by lazy { build() }

    private val d = a.resources.displayMetrics.density
    private fun dp(v: Int) = (v * d).toInt()

    private class Voice(val index: Int, val name: String, val algo: Int)

    private var file: File? = null
    private var voices: List<Voice> = emptyList()
    private var selected = -1
    private var busy = false
    private var pending: Voice? = null
    private var touched = false

    private lateinit var titleTv: TextView
    private lateinit var badgeTv: TextView
    private lateinit var statusTv: TextView
    private lateinit var importBtn: LedButton
    private lateinit var grid: GridView

    private val adapter = object : BaseAdapter() {
        override fun getCount() = voices.size
        override fun getItem(position: Int): Any = voices[position]
        override fun getItemId(position: Int) = voices[position].index.toLong()
        override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
            val c = (convertView as? Dx7Card) ?: Dx7Card(a)
            val v = voices[position]
            c.bind(v.index + 1, v.name, v.algo, v.index == selected)
            return c
        }
    }

    /** One patch card, drawn (no child views, no allocation in onDraw). */
    private class Dx7Card(ctx: Context) : View(ctx) {
        private val d = ctx.resources.displayMetrics.density
        private val fill = Paint(Paint.ANTI_ALIAS_FLAG)
        private val line = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = d }
        private val numP = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Hw.MONO; textSize = 11f * d; color = Hw.INK_DIM }
        private val nameP = TextPaint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Hw.BOLD; textSize = 13.5f * d; color = Hw.SILK }
        private val algP = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Hw.MONO; textSize = 9.5f * d; textAlign = Paint.Align.CENTER }
        private val r = RectF()
        private val badge = RectF()
        private var num = ""
        private var name = ""
        private var shownName = ""
        private var algText = ""
        private var accent = Hw.MOD_OSC
        private var sel = false

        init {
            layoutParams = AbsListView.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, (36 * d).toInt())
        }

        fun bind(number: Int, name: String, algo: Int, selected: Boolean) {
            val n = String.format(Locale.US, "%02d", number)
            val al = if (algo > 0) String.format(Locale.US, "ALG %02d", algo) else "ALG --"
            val col = if (algo > 0) Hw.partColor(algo - 1) else Hw.ENGRAVE
            if (n != num || name != this.name || al != algText || col != accent || selected != sel) {
                num = n; this.name = name; algText = al; accent = col; sel = selected
                ellipsize()
                invalidate()
            }
        }

        override fun onMeasure(ws: Int, hs: Int) {
            setMeasuredDimension(getDefaultSize(suggestedMinimumWidth, ws), (36 * d).toInt())
        }

        override fun onSizeChanged(w: Int, h: Int, ow: Int, oh: Int) {
            super.onSizeChanged(w, h, ow, oh)
            ellipsize()
        }

        private fun ellipsize() {
            val w = width.toFloat()
            if (w <= 0f) { shownName = name; return }
            val avail = w - 34f * d - (algP.measureText(algText) + 14f * d) - 6f * d
            shownName = TextUtils.ellipsize(name, nameP, avail.coerceAtLeast(10f * d), TextUtils.TruncateAt.END).toString()
        }

        override fun onDraw(c: Canvas) {
            val w = width.toFloat()
            val h = height.toFloat()
            r.set(d / 2f, d / 2f, w - d / 2f, h - d / 2f)
            fill.color = if (isPressed) Hw.PANEL_RAISED else if (sel) Hw.withAlpha(accent, 34) else Hw.KEY
            c.drawRoundRect(r, 6f * d, 6f * d, fill)
            line.color = if (sel) Hw.withAlpha(accent, 200) else if (isPressed) Hw.PANEL_HI else Hw.PANEL_EDGE
            c.drawRoundRect(r, 6f * d, 6f * d, line)
            // algorithm colour strip
            fill.color = Hw.withAlpha(accent, if (sel) 255 else 150)
            c.drawRect(3f * d, 8f * d, 6f * d, h - 8f * d, fill)

            val base = h / 2f - (nameP.ascent() + nameP.descent()) / 2f
            numP.color = if (sel) accent else Hw.INK_DIM
            c.drawText(num, 11f * d, h / 2f - (numP.ascent() + numP.descent()) / 2f, numP)
            c.drawText(shownName, 34f * d, base, nameP)

            val bw = algP.measureText(algText) + 10f * d
            badge.set(w - bw - 6f * d, h / 2f - 8f * d, w - 6f * d, h / 2f + 8f * d)
            fill.color = Hw.withAlpha(accent, 28)
            c.drawRoundRect(badge, 8f * d, 8f * d, fill)
            algP.color = accent
            c.drawText(algText, badge.centerX(), h / 2f - (algP.ascent() + algP.descent()) / 2f, algP)
        }
    }

    /** Open the cartridge browser for [f] (a `.syx` already inside the app library). */
    fun show(f: File) {
        view
        file = f
        voices = emptyList(); selected = -1; pending = null; touched = false
        titleTv.text = f.name
        badgeTv.text = L10n.t("LOADING…")
        statusTv.text = ""
        importBtn.isEnabled = false
        adapter.notifyDataSetChanged()
        a.openSheet(this)
        a.loader.execute {
            val r = FormatFiles.json { bridge.inspectDx7Sysex(f.absolutePath) }
            val list = ArrayList<Voice>()
            val arr = r.optJSONArray("voices")
            for (i in 0 until (arr?.length() ?: 0)) {
                val o = arr!!.optJSONObject(i) ?: continue
                list += Voice(o.optInt("index", i), o.optString("name", "VOICE ${i + 1}").trim(), o.optInt("algo", 0))
            }
            a.ui.post {
                if (file !== f) return@post
                if (!r.optBoolean("ok") || list.isEmpty()) {
                    badgeTv.text = L10n.t("NO VOICES")
                    statusTv.text = r.optString("error", L10n.t("Not a DX7 voice dump."))
                    a.log("DX7: ${r.optString("error", "no voices in ${f.name}")}", Ui.ERR)
                    return@post
                }
                voices = list
                badgeTv.text = "${list.size} " + L10n.t("VOICES")
                importBtn.isEnabled = true
                adapter.notifyDataSetChanged()
            }
        }
    }

    override fun onHide() {
        FormatFiles.auditionStop(a)
        if (touched) {
            touched = false
            a.ui.post { runCatching { a.edit.buildPage() } }
        }
    }

    // ------------------------------------------------------------------ build

    private fun build(): View {
        val root = LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(10), dp(2), dp(10), dp(8))
        }
        val head = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        titleTv = Hw.engraved(a, "", 12f, tr = false, color = Hw.MOD_FILTER)
        head.addView(titleTv, LinearLayout.LayoutParams(0, -2, 1f))
        badgeTv = Hw.lcd(a, "", 11f, Hw.MOD_FILTER).apply { setPadding(dp(8), dp(2), dp(8), dp(2)) }
        head.addView(badgeTv, LinearLayout.LayoutParams(-2, -2).apply { marginStart = dp(8); marginEnd = dp(10) })
        importBtn = LedButton(a, "IMPORT ALL TO LIBRARY", tr = false, led = false, accentColor = Hw.MOD_FX).apply {
            lit = true
            textSize = 11.5f; minHeight = 0; minimumHeight = 0; setPadding(dp(12), 0, dp(12), 0)
            setOnClickListener { importAll() }
        }
        head.addView(importBtn, LinearLayout.LayoutParams(-2, dp(28)))
        head.addView(
            LedButton(a, "✕", tr = false, led = false).apply {
                minHeight = 0; minimumHeight = 0; setPadding(0, 0, 0, 0)
                setOnClickListener { dismiss() }
            }, LinearLayout.LayoutParams(dp(36), dp(28)).apply { marginStart = dp(6) }
        )
        root.addView(head, LinearLayout.LayoutParams(-1, dp(32)))

        grid = GridView(a).apply {
            numColumns = 4
            horizontalSpacing = dp(4)
            verticalSpacing = dp(4)
            stretchMode = GridView.STRETCH_COLUMN_WIDTH
            adapter = this@Dx7BankSheet.adapter
            selector = ColorDrawable(0)
            cacheColorHint = 0
            isVerticalScrollBarEnabled = false
            overScrollMode = View.OVER_SCROLL_NEVER
            onItemClickListener = AdapterView.OnItemClickListener { _, _, pos, _ -> voices.getOrNull(pos)?.let { loadVoice(it) } }
        }
        root.addView(grid, LinearLayout.LayoutParams(-1, 0, 1f).apply { topMargin = dp(4) })

        statusTv = Ui.text(a, "", 11.5f, Hw.ENGRAVE, tr = false).apply { typeface = Hw.MONO; maxLines = 1 }
        root.addView(statusTv, LinearLayout.LayoutParams(-1, dp(18)).apply { topMargin = dp(3) })
        return root
    }

    // ------------------------------------------------------------------ actions

    private fun loadVoice(v: Voice) {
        val f = file ?: return
        if (busy) { pending = v; return }   // latest tap wins while a load is running
        busy = true
        pending = null
        History.checkpoint()
        a.loader.execute {
            val r = FormatFiles.json { bridge.loadDx7Voice(f.absolutePath, v.index) }
            a.ui.post {
                busy = false
                if (a.isDestroyed || file !== f) return@post
                if (r.optBoolean("ok")) {
                    val name = r.optString("name", v.name).ifEmpty { v.name }
                    touched = true
                    selected = v.index
                    Params.notifyChanged()
                    a.log("DX7: \"$name\" loaded")
                    statusTv.text = "▶ $name  (${String.format(Locale.US, "%02d", v.index + 1)})"
                    adapter.notifyDataSetChanged()
                    FormatFiles.auditionPlay(a)
                } else {
                    val err = r.optString("error", "load failed")
                    a.log("DX7 load failed: $err", Ui.ERR)
                    statusTv.text = err
                }
                pending?.let { next -> pending = null; if (next !== v) loadVoice(next) }
            }
        }
    }

    private fun importAll() {
        val f = file ?: return
        if (busy || voices.isEmpty()) return
        busy = true
        importBtn.isEnabled = false
        val bank = FormatFiles.safeName(f.nameWithoutExtension).replace(' ', '_')
        val out = a.library.userPresets.apply { mkdirs() }   // flat: <dx7_bank>_NN_<name>.json, picked up by LIB at once
        statusTv.text = L10n.t("importing…")
        a.loader.execute {
            val r = FormatFiles.json { bridge.importDx7Bank(f.absolutePath, out.absolutePath, "dx7_$bank") }
            a.ui.post {
                busy = false
                importBtn.isEnabled = voices.isNotEmpty()
                if (a.isDestroyed) return@post
                if (r.optBoolean("ok")) {
                    val n = r.optInt("count", r.optJSONArray("names")?.length() ?: 0)
                    a.log("DX7: $n voices imported to presets/user (dx7_${bank}_NN_*)")
                    statusTv.text = "✓ $n " + L10n.t("voices saved to") + " dx7_$bank"
                    FormatFiles.toast(a, L10n.f("%s voices imported", n.toString()))
                    runCatching { a.libraryPanel.refresh() }
                } else {
                    val err = r.optString("error", "import failed")
                    a.log("DX7 import failed: $err", Ui.ERR)
                    statusTv.text = err
                }
            }
        }
    }
}
