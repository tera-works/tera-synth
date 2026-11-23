package com.hsynth.app

import android.annotation.SuppressLint
import android.app.AlertDialog
import android.graphics.drawable.GradientDrawable
import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.PopupWindow
import android.widget.TextView
import java.util.Random
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

/**
 * Phase 4 B5 — Smart Patch Mutate / Init. Pure Kotlin on top of `Params.meta`, `NativeEngine.nativeGetParam /
 * setParam` and `History` (no new JNI). The random walk itself ([PatchMutate.mutateParams]) only talks to two lambdas,
 * so it can be unit-tested on the JVM.
 */
enum class MutateScope { PAGE, PATCH }

object PatchMutate {
    /** Never mutated (structure, routing, sequencer data, global performance settings, sample / IR selections …). */
    private val EXCLUDE = Regex(
        """\.type$|\.on$|^fx\.slot|\.fm\.algo$|\.wt\.table$|^voice\.mode$|^tune\.|^perf\.|^mix\.|^master\.|^seq\.s|^mseg\.p|^morph\.""" +
            // also: sample / IR selections, transport-ish and per-note-structural params
            """|\.sample|\.impulse|\.ir$|\.pcm\.start$|\.coarse$|^looper\.|^rhythm\.|^arp\.pat\.|^fx\.tempo$"""
    )

    /** Global / performance settings that "Init" must not touch either (they are not part of the sound). */
    private val INIT_KEEP = Regex("""^tune\.|^perf\.|^master\.""")

    /** Does [group] belong to one of [groups]? A group also owns its `"<group> / <engine>"` sub-groups. */
    fun inGroups(group: String, groups: Set<String>): Boolean =
        groups.any { group == it || group.startsWith("$it / ") }

    fun isExcluded(name: String): Boolean = EXCLUDE.containsMatchIn(name)

    /**
     * Parameters a mutation may touch: not excluded, in [groups] (`null` = whole patch), and — so the amount is not
     * diluted by things that make no sound — not belonging to an `off` layer or to an engine the layer is not using.
     */
    fun eligible(groups: Set<String>?): List<ParamMeta> {
        val dead = HashSet<String>()   // lower-case group names that are silent right now
        for (l in LAYERS) {
            val t = Params.metaOf("$l.type") ?: continue
            val cur = t.options.getOrNull(NativeEngine.nativeGetParam(t.id).roundToInt())?.lowercase() ?: continue
            val g = "layer $l"
            if (cur == "off") {
                dead += g
                for (o in t.options) dead += "$g / ${o.lowercase()}"
            } else {
                for (o in t.options) if (!o.equals(cur, ignoreCase = true)) dead += "$g / ${o.lowercase()}"
            }
        }
        val out = ArrayList<ParamMeta>()
        for (m in Params.meta) {
            if (isExcluded(m.name)) continue
            if (groups != null && !inGroups(m.group, groups)) continue
            if (m.group.lowercase() in dead) continue
            out += m
        }
        return out
    }

    /** σ of the normalised random walk: 0.25 @ 10 %, 0.30 @ 25 %, 0.35 @ 50 % (linear in between, clamped outside). */
    fun sigmaFor(amount: Float): Float = when {
        amount <= 0.10f -> 0.25f
        amount <= 0.25f -> 0.25f + 0.05f * (amount - 0.10f) / 0.15f
        amount <= 0.50f -> 0.30f + 0.05f * (amount - 0.25f) / 0.25f
        else -> 0.35f
    }

    /**
     * Mutate [groups] (`null` = whole patch) by [amount] (0.10 / 0.25 / 0.50 = share of eligible continuous params
     * touched). Takes one `History.checkpoint()` first. Does NOT call `Params.notifyChanged()` (the caller does,
     * once). @return number of parameters whose value actually changed.
     */
    fun mutate(groups: Set<String>?, amount: Float, seed: Long): Int {
        val metas = eligible(groups)
        if (metas.isEmpty()) return 0
        History.checkpoint()
        return mutateParams(
            metas, { NativeEngine.nativeGetParam(it.id) }, { m, v -> NativeEngine.setParam(m.id, v) }, amount, seed
        )
    }

