package com.hsynth.app

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.text.InputType
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.inputmethod.EditorInfo
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

/*
 * B2 — Live Sampling Wizard: ① SOURCE → ② RECORD → ③ TRIM → ④ ASSIGN, a CENTER sheet (820 × 360 dp).
 *
 * The take lives in a native buffer (`nativeSampler*`, ≤ 30 s stereo float). Analysis (auto trim, normalise gain, loop
 * points, root note) and export run on `a.loader`; the buffer is discarded with `nativeSamplerDiscard()` whenever the
 * wizard hides, so nothing stays allocated after it closes.
 */

/** Waveform of the take with START / END (+ LOOP S / LOOP E) handles; the domain is sample frames. */
@SuppressLint("ViewConstructor")
class SampleEditView(ctx: Context) : WaveSeekView(ctx, tick = false, accent = Hw.MOD_FX) {
    init {
        hLabel[0] = "S"; hLabel[1] = "E"; hLabel[2] = "L"; hLabel[3] = "L"
        hColor[0] = Hw.MOD_GOOD; hColor[1] = Hw.MOD_RED; hColor[2] = Hw.MOD_ENV; hColor[3] = Hw.MOD_ENV
        pairColor[0] = Hw.MOD_GOOD
        pairColor[1] = Hw.MOD_ENV
        dimOutside = true
        showPlayhead = false
    }

    var frames = 0
        private set

    fun setTake(frames: Int, sampleRate: Int, peaks: FloatArray?) {
        this.frames = frames
        secPerUnit = 1.0 / max(1, sampleRate)
        span = max(1, frames).toDouble()
        setPeaks(peaks)
    }

    var start: Int
        get() = hv[0].roundToInt()
        set(v) { hv[0] = v.toDouble().coerceIn(0.0, span); invalidate() }
    var end: Int
        get() = hv[1].roundToInt()
        set(v) { hv[1] = v.toDouble().coerceIn(0.0, span); invalidate() }
    var loopStart: Int
        get() = if (hv[2] < 0.0) -1 else hv[2].roundToInt()
        set(v) { hv[2] = if (v < 0) -1.0 else v.toDouble().coerceIn(0.0, span); invalidate() }
    var loopEnd: Int
        get() = if (hv[3] < 0.0) -1 else hv[3].roundToInt()
        set(v) { hv[3] = if (v < 0) -1.0 else v.toDouble().coerceIn(0.0, span); invalidate() }
}

/** Horizontal dB bar (−60 … 0) with a peak tick; zero-alloc. */
@SuppressLint("ViewConstructor")
private class RecMeterView(ctx: Context) : View(ctx) {
    private val d = resources.displayMetrics.density
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)
    private val r = RectF()
    private var db = -99f
    private var hold = -99f

    fun set(levelDb: Float, peakDb: Float) {
        if (levelDb == db && peakDb == hold) return
        db = levelDb
        hold = peakDb
        invalidate()
    }

    private fun x(v: Float, w: Float) = ((v.coerceIn(-60f, 0f) + 60f) / 60f) * w

    override fun onDraw(c: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        r.set(0f, 0f, w, h)
        p.style = Paint.Style.FILL
        p.color = Hw.WELL
        c.drawRoundRect(r, 4f * d, 4f * d, p)
        val pad = 2f * d
        val lw = x(db, w - 2 * pad)
        val g = x(-12f, w - 2 * pad)
        val a = x(-3f, w - 2 * pad)
        p.color = Hw.MOD_GOOD
        c.drawRect(pad, pad, pad + min(lw, g), h - pad, p)
        if (lw > g) { p.color = Hw.MOD_FILTER; c.drawRect(pad + g, pad, pad + min(lw, a), h - pad, p) }
        if (lw > a) { p.color = Hw.MOD_RED; c.drawRect(pad + a, pad, pad + lw, h - pad, p) }
        if (hold > -60f) {
            p.color = Hw.SILK
            val hx = pad + x(hold, w - 2 * pad)
            c.drawRect(hx - d, pad, hx + d, h - pad, p)
        }
        p.style = Paint.Style.STROKE
        p.strokeWidth = d
        p.color = Hw.PANEL_EDGE
        r.inset(d / 2f, d / 2f)
        c.drawRoundRect(r, 4f * d, 4f * d, p)
    }
}

