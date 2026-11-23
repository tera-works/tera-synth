package com.hsynth.app

import android.content.Context
import android.media.MediaCodec
import android.media.MediaExtractor
import android.media.MediaFormat
import android.media.MediaMetadataRetriever
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.provider.OpenableColumns
import java.io.BufferedOutputStream
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.io.InputStream
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import java.util.concurrent.CancellationException
import java.util.concurrent.atomic.AtomicBoolean

/*
 * B1 — backing-track decoder (Phase 4 Wave 1B). Standalone: nothing here touches the engine or the UI.
 *
 * Any audio file Android can decode (MP3, AAC / M4A, OGG, OPUS, WAV, FLAC, ...) becomes a file the engine can stream:
 *   - a local / SAF `.wav` (PCM / float / extensible) or `.flac` is NOT re-encoded: a local file is returned as is, a
 *     SAF uri is copied byte for byte into the cache;
 *   - everything else is decoded with MediaExtractor + MediaCodec into stereo 16-bit PCM WAV (`cacheDir/audio/<id>.wav`,
 *     source rate kept, mono is duplicated, > 2 channels keep front L / R), streamed to disk through a small buffer, so a
 *     one hour track never needs more RAM than a 64 KB block.
 * Wiring (Wave 2, PlayerPanel AUDIO page): `AudioDecode.decodeAsync(...)` on the tapped uri, show the progress chip,
 * then `nativeAudioLoad(decoded.wavFile.absolutePath)`.
 */

/** Result of [AudioDecode]: [wavFile] is a `.wav` (decoded / copied) or a `.flac` (copied) the engine reads natively. */
data class DecodedAudio(
    val wavFile: File,
    val title: String,
    val durationSec: Double,
    val sampleRate: Int,
    val channels: Int,
)

/** Cooperative cancel flag for a running decode. */
class AudioCancel {
    private val flag = AtomicBoolean(false)
    val cancelled: Boolean get() = flag.get()
    fun cancel() = flag.set(true)
}

object AudioDecode {
    /** LRU cap of `cacheDir/audio` (the two most recently used files are always kept, even beyond the cap). */
    const val CACHE_CAP_BYTES = 256L * 1024 * 1024
    private const val MAX_OUTPUT_BYTES = 1_800_000_000L      // a WAV header can describe < 4 GB; stay far below
    private const val DEQUEUE_US = 10_000L
    private const val STALL_LIMIT_MS = 15_000L                // codec produced nothing for this long -> give up
    private const val PROGRESS_MIN_MS = 100L
    private const val STALE_TMP_MS = 10 * 60_000L

    private val cacheLock = Any()

    fun cacheDir(ctx: Context): File = File(ctx.cacheDir, "audio").apply { mkdirs() }

    // ====================================================================================== public API

    /** Decode [uri] (content:// or file://). Blocking: call from `a.loader` or use [decodeAsync]. */
    @Throws(IOException::class, CancellationException::class)
    fun decode(ctx: Context, uri: Uri, cancel: AudioCancel = AudioCancel(), onProgress: (Float) -> Unit = {}): DecodedAudio =
        decodeSource(Source.of(ctx, uri), cancel, onProgress)

    /** Decode a local [file]. Blocking. */
    @Throws(IOException::class, CancellationException::class)
    fun decode(ctx: Context, file: File, cancel: AudioCancel = AudioCancel(), onProgress: (Float) -> Unit = {}): DecodedAudio =
        decodeSource(Source.of(ctx, file), cancel, onProgress)

    /**
     * Run [decode] on the process-wide loader thread. [onProgress] and [onDone] are posted to the UI thread
     * (`onDone(result, null)` or `onDone(null, error)`; a cancelled decode reports a [CancellationException]).
     */
    fun decodeAsync(
        ctx: Context, uri: Uri, cancel: AudioCancel = AudioCancel(),
        onProgress: (Float) -> Unit, onDone: (DecodedAudio?, Throwable?) -> Unit,
    ) {
        val app = ctx.applicationContext
        val ui = Handler(Looper.getMainLooper())
        AppServices.loader.execute {
            val result = try {
                decode(app, uri, cancel) { p -> ui.post { onProgress(p) } } to null
            } catch (t: Throwable) {
                null to t
            }
            ui.post { onDone(result.first, result.second) }
        }
    }