    /**
     * The random walk, free of Android / JNI: reads through [read], writes through [write], returns the number of
     * changed values.
     *  - float / int: a random [amount] share of them is moved by N(0, σ) in normalised (curve-aware) space
     *    (ints: ± σ·range, at least one step);
     *  - enum: each changes with probability `0.3·amount` (neighbour or random other option);
     *  - bool: each flips with probability `0.15·amount`.
     * Safe clamps: `amp.sustain ≥ 0.25`, `*.level ≥ 0.2`, `filter.reso ≤ 0.85`, `filter.cutoff` stays within ±0.35
     * (normalised) of its original unless [amount] is 50 % or more.
     */
    fun mutateParams(
        metas: List<ParamMeta>,
        read: (ParamMeta) -> Float,
        write: (ParamMeta, Float) -> Unit,
        amount: Float,
        seed: Long,
    ): Int {
        val rnd = Random(seed)
        val sigma = sigmaFor(amount)
        var changed = 0

        val pool = metas.filter { (it.kind == "float" || it.kind == "int") && it.max > it.min }.toMutableList()
        pool.shuffle(rnd)
        val take = if (pool.isEmpty()) 0 else (amount * pool.size).roundToInt().coerceIn(1, pool.size)
        for (i in 0 until take) {
            val m = pool[i]
            val cur = read(m)
            val nv = mutateContinuous(m, cur, sigma, amount, rnd)
            if (abs(nv - cur) > 1e-6f) { write(m, nv); changed++ }
        }

        for (m in metas) {
            val p = when (m.kind) { "enum" -> 0.3f * amount; "bool" -> 0.15f * amount; else -> continue }
            if (rnd.nextFloat() >= p) continue
            val cur = read(m)
            val nv = if (m.kind == "bool") {
                if (cur > 0.5f) 0f else 1f
            } else {
                val n = m.options.size
                if (n < 2) continue
                val c = cur.roundToInt().coerceIn(0, n - 1)
                if (rnd.nextBoolean()) {
                    (if (c == 0) 1 else if (c == n - 1) n - 2 else if (rnd.nextBoolean()) c + 1 else c - 1).toFloat()
                } else {
                    val o = rnd.nextInt(n - 1)          // random option ≠ current
                    (if (o >= c) o + 1 else o).toFloat()
                }
            }
            if (nv != cur) { write(m, nv); changed++ }
        }
        return changed
    }

    private fun mutateContinuous(m: ParamMeta, cur: Float, sigma: Float, amount: Float, rnd: Random): Float {
        var v: Float
        if (m.kind == "int") {
            v = (cur + rnd.nextGaussian().toFloat() * sigma * (m.max - m.min)).roundToInt().toFloat().coerceIn(m.min, m.max)
            if (v == cur) v = (cur + if (rnd.nextBoolean()) 1f else -1f).coerceIn(m.min, m.max)
        } else {
            val n0 = m.toNormalized(cur)
            var n = (n0 + rnd.nextGaussian().toFloat() * sigma).coerceIn(0f, 1f)
            if ((m.name == "filter.cutoff" || m.name == "filter2.cutoff") && amount < 0.499f) {
                n = n.coerceIn(max(0f, n0 - 0.35f), min(1f, n0 + 0.35f))
            }
            v = m.fromNormalized(n)
        }
        if (m.name == "amp.sustain" && m.max >= 0.25f) v = max(v, 0.25f)
        if ((m.name.endsWith(".level") || m.name == "level") && m.max >= 0.2f) v = max(v, 0.2f)
        if ((m.name == "filter.reso" || m.name == "filter2.reso") && m.min <= 0.85f) v = min(v, 0.85f)
        return v.coerceIn(m.min, m.max)
    }

    /**
     * Reset every parameter of [groups] (`null` = whole patch, sub-groups included) to `meta.def`, after one
     * `History.checkpoint()`. Global settings (`tune.` / `perf.` / `master.`) are left alone. Does NOT call
     * `Params.notifyChanged()`. @return number of parameters that changed.
     */
    fun initGroups(groups: Set<String>?): Int {
        val metas = Params.meta.filter { m ->
            (groups == null || inGroups(m.group, groups)) && !INIT_KEEP.containsMatchIn(m.name)
        }
        if (metas.isEmpty()) return 0
        History.checkpoint()
        var changed = 0
        for (m in metas) {
            val cur = NativeEngine.nativeGetParam(m.id)
            if (abs(cur - m.def) > 1e-6f) { NativeEngine.setParam(m.id, m.def); changed++ }
        }
        return changed
    }

    private val LAYERS = arrayOf("a", "b", "c")
}

// ====================================================================================================== bar

/**
 * What the [MutateBar] shows after an action. [again] (mutate only) undoes and re-rolls, returning the new outcome.
 */
class MutateOutcome(val label: String, val count: Int, val again: (() -> MutateOutcome)? = null)