/** 4 × 4 pad picker (160 × 160 dp): one View, shows which pads already hold a sample. */
@SuppressLint("ViewConstructor")
private class MiniPadGrid(ctx: Context, private val has: BooleanArray) : View(ctx) {
    var selected = -1
        set(v) { if (field != v) { field = v; invalidate() } }
    var onPick: ((Int) -> Unit)? = null
    private val d = resources.displayMetrics.density
    private val p = Paint(Paint.ANTI_ALIAS_FLAG)
    private val t = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Hw.MONO; textAlign = Paint.Align.CENTER; textSize = 11f * d }
    private val r = RectF()
    private val nums = Array(16) { "${it + 1}" }

    init { isClickable = true }

    private fun padAt(x: Float, y: Float): Int {
        val cw = width / 4f
        val ch = height / 4f
        val col = (x / cw).toInt().coerceIn(0, 3)
        val row = (y / ch).toInt().coerceIn(0, 3)
        return (3 - row) * 4 + col
    }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        if (e.actionMasked == MotionEvent.ACTION_UP) {
            val i = padAt(e.x, e.y)
            selected = i
            onPick?.invoke(i)
            performClick()
        }
        return true
    }

    override fun performClick(): Boolean = super.performClick()

    override fun onDraw(c: Canvas) {
        val gap = 4f * d
        val cw = (width - gap * 3) / 4f
        val ch = (height - gap * 3) / 4f
        for (i in 0 until 16) {
            val col = i % 4
            val row = 3 - i / 4
            val l = col * (cw + gap)
            val tp = row * (ch + gap)
            r.set(l, tp, l + cw, tp + ch)
            val sel = i == selected
            p.style = Paint.Style.FILL
            p.color = if (sel) Hw.withAlpha(Hw.MOD_FX, 90) else Hw.PANEL_RAISED
            c.drawRoundRect(r, 4f * d, 4f * d, p)
            p.style = Paint.Style.STROKE
            p.strokeWidth = if (sel) 1.8f * d else d
            p.color = if (sel) Hw.MOD_FX else Hw.PANEL_EDGE
            c.drawRoundRect(r, 4f * d, 4f * d, p)
            t.color = if (sel) Hw.SILK else Hw.ENGRAVE
            c.drawText(nums[i], r.centerX(), r.centerY() + 4f * d, t)
            if (has[i]) {
                p.style = Paint.Style.FILL
                p.color = Hw.MOD_OSC
                c.drawCircle(r.right - 6f * d, r.top + 6f * d, 2.2f * d, p)
            }
        }
    }
}

class SampleWizardSheet(private val a: MainActivity) : Sheet(Edge.CENTER, 820, 360), FrameTicker.Client {
    override val view: View by lazy { build() }

    private val d = a.resources.displayMetrics.density
    private fun dp(v: Int) = (v * d).toInt()

    // ---- state
    private var step = 0
    private var reached = 0
    private var source = 0                 // 0 audio input, 1 internal resample
    private var maxSec = 10f
    private var armOn = true
    private var recActive = false
    private var lastSec = 0.0
    private var frames = 0
    private var sr = 48000
    private var analysis: FloatArray? = null
    private var normalize = true
    private var gainDb = 0f
    private var loopOn = false
    private var xfadeMs = 10f
    private var root = 60
    private var previewOn = false
    private var targetLayer = 0            // 0..2, -1 = pad
    private var targetPad = -1
    private var defaultLayer = -1
    private var defaultPad = -1
    private var exporting = false
    private var gen = 0
    private val padHas = BooleanArray(16)

    // ---- views
    private lateinit var steps: SubTabBar
    private lateinit var backBtn: LedButton
    private val pages = arrayOfNulls<View>(4)
    private lateinit var srcInputBtn: LedButton
    private lateinit var srcResampleBtn: LedButton
    private lateinit var lenA: DragNumber
    private lateinit var lenB: DragNumber
    private lateinit var warnTv: TextView
    private lateinit var recBtn: LedButton
    private lateinit var elapsedTv: TextView
    private lateinit var recMeter: RecMeterView
    private lateinit var recPeakTv: TextView
    private lateinit var recStatusTv: TextView
    private lateinit var edit: SampleEditView
    private lateinit var normBtn: LedButton
    private lateinit var loopBtn: LedButton
    private lateinit var previewBtn: LedButton
    private lateinit var xfadeNum: DragNumber
    private lateinit var rootNum: DragNumber
    private lateinit var trimInfo: TextView
    private lateinit var layerBtns: Array<LedButton>
    private lateinit var padGrid: MiniPadGrid
    private lateinit var nameEt: EditText
    private lateinit var finishBtn: LedButton
    private lateinit var assignInfo: TextView

