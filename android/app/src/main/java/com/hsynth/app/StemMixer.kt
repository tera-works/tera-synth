package com.hsynth.app

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.LinearLayout
import android.widget.TextView
import org.json.JSONObject
import java.io.File
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.math.roundToInt

/*
 * Phase 5 — "✦ STEMS (NPU)" block of SONG > AUDIO.
 *
 *   idle ──[✦ STEMS (NPU)]──► models present?  yes ─► separate (ONNX / NNAPI, progress + CANCEL)
 *                                              no  ─► inline choice card:
 *                                                      · Download Spleeter 4-Stems FP16 (~40 MB) → download → separate
 *                                                      · Fast Spectral (instant, offline)       → separate
 *   done ──► nativeAudioLoadStems(vocals, drums, bass, other) ──► 4-stem mixer strip + 1-tap preset chips
 *
 * All heavy work runs on StemSeparator's background executor / `a.loader`; callbacks land on the UI thread.
 */
class StemPanel(
    private val a: MainActivity,
    /** The decoded WAV of the loaded backing track, or null when nothing is loaded. */
    private val currentWav: () -> File?,
    /** Called on the UI thread after the engine switched to the stem sources (re-push loop markers etc.). */
    private val onStemsLoaded: () -> Unit,
) {
    val view: LinearLayout
    private val d = { v: Int -> Ui.dp(a, v) }

    private enum class State { IDLE, CHOICE, DOWNLOAD, SEPARATE, LOADING, READY }

    private var state = State.IDLE
    private var stemsActive = false   // the engine currently plays the 4 stems of the loaded track
    private var cancel = AtomicBoolean(false)
    private var token = 0

    private val names = arrayOf("VOCALS", "DRUMS", "BASS", "KEYS")
    private val gain = FloatArray(4) { UNITY }
    private val mute = BooleanArray(4)
    private val solo = BooleanArray(4)

    private val stemsBtn: LedButton
    private val cancelBtn: LedButton
    private val statusTv = Ui.text(a, "", 11.5f, Hw.ENGRAVE, tr = false).apply { typeface = Hw.MONO; maxLines = 2 }
    private val bar = StemProgressView(a)
    private val choiceBox: LinearLayout
    private val mixerBox: LinearLayout
    private val chipAll: LedButton
    private val chipMinusOne: LedButton
    private val chipKaraoke: LedButton
    private val chipDrumsBass: LedButton
    private val bars = arrayOfNulls<HSegBar>(4)
    private val muteBtns = arrayOfNulls<LedButton>(4)
    private val soloBtns = arrayOfNulls<LedButton>(4)

    init {
        stemsBtn = LedButton(a, L10n.t("✦ STEMS (NPU)"), tr = false, led = false, accentColor = Hw.MOD_FX).apply {
            textSize = 13f; minHeight = 0; minimumHeight = 0; setPadding(d(12), 0, d(12), 0)
            setOnClickListener { onPressed() }
        }
        cancelBtn = LedButton(a, L10n.t("CANCEL"), tr = false, led = false, accentColor = Ui.ERR).apply {
            textSize = 12f; minHeight = 0; minimumHeight = 0; setPadding(d(10), 0, d(10), 0)
            visibility = View.GONE
            setOnClickListener { cancelJob() }
        }
        val top = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(stemsBtn, LinearLayout.LayoutParams(d(150), d(40)))
            addView(statusTv, LinearLayout.LayoutParams(0, -2, 1f).apply { marginStart = d(10); marginEnd = d(8) })
            addView(cancelBtn, LinearLayout.LayoutParams(-2, d(34)))
        }

        choiceBox = buildChoice()
        chipAll = chip("ALL") { preset(Preset.ALL) }
        chipMinusOne = chip("MINUS-ONE (NO KEYS)") { preset(Preset.MINUS_ONE) }
        chipKaraoke = chip("KARAOKE (NO VOX)") { preset(Preset.KARAOKE) }
        chipDrumsBass = chip("DRUMS+BASS") { preset(Preset.DRUMS_BASS) }
        mixerBox = buildMixer()
        choiceBox.visibility = View.GONE
        mixerBox.visibility = View.GONE
        bar.visibility = View.GONE

        view = LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            addView(top, LinearLayout.LayoutParams(-1, -2))
            addView(bar, LinearLayout.LayoutParams(-1, d(6)).apply { topMargin = d(6) })
            addView(choiceBox, LinearLayout.LayoutParams(-1, -2).apply { topMargin = d(6) })
            addView(mixerBox, LinearLayout.LayoutParams(-1, -2).apply { topMargin = d(6) })
        }
        statusTv.text = L10n.t("Split the backing track into vocals / drums / bass / keys")
    }

    // ------------------------------------------------------------------ public

    /** A different track was selected (or the queue emptied): drop the stems UI and stop any running job. */
    fun reset() {
        token++
        cancel.set(true)
        stemsActive = false
        setState(State.IDLE)
        statusTv.text = L10n.t("Split the backing track into vocals / drums / bass / keys")
        resetMix()
    }

    // ------------------------------------------------------------------ flow

    private fun onPressed() {
        when (state) {
            State.DOWNLOAD, State.SEPARATE, State.LOADING -> return
            State.CHOICE -> { setState(State.IDLE); return }
            else -> {}
        }
        if (currentWav() == null) {
            statusTv.text = L10n.t("Load a backing track first (OPEN…)")
            return
        }
        if (StemSeparator.hasSpleeterModels(a)) separate(neural = true) else setState(State.CHOICE)
    }

    private fun setState(s: State) {
        state = s
        if (s == State.READY) stemsActive = true
        val busy = s == State.DOWNLOAD || s == State.SEPARATE || s == State.LOADING
        choiceBox.visibility = if (s == State.CHOICE) View.VISIBLE else View.GONE
        mixerBox.visibility = if (stemsActive && !busy) View.VISIBLE else View.GONE
        bar.visibility = if (s == State.DOWNLOAD || s == State.SEPARATE) View.VISIBLE else View.GONE
        cancelBtn.visibility = if (s == State.DOWNLOAD || s == State.SEPARATE) View.VISIBLE else View.GONE
        stemsBtn.isEnabled = !busy
        stemsBtn.lit = stemsActive || s == State.CHOICE
    }

    private fun cancelJob() {
        cancel.set(true)
        statusTv.text = L10n.t("cancelling…")
    }

    private fun download() {
        val tok = ++token
        cancel = AtomicBoolean(false)
        setState(State.DOWNLOAD)
        bar.fraction = 0f
        statusTv.text = L10n.t("Downloading Spleeter models…")
        StemSeparator.downloadSpleeterModelsAsync(a, cancel,
            onProgress = { f, msg -> if (tok == token) { bar.fraction = f; statusTv.text = "$msg  ${(f * 100).roundToInt()}%" } },
            onDone = { r ->
                if (tok != token || a.isDestroyed) return@downloadSpleeterModelsAsync
                if (r.isSuccess && StemSeparator.hasSpleeterModels(a)) {
                    a.log("Spleeter 4-stem models downloaded")
                    separate(neural = true)
                } else {
                    val cancelled = r.exceptionOrNull() is StemSeparator.Cancelled || cancel.get()
                    statusTv.text = if (cancelled) L10n.t("cancelled")
                    else L10n.f("Download failed: %s", r.exceptionOrNull()?.message ?: "?")
                    if (!cancelled) a.log("Spleeter download failed: ${r.exceptionOrNull()?.message}", Ui.ERR)
                    setState(State.CHOICE)   // offer the fast spectral path
                }
            })
    }

    private fun separate(neural: Boolean) {
        val wav = currentWav() ?: run { setState(State.IDLE); return }
        val tok = ++token
        cancel = AtomicBoolean(false)
        setState(State.SEPARATE)
        bar.fraction = 0f
        statusTv.text = L10n.t(if (neural) "Neural separation…" else "Fast spectral separation…")
        StemSeparator.separateNativeAsync(a, wav, neural, cancel,
            onProgress = { f, msg -> if (tok == token) { bar.fraction = f; statusTv.text = "$msg  ${(f * 100).roundToInt()}%" } },
            onDone = { r ->
                if (tok != token || a.isDestroyed) return@separateNativeAsync
                if (!r.ok) {
                    statusTv.text = if (r.cancelled) L10n.t("cancelled") else L10n.f("Separation failed: %s", r.error)
                    if (!r.cancelled) a.log("stem separation failed: ${r.error}", Ui.ERR)
                    setState(State.IDLE)
                    return@separateNativeAsync
                }
                loadIntoEngine(r, tok)
            })
    }

    private fun loadIntoEngine(r: StemResult, tok: Int) {
        setState(State.LOADING)
        statusTv.text = L10n.t("Loading stems…")
        val p = StemSeparator.STEMS.map { r.stems[it]?.absolutePath ?: "" }
        a.loader.execute {
            val st = runCatching { NativeEngine.nativeAudioState() }.getOrNull()
            val pos = st?.getOrNull(0) ?: 0.0
            val wasPlaying = (st?.getOrNull(2) ?: 0.0) > 0.5
            val res = FormatFiles.json { NativeEngine.nativeAudioLoadStems(p[0], p[1], p[2], p[3]) }
            val ok = res.optBoolean("ok")
            if (ok) {
                runCatching {
                    NativeEngine.nativeAudioSeek(pos)
                    for (i in 0 until 4) NativeEngine.nativeAudioSetStem(i, gain[i] * MAX_GAIN, mute[i], solo[i])
                    if (wasPlaying) NativeEngine.nativeAudioPlay(true)
                }
            }
            val state = if (ok) runCatching { JSONObject(NativeEngine.nativeAudioStemState()) }.getOrNull() else null
            a.ui.post {
                if (tok != token || a.isDestroyed) return@post
                if (!ok) {
                    statusTv.text = L10n.f("Separation failed: %s", res.optString("error", "stems could not be loaded"))
                    a.log("stems load failed: ${res.optString("error")}", Ui.ERR)
                    setState(State.IDLE)
                    return@post
                }
                adoptState(state)
                setState(State.READY)
                syncMixerUi()
                val secs = r.elapsedMs / 1000.0
                statusTv.text = String.format(java.util.Locale.US, "%s  ·  %s%s  ·  %.1f s", L10n.t("STEMS READY"), r.engineUsed,
                    if (r.fromCache) " (" + L10n.t("cached") + ")" else "", secs)
                a.log("stems: ${r.engineUsed}${if (r.fromCache) " (cache)" else ""}")
                onStemsLoaded()
            }
        }
    }

    private fun adoptState(s: JSONObject?) {
        val arr = s?.optJSONArray("stems") ?: return
        for (i in 0 until minOf(4, arr.length())) {
            val o = arr.optJSONObject(i) ?: continue
            gain[i] = (o.optDouble("gain", UNITY.toDouble() * MAX_GAIN).toFloat() / MAX_GAIN).coerceIn(0f, 1f)
            mute[i] = o.optBoolean("mute", false)
            solo[i] = o.optBoolean("solo", false)
        }
    }

    // ------------------------------------------------------------------ mixer

    private enum class Preset { ALL, MINUS_ONE, KARAOKE, DRUMS_BASS }

    private fun resetMix() {
        for (i in 0 until 4) { gain[i] = UNITY; mute[i] = false; solo[i] = false }
        syncMixerUi()
    }

    private fun preset(p: Preset) {
        for (i in 0 until 4) { solo[i] = false }
        when (p) {
            Preset.ALL -> for (i in 0 until 4) { mute[i] = false; gain[i] = UNITY }
            Preset.MINUS_ONE -> for (i in 0 until 4) mute[i] = i == KEYS
            Preset.KARAOKE -> for (i in 0 until 4) mute[i] = i == VOCALS
            Preset.DRUMS_BASS -> for (i in 0 until 4) mute[i] = i == VOCALS || i == KEYS
        }
        for (i in 0 until 4) push(i)
        syncMixerUi()
    }

    private fun push(i: Int) {
        runCatching { NativeEngine.nativeAudioSetStem(i, gain[i] * MAX_GAIN, mute[i], solo[i]) }
    }

    private fun syncMixerUi() {
        for (i in 0 until 4) {
            bars[i]?.apply {
                value = gain[i]
                valueText = "${(gain[i] * MAX_GAIN * 100).roundToInt()}"
                muted = mute[i] || (solo.any { it } && !solo[i])
            }
            muteBtns[i]?.lit = mute[i]
            soloBtns[i]?.lit = solo[i]
        }
        val anySolo = solo.any { it }
        val m = mute
        chipAll.lit = !anySolo && m.none { it } && gain.all { it == UNITY }
        chipMinusOne.lit = !anySolo && m[KEYS] && !m[VOCALS] && !m[1] && !m[2]
        chipKaraoke.lit = !anySolo && m[VOCALS] && !m[KEYS] && !m[1] && !m[2]
        chipDrumsBass.lit = !anySolo && m[VOCALS] && m[KEYS] && !m[1] && !m[2]
    }

    private fun chip(label: String, onClick: () -> Unit) =
        LedButton(a, L10n.t(label), tr = false, led = false, accentColor = Hw.MOD_FX).apply {
            textSize = 11.5f; minHeight = 0; minimumHeight = 0; setPadding(d(10), 0, d(10), 0)
            setOnClickListener { onClick() }
        }

    private fun buildMixer(): LinearLayout {
        val box = LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            background = Hw.panelBg(a, color = Hw.PANEL, stroke = Hw.PANEL_EDGE, radiusDp = 8f)
            setPadding(d(10), d(6), d(10), d(8))
        }
        val head = FlowLayout(a, d(5), d(5)).apply {
            addView(Hw.engraved(a, L10n.t("4-STEM MIXER"), 10.5f, tr = false, color = Hw.MOD_FX), ViewGroup.LayoutParams(-2, d(28)))
            addView(chipAll, ViewGroup.LayoutParams(-2, d(28)))
            addView(chipMinusOne, ViewGroup.LayoutParams(-2, d(28)))
            addView(chipKaraoke, ViewGroup.LayoutParams(-2, d(28)))
            addView(chipDrumsBass, ViewGroup.LayoutParams(-2, d(28)))
        }
        box.addView(head, LinearLayout.LayoutParams(-1, -2))
        for (i in 0 until 4) {
            val col = Hw.partColor(i)
            val label = TextView(a).apply {
                text = L10n.t(names[i]); textSize = 11f; typeface = Hw.BOLD; setTextColor(col); maxLines = 1; letterSpacing = 0.06f
            }
            val sb = HSegBar(a, col, 22, 34f).apply {
                onChange = { v ->
                    gain[i] = v
                    valueText = "${(v * MAX_GAIN * 100).roundToInt()}"
                    push(i)
                    syncChipsOnly()
                }
                onReset = { value = UNITY; onChange?.invoke(UNITY) }
            }
            bars[i] = sb
            val mb = LedButton(a, "M", tr = false, led = false, accentColor = Ui.ERR).apply {
                textSize = 13f; minHeight = 0; minimumHeight = 0; setPadding(0, 0, 0, 0)
                setOnClickListener { mute[i] = !mute[i]; push(i); syncMixerUi() }
            }
            val so = LedButton(a, "S", tr = false, led = false, accentColor = Hw.MOD_FILTER).apply {
                textSize = 13f; minHeight = 0; minimumHeight = 0; setPadding(0, 0, 0, 0)
                setOnClickListener {
                    solo[i] = !solo[i]
                    for (k in 0 until 4) push(k)
                    syncMixerUi()
                }
            }
            muteBtns[i] = mb; soloBtns[i] = so
            val row = LinearLayout(a).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                addView(label, LinearLayout.LayoutParams(d(66), -2))
                addView(sb, LinearLayout.LayoutParams(0, d(30), 1f))
                addView(mb, LinearLayout.LayoutParams(d(40), d(30)).apply { marginStart = d(8) })
                addView(so, LinearLayout.LayoutParams(d(40), d(30)).apply { marginStart = d(4) })
            }
            box.addView(row, LinearLayout.LayoutParams(-1, d(32)).apply { topMargin = d(4) })
        }
        return box
    }

    private fun syncChipsOnly() {
        chipAll.lit = !solo.any { it } && mute.none { it } && gain.all { it == UNITY }
    }

    private fun buildChoice(): LinearLayout {
        val box = LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            background = Hw.panelBg(a, color = Hw.PANEL_RAISED, stroke = Hw.withAlpha(Hw.MOD_FX, 160), radiusDp = 8f)
            setPadding(d(12), d(8), d(12), d(10))
        }
        box.addView(Hw.engraved(a, L10n.t("SEPARATE STEMS"), 11f, tr = false, color = Hw.MOD_FX), LinearLayout.LayoutParams(-2, -2))
        box.addView(Ui.text(a, "Spleeter models are not on this device yet. Pick how to split the track:", 12f, Hw.INK_DIM),
            LinearLayout.LayoutParams(-1, -2).apply { topMargin = d(3) })
        val row = FlowLayout(a, d(8), d(8)).apply {
            addView(LedButton(a, L10n.t("Download Spleeter 4-Stems FP16 (~40 MB)"), tr = false, led = false, accentColor = Hw.MOD_FX).apply {
                lit = true; textSize = 12.5f; minHeight = 0; minimumHeight = 0; setPadding(d(12), 0, d(12), 0)
                setOnClickListener { download() }
            }, ViewGroup.LayoutParams(-2, d(36)))
            addView(LedButton(a, L10n.t("Fast Spectral (Instant, Offline)"), tr = false, led = false, accentColor = Hw.MOD_OSC).apply {
                textSize = 12.5f; minHeight = 0; minimumHeight = 0; setPadding(d(12), 0, d(12), 0)
                setOnClickListener { separate(neural = false) }
            }, ViewGroup.LayoutParams(-2, d(36)))
            addView(LedButton(a, "✕", tr = false, led = false).apply {
                minHeight = 0; minimumHeight = 0; setPadding(0, 0, 0, 0)
                setOnClickListener { setState(State.IDLE) }
            }, ViewGroup.LayoutParams(d(36), d(36)))
        }
        box.addView(row, LinearLayout.LayoutParams(-1, -2).apply { topMargin = d(8) })
        return box
    }

    companion object {
        /** Engine stem gain is 0..1.5; the bar is 0..1, so unity sits at 2/3. */
        const val MAX_GAIN = 1.5f
        const val UNITY = 1f / MAX_GAIN
        private const val VOCALS = 0
        private const val KEYS = 3
    }
}

/** Thin read-only progress bar (6 dp). No allocation in [onDraw]. */
private class StemProgressView(ctx: Context) : View(ctx) {
    var fraction = 0f
        set(v) { val c = v.coerceIn(0f, 1f); if (c != field) { field = c; invalidate() } }
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)
    private val r = RectF()

    override fun onDraw(c: Canvas) {
        val h = height.toFloat()
        p.color = Hw.PANEL_EDGE
        r.set(0f, 0f, width.toFloat(), h)
        c.drawRoundRect(r, h / 2f, h / 2f, p)
        p.color = Hw.MOD_FX
        r.set(0f, 0f, width * fraction, h)
        c.drawRoundRect(r, h / 2f, h / 2f, p)
    }
}