/**
 * 28 dp black pill pinned above the quick strip: `MUTATED 25% · 14 params  [UNDO] [AGAIN] [KEEP]`
 * (`INIT PAGE · 31 params  [UNDO] [KEEP]` after an init). UNDO = `History.undo()`; AGAIN = undo + new seed;
 * KEEP — or 8 s without a tap — dismisses. Starts `GONE`; the host adds it to a FrameLayout with [barLayoutParams].
 */
@SuppressLint("ViewConstructor")
class MutateBar(a: MainActivity) : LinearLayout(a) {
    private val d = resources.displayMetrics.density
    private val label = TextView(a).apply {
        textSize = 11.5f
        typeface = Hw.BOLD
        setTextColor(Hw.SILK)
        maxLines = 1
        setPadding((12 * d).toInt(), 0, (4 * d).toInt(), 0)
    }
    private val undoBtn = chip(a, "UNDO", Hw.MOD_FILTER) {
        History.undo()
        dismiss()
    }
    private val againBtn = chip(a, "AGAIN", Hw.MOD_OSC) {
        val next = current?.again?.invoke()
        if (next != null) show(next) else dismiss()
    }
    private val keepBtn = chip(a, "KEEP", Hw.MOD_GOOD) { dismiss() }
    private val hide = Runnable { dismiss() }
    private var current: MutateOutcome? = null

    init {
        orientation = HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        visibility = GONE
        isClickable = true   // never let a tap fall through to the page underneath
        background = GradientDrawable().apply {
            setColor(Hw.WELL)
            cornerRadius = 14f * d
            setStroke(d.toInt().coerceAtLeast(1), Hw.PANEL_EDGE)
        }
        addView(label, LayoutParams(-2, -1))
        addView(undoBtn, LayoutParams(-2, -1))
        addView(againBtn, LayoutParams(-2, -1))
        addView(keepBtn, LayoutParams(-2, -1).apply { marginEnd = (4 * d).toInt() })
    }

    private fun chip(a: MainActivity, text: String, color: Int, onClick: () -> Unit) = TextView(a).apply {
        this.text = text
        textSize = 11f
        typeface = Hw.BOLD
        letterSpacing = 0.05f
        gravity = Gravity.CENTER
        setTextColor(color)
        setPadding((9 * d).toInt(), 0, (9 * d).toInt(), 0)
        setOnClickListener { onClick() }
    }

    /** Show [o] and (re)start the 8 s auto-dismiss. */
    fun show(o: MutateOutcome) {
        current = o
        label.text = o.label
        againBtn.visibility = if (o.again != null) VISIBLE else GONE
        visibility = VISIBLE
        removeCallbacks(hide)
        postDelayed(hide, AUTO_DISMISS_MS)
    }

    fun dismiss() {
        removeCallbacks(hide)
        current = null
        visibility = GONE
    }

    override fun onDetachedFromWindow() {
        removeCallbacks(hide)
        super.onDetachedFromWindow()
    }

    companion object {
        const val AUTO_DISMISS_MS = 8000L

        /** Bottom-centre, just above the 44 dp EDIT quick strip. */
        fun barLayoutParams(a: MainActivity): FrameLayout.LayoutParams {
            val d = a.resources.displayMetrics.density
            return FrameLayout.LayoutParams(-2, (28 * d).toInt(), Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL)
                .apply { bottomMargin = (52 * d).toInt() }
        }
    }
}

// ====================================================================================================== popup

private const val PREF_SCOPE = "mutateScope"

private fun runMutate(groups: Set<String>?, amount: Float, seed: Long): MutateOutcome {
    val n = PatchMutate.mutate(groups, amount, seed)
    Params.notifyChanged()
    val pct = (amount * 100f).roundToInt()
    return MutateOutcome("MUTATED $pct% · $n ${L10n.t("params")}", n) {
        History.undo()
        runMutate(groups, amount, System.nanoTime() xor seed * 31L)
    }
}

private fun runInit(groups: Set<String>?, title: String): MutateOutcome {
    val n = PatchMutate.initGroups(groups)
    Params.notifyChanged()
    return MutateOutcome("$title · $n ${L10n.t("params")}", n)
}

/**
 * The EDIT toolbar `⋯` popup (260 × 220 dp, black, 1 dp edge):
 * ```
 * SCOPE  [PAGE | PATCH]                    (pref "mutateScope"; PAGE = [pageGroups], e.g. the selected layer on OSC)
 * MUTATE [10 %] [25 %] [50 %]
 * INIT   ▸ Init this page   ▸ Init layer A (only when [layerPrefix] != null)   ▸ Init whole patch… (confirm)
 * ```
 * Every action is applied immediately (one history checkpoint, one `Params.notifyChanged()`), the popup closes, and
 * [onApplied] receives the [MutateOutcome] to hand to a [MutateBar]. An empty [pageGroups] makes PAGE behave like
 * PATCH and hides "Init this page".
 */
