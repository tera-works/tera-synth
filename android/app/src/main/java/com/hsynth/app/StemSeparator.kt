package com.hsynth.app

import ai.onnxruntime.OnnxJavaType
import ai.onnxruntime.OnnxTensor
import ai.onnxruntime.OrtEnvironment
import ai.onnxruntime.OrtException
import ai.onnxruntime.OrtSession
import ai.onnxruntime.TensorInfo
import ai.onnxruntime.providers.NNAPIFlags
import android.content.Context
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.os.SystemClock
import android.util.Half
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.io.RandomAccessFile
import java.net.HttpURLConnection
import java.net.URL
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer
import java.nio.channels.FileChannel
import java.security.MessageDigest
import java.util.Collections
import java.util.EnumSet
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

// ---- native bridges injected by the caller (Wave 2 wires them to NativeEngine; this file has no JNI dependency) ----

/**
 * `(wavPath, magOutPath, maxFrames) -> json` (maxFrames <= 0 = whole file). Writes the STFT magnitude of [wavPath] to [magOutPath] as little-endian
 * float32, layout `[frame][bin][channel]` (channel innermost, `channels = 2`, `bins = 1024`), and returns
 * `{"ok":true,"frames":N,"bins":1024,"channels":2}`. Anything with `"error"` / `"ok":false` is a failure.
 */
typealias StftExtractFn = (wavPath: String, magPath: String, maxFrames: Int) -> String

/**
 * `(wavPath, maskPaths, outDir) -> json | ""`. [maskPaths] = `mask_{vocals,drums,bass,other}.bin` in the same layout as
 * the magnitude file (one predicted stem magnitude per file). The engine builds the Wiener soft masks, runs the ISTFT
 * against [wavPath] and writes `vocals.wav drums.wav bass.wav other.wav` into [outDir]. Empty string / `{"ok":true}` = ok.
 */
typealias StftReconstructFn = (wavPath: String, maskPaths: Array<String>, outDir: String) -> String

/** `(wavPath, outDir) -> json | ""`. Model-free spectral 4-stem split (~2 s); writes the same 4 WAVs into [outDir]. */
typealias FastSeparateFn = (wavPath: String, outDir: String) -> String

/**
 * Outcome of [StemSeparator.separate]. [stems] is keyed by `vocals / drums / bass / other` (all 4 present when [ok]).
 * [engineUsed] = "Edge TPU (ONNX NNAPI)" | "ONNX CPU" | "Fast Spectral DSP".
 */
data class StemResult(
    val ok: Boolean,
    val stems: Map<String, File> = emptyMap(),
    val engineUsed: String = "",
    val fromCache: Boolean = false,
    val elapsedMs: Long = 0,
    val error: String = "",
) {
    val cancelled: Boolean get() = !ok && error == StemSeparator.CANCELLED
}

/**
 * Phase 5 stem separation: Spleeter 4-stem (FP16 ONNX on the Edge TPU through NNAPI, CPU fallback) with an instant
 * model-free spectral fallback, both writing WAVs into a content-addressed cache.
 *
 * Threading: everything blocking here must run OFF the UI thread (a guard refuses otherwise). Use [executor]
 * (`hsynth-npu`, background priority) via [separateAsync] / [downloadSpleeterModelsAsync], or `a.loader`.
 * Cancellation: flip the [AtomicBoolean]; it is polled between audio chunks / network reads (native STFT calls
 * cannot be interrupted, they are short).
 */
object StemSeparator {
    val STEMS = listOf("vocals", "drums", "bass", "other")
    const val CANCELLED = "cancelled"
    const val ENGINE_NPU = "Edge TPU (ONNX NNAPI)"
    const val ENGINE_CPU = "ONNX CPU"
    const val ENGINE_FAST = "Fast Spectral DSP"

    private const val MODEL_URL = "https://huggingface.co/Best-Practice/spleeter-4stems-onnx/resolve/main/%s.fp16.onnx"
    private const val MIN_MODEL_BYTES = 1L shl 20
    private const val T = 512          // frames per network window
    private const val F = 1024         // bins used by Spleeter
    private const val C = 2            // channels

    class Cancelled : RuntimeException(CANCELLED)

    /** Single background worker for NPU / download jobs (background priority: never competes with the audio threads). */
    val executor: ExecutorService = Executors.newSingleThreadExecutor { r ->
        Thread({
            Process.setThreadPriority(Process.THREAD_PRIORITY_BACKGROUND)
            r.run()
        }, "hsynth-npu")
    }

