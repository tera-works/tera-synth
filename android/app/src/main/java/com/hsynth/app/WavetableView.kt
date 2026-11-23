package com.hsynth.app

import android.annotation.SuppressLint
import android.app.AlertDialog
import android.content.Context
import android.content.Intent
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.os.SystemClock
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewConfiguration
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import java.io.File
import java.lang.ref.WeakReference
import java.util.Locale
import kotlin.math.abs

/*
 * Wavetable UI for EDIT > OSC (layer type WT): a 2.5D ribbon + a chip bar with built-in / user tables and an
 * importer. Engine access only through [FormatBridge] (`wavetablePreview`, `loadAnyWavetable`) and the existing
 * parameter model (`<l>.wt.pos`, `<l>.wt.warpamt`, `<l>.wt.table`), so this file compiles on its own.
 */

/** Source of one wavetable cycle for the ribbon (a `fun interface`: no boxing of [pos] while scrubbing). */
fun interface WtPreviewProvider {
    /** Fill [out] with one cycle at table position [pos] (0..1). @return false when no table is loaded. */
    fun preview(pos: Float, out: FloatArray): Boolean
}

// ====================================================================================================== ribbon

/**
 * Serum / Vital style stacked wavetable ribbon, 124 dp tall, on a pure-black well.
 *
 * Five ghost frames recede up-right behind the bright current frame (Cyan fading to Violet). Drag horizontally to
 * scrub `wt.pos`, vertically to scrub `wt.warpamt`; the first axis that passes the touch slop is locked for the
 * gesture so a sideways sweep never nudges the warp. A [History.checkpoint] is taken on DOWN (one gesture = one undo).
 *
 * All paints, paths and buffers are allocated once; [onDraw] allocates nothing.
 */
class WavetableRibbonView(ctx: Context) : View(ctx) {
    /** Called (throttled, and once more on release) after the view changed a parameter, so knobs can follow. */
    var onEdited: (() -> Unit)? = null

    var accent: Int = Hw.MOD_OSC
        set(v) { if (field != v) { field = v; buildGhostColors(); invalidate() } }
    var ghostAccent: Int = Hw.MOD_ENV
        set(v) { if (field != v) { field = v; buildGhostColors(); invalidate() } }

    /** Current table position / warp amount, 0..1 (read-only mirror of the parameters). */
    var pos = 0f
        private set
    var warp = 0f
        private set

    private val d = resources.displayMetrics.density
    private val slop = ViewConfiguration.get(ctx).scaledTouchSlop

    /** frames[0..N) = current frame; frames[k*N..] = ghost k (1 = nearest, GHOSTS = farthest). */
    private val frames = FloatArray((GHOSTS + 1) * N)
    private val tmp = FloatArray(N)
    private val paths = Array(GHOSTS + 1) { Path() }
    private val ghostCol = IntArray(GHOSTS)