fun showMutatePopup(
    a: MainActivity,
    anchor: View,
    pageGroups: Set<String>,
    layerPrefix: String?,
    onApplied: (MutateOutcome) -> Unit,
): PopupWindow {
    val d = a.resources.displayMetrics.density
    fun dp(v: Int) = (v * d).toInt()

    var scope = MutateScope.values()[a.prefs.getInt(PREF_SCOPE, 0).coerceIn(0, MutateScope.values().size - 1)]
    val root = LinearLayout(a).apply {
        orientation = LinearLayout.VERTICAL
        setPadding(dp(8), dp(8), dp(8), dp(8))
    }
    val pw = PopupWindow(root, dp(260), dp(220), true).apply {
        setBackgroundDrawable(Hw.panelBg(a, Hw.WELL, Hw.PANEL_EDGE, 8f))
        isOutsideTouchable = true
        elevation = 8f * d
    }

    fun scopeGroups(): Set<String>? = if (scope == MutateScope.PAGE && pageGroups.isNotEmpty()) pageGroups else null
    fun finish(o: MutateOutcome) { pw.dismiss(); onApplied(o) }

    // scope
    val scopeRow = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
    scopeRow.addView(Hw.engraved(a, "Scope", 10f), LinearLayout.LayoutParams(dp(54), -2))
    val seg = SegRow(a, arrayOf(L10n.t("PAGE"), L10n.t("PATCH")), Hw.MOD_OSC) { i ->
        scope = MutateScope.values()[i]
        a.prefs.edit().putInt(PREF_SCOPE, i).apply()
    }
    seg.selected = scope.ordinal
    scopeRow.addView(seg, LinearLayout.LayoutParams(0, dp(30), 1f))
    root.addView(scopeRow, LinearLayout.LayoutParams(-1, dp(30)))

    // mutate amounts
    root.addView(
        Hw.engraved(a, "Mutate", 10f),
        LinearLayout.LayoutParams(-2, dp(16)).apply { topMargin = dp(6) }
    )
    val amounts = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL }
    for ((i, pct) in intArrayOf(10, 25, 50).withIndex()) {
        val b = LedButton(a, "$pct %", tr = false, led = false, accentColor = Hw.MOD_OSC).apply {
            textSize = 15f
            minHeight = 0
            minimumHeight = 0
            setOnClickListener { finish(runMutate(scopeGroups(), pct / 100f, System.nanoTime())) }
        }
        amounts.addView(b, LinearLayout.LayoutParams(0, -1, 1f).apply { if (i < 2) marginEnd = dp(6) })
    }
    root.addView(amounts, LinearLayout.LayoutParams(-1, dp(40)))

    // init rows
    root.addView(
        Hw.engraved(a, "Init", 10f),
        LinearLayout.LayoutParams(-2, dp(16)).apply { topMargin = dp(8) }
    )
    fun row(text: String, onClick: () -> Unit) {
        val t = TextView(a).apply {
            this.text = text
            textSize = 13f
            typeface = Hw.BOLD
            setTextColor(Hw.SILK)
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(10), 0, dp(10), 0)
            background = Hw.keyBg(a)
            isClickable = true
            setOnClickListener { onClick() }
        }
        root.addView(t, LinearLayout.LayoutParams(-1, dp(28)).apply { topMargin = dp(3) })
    }
    if (pageGroups.isNotEmpty()) {
        row("▸ " + L10n.t("Init this page")) { finish(runInit(pageGroups, "INIT PAGE")) }
    }
    if (layerPrefix != null) {
        val letter = layerPrefix.uppercase()
        row("▸ " + L10n.t("Init layer") + " $letter") {
            finish(runInit(setOf("Layer $letter"), "INIT LAYER $letter"))
        }
    }
    row("▸ " + L10n.t("Init whole patch") + "…") {
        AlertDialog.Builder(a)
            .setTitle(L10n.t("Init whole patch"))
            .setMessage(L10n.t("Reset every sound parameter to its default? You can undo this."))
            .setNegativeButton(android.R.string.cancel, null)
            .setPositiveButton(android.R.string.ok) { _, _ -> finish(runInit(null, "INIT PATCH")) }
            .show()
    }

    pw.showAsDropDown(anchor, 0, dp(4))
    return pw
}