    private val main = Handler(Looper.getMainLooper())

    // ------------------------------------------------------------------ models

    fun modelDir(ctx: Context) = File(ctx.filesDir, "models/spleeter4")

    fun modelFile(ctx: Context, stem: String) = File(modelDir(ctx), "$stem.fp16.onnx")

    fun hasSpleeterModels(ctx: Context): Boolean =
        STEMS.all { modelFile(ctx, it).let { f -> f.isFile && f.length() > MIN_MODEL_BYTES } }

    /** Blocking download of the 4 FP16 models (skips the ones already present). Never call on the UI thread. */
    fun downloadSpleeterModels(ctx: Context, cancel: AtomicBoolean, onProgress: (Float, String) -> Unit): Result<Unit> = runCatching {
        check(Looper.myLooper() != Looper.getMainLooper()) { "downloadSpleeterModels must not run on the UI thread" }
        val dir = modelDir(ctx)
        if (!dir.isDirectory && !dir.mkdirs()) throw IOException("cannot create ${dir.path}")
        val prog = Throttled(onProgress)
        for ((i, stem) in STEMS.withIndex()) {
            val dest = modelFile(ctx, stem)
            if (dest.isFile && dest.length() > MIN_MODEL_BYTES) {
                prog.update((i + 1) / STEMS.size.toFloat(), "Model $stem ready")
                continue
            }
            val tmp = File(dir, "${dest.name}.tmp")
            download(String.format(MODEL_URL, stem), tmp, cancel) { f ->
                prog.update((i + f) / STEMS.size.toFloat(), "Downloading $stem model")
            }
            if (tmp.length() <= MIN_MODEL_BYTES) {
                tmp.delete()
                throw IOException("$stem model download is incomplete")
            }
            if (dest.exists()) dest.delete()
            if (!tmp.renameTo(dest)) throw IOException("cannot move ${tmp.name} into place")
        }
        prog.update(1f, "Spleeter models ready", force = true)
    }

    private fun download(url: String, tmp: File, cancel: AtomicBoolean, onFrac: (Float) -> Unit) {
        val conn = URL(url).openConnection() as HttpURLConnection
        try {
            conn.connectTimeout = 15_000
            conn.readTimeout = 30_000
            conn.instanceFollowRedirects = true
            conn.setRequestProperty("User-Agent", "HSynth-Android")
            val code = conn.responseCode
            if (code != HttpURLConnection.HTTP_OK) throw IOException("HTTP $code")
            val total = conn.contentLengthLong
            val buf = ByteArray(32 * 1024)
            var got = 0L
            var lastFrac = -1f
            conn.inputStream.use { ins ->
                FileOutputStream(tmp).use { out ->
                    while (true) {
                        if (cancel.get()) throw Cancelled()
                        val n = ins.read(buf)
                        if (n < 0) break
                        out.write(buf, 0, n)
                        got += n
                        if (total > 0) {
                            val f = (got.toDouble() / total).toFloat().coerceIn(0f, 1f)
                            if (f - lastFrac >= 0.005f) { lastFrac = f; onFrac(f) }
                        }
                    }
                    out.fd.sync()
                }
            }
            if (total > 0 && got != total) throw IOException("truncated download ($got of $total bytes)")
            onFrac(1f)
        } catch (t: Throwable) {
            tmp.delete()
            throw t
        } finally {
            conn.disconnect()
        }
    }

    // ------------------------------------------------------------------ async wrappers (callbacks on the UI thread)

    fun separateAsync(
        ctx: Context, wavFile: File, useNeuralIfAvailable: Boolean,
        stftExtract: StftExtractFn, stftReconstruct: StftReconstructFn, fastSeparate: FastSeparateFn,
        cancel: AtomicBoolean, onProgress: (Float, String) -> Unit, onDone: (StemResult) -> Unit,
    ) {
        val app = ctx.applicationContext
        executor.execute {
            val r = separate(app, wavFile, useNeuralIfAvailable, stftExtract, stftReconstruct, fastSeparate, cancel) { f, s ->
                main.post { onProgress(f, s) }
            }
            main.post { onDone(r) }
        }
    }