    private val wellP = Paint(Paint.ANTI_ALIAS_FLAG)
    private val gridP = Paint(Paint.ANTI_ALIAS_FLAG).apply { strokeWidth = d; color = Hw.withAlpha(Hw.PANEL_EDGE, 200) }
    private val ghostP = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeJoin = Paint.Join.ROUND; strokeWidth = 1.1f * d }
    private val glowP = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeJoin = Paint.Join.ROUND; strokeWidth = 5f * d }
    private val mainP = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeJoin = Paint.Join.ROUND; strokeWidth = 1.9f * d }
    private val trackP = Paint(Paint.ANTI_ALIAS_FLAG).apply { strokeWidth = 2f * d; strokeCap = Paint.Cap.ROUND }
    private val txtP = Paint(Paint.ANTI_ALIAS_FLAG).apply { typeface = Hw.MONO; textSize = 10f * d }
    private val well = RectF()

    private var provider: WtPreviewProvider? = null
    private var posName = "a.wt.pos"
    private var warpName = "a.wt.warpamt"
    private var posText = "POS 0.000"
    private var warpText = "WARP 0.000"
    private var dirty = true

    // gesture
    private var downX = 0f
    private var downY = 0f
    private var downPos = 0f
    private var downWarp = 0f
    private var axis = AXIS_NONE
    private var lastEmit = 0L

    init {
        buildGhostColors()
        contentDescription = "Wavetable"
    }

    override fun onMeasure(widthSpec: Int, heightSpec: Int) {
        setMeasuredDimension(getDefaultSize(suggestedMinimumWidth, widthSpec), (HEIGHT_DP * d).toInt())
    }

    override fun onSizeChanged(w: Int, h: Int, ow: Int, oh: Int) {
        super.onSizeChanged(w, h, ow, oh)
        dirty = true
    }

    // ------------------------------------------------------------------ public API

    /** Show one cycle without ghosts' provider (e.g. from a one-off preview). Resampled to the internal length. */
    fun setSamples(samples: FloatArray) {
        if (samples.isEmpty()) {
            java.util.Arrays.fill(frames, 0, N, 0f)
        } else if (samples.size == N) {
            System.arraycopy(samples, 0, frames, 0, N)
        } else {
            val last = samples.size - 1
            for (i in 0 until N) {
                val f = i * last / (N - 1).toFloat()
                val i0 = f.toInt().coerceIn(0, last)
                val i1 = (i0 + 1).coerceAtMost(last)
                val t = f - i0
                frames[i] = samples[i0] * (1f - t) + samples[i1] * t
            }
        }
        dirty = true
        invalidate()
    }

    /**
     * Sync with the parameters: [pos] / [warp] (0..1), [layerPrefix] `a` / `b` / `c` (used when dragging) and the
     * [previewProvider] that returns the cycles. Re-reads the current + ghost frames and redraws.
     */
    fun refresh(pos: Float, warp: Float, layerPrefix: String, previewProvider: WtPreviewProvider?) {
        val p = layerPrefix.trimEnd('.').ifEmpty { "a" }
        posName = "$p.wt.pos"
        warpName = "$p.wt.warpamt"
        provider = previewProvider
        this.pos = pos.coerceIn(0f, 1f)
        this.warp = warp.coerceIn(0f, 1f)
        updateTexts()
        fetchFrames()
        invalidate()
    }

    // ------------------------------------------------------------------ data

    private fun buildGhostColors() {
        for (k in 0 until GHOSTS) {
            val t = k / (GHOSTS - 1).toFloat()
            val r = Color.red(accent) + ((Color.red(ghostAccent) - Color.red(accent)) * t).toInt()
            val g = Color.green(accent) + ((Color.green(ghostAccent) - Color.green(accent)) * t).toInt()
            val b = Color.blue(accent) + ((Color.blue(ghostAccent) - Color.blue(accent)) * t).toInt()
            ghostCol[k] = Color.argb(150 - 24 * k, r, g, b)
        }
    }

    private fun updateTexts() {
        posText = String.format(Locale.US, "POS %.3f", pos)
        warpText = String.format(Locale.US, "WARP %.3f", warp)
    }

    /** Current frame at [pos] + ghosts stepping away from the nearer table end (so they never pile up on a clamp). */
    private fun fetchFrames() {
        val prov = provider ?: return
        if (prov.preview(pos, tmp)) System.arraycopy(tmp, 0, frames, 0, N) else java.util.Arrays.fill(frames, 0, N, 0f)
        val dir = if (pos > 0.5f) -1f else 1f
        for (k in 1..GHOSTS) {
            val gp = (pos + dir * k * GHOST_STEP).coerceIn(0f, 1f)
            if (prov.preview(gp, tmp)) System.arraycopy(tmp, 0, frames, k * N, N)
            else java.util.Arrays.fill(frames, k * N, (k + 1) * N, 0f)
        }
        dirty = true
    }

    /** Frame geometry → paths (only when data or size changed). Allocation-free: paths are rewound, not recreated. */
    private fun rebuildPaths() {
        dirty = false
        val w = width.toFloat()
        val h = height.toFloat()
        val ddx = GHOST_DX_DP * d
        val ddy = GHOST_DY_DP * d
        val x0 = PAD_DP * d
        val ww = (w - 2f * x0 - GHOSTS * ddx).coerceAtLeast(1f)
        val top = PAD_DP * d + GHOSTS * ddy
        val bottom = h - 22f * d
        val mid = (top + bottom) / 2f
        val amp = (bottom - top) / 2f * 0.92f
        val dx = ww / (N - 1)
        for (k in 0..GHOSTS) {
            val p = paths[k]
            p.rewind()
            val ox = x0 + k * ddx
            val oy = mid - k * ddy
            val base = k * N
            p.moveTo(ox, oy - frames[base].coerceIn(-1f, 1f) * amp)
            for (i in 1 until N) p.lineTo(ox + i * dx, oy - frames[base + i].coerceIn(-1f, 1f) * amp)
        }
    }

    // ------------------------------------------------------------------ draw

    override fun onDraw(c: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        val rad = 8f * d
        well.set(d / 2f, d / 2f, w - d / 2f, h - d / 2f)
        wellP.style = Paint.Style.FILL
        wellP.color = Hw.WELL
        c.drawRoundRect(well, rad, rad, wellP)
        wellP.style = Paint.Style.STROKE
        wellP.strokeWidth = d
        wellP.color = Hw.PANEL_EDGE
        c.drawRoundRect(well, rad, rad, wellP)

        if (dirty) rebuildPaths()
        val x0 = PAD_DP * d
        val top = PAD_DP * d + GHOSTS * GHOST_DY_DP * d
        val bottom = h - 22f * d
        val mid = (top + bottom) / 2f
        val ww = (w - 2f * x0 - GHOSTS * GHOST_DX_DP * d).coerceAtLeast(1f)

        // zero line of the current frame
        c.drawLine(x0, mid, x0 + ww, mid, gridP)

        // ghosts, farthest first
        for (k in GHOSTS downTo 1) {
            ghostP.color = ghostCol[k - 1]
            c.drawPath(paths[k], ghostP)
        }
        // current frame: soft glow + bright line
        glowP.color = Hw.withAlpha(accent, 46)
        c.drawPath(paths[0], glowP)
        mainP.color = accent
        c.drawPath(paths[0], mainP)

        // position track (bottom) + thumb
        val ty = h - 17f * d
        val tx0 = x0
        val tx1 = w - x0
        trackP.color = Hw.PANEL_EDGE
        c.drawLine(tx0, ty, tx1, ty, trackP)
        val thumbX = tx0 + (tx1 - tx0) * pos
        trackP.color = Hw.withAlpha(accent, 200)
        c.drawLine(tx0, ty, thumbX, ty, trackP)
        trackP.color = Hw.SILK
        c.drawCircle(thumbX, ty, 2.6f * d, trackP)

        // warp meter (right edge, fills upward)
        val wx = w - 5f * d
        val wTop = 12f * d
        val wBot = ty - 6f * d
        trackP.color = Hw.PANEL_EDGE
        c.drawLine(wx, wTop, wx, wBot, trackP)
        trackP.color = Hw.withAlpha(ghostAccent, 220)
        c.drawLine(wx, wBot, wx, wBot - (wBot - wTop) * warp, trackP)

        // readouts
        txtP.textAlign = Paint.Align.LEFT
        txtP.color = if (axis == AXIS_X) Hw.SILK else Hw.ENGRAVE
        c.drawText(posText, x0, h - 5f * d, txtP)
        txtP.textAlign = Paint.Align.RIGHT
        txtP.color = if (axis == AXIS_Y) Hw.SILK else Hw.ENGRAVE
        c.drawText(warpText, w - x0 - 6f * d, h - 5f * d, txtP)
    }

    // ------------------------------------------------------------------ touch

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                parent?.requestDisallowInterceptTouchEvent(true)
                History.checkpoint()
                downX = e.x; downY = e.y
                downPos = pos; downWarp = warp
                axis = AXIS_NONE
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                val dx = e.x - downX
                val dy = e.y - downY
                if (axis == AXIS_NONE) {
                    if (abs(dx) < slop && abs(dy) < slop) return true
                    axis = if (abs(dx) >= abs(dy)) AXIS_X else AXIS_Y
                }
                if (axis == AXIS_X) {
                    val span = (width - 2f * PAD_DP * d).coerceAtLeast(1f)
                    setPos(downPos + dx / span)
                } else {
                    val span = (height * 0.8f).coerceAtLeast(1f)
                    setWarp(downWarp - dy / span)   // up = more
                }
                return true
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                parent?.requestDisallowInterceptTouchEvent(false)
                axis = AXIS_NONE
                emit(force = true)
                invalidate()
                return true
            }
        }
        return super.onTouchEvent(e)
    }

    private fun setPos(v: Float) {
        val nv = v.coerceIn(0f, 1f)
        if (nv == pos) return
        pos = nv
        Params.set(posName, nv)
        updateTexts()
        fetchFrames()
        Hw.onParamTouched?.invoke("WT POS", String.format(Locale.US, "%.3f", nv))
        emit(force = false)
        invalidate()
    }

    private fun setWarp(v: Float) {
        val nv = v.coerceIn(0f, 1f)
        if (nv == warp) return
        warp = nv
        Params.set(warpName, nv)
        updateTexts()
        Hw.onParamTouched?.invoke("WT WARP", String.format(Locale.US, "%.3f", nv))
        emit(force = false)
        invalidate()
    }

    private fun emit(force: Boolean) {
        val now = SystemClock.uptimeMillis()
        if (!force && now - lastEmit < EMIT_MS) return
        lastEmit = now
        onEdited?.invoke()
    }

    companion object {
        const val HEIGHT_DP = 124
        const val N = 256               // points per frame
        const val GHOSTS = 5
        private const val GHOST_STEP = 0.07f
        private const val GHOST_DX_DP = 5f
        private const val GHOST_DY_DP = 6f
        private const val PAD_DP = 8f
        private const val EMIT_MS = 40L
        private const val AXIS_NONE = 0
        private const val AXIS_X = 1
        private const val AXIS_Y = 2
    }
}