    private val elapsed = CharArray(8)
    private val noteNames = arrayOf("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
    private fun noteText(m: Int) = noteNames[((m % 12) + 12) % 12] + (m / 12 - 1)

    override val intervalMs: Int get() = 50
    override val tickView: View? get() = view

    /** Called by `MainActivity.openSampleWizard` before the sheet opens. */
    fun prepare(defaultLayer: Int, defaultPad: Int) {
        this.defaultLayer = defaultLayer
        this.defaultPad = defaultPad
    }

    // ------------------------------------------------------------------ sheet lifecycle

    override fun onShow() {
        resetState()
        showStep(0)
        FrameTicker.register(this)
    }

    override fun onHide() {
        FrameTicker.unregister(this)
        gen++
        if (previewOn) { previewOn = false; NativeEngine.nativeSamplerAudition(false, false, 0, 0) }
        if (recActive) { recActive = false; NativeEngine.nativeSamplerStop() }
        NativeEngine.nativeSamplerDiscard()
    }

    private fun resetState() {
        // a previous take (from an earlier open) never survives
        if (recActive) NativeEngine.nativeSamplerStop()
        NativeEngine.nativeSamplerDiscard()
        recActive = false
        previewOn = false
        exporting = false
        analysis = null
        frames = 0
        reached = 0
        loopOn = false
        lastSec = 0.0
        edit.setTake(0, sr, null)
        edit.hv.fill(-1.0)
        warnTv.text = ""
        recStatusTv.text = ""
        elapsedTv.text = "00:00.0"
        recMeter.set(-99f, -99f)
        recPeakTv.text = "—"
        for (i in 0 until 16) padHas[i] = a.isPlayerReady && a.player.padsPage.cfg[i].has
        if (defaultPad in 0..15) { targetPad = defaultPad; targetLayer = -1 }
        else { targetPad = -1; targetLayer = if (defaultLayer in 0..2) defaultLayer else 0 }
        nameEt.setText("Rec_" + SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date()))
        syncSource()
        syncTarget()
        syncTrimButtons()
        finishBtn.isEnabled = true
        finishBtn.text = "FINISH ✓"
    }

    // ------------------------------------------------------------------ build

    private fun btn(label: String, accent: Int, led: Boolean = false, size: Float = 12f, onClick: () -> Unit) =
        LedButton(a, label, tr = false, led = led, accentColor = accent).apply {
            textSize = size
            minHeight = 0
            minimumHeight = 0
            setPadding(if (led) dp(15) else dp(4), 0, dp(4), 0)
            setOnClickListener { onClick() }
        }