    /** Delete least-recently-used cache files until the folder is <= [maxBytes]. [keep] and the 2 newest files survive. */
    fun trimCache(ctx: Context, maxBytes: Long = CACHE_CAP_BYTES, keep: File? = null) {
        synchronized(cacheLock) {
            val dir = File(ctx.cacheDir, "audio")
            val files = dir.listFiles { f -> f.isFile && !f.name.endsWith(".tmp") }?.sortedBy { it.lastModified() } ?: return
            var total = files.sumOf { it.length() }
            val protectedFrom = (files.size - 2).coerceAtLeast(0)          // the two newest are never evicted
            for ((i, f) in files.withIndex()) {
                if (total <= maxBytes) break
                if (i >= protectedFrom || (keep != null && f.absolutePath == keep.absolutePath)) continue
                val n = f.length()
                if (f.delete()) total -= n
            }
        }
    }

    fun clearCache(ctx: Context) {
        synchronized(cacheLock) { File(ctx.cacheDir, "audio").listFiles()?.forEach { it.delete() } }
    }

    fun cacheBytes(ctx: Context): Long =
        File(ctx.cacheDir, "audio").listFiles()?.sumOf { it.length() } ?: 0L

    // ====================================================================================== source abstraction

    private class Source(val ctx: Context, val uri: Uri?, val file: File?) {
        val displayName: String by lazy { queryName() }
        /** Stable identity string for the cache key (path / uri + size + modification time). */
        val identity: String by lazy { queryIdentity() }

        fun open(): InputStream =
            if (file != null) file.inputStream()
            else ctx.contentResolver.openInputStream(uri!!) ?: throw IOException("cannot open $uri")

        fun attach(ex: MediaExtractor) {
            if (file != null) ex.setDataSource(file.absolutePath) else ex.setDataSource(ctx, uri!!, null)
        }

        fun attach(r: MediaMetadataRetriever) {
            if (file != null) r.setDataSource(file.absolutePath) else r.setDataSource(ctx, uri!!)
        }

        private fun queryName(): String {
            if (file != null) return file.name
            val u = uri!!
            try {
                ctx.contentResolver.query(u, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
                    if (c.moveToFirst()) c.getString(0)?.let { if (it.isNotBlank()) return it }
                }
            } catch (_: Exception) {}
            return u.lastPathSegment?.substringAfterLast('/') ?: "audio"
        }

        private fun queryIdentity(): String {
            if (file != null) return "f:${file.absolutePath}:${file.length()}:${file.lastModified()}"
            val u = uri!!
            var size = -1L
            var mod = -1L
            try {
                ctx.contentResolver.query(u, null, null, null, null)?.use { c ->
                    if (c.moveToFirst()) {
                        c.getColumnIndex(OpenableColumns.SIZE).let { if (it >= 0 && !c.isNull(it)) size = c.getLong(it) }
                        c.getColumnIndex("last_modified").let { if (it >= 0 && !c.isNull(it)) mod = c.getLong(it) }
                    }
                }
            } catch (_: Exception) {}
            return "u:$u:$size:$mod"
        }

        companion object {
            fun of(ctx: Context, uri: Uri): Source {
                val app = ctx.applicationContext
                if (uri.scheme == "file") uri.path?.let { return Source(app, null, File(it)) }
                return Source(app, uri, null)
            }
            fun of(ctx: Context, file: File) = Source(ctx.applicationContext, null, file)
        }
    }

    // ====================================================================================== pipeline