    /** [separateAsync] wired straight to the JNI STFT / reconstruct / fast-spectral calls. */
    fun separateNativeAsync(
        ctx: Context, wavFile: File, useNeuralIfAvailable: Boolean,
        cancel: AtomicBoolean, onProgress: (Float, String) -> Unit, onDone: (StemResult) -> Unit,
    ) = separateAsync(
        ctx, wavFile, useNeuralIfAvailable,
        { w, m, n -> runCatching { NativeEngine.nativeStftExtract(w, m, n) }.getOrElse { "{\"ok\":false,\"error\":\"${it.message}\"}" } },
        { w, masks, d -> runCatching { NativeEngine.nativeStftReconstruct(w, masks, d) }.getOrElse { "{\"ok\":false,\"error\":\"${it.message}\"}" } },
        { w, d -> runCatching { NativeEngine.nativeSeparateStemsFast(w, d) }.getOrElse { "{\"ok\":false,\"error\":\"${it.message}\"}" } },
        cancel, onProgress, onDone,
    )

    fun downloadSpleeterModelsAsync(ctx: Context, cancel: AtomicBoolean, onProgress: (Float, String) -> Unit, onDone: (Result<Unit>) -> Unit) {
        val app = ctx.applicationContext
        executor.execute {
            val r = downloadSpleeterModels(app, cancel) { f, s -> main.post { onProgress(f, s) } }
            main.post { onDone(r) }
        }
    }

    // ------------------------------------------------------------------ separation

    /**
     * Blocking 4-stem separation of [wavFile]. Order: stem cache → Spleeter on ONNX (when [useNeuralIfAvailable] and the
     * models are downloaded) → fast spectral DSP. Results are cached in `cacheDir/stems/<sha256 prefix>_<neural|fast>/`
     * and written atomically (work dir renamed on success), so a cancelled / failed run never leaves a half cache.
     */
    fun separate(
        ctx: Context, wavFile: File, useNeuralIfAvailable: Boolean,
        stftExtract: StftExtractFn, stftReconstruct: StftReconstructFn, fastSeparate: FastSeparateFn,
        cancel: AtomicBoolean, onProgress: (Float, String) -> Unit,
    ): StemResult {
        val t0 = SystemClock.elapsedRealtime()
        fun fail(msg: String) = StemResult(false, error = msg, elapsedMs = SystemClock.elapsedRealtime() - t0)
        if (Looper.myLooper() == Looper.getMainLooper()) return fail("StemSeparator.separate must not run on the UI thread")
        if (!wavFile.isFile) return fail("audio file not found: ${wavFile.name}")

        val prog = Throttled(onProgress)
        var work: File? = null
        try {
            prog.update(0f, "Fingerprinting audio")
            val key = fingerprint(wavFile, cancel)
            val root = File(ctx.cacheDir, "stems").apply { mkdirs() }
            val neuralDir = File(root, "${key}_neural")
            val fastDir = File(root, "${key}_fast")

            // 1) cache
            val neural = useNeuralIfAvailable && hasSpleeterModels(ctx)
            if (useNeuralIfAvailable) cached(neuralDir)?.let { return hit(it, neuralDir, ENGINE_NPU, t0, prog) }
            if (!neural) cached(fastDir)?.let { return hit(it, fastDir, ENGINE_FAST, t0, prog) }

            val finalDir = if (neural) neuralDir else fastDir
            val w = File(root, finalDir.name + ".work").also { it.deleteRecursively(); it.mkdirs() }
            work = w

            // 2) neural or fast
            val engine = if (neural) runNeural(ctx, wavFile, w, stftExtract, stftReconstruct, cancel, prog)
            else {
                prog.update(0.10f, "Fast spectral separation", force = true)
                nativeOk("fastSeparate", fastSeparate(wavFile.absolutePath, w.absolutePath))
                ENGINE_FAST
            }
            if (cancel.get()) throw Cancelled()
            if (STEMS.any { !validWav(File(w, "$it.wav")) }) return fail("engine did not produce all 4 stems")

            // 3) publish atomically
            w.listFiles()?.forEach { if (it.extension.equals("bin", true)) it.delete() }
            File(w, "engine.txt").writeText(engine)
            finalDir.deleteRecursively()
            if (!w.renameTo(finalDir)) return fail("cannot publish stems into ${finalDir.name}")
            work = null
            prog.update(1f, "Done", force = true)
            return StemResult(true, stemMap(finalDir), engine, false, SystemClock.elapsedRealtime() - t0)
        } catch (_: Cancelled) {
            return fail(CANCELLED)
        } catch (t: Throwable) {
            return fail(t.message ?: t.javaClass.simpleName)
        } finally {
            work?.deleteRecursively()
        }
    }