    private fun build(): View {
        steps = SubTabBar(a, arrayOf("① SOURCE", "② RECORD", "③ TRIM", "④ ASSIGN"), accent = Hw.MOD_FX, tr = false).apply {
            onSelect = { i ->
                if (i <= reached) {
                    goStep(i)
                } else {
                    selected = step
                }
            }
        }
        backBtn = btn("◂", Hw.MOD_FX, size = 15f) { if (step > 0) goStep(step - 1) }
        val close = btn("✕", Hw.MOD_RED, size = 14f) { dismiss() }
        val header = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(10), 0, dp(10), 0)
            addView(backBtn, LinearLayout.LayoutParams(dp(34), dp(28)).apply { marginEnd = dp(6) })
            addView(steps, LinearLayout.LayoutParams(0, dp(26), 1f))
            addView(close, LinearLayout.LayoutParams(dp(34), dp(28)).apply { marginStart = dp(6) })
        }

        pages[0] = buildSource()
        pages[1] = buildRecord()
        pages[2] = buildTrim()
        pages[3] = buildAssign()
        val body = FrameLayout(a).apply {
            setPadding(dp(12), dp(6), dp(12), dp(8))
            for (p in pages) addView(p, FrameLayout.LayoutParams(-1, -1))
        }
        return LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            addView(header, LinearLayout.LayoutParams(-1, dp(36)))
            addView(body, LinearLayout.LayoutParams(-1, 0, 1f))
        }
    }

    private fun lenNumber(): DragNumber = DragNumber(a, 1f, 30f, 1f, 10f, { "${it.roundToInt()} s" }, { maxSec }) { v ->
        maxSec = v
        if (::lenA.isInitialized) lenA.refresh()
        if (::lenB.isInitialized) lenB.refresh()
    }.apply { textSize = 13f }

    // ① SOURCE
    private fun buildSource(): View {
        lenA = lenNumber()
        lenB = lenNumber()
        srcInputBtn = btn("USE AUDIO INPUT", Hw.MOD_OSC, led = true) { source = 0; syncSource() }
        srcResampleBtn = btn("USE INTERNAL RESAMPLE", Hw.MOD_FX, led = true) { source = 1; syncSource() }
        val armToggle = ToggleSwitch(a, "ARM on signal (−40 dB)", tr = false, vertical = false, accent = Hw.MOD_OSC).apply {
            on = armOn
            onToggle = { armOn = it }
        }
        fun lenRow(n: DragNumber) = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(Hw.engraved(a, "MAX LEN", 10.5f, tr = false), LinearLayout.LayoutParams(dp(70), -2))
            addView(n, LinearLayout.LayoutParams(dp(80), dp(32)))
        }
        val cardA = Hw.Section(a, "AUDIO INPUT", tr = false, accent = Hw.MOD_OSC).apply {
            body.addView(srcInputBtn, LinearLayout.LayoutParams(-1, dp(32)))
            body.addView(InputStatusRow(a), LinearLayout.LayoutParams(-1, dp(30)).apply { topMargin = dp(8) })
            body.addView(armToggle, LinearLayout.LayoutParams(-1, dp(36)).apply { topMargin = dp(4) })
            body.addView(lenRow(lenA), LinearLayout.LayoutParams(-1, dp(34)).apply { topMargin = dp(4) })
        }
        val cardB = Hw.Section(a, "INTERNAL RESAMPLE", tr = false, accent = Hw.MOD_FX).apply {
            body.addView(srcResampleBtn, LinearLayout.LayoutParams(-1, dp(32)))
            body.addView(Ui.text(a, "Records the master output, including effects and the Master EQ — play the sound you want to capture while recording.", 11.5f, Hw.ENGRAVE),
                LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(8) })
            body.addView(lenRow(lenB), LinearLayout.LayoutParams(-1, dp(34)).apply { topMargin = dp(10) })
        }
        warnTv = Ui.text(a, "", 12f, Ui.WARN, tr = false).apply { maxLines = 1 }
        val next = btn("NEXT ▸", Hw.MOD_FX, size = 14f) { sourceNext() }
        val bottom = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(warnTv, LinearLayout.LayoutParams(0, -2, 1f))
            addView(next, LinearLayout.LayoutParams(dp(120), dp(40)))
        }
        return LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            val cards = LinearLayout(a).apply {
                orientation = LinearLayout.HORIZONTAL
                addView(cardA, LinearLayout.LayoutParams(0, -1, 1f).apply { marginEnd = dp(10) })
                addView(cardB, LinearLayout.LayoutParams(0, -1, 1f))
            }
            addView(cards, LinearLayout.LayoutParams(-1, dp(200)))
            addView(bottom, LinearLayout.LayoutParams(-1, dp(44)).apply { topMargin = dp(12) })
        }
    }

    private fun syncSource() {
        srcInputBtn.lit = source == 0
        srcResampleBtn.lit = source == 1
        if (::lenA.isInitialized) { lenA.refresh(); lenB.refresh() }
    }

    private fun sourceNext() {
        if (source == 0 && !NativeEngine.nativeInputEnabled()) {
            warnTv.text = L10n.t("Audio input is off — tap “Enable Audio Input” first.")
            return
        }
        warnTv.text = ""
        reached = max(reached, 1)
        goStep(1)
    }

    // ② RECORD
    private fun buildRecord(): View {
        recBtn = btn("● REC", Hw.MOD_RED, led = true, size = 20f) { recToggle() }
        elapsedTv = TextView(a).apply {
            text = "00:00.0"
            textSize = 48f
            typeface = Hw.MONO
            setTextColor(Hw.LCD_FG)
            gravity = Gravity.CENTER
            includeFontPadding = false
        }
        recMeter = RecMeterView(a)
        recPeakTv = Ui.text(a, "—", 13f, Hw.ENGRAVE, tr = false).apply { typeface = Hw.MONO; gravity = Gravity.CENTER_VERTICAL }
        recStatusTv = Ui.text(a, "", 12f, Hw.ENGRAVE, tr = false).apply { gravity = Gravity.CENTER }
        return LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
            addView(recBtn, LinearLayout.LayoutParams(dp(160), dp(64)).apply { topMargin = dp(10) })
            addView(elapsedTv, LinearLayout.LayoutParams(-2, -2).apply { topMargin = dp(10) })
            addView(LinearLayout(a).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                addView(recMeter, LinearLayout.LayoutParams(dp(400), dp(16)))
                addView(recPeakTv, LinearLayout.LayoutParams(dp(90), -2).apply { marginStart = dp(10) })
            }, LinearLayout.LayoutParams(-2, dp(20)).apply { topMargin = dp(10) })
            addView(recStatusTv, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(10) })
        }
    }

    private fun recToggle() {
        if (!recActive) {
            val arm = if (source == 0 && armOn) -40f else -120f
            if (!NativeEngine.nativeSamplerStart(source, maxSec, arm)) {
                recStatusTv.text = L10n.t(if (source == 0) "Could not start — is the audio input enabled?" else "Could not start recording")
                return
            }
            recActive = true
            recStatusTv.text = ""
            syncRecButton(1)
        } else {
            val sec = NativeEngine.nativeSamplerStop()
            recActive = false
            finishRecording(sec)
        }
    }

    private fun finishRecording(sec: Double) {
        syncRecButton(0)
        if (sec < 0.02) {
            recStatusTv.text = L10n.t("Nothing was recorded.")
            return
        }
        lastSec = sec
        reached = max(reached, 2)
        enterTrim()
    }

    private fun syncRecButton(state: Int) {
        recBtn.text = if (state == 0) "● REC" else "■ STOP"
        recBtn.lit = state != 0
    }

    override fun onFrame(nowMs: Long) {
        if (step != 1) return
        val s = NativeEngine.nativeSamplerState()
        if (s.size < 3) return
        val state = s[0].toInt()
        val sec = s[1]
        // elapsed "MM:SS.t" in a CharArray: no strings per frame
        val t = (sec * 10.0).toInt().coerceAtLeast(0)
        val whole = t / 10
        val m = whole / 60
        val ss = whole % 60
        elapsed[0] = '0' + (m / 10) % 10
        elapsed[1] = '0' + m % 10
        elapsed[2] = ':'
        elapsed[3] = '0' + ss / 10
        elapsed[4] = '0' + ss % 10
        elapsed[5] = '.'
        elapsed[6] = '0' + t % 10
        elapsedTv.setText(elapsed, 0, 7)
        val live = if (source == 0) InputMeter.db else s[2]
        recMeter.set(live, s[2])
        recPeakTv.text = if (s[2] <= -90f) "—" else "%.1f dB".format(s[2])
        if (recActive) {
            if (state == 1) recStatusTv.text = L10n.t("ARMED — waiting for a signal above −40 dB")
            else if (state == 2) recStatusTv.text = L10n.t("RECORDING…")
            else if (state == 3) {          // maximum length reached
                recActive = false
                finishRecording(sec.toDouble())
            }
        }
    }

    // ③ TRIM
    private fun buildTrim(): View {
        edit = SampleEditView(a).apply {
            onHandle = { i, _, final -> onHandleMoved(i, final) }
        }
        val autoBtn = btn("AUTO TRIM", Hw.MOD_FX) { autoTrim() }
        normBtn = btn("NORMALIZE", Hw.MOD_FX, led = true) { normalize = !normalize; syncTrimButtons() }
        loopBtn = btn("LOOP", Hw.MOD_ENV, led = true) { setLoop(!loopOn) }
        val zxBtn = btn("ZERO-X LOOP", Hw.MOD_ENV) { zeroXLoop() }
        xfadeNum = DragNumber(a, 0f, 50f, 1f, 10f, { "${it.roundToInt()} ms" }, { xfadeMs }) { xfadeMs = it }.apply { textSize = 12f; setPadding(0, 0, 0, 0) }
        rootNum = DragNumber(a, 0f, 127f, 1f, 60f, { noteText(it.roundToInt()) }, { root.toFloat() }) { root = it.roundToInt() }.apply { textSize = 12f; setPadding(0, 0, 0, 0) }
        val detBtn = btn("DET", Hw.MOD_FX) { detectRoot() }
        previewBtn = btn("▶ PREVIEW", Hw.MOD_GOOD, led = true) { togglePreview() }
        trimInfo = Ui.text(a, "", 11f, Hw.ENGRAVE, tr = false).apply { typeface = Hw.MONO; maxLines = 1 }
        val nextBtn = btn("NEXT ▸", Hw.MOD_FX, size = 13f) { reached = max(reached, 3); goStep(3) }

        fun lab(l: String, v: View, w: Int) = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(Hw.engraved(a, l, 9.5f, tr = false), LinearLayout.LayoutParams(-2, -2).apply { marginEnd = dp(4) })
            addView(v, LinearLayout.LayoutParams(dp(w), dp(32)))
        }
        val row = FlowLayout(a, dp(6), dp(6)).apply {
            addView(autoBtn, ViewGroup.LayoutParams(dp(88), dp(32)))
            addView(normBtn, ViewGroup.LayoutParams(dp(150), dp(32)))
            addView(loopBtn, ViewGroup.LayoutParams(dp(62), dp(32)))
            addView(zxBtn, ViewGroup.LayoutParams(dp(104), dp(32)))
            addView(lab("XFADE", xfadeNum, 56), ViewGroup.LayoutParams(-2, -2))
            addView(lab("ROOT", rootNum, 52), ViewGroup.LayoutParams(-2, -2))
            addView(detBtn, ViewGroup.LayoutParams(dp(40), dp(32)))
            addView(previewBtn, ViewGroup.LayoutParams(dp(106), dp(32)))
        }
        return LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            addView(edit, LinearLayout.LayoutParams(-1, dp(150)))
            addView(row, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(8) })
            addView(LinearLayout(a).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                addView(trimInfo, LinearLayout.LayoutParams(0, -2, 1f))
                addView(nextBtn, LinearLayout.LayoutParams(dp(110), dp(34)))
            }, LinearLayout.LayoutParams(-1, dp(38)).apply { topMargin = dp(4) })
        }
    }

    private fun enterTrim() {
        goStep(2)
        val token = ++gen
        trimInfo.text = L10n.t("Analysing…")
        a.loader.execute {
            val an = try { NativeEngine.nativeSamplerAnalyze() } catch (_: Throwable) { null }
            val peaks = try { NativeEngine.nativeSamplerPeaks(800) } catch (_: Throwable) { null }
            a.ui.post {
                if (token != gen) return@post
                analysis = an
                sr = an?.getOrNull(6)?.roundToInt()?.takeIf { it > 1000 } ?: 48000
                frames = max(2, (lastSec * sr).roundToInt())
                edit.setTake(frames, sr, peaks)
                edit.setZoomStep(0)
                gainDb = an?.getOrNull(2) ?: 0f
                val rm = an?.getOrNull(5)?.roundToInt() ?: -1
                root = if (rm in 0..127) rm else 60
                rootNum.refresh()
                edit.start = 0
                edit.end = frames
                autoTrim()
                loopOn = false
                edit.loopStart = -1
                edit.loopEnd = -1
                syncTrimButtons()
                updateTrimInfo()
            }
        }
    }

    private fun autoTrim() {
        val an = analysis
        if (an != null && an.size >= 2) {
            val s = an[0].roundToInt()
            val e = an[1].roundToInt()
            if (e - s >= 2 && s >= 0 && e <= frames) { edit.start = s; edit.end = e }
        }
        clampLoop()
        edit.invalidate()
        restartPreviewIfOn()
        updateTrimInfo()
    }

    private fun zeroXLoop() {
        val an = analysis
        val s = an?.getOrNull(3)?.roundToInt() ?: -1
        val e = an?.getOrNull(4)?.roundToInt() ?: -1
        if (s >= 0 && e > s) {
            edit.loopStart = s
            edit.loopEnd = e
            loopOn = true
        } else {
            // no clean loop found: loop the trimmed region
            edit.loopStart = edit.start
            edit.loopEnd = edit.end
            loopOn = true
            trimInfo.text = L10n.t("No zero-crossing loop found — using the whole region.")
        }
        clampLoop()
        syncTrimButtons()
        restartPreviewIfOn()
        if (trimInfo.text.isEmpty()) updateTrimInfo()
    }

    private fun setLoop(on: Boolean) {
        loopOn = on
        if (on && edit.loopStart < 0) {
            val an = analysis
            val s = an?.getOrNull(3)?.roundToInt() ?: -1
            val e = an?.getOrNull(4)?.roundToInt() ?: -1
            if (s >= edit.start && e > s && e <= edit.end) { edit.loopStart = s; edit.loopEnd = e }
            else { edit.loopStart = edit.start; edit.loopEnd = edit.end }
        }
        if (!on) { edit.loopStart = -1; edit.loopEnd = -1 }
        syncTrimButtons()
        restartPreviewIfOn()
        updateTrimInfo()
    }

    private fun clampLoop() {
        if (edit.loopStart < 0) return
        val ls = edit.loopStart.coerceIn(edit.start, edit.end - 1)
        val le = edit.loopEnd.coerceIn(ls + 1, edit.end)
        edit.loopStart = ls
        edit.loopEnd = le
    }

    private fun onHandleMoved(i: Int, final: Boolean) {
        if (i < 2) clampLoop()
        if (final) { restartPreviewIfOn(); updateTrimInfo() }
        else if (i < 2) edit.invalidate()
    }

    private fun detectRoot() {
        val rm = analysis?.getOrNull(5)?.roundToInt() ?: -1
        if (rm in 0..127) { root = rm; rootNum.refresh() }
        else trimInfo.text = L10n.t("Pitch could not be detected.")
    }

    private fun syncTrimButtons() {
        if (!::normBtn.isInitialized) return
        normBtn.text = if (normalize) "NORMALIZE %+.1f dB".format(gainDb) else "NORMALIZE"
        normBtn.lit = normalize
        loopBtn.lit = loopOn
        previewBtn.lit = previewOn
        previewBtn.text = if (previewOn) "■ PREVIEW" else "▶ PREVIEW"
        xfadeNum.refresh()
        rootNum.refresh()
    }

    private fun updateTrimInfo() {
        val len = (edit.end - edit.start).coerceAtLeast(0)
        trimInfo.text = "%.2f s  ·  %d frames @ %.1f kHz%s".format(len.toDouble() / sr, len, sr / 1000.0,
            if (loopOn && edit.loopStart >= 0) "  ·  loop %.2f–%.2f s".format(edit.loopStart.toDouble() / sr, edit.loopEnd.toDouble() / sr) else "")
    }

    private fun togglePreview() {
        previewOn = !previewOn
        if (previewOn) startAudition() else NativeEngine.nativeSamplerAudition(false, false, 0, 0)
        syncTrimButtons()
    }

    private fun startAudition() {
        if (loopOn && edit.loopStart >= 0) NativeEngine.nativeSamplerAudition(true, true, edit.loopStart, edit.loopEnd)
        else NativeEngine.nativeSamplerAudition(true, false, edit.start, edit.end)
    }

    private fun restartPreviewIfOn() { if (previewOn) startAudition() }

    // ④ ASSIGN
    private fun buildAssign(): View {
        layerBtns = Array(3) { i ->
            btn("LAYER ${"ABC"[i]}", Hw.MOD_OSC, led = true, size = 13f) { targetLayer = i; targetPad = -1; syncTarget() }
        }
        padGrid = MiniPadGrid(a, padHas).apply {
            onPick = { p -> targetPad = p; targetLayer = -1; syncTarget() }
        }
        nameEt = EditText(a).apply {
            setSingleLine()
            textSize = 14f
            typeface = Hw.MONO
            setTextColor(Hw.LCD_FG)
            background = Hw.lcdBg(a)
            setPadding(dp(8), 0, dp(8), 0)
            inputType = InputType.TYPE_CLASS_TEXT
            imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI or EditorInfo.IME_FLAG_NO_FULLSCREEN or EditorInfo.IME_ACTION_DONE
        }
        assignInfo = Ui.text(a, "", 11.5f, Hw.ENGRAVE, tr = false).apply { maxLines = 2 }
        finishBtn = btn("FINISH ✓", Hw.MOD_GOOD, size = 15f) { finish() }

        val left = LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            addView(Hw.engraved(a, "TARGET", 10.5f, tr = false, color = Hw.MOD_FX), LinearLayout.LayoutParams(-2, -2))
            addView(LinearLayout(a).apply {
                orientation = LinearLayout.HORIZONTAL
                for (i in 0 until 3) addView(layerBtns[i], LinearLayout.LayoutParams(dp(120), dp(36)).apply { marginEnd = dp(6) })
            }, LinearLayout.LayoutParams(-2, dp(36)).apply { topMargin = dp(6) })
            addView(Hw.engraved(a, "NAME", 10.5f, tr = false, color = Hw.MOD_FX), LinearLayout.LayoutParams(-2, -2).apply { topMargin = dp(14) })
            addView(nameEt, LinearLayout.LayoutParams(-1, dp(36)).apply { topMargin = dp(6) })
            addView(assignInfo, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(8) })
            addView(View(a), LinearLayout.LayoutParams(-1, 0, 1f))
            addView(finishBtn, LinearLayout.LayoutParams(dp(180), dp(44)))
        }
        val right = LinearLayout(a).apply {
            orientation = LinearLayout.VERTICAL
            addView(Hw.engraved(a, "OR PAD", 10.5f, tr = false, color = Hw.MOD_FX), LinearLayout.LayoutParams(-2, -2))
            addView(padGrid, LinearLayout.LayoutParams(dp(160), dp(160)).apply { topMargin = dp(6) })
        }
        return LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(left, LinearLayout.LayoutParams(0, -1, 1f).apply { marginEnd = dp(16) })
            addView(right, LinearLayout.LayoutParams(dp(170), -1))
        }
    }

    private fun syncTarget() {
        if (!::layerBtns.isInitialized) return
        for (i in 0 until 3) layerBtns[i].lit = targetLayer == i
        padGrid.selected = targetPad
        assignInfo.text = if (targetPad >= 0) "→ PAD ${targetPad + 1}  (files/pads/pad_%02d.wav)".format(targetPad + 1)
        else "→ LAYER ${"ABC"[targetLayer.coerceIn(0, 2)]}  (PCM, appears in LIB › Samples)"
    }

    private fun finish() {
        if (exporting) return
        if (edit.end - edit.start < 2) { assignInfo.text = L10n.t("The trimmed region is empty."); return }
        val name = a.library.safeName(nameEt.text.toString().trim().ifEmpty { "Rec" })
        val pad = targetPad
        val layer = targetLayer
        val dest: File = if (pad >= 0) {
            if (!a.isPlayerReady) { assignInfo.text = "pads are not ready"; return }
            a.player.padsPage.sampleFile(pad, "wav")
        } else {
            val dir = File(a.library.libraryRoot, "user/wav").apply { mkdirs() }
            var f = File(dir, "$name.wav")
            var n = 2
            while (f.exists()) { f = File(dir, "$name (${n}).wav"); n++ }
            f
        }
        val gain = if (normalize) gainDb else 0f
        val useLoop = loopOn && edit.loopStart >= 0 && edit.loopEnd > edit.loopStart
        val ls = if (useLoop) edit.loopStart else -1
        val le = if (useLoop) edit.loopEnd else -1
        val s = edit.start
        val e = edit.end
        val xf = if (useLoop) xfadeMs else 0f
        val rt = root
        // the preview must not keep running while the take is exported
        if (previewOn) { previewOn = false; NativeEngine.nativeSamplerAudition(false, false, 0, 0); syncTrimButtons() }
        exporting = true
        finishBtn.isEnabled = false
        finishBtn.text = "EXPORTING…"
        val token = gen
        a.loader.execute {
            val err = try { NativeEngine.nativeSamplerExport(dest.absolutePath, s, e, gain, ls, le, xf, rt) } catch (t: Throwable) { t.message ?: "export failed" }
            a.ui.post {
                if (token != gen) return@post
                exporting = false
                finishBtn.isEnabled = true
                finishBtn.text = "FINISH ✓"
                if (err.isNotEmpty()) {
                    assignInfo.text = err
                    a.log("sample export failed: $err", Ui.ERR)
                    return@post
                }
                a.log("sample recorded: ${dest.name}")
                if (pad >= 0) {
                    a.player.padsPage.assignFile(pad, dest, name) { pe -> if (pe != null) a.log("pad ${pad + 1}: $pe", Ui.ERR) }
                } else {
                    a.loadSamples(layer.coerceIn(0, 2), dest) { a.libraryPanel.refresh() }
                    a.libraryPanel.refresh()
                }
                dismiss()
            }
        }
    }

    // ------------------------------------------------------------------ steps

    private fun goStep(i: Int) {
        if (i != 2 && previewOn) { previewOn = false; NativeEngine.nativeSamplerAudition(false, false, 0, 0); syncTrimButtons() }
        // leaving RECORD while recording: keep recording only if it is still on this page
        if (i != 1 && recActive) { NativeEngine.nativeSamplerStop(); recActive = false; syncRecButton(0) }
        showStep(i)
    }

    private fun showStep(i: Int) {
        step = i.coerceIn(0, 3)
        for (k in 0 until 4) pages[k]?.visibility = if (k == step) View.VISIBLE else View.GONE
        steps.selected = step
        backBtn.alpha = if (step > 0) 1f else 0.35f
        if (step == 1) syncRecButton(if (recActive) 1 else 0)
        if (step == 3) { for (k in 0 until 16) padHas[k] = a.isPlayerReady && a.player.padsPage.cfg[k].has; padGrid.invalidate(); syncTarget() }
        FrameTicker.poke()
    }
}