    private fun decodeSource(src: Source, cancel: AudioCancel, onProgress: (Float) -> Unit): DecodedAudio {
        if (Looper.myLooper() == Looper.getMainLooper()) throw IllegalStateException("AudioDecode must run off the UI thread")
        val ctx = src.ctx
        val dir = cacheDir(ctx)
        val title = titleOf(src)

        // 1) already engine-readable: WAV (PCM / float) or FLAC -> no re-encoding
        val head = ByteArray(64)
        val got = try { src.open().use { readFully(it, head) } } catch (e: IOException) { throw IOException("cannot read ${src.displayName}: ${e.message}", e) }
        val kind = sniff(head, got)
        if (kind != Kind.OTHER) {
            if (src.file != null) {
                val info = if (kind == Kind.WAV) readWavInfo(src.file) else readFlacInfo(src.file)
                if (info != null) { onProgress(1f); return DecodedAudio(src.file, title, info.durationSec, info.sampleRate, info.channels) }
            } else {
                val out = File(dir, "${cacheKey(src)}.${if (kind == Kind.WAV) "wav" else "flac"}")
                val cached = reuse(out)
                if (cached == null) {
                    copyToCache(src, out, cancel, onProgress)
                }
                val info = if (kind == Kind.WAV) readWavInfo(out) else readFlacInfo(out)
                if (info != null) {
                    out.setLastModified(System.currentTimeMillis())
                    trimCache(ctx, keep = out)
                    onProgress(1f)
                    return DecodedAudio(out, title, info.durationSec, info.sampleRate, info.channels)
                }
                out.delete()                                   // unsupported WAV flavour -> fall through to the codec
            }
        }

        // 2) cache hit?
        val out = File(dir, "${cacheKey(src)}.wav")
        reuse(out)?.let { hit ->
            readWavInfo(hit)?.let {
                hit.setLastModified(System.currentTimeMillis())
                onProgress(1f)
                return DecodedAudio(hit, title, it.durationSec, it.sampleRate, it.channels)
            }
            hit.delete()
        }

        // 3) decode with MediaCodec
        sweepStaleTmp(dir)
        val tmp = File(dir, "${out.name}.${System.nanoTime()}.tmp")
        try {
            val info = transcode(src, tmp, cancel, onProgress)
            synchronized(cacheLock) {
                out.delete()
                if (!tmp.renameTo(out)) throw IOException("cannot publish ${out.name}")
            }
            trimCache(ctx, keep = out)
            onProgress(1f)
            return DecodedAudio(out, title, info.durationSec, info.sampleRate, info.channels)
        } finally {
            tmp.delete()
        }
    }

    private enum class Kind { WAV, FLAC, OTHER }

    private fun sniff(b: ByteArray, n: Int): Kind {
        if (n >= 12 && b[0] == 'R'.code.toByte() && b[1] == 'I'.code.toByte() && b[2] == 'F'.code.toByte() && b[3] == 'F'.code.toByte() &&
            b[8] == 'W'.code.toByte() && b[9] == 'A'.code.toByte() && b[10] == 'V'.code.toByte() && b[11] == 'E'.code.toByte()) return Kind.WAV
        if (n >= 4 && b[0] == 'f'.code.toByte() && b[1] == 'L'.code.toByte() && b[2] == 'a'.code.toByte() && b[3] == 'C'.code.toByte()) return Kind.FLAC
        return Kind.OTHER
    }

    private fun readFully(i: InputStream, b: ByteArray): Int {
        var n = 0
        while (n < b.size) {
            val r = i.read(b, n, b.size - n)
            if (r < 0) break
            n += r
        }
        return n
    }

    private fun cacheKey(src: Source): String {
        val d = MessageDigest.getInstance("SHA-256").digest(src.identity.toByteArray(Charsets.UTF_8))
        val sb = StringBuilder(16)
        for (i in 0 until 8) sb.append(String.format("%02x", d[i].toInt() and 0xFF))
        return sb.toString()
    }

    private fun reuse(f: File): File? = if (f.isFile && f.length() > 44) f else null

    private fun sweepStaleTmp(dir: File) {
        val now = System.currentTimeMillis()
        dir.listFiles { f -> f.name.endsWith(".tmp") }?.forEach { if (now - it.lastModified() > STALE_TMP_MS) it.delete() }
    }