    fun clearStemCache(ctx: Context) { File(ctx.cacheDir, "stems").deleteRecursively() }

    fun stemCacheBytes(ctx: Context): Long =
        File(ctx.cacheDir, "stems").walkTopDown().filter { it.isFile }.sumOf { it.length() }

    private fun hit(stems: Map<String, File>, dir: File, fallbackEngine: String, t0: Long, prog: Throttled): StemResult {
        val engine = runCatching { File(dir, "engine.txt").readText().trim() }.getOrNull()?.ifEmpty { null } ?: fallbackEngine
        prog.update(1f, "Loaded from cache", force = true)
        return StemResult(true, stems, engine, true, SystemClock.elapsedRealtime() - t0)
    }

    private fun stemMap(dir: File) = STEMS.associateWith { File(dir, "$it.wav") }

    private fun validWav(f: File) = f.isFile && f.length() > 44

    private fun cached(dir: File): Map<String, File>? =
        if (dir.isDirectory && STEMS.all { validWav(File(dir, "$it.wav")) }) stemMap(dir) else null

    /** SHA-256 over length + first 4 MiB + last 1 MiB (fast on 100 MB files), first 16 hex chars. */
    private fun fingerprint(f: File, cancel: AtomicBoolean): String {
        val md = MessageDigest.getInstance("SHA-256")
        val len = f.length()
        md.update(ByteBuffer.allocate(8).putLong(len).array())
        RandomAccessFile(f, "r").use { raf ->
            val buf = ByteArray(64 * 1024)
            fun feed(from: Long, to: Long) {
                raf.seek(from)
                var left = to - from
                while (left > 0) {
                    if (cancel.get()) throw Cancelled()
                    val n = raf.read(buf, 0, minOf(buf.size.toLong(), left).toInt())
                    if (n <= 0) break
                    md.update(buf, 0, n)
                    left -= n
                }
            }
            val head = 4L shl 20
            val tail = 1L shl 20
            if (len <= head + tail) feed(0, len) else { feed(0, head); feed(len - tail, len) }
        }
        val d = md.digest()
        val sb = StringBuilder(16)
        for (i in 0 until 8) sb.append(Character.forDigit((d[i].toInt() shr 4) and 15, 16)).append(Character.forDigit(d[i].toInt() and 15, 16))
        return sb.toString()
    }

    // ------------------------------------------------------------------ neural path

    private fun runNeural(
        ctx: Context, wav: File, work: File,
        stftExtract: StftExtractFn, stftReconstruct: StftReconstructFn, cancel: AtomicBoolean, prog: Throttled,
    ): String {
        val mag = File(work, "mag.bin")
        prog.update(0.03f, "STFT analysis", force = true)
        val info = nativeOk("stftExtract", stftExtract(wav.absolutePath, mag.absolutePath, 0))
        val frames = info.optInt("frames", 0)
        val bins = info.optInt("bins", F)
        val channels = info.optInt("channels", C)
        if (frames <= 0 || bins != F || channels != C) throw IOException("unexpected STFT shape frames=$frames bins=$bins channels=$channels")
        if (mag.length() < frames.toLong() * F * C * 4L) throw IOException("magnitude file is smaller than the STFT shape")
        if (cancel.get()) throw Cancelled()

        val env = OrtEnvironment.getEnvironment()
        val masks = Array(STEMS.size) { File(work, "mask_${STEMS[it]}.bin") }
        var anyNnapi = false
        RandomAccessFile(mag, "r").use { raf ->
            // packed LE float32, [frame][bin][channel]
            val magBuf = raf.channel.map(FileChannel.MapMode.READ_ONLY, 0, raf.length()).order(ByteOrder.LITTLE_ENDIAN).asFloatBuffer()
            val span = 0.75f / STEMS.size
            for ((i, stem) in STEMS.withIndex()) {
                val base = 0.08f + span * i
                val nn = inferStem(env, modelFile(ctx, stem), magBuf, frames, masks[i], cancel) { f ->
                    prog.update(base + span * f, "Neural separation · $stem")
                }
                anyNnapi = anyNnapi || nn
            }
        }
        if (cancel.get()) throw Cancelled()
        prog.update(0.87f, "Wiener soft-mask resynthesis", force = true)
        nativeOk("stftReconstruct", stftReconstruct(wav.absolutePath, Array(masks.size) { masks[it].absolutePath }, work.absolutePath))
        return if (anyNnapi) ENGINE_NPU else ENGINE_CPU
    }