// ====================================================================================================== browser bar

/**
 * Compact panel for EDIT > OSC when layer [layer] (0..2 = A..C) is a WT layer: the [WavetableRibbonView] on top, then a
 * horizontally scrolling row of table chips — the built-in tables (`basic`, `pwm`, `harmonics`, …) followed by the
 * `.wt` / `.vitaltable` / `.wav` files in `files/library/user/wt/` — and `+ IMPORT .WT / .VITAL / .WAV`.
 *
 * Built-in chips just set `<l>.wt.table` (undoable); user chips load the file through
 * [FormatBridge.loadAnyWavetable] on `a.loader`; long-press a user chip to delete the file.
 * Call [refresh] from the EDIT page's `refreshValues()` so the ribbon follows MIDI / modulation edits; set [onEdited]
 * to refresh the knobs below when the ribbon is dragged.
 */
class WavetableBrowserBar(
    private val a: MainActivity,
    private val layer: Int,
    private val bridge: FormatBridge,
) : LinearLayout(a) {
    /** Fired after the bar changed parameters (ribbon drag, table chip, import). */
    var onEdited: (() -> Unit)? = null

    val ribbon = WavetableRibbonView(a)

    private val d = resources.displayMetrics.density
    private fun dp(v: Int) = (v * d).toInt()
    private val prefix = "abc"[layer.coerceIn(0, 2)].toString()
    private val chipRow = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
    private val builtinNames: List<String> by lazy {
        val opts = Params.metaOf("$prefix.wt.table")?.options?.filterNot { it.equals("user", ignoreCase = true) }.orEmpty()
        opts.ifEmpty { listOf("basic", "pwm", "harmonics", "formant", "sync", "fold", "digital", "organ") }
    }
    private val provider = WtPreviewProvider { p, out -> runCatching { bridge.wavetablePreview(layer, p, out) }.getOrDefault(false) }
    private var userFiles: List<File> = emptyList()
    private var activeUser: String? = null   // file name of the user table loaded through this bar
    private var chips: List<LedButton> = emptyList()
    private var busy = false

    init {
        orientation = VERTICAL
        ribbon.accent = Hw.MOD_OSC
        ribbon.onEdited = { onEdited?.invoke() }
        addView(ribbon, LayoutParams(-1, dp(WavetableRibbonView.HEIGHT_DP)))

        val scroll = HorizontalScrollView(a).apply {
            isHorizontalScrollBarEnabled = false
            overScrollMode = View.OVER_SCROLL_NEVER
            addView(chipRow)
        }
        addView(scroll, LayoutParams(-1, dp(36)).apply { topMargin = dp(6) })
        rebuildChips()
        refresh()
    }

    /** Re-read position / warp / table from the parameters and redraw (UI thread). */
    fun refresh() {
        ribbon.refresh(Params.get("$prefix.wt.pos"), Params.get("$prefix.wt.warpamt"), prefix, provider)
        updateChipState()
    }

    // ------------------------------------------------------------------ chips

    private fun scanUserFiles() {
        val dir = FormatFiles.userDir(a, FormatKind.WT)
        userFiles = dir.listFiles { f -> f.isFile && f.extension.lowercase(Locale.ROOT) in FormatKind.WT.exts }
            ?.sortedBy { it.name.lowercase(Locale.ROOT) } ?: emptyList()
    }

    private fun rebuildChips() {
        scanUserFiles()
        chipRow.removeAllViews()
        val list = ArrayList<LedButton>()
        builtinNames.forEachIndexed { i, n ->
            val b = chip(n, Hw.MOD_OSC) { pickBuiltin(i) }
            list += b
            chipRow.addView(b, chipLp())
        }
        userFiles.forEach { f ->
            val b = chip(f.nameWithoutExtension, Hw.MOD_ENV) { pickUser(f) }
            b.setOnLongClickListener { confirmDelete(f); true }
            list += b
            chipRow.addView(b, chipLp())
        }
        chips = list
        chipRow.addView(
            LedButton(a, "+ IMPORT .WT / .VITAL / .WAV", tr = false, led = false, accentColor = Hw.MOD_FX).apply {
                textSize = 12f; minHeight = 0; minimumHeight = 0; setPadding(dp(12), 0, dp(12), 0)
                lit = true
                setOnClickListener { pickImport() }
            }, LayoutParams(-2, dp(30)).apply { marginStart = dp(6) }
        )
        updateChipState()
    }

    private fun chip(label: String, accent: Int, onClick: () -> Unit) =
        LedButton(a, label, tr = false, led = false, accentColor = accent).apply {
            textSize = 12f; minHeight = 0; minimumHeight = 0; setPadding(dp(11), 0, dp(11), 0)
            setOnClickListener { onClick() }
        }

    private fun chipLp() = LayoutParams(-2, dp(30)).apply { marginEnd = dp(4) }

    private fun updateChipState() {
        if (chips.isEmpty()) return
        val meta = Params.metaOf("$prefix.wt.table")
        val cur = Params.get("$prefix.wt.table").toInt()
        val onUser = meta != null && cur == meta.options.size - 1
        val loaded = runCatching { NativeEngine.nativeSampleSetName(layer) }.getOrDefault("")
        for (i in chips.indices) {
            chips[i].lit = if (i < builtinNames.size) {
                !onUser && cur == i
            } else {
                val f = userFiles.getOrNull(i - builtinNames.size)
                onUser && f != null && (f.name == activeUser || (activeUser == null && loaded.isNotEmpty() &&
                    (loaded.equals(f.name, true) || loaded.equals(f.nameWithoutExtension, true))))
            }
        }
    }

    // ------------------------------------------------------------------ actions

    private fun pickBuiltin(i: Int) {
        History.checkpoint()
        Params.set("$prefix.wt.table", i.toFloat())
        activeUser = null
        FormatFiles.auditionStop(a)
        refresh()
        onEdited?.invoke()
    }

    private fun pickUser(f: File) {
        if (busy) return
        busy = true
        FormatFiles.loadWavetable(a, bridge, layer, f, rebuildPage = false) { ok ->
            busy = false
            if (ok) activeUser = f.name
            refresh()
            onEdited?.invoke()
        }
    }

    private fun confirmDelete(f: File) {
        AlertDialog.Builder(a)
            .setTitle(L10n.f("Delete %s?", f.name))
            .setMessage(L10n.t("This removes the file from the app library."))
            .setPositiveButton(L10n.t("Delete")) { _, _ ->
                if (f.delete()) a.log("deleted ${f.name}") else a.log("could not delete ${f.name}", Ui.ERR)
                if (activeUser == f.name) activeUser = null
                rebuildChips()
            }
            .setNegativeButton(L10n.t("Cancel"), null)
            .show()
    }

    private fun pickImport() {
        pendingBar = WeakReference(this)
        ActivityResults.register(FormatFiles.REQ_WT_IMPORT) { data -> pendingBar?.get()?.onPicked(data) }
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"   // .wt / .vitaltable have no registered MIME type
        }
        @Suppress("DEPRECATION")
        a.startActivityForResult(intent, FormatFiles.REQ_WT_IMPORT)
    }

    private fun onPicked(data: Intent?) {
        val uri = data?.data ?: return
        if (busy) return
        busy = true
        a.log("importing wavetable…")
        a.loader.execute {
            try {
                val f = FormatFiles.importSingle(a, uri, FormatKind.WT)
                a.ui.post {
                    if (a.isDestroyed) return@post
                    rebuildChips()
                    busy = false
                    pickUser(f)
                }
            } catch (e: Exception) {
                a.ui.post {
                    busy = false
                    a.log("wavetable import failed: ${e.message}", Ui.ERR)
                }
            }
        }
    }

    private companion object {
        /** The bar that started the picker (one result handler is shared by A / B / C bars). */
        var pendingBar: WeakReference<WavetableBrowserBar>? = null
    }
}