    private fun copyToCache(src: Source, out: File, cancel: AudioCancel, onProgress: (Float) -> Unit) {
        val tmp = File(out.parentFile, "${out.name}.${System.nanoTime()}.tmp")
        try {
            val total = try {
                src.ctx.contentResolver.query(src.uri!!, arrayOf(OpenableColumns.SIZE), null, null, null)?.use { c ->
                    if (c.moveToFirst() && !c.isNull(0)) c.getLong(0) else -1L
                } ?: -1L
            } catch (_: Exception) { -1L }
            var done = 0L
            var last = 0L
            val buf = ByteArray(64 * 1024)
            src.open().use { input ->
                FileOutputStream(tmp).use { o ->
                    while (true) {
                        if (cancel.cancelled) throw CancellationException("cancelled")
                        val n = input.read(buf)
                        if (n < 0) break
                        o.write(buf, 0, n)
                        done += n
                        if (done > MAX_OUTPUT_BYTES * 2) throw IOException("file too large")
                        val now = SystemClock.elapsedRealtime()
                        if (total > 0 && now - last >= PROGRESS_MIN_MS) { last = now; onProgress((done.toFloat() / total).coerceIn(0f, 0.99f)) }
                    }
                }
            }
            synchronized(cacheLock) {
                out.delete()
                if (!tmp.renameTo(out)) throw IOException("cannot publish ${out.name}")
            }
        } finally {
            tmp.delete()
        }
    }