    /** Tensor axis roles of a rank-4 Spleeter input / output. */
    private class Layout(val axC: Int, val axB: Int, val axT: Int, val axF: Int)

    /** Finds channel / batch / time / freq axes from a (possibly dynamic) shape: `[2,B,512,1024]` or `[B,512,1024,2]`. */
    private fun resolveLayout(shape: LongArray): Layout? {
        if (shape.size != 4) return null
        val axF = shape.indexOfFirst { it == F.toLong() }
        val axT = shape.indices.firstOrNull { it != axF && shape[it] == T.toLong() } ?: -1
        if (axF < 0 || axT < 0) return null
        val rest = shape.indices.filter { it != axF && it != axT }
        val axC = rest.firstOrNull { shape[it] == C.toLong() && (it == 0 || it == 3) }
            ?: if (3 in rest) 3 else rest.minOrNull()!!
        val axB = rest.first { it != axC }
        return Layout(axC, axB, axT, axF)
    }

    /** Concrete dims for [lay] (dynamic batch → [batch]) and the row-major strides of them. */
    private fun dimsOf(shape: LongArray, lay: Layout, batch: Int): IntArray {
        val d = IntArray(4)
        d[lay.axC] = C
        d[lay.axB] = if (shape[lay.axB] > 0) shape[lay.axB].toInt() else batch
        d[lay.axT] = T
        d[lay.axF] = F
        return d
    }

    private fun stridesOf(d: IntArray) = IntArray(4).also {
        var s = 1
        for (i in 3 downTo 0) { it[i] = s; s *= d[i] }
    }

    /**
     * Runs one stem model over the whole spectrogram (512-frame windows, zero padded at the end) and writes the
     * predicted magnitude to [maskFile] (same packed `[frame][bin][channel]` float32 LE layout as the STFT file). The session is closed on
     * return so peak RAM stays at one model. @return true when NNAPI (Edge TPU) executed the model.
     */
    private fun inferStem(
        env: OrtEnvironment, model: File, mag: FloatBuffer, frames: Int, maskFile: File,
        cancel: AtomicBoolean, onFrac: (Float) -> Unit,
    ): Boolean {
        val (session, nnapi) = openSession(env, model.absolutePath)
        session.use {
            val inName = session.inputNames.first()
            val tinfo = session.inputInfo[inName]?.info as? TensorInfo ?: throw IOException("${model.name}: input is not a tensor")
            val inShape = tinfo.shape
            val lay = resolveLayout(inShape) ?: throw IOException("${model.name}: unsupported input shape ${inShape.contentToString()}")
            val batch = if (inShape[lay.axB] > 0) inShape[lay.axB].toInt() else 1
            val dims = dimsOf(inShape, lay, batch)
            val st = stridesOf(dims)
            val sB = st[lay.axB]; val sC = st[lay.axC]; val sT = st[lay.axT]; val sF = st[lay.axF]
            val fp16 = when (tinfo.type) {
                OnnxJavaType.FLOAT -> false
                OnnxJavaType.FLOAT16 -> true
                else -> throw IOException("${model.name}: unsupported input type ${tinfo.type}")
            }
            val shapeL = LongArray(4) { dims[it].toLong() }
            val elems = batch * C * T * F
            val bytes = ByteBuffer.allocateDirect(elems * (if (fp16) 2 else 4)).order(ByteOrder.nativeOrder())
            val inF = bytes.asFloatBuffer()
            val inH = bytes.asShortBuffer()

            val totalChunks = (frames + T - 1) / T
            val outBytes = ByteBuffer.allocate(T * F * C * 4).order(ByteOrder.LITTLE_ENDIAN)
            val outF = outBytes.asFloatBuffer()
            RandomAccessFile(maskFile, "rw").use { out ->
                out.setLength(frames.toLong() * F * C * 4L)
                val chan = out.channel
                var chunk = 0
                while (chunk < totalChunks) {
                    if (cancel.get()) throw Cancelled()
                    // ---- fill [batch] windows
                    for (b in 0 until batch) {
                        val start = (chunk + b) * T
                        for (c in 0 until C) for (t in 0 until T) {
                            val fr = start + t
                            val rowBase = fr * F * C + c
                            val o = b * sB + c * sC + t * sT
                            if (fr < frames) {
                                if (fp16) for (f in 0 until F) inH.put(o + f * sF, Half.toHalf(mag.get(rowBase + f * C)))
                                else for (f in 0 until F) inF.put(o + f * sF, mag.get(rowBase + f * C))
                            } else {
                                if (fp16) for (f in 0 until F) inH.put(o + f * sF, 0)
                                else for (f in 0 until F) inF.put(o + f * sF, 0f)
                            }
                        }
                    }
                    // ---- run
                    inF.rewind(); bytes.rewind()
                    val tensor = if (fp16) OnnxTensor.createTensor(env, bytes, shapeL, OnnxJavaType.FLOAT16)
                    else OnnxTensor.createTensor(env, inF, shapeL)
                    tensor.use { tin ->
                        session.run(Collections.singletonMap(inName, tin)).use { res ->
                            val ot = res[0] as? OnnxTensor ?: throw IOException("${model.name}: output is not a tensor")
                            val oShape = (ot.info as TensorInfo).shape
                            if (oShape.size != 4) throw IOException("${model.name}: unexpected output shape ${oShape.contentToString()}")
                            val oLay = resolveLayout(oShape) ?: lay
                            val oDims = dimsOf(oShape, oLay, batch)
                            val oSt = stridesOf(oDims)
                            val ob = ot.floatBuffer
                            if (ob.remaining().toLong() < oDims[0].toLong() * oDims[1] * oDims[2] * oDims[3])
                                throw IOException("${model.name}: unexpected output shape ${oShape.contentToString()}")
                            val oB = minOf(batch, oDims[oLay.axB])
                            for (b in 0 until oB) {
                                val start = (chunk + b) * T
                                if (start >= frames) break
                                val n = minOf(T, frames - start)
                                for (c in 0 until C) {
                                    for (t in 0 until n) {
                                        val o = b * oSt[oLay.axB] + c * oSt[oLay.axC] + t * oSt[oLay.axT]
                                        val dst = t * F * C + c
                                        for (f in 0 until F) {
                                            val v = ob.get(o + f * oSt[oLay.axF])
                                            outF.put(dst + f * C, if (v > 0f) v else 0f)   // also maps NaN to 0
                                        }
                                    }
                                }
                                outBytes.position(0).limit(n * F * C * 4)
                                chan.write(outBytes, start.toLong() * F * C * 4L)
                                outBytes.clear()
                            }
                        }
                    }
                    chunk += batch
                    onFrac(minOf(chunk, totalChunks).toFloat() / totalChunks)
                }
            }
        }
        return nnapi
    }