    private fun titleOf(src: Source): String {
        val fallback = src.displayName.substringBeforeLast('.').ifBlank { src.displayName }
        val r = MediaMetadataRetriever()
        return try {
            src.attach(r)
            r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_TITLE)?.trim()?.takeIf { it.isNotEmpty() } ?: fallback
        } catch (_: Exception) {
            fallback
        } finally {
            try { r.release() } catch (_: Exception) {}
        }
    }

    // ====================================================================================== MediaCodec -> WAV

    private class Info(val sampleRate: Int, val channels: Int, val durationSec: Double)

    private fun transcode(src: Source, dst: File, cancel: AudioCancel, onProgress: (Float) -> Unit): Info {
        val ex = MediaExtractor()
        var codec: MediaCodec? = null
        try {
            try { src.attach(ex) } catch (e: Exception) { throw IOException("unsupported or unreadable audio: ${e.message}", e) }
            var track = -1
            var fmt: MediaFormat? = null
            for (i in 0 until ex.trackCount) {
                val f = ex.getTrackFormat(i)
                if (f.getString(MediaFormat.KEY_MIME)?.startsWith("audio/") == true) { track = i; fmt = f; break }
            }
            if (track < 0 || fmt == null) throw IOException("no audio track")
            ex.selectTrack(track)
            val mime = fmt.getString(MediaFormat.KEY_MIME)!!
            val durationUs = if (fmt.containsKey(MediaFormat.KEY_DURATION)) fmt.getLong(MediaFormat.KEY_DURATION) else -1L

            var sampleRate = if (fmt.containsKey(MediaFormat.KEY_SAMPLE_RATE)) fmt.getInteger(MediaFormat.KEY_SAMPLE_RATE) else 44100
            var srcChannels = if (fmt.containsKey(MediaFormat.KEY_CHANNEL_COUNT)) fmt.getInteger(MediaFormat.KEY_CHANNEL_COUNT) else 2
            var floatPcm = false

            val c = try { MediaCodec.createDecoderByType(mime) } catch (e: Exception) { throw IOException("no decoder for $mime", e) }
            codec = c
            c.configure(fmt, null, null, 0)
            c.start()

            val info = MediaCodec.BufferInfo()
            var inputDone = false
            var outputDone = false
            var frames = 0L
            var pcmBytes = 0L
            var lastProgress = 0L
            var lastOutputAt = SystemClock.elapsedRealtime()

            var scratch = ByteArray(0)
            FileOutputStream(dst).use { fos ->
                val out = BufferedOutputStream(fos, 64 * 1024)
                out.write(ByteArray(44))                                    // header patched at the end
                while (!outputDone) {
                    if (cancel.cancelled) throw CancellationException("cancelled")

                    if (!inputDone) {
                        val ii = c.dequeueInputBuffer(DEQUEUE_US)
                        if (ii >= 0) {
                            val ib = c.getInputBuffer(ii)!!
                            val n = ex.readSampleData(ib, 0)
                            if (n < 0) {
                                c.queueInputBuffer(ii, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                                inputDone = true
                            } else {
                                c.queueInputBuffer(ii, 0, n, ex.sampleTime.coerceAtLeast(0L), 0)
                                ex.advance()
                            }
                        }
                    }

                    val oi = c.dequeueOutputBuffer(info, DEQUEUE_US)
                    when {
                        oi == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                            val of = c.outputFormat
                            lastOutputAt = SystemClock.elapsedRealtime()
                            if (of.containsKey(MediaFormat.KEY_SAMPLE_RATE)) sampleRate = of.getInteger(MediaFormat.KEY_SAMPLE_RATE)
                            if (of.containsKey(MediaFormat.KEY_CHANNEL_COUNT)) srcChannels = of.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
                            floatPcm = of.containsKey(MediaFormat.KEY_PCM_ENCODING) &&
                                of.getInteger(MediaFormat.KEY_PCM_ENCODING) == android.media.AudioFormat.ENCODING_PCM_FLOAT
                        }
                        oi >= 0 -> {
                            lastOutputAt = SystemClock.elapsedRealtime()
                            if (info.size > 0) {
                                val ob = c.getOutputBuffer(oi)!!
                                ob.position(info.offset)
                                ob.limit(info.offset + info.size)
                                val ch = srcChannels.coerceAtLeast(1)
                                val bytesPerSample = if (floatPcm) 4 else 2
                                val inFrames = info.size / (bytesPerSample * ch)
                                val need = inFrames * 4
                                if (scratch.size < need) scratch = ByteArray(need)
                                convert(ob, inFrames, ch, floatPcm, scratch)
                                out.write(scratch, 0, need)
                                frames += inFrames
                                pcmBytes += need
                                if (pcmBytes > MAX_OUTPUT_BYTES) throw IOException("decoded audio too long")
                            }
                            if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) outputDone = true
                            c.releaseOutputBuffer(oi, false)

                            val now = SystemClock.elapsedRealtime()
                            if (now - lastProgress >= PROGRESS_MIN_MS) {
                                lastProgress = now
                                val p = if (durationUs > 0) (info.presentationTimeUs.toFloat() / durationUs) else -1f
                                if (p >= 0f) onProgress(p.coerceIn(0f, 0.99f))
                            }
                        }
                        oi == MediaCodec.INFO_TRY_AGAIN_LATER -> {
                            if (SystemClock.elapsedRealtime() - lastOutputAt > STALL_LIMIT_MS)
                                throw IOException("decoder stalled")
                        }
                    }
                }
                out.flush()
            }
            if (frames == 0L) throw IOException("decoder produced no audio")
            patchHeader(dst, sampleRate, frames)
            return Info(sampleRate, 2, frames.toDouble() / sampleRate)
        } finally {
            try { codec?.stop() } catch (_: Exception) {}
            try { codec?.release() } catch (_: Exception) {}
            try { ex.release() } catch (_: Exception) {}
        }
    }

    /** [inFrames] frames of [ch]-channel PCM16 / float32 (native byte order) -> stereo PCM16 little endian in [dst]. */
    private fun convert(src: ByteBuffer, inFrames: Int, ch: Int, float: Boolean, dst: ByteArray) {
        src.order(ByteOrder.nativeOrder())
        var o = 0
        if (float) {
            val fb = src.asFloatBuffer()
            for (f in 0 until inFrames) {
                val base = f * ch
                val l = fb.get(base)
                val r = if (ch >= 2) fb.get(base + 1) else l
                o = putS16(dst, o, floatToS16(l))
                o = putS16(dst, o, floatToS16(r))
            }
        } else {
            val sb = src.asShortBuffer()
            for (f in 0 until inFrames) {
                val base = f * ch
                val l = sb.get(base).toInt()
                val r = if (ch >= 2) sb.get(base + 1).toInt() else l
                o = putS16(dst, o, l)
                o = putS16(dst, o, r)
            }
        }
    }

    private fun floatToS16(v: Float): Int {
        val x = if (v > 1f) 1f else if (v < -1f) -1f else v
        return Math.round(x * 32767f)
    }

    private fun putS16(d: ByteArray, at: Int, v: Int): Int {
        d[at] = v.toByte()
        d[at + 1] = (v shr 8).toByte()
        return at + 2
    }

    private fun patchHeader(f: File, sampleRate: Int, frames: Long) {
        val dataBytes = frames * 4
        val h = ByteBuffer.allocate(44).order(ByteOrder.LITTLE_ENDIAN)
        h.put("RIFF".toByteArray(Charsets.US_ASCII)).putInt((36 + dataBytes).toInt())
        h.put("WAVE".toByteArray(Charsets.US_ASCII))
        h.put("fmt ".toByteArray(Charsets.US_ASCII)).putInt(16).putShort(1).putShort(2)
            .putInt(sampleRate).putInt(sampleRate * 4).putShort(4).putShort(16)
        h.put("data".toByteArray(Charsets.US_ASCII)).putInt(dataBytes.toInt())
        RandomAccessFile(f, "rw").use { it.seek(0); it.write(h.array()) }
    }

    // ====================================================================================== header readers (no decoding)

    /** WAV `fmt ` + `data` sizes. Null for flavours the engine cannot read (ADPCM, ...). */
    private fun readWavInfo(f: File): Info? = try {
        RandomAccessFile(f, "r").use { raf ->
            val head = ByteArray(12)
            raf.readFully(head)
            var pos = 12L
            val len = raf.length()
            var sr = 0
            var ch = 0
            var bits = 0
            var tag = 0
            var data = -1L
            val chunk = ByteArray(8)
            while (pos + 8 <= len) {
                raf.seek(pos)
                raf.readFully(chunk)
                val id = String(chunk, 0, 4, Charsets.US_ASCII)
                val size = ByteBuffer.wrap(chunk, 4, 4).order(ByteOrder.LITTLE_ENDIAN).int.toLong() and 0xFFFFFFFFL
                if (id == "fmt " && size >= 16) {
                    val b = ByteArray(16)
                    raf.readFully(b)
                    val bb = ByteBuffer.wrap(b).order(ByteOrder.LITTLE_ENDIAN)
                    tag = bb.short.toInt() and 0xFFFF
                    ch = bb.short.toInt() and 0xFFFF
                    sr = bb.int
                    bb.int; bb.short
                    bits = bb.short.toInt() and 0xFFFF
                } else if (id == "data") {
                    data = minOf(size, len - pos - 8)
                    break
                }
                pos += 8 + size + (size and 1L)
            }
            if (sr <= 0 || ch <= 0 || bits <= 0 || data < 0) return null
            if (tag != 1 && tag != 3 && tag != 0xFFFE) return null
            Info(sr, ch, data.toDouble() / (ch * (bits / 8.0)) / sr)
        }
    } catch (_: Exception) { null }

    /** FLAC STREAMINFO: sample rate (20 bit), channels (3 bit), total samples (36 bit). */
    private fun readFlacInfo(f: File): Info? = try {
        RandomAccessFile(f, "r").use { raf ->
            val b = ByteArray(8 + 34)
            raf.readFully(b)
            if (String(b, 0, 4, Charsets.US_ASCII) != "fLaC" || (b[4].toInt() and 0x7F) != 0) return null
            val v = ((b[18].toLong() and 0xFF) shl 56) or ((b[19].toLong() and 0xFF) shl 48) or ((b[20].toLong() and 0xFF) shl 40) or
                ((b[21].toLong() and 0xFF) shl 32) or ((b[22].toLong() and 0xFF) shl 24) or ((b[23].toLong() and 0xFF) shl 16) or
                ((b[24].toLong() and 0xFF) shl 8) or (b[25].toLong() and 0xFF)
            val sr = ((v ushr 44) and 0xFFFFF).toInt()
            val ch = (((v ushr 41) and 0x7).toInt()) + 1
            val total = v and 0xFFFFFFFFFL
            if (sr <= 0) return null
            Info(sr, ch, total.toDouble() / sr)
        }
    } catch (_: Exception) { null }
}