    /** NNAPI (FP16) first; if the provider or the model rejects it, retry on plain CPU. */
    private fun openSession(env: OrtEnvironment, path: String): Pair<OrtSession, Boolean> {
        var last: OrtException? = null
        for (useNnapi in booleanArrayOf(true, false)) {
            try {
                OrtSession.SessionOptions().use { o ->
                    o.setIntraOpNumThreads(2)
                    if (useNnapi) o.addNnapi(EnumSet.of(NNAPIFlags.USE_FP16))
                    return env.createSession(path, o) to useNnapi
                }
            } catch (e: OrtException) {
                last = e
            }
        }
        throw last ?: IOException("cannot open ONNX session")
    }

    // ------------------------------------------------------------------ helpers

    /** Accepts "" / {"ok":true,…} as success; JSON with "error" / "ok":false or any other text as failure. */
    private fun nativeOk(what: String, raw: String?): JSONObject {
        val s = raw?.trim().orEmpty()
        if (s.isEmpty()) return JSONObject().put("ok", true)
        if (!s.startsWith("{")) throw IOException("$what: $s")
        val o = JSONObject(s)
        val err = o.optString("error", "")
        if (!o.optBoolean("ok", true) || err.isNotEmpty()) throw IOException("$what: ${err.ifEmpty { "failed" }}")
        return o
    }

    /** Progress limiter: ≤ ~10 callbacks / s, and only on a visible (≥ 0.5 %) step, unless forced. */
    private class Throttled(private val cb: (Float, String) -> Unit) {
        private var lastF = -1f
        private var lastT = 0L
        private var lastMsg = ""
        fun update(f: Float, msg: String, force: Boolean = false) {
            val now = SystemClock.elapsedRealtime()
            val fc = f.coerceIn(0f, 1f)
            if (!force && msg == lastMsg && (fc - lastF < 0.005f || now - lastT < 100L)) return
            lastF = fc; lastT = now; lastMsg = msg
            cb(fc, msg)
        }
    }
}
