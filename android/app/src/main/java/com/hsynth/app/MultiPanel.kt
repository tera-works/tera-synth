package com.hsynth.app

import android.annotation.SuppressLint
import android.app.AlertDialog
import android.content.Context
import android.view.GestureDetector
import android.view.Gravity
import android.view.MotionEvent
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import kotlin.math.roundToInt

/** LCD number: drag up / down to change, double tap for the default. */
@SuppressLint("ViewConstructor")
class DragNumber(ctx: Context, private val min: Float, private val max: Float, private val step: Float, private val def: Float,
                 private val format: (Float) -> String, private val get: () -> Float, private val set: (Float) -> Unit) : TextView(ctx) {
    private val d = resources.displayMetrics.density
    private var downY = 0f
    private var downV = 0f
    private val gestures = GestureDetector(ctx, object : GestureDetector.SimpleOnGestureListener() {
        override fun onDoubleTap(e: MotionEvent): Boolean { set(def); refresh(); return true }
    })

    init {
        textSize = 13f
        setTextColor(Hw.LCD_FG)
        typeface = Hw.MONO
        background = Hw.lcdBg(ctx)
        gravity = Gravity.CENTER
        setPadding((4 * d).toInt(), (6 * d).toInt(), (4 * d).toInt(), (6 * d).toInt())
        maxLines = 1
        refresh()
    }

    fun refresh() { text = format(get()) }

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        gestures.onTouchEvent(e)
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> { parent?.requestDisallowInterceptTouchEvent(true); downY = e.y; downV = get() }
            MotionEvent.ACTION_MOVE -> {
                val steps = ((downY - e.y) / (10 * d)).toInt()
                val v = (downV + steps * step).coerceIn(min, max)
                if (v != get()) { set(v); refresh() }
            }
        }
        return true
    }
}

/**
 * Multi: 16 parts (sound, MIDI channel, key range, transpose, level, pan,
 * mute, effect send), the part being edited, and 8 scenes (whole multis).
 * Parts on the same channel layer; different key ranges split.
 */
class MultiPanel(private val a: MainActivity) {
    val view: LinearLayout
    private val refreshers = mutableListOf<() -> Unit>()
    private val sceneButtons = mutableListOf<LedButton>()
    private val names = Array(8) { "" }
    private var storeMode = false
    private val sceneDir = File(a.filesDir, "scenes").apply { mkdirs() }
    private var currentScene = -1
    private lateinit var multiSwitch: ToggleSwitch
    private lateinit var storeBtn: LedButton

    private val noteNames = arrayOf("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
    private fun note(v: Float) = v.roundToInt().let { noteNames[it % 12] + (it / 12 - 1) }

    init {
        val d = { v: Int -> Ui.dp(a, v) }
        // mode + scenes
        val mode = Hw.Section(a, "Multi")
        multiSwitch = ToggleSwitch(a, "Multi Mode", vertical = false).apply {
            onToggle = { on -> NativeEngine.nativeSetMulti(on); if (!on) a.setEditPart(0); refresh() }
        }
        mode.body.addView(multiSwitch, LinearLayout.LayoutParams(-2, -2))
        mode.body.addView(Ui.text(a, "Multi mode: 16 parts, layered by MIDI channel and split by key range. The on-screen keyboard plays channel 1.", 12f, Ui.DIM),
            LinearLayout.LayoutParams(-1, -2).apply { topMargin = d(4) })

        val scenes = Hw.Section(a, "Scenes")
        val row = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL }
        for (i in 0 until 8) {
            val b = LedButton(a, "${i + 1}", tr = false).apply { textSize = 13f; setOnClickListener { sceneTapped(i) } }
            sceneButtons += b
            row.addView(b, LinearLayout.LayoutParams(0, -2, 1f).apply { marginEnd = d(4) })
        }
        storeBtn = LedButton(a, "Store").apply {
            textSize = 13f
            setOnClickListener { storeMode = !storeMode; lit = storeMode }
        }
        row.addView(storeBtn, LinearLayout.LayoutParams(-2, -2))
        scenes.body.addView(row)
        scenes.body.addView(Ui.text(a, "Tap a scene to recall it. Store, then a scene, saves the current multi there.", 12f, Ui.DIM),
            LinearLayout.LayoutParams(-1, -2).apply { topMargin = d(4) })

        val top = LinearLayout(a).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(mode, LinearLayout.LayoutParams(0, -1, 1f).apply { marginEnd = d(8) })
            addView(scenes, LinearLayout.LayoutParams(0, -1, 1.6f))
        }

        // parts table
        val parts = Hw.Section(a, "Parts")
        val head = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL }
        val cols = listOf("Edit" to 52, "Sound" to 0, "Ch" to 54, "Low" to 58, "High" to 58, "Trans" to 58, "Vol" to 58, "Pan" to 58, "Send" to 58, "Mute" to 56)
        for ((t, w) in cols) head.addView(Hw.engraved(a, t, 10.5f).apply { gravity = Gravity.CENTER },
            if (w > 0) LinearLayout.LayoutParams(d(w), -2).apply { marginEnd = d(4) } else LinearLayout.LayoutParams(0, -2, 1f).apply { marginEnd = d(4) })
        parts.body.addView(head, LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = d(4) })
        for (k in 0 until 16) parts.body.addView(partRow(k), LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = d(4) })

        val col = Ui.column(a).apply {
            setPadding(d(10), d(8), d(10), d(16))
            addView(top, Hw.sectionLp(a))
            addView(parts, Hw.sectionLp(a))
        }
        view = Ui.column(a).apply { addView(ScrollView(a).apply { addView(col) }, LinearLayout.LayoutParams(-1, -1)) }
        loadSceneNames()
    }

    private fun mix(k: Int, id: Int) = NativeEngine.nativeGetPartMix(k, id)
    private fun setMix(k: Int, id: Int, v: Float) = NativeEngine.nativeSetPartMix(k, id, v)

    private fun partRow(k: Int): LinearLayout {
        val d = { v: Int -> Ui.dp(a, v) }
        val row = LinearLayout(a).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        val edit = LedButton(a, "${k + 1}", tr = false).apply { textSize = 13f; setOnClickListener { a.setEditPart(k); refresh() } }
        row.addView(edit, LinearLayout.LayoutParams(d(52), -2).apply { marginEnd = d(4) })
        val sound = Hw.lcd(a, "", 12f).apply { setOnClickListener { choosePreset(k) } }
        row.addView(sound, LinearLayout.LayoutParams(0, -2, 1f).apply { marginEnd = d(4) })
        fun num(id: Int, w: Int, min: Float, max: Float, step: Float, def: Float, fmt: (Float) -> String) =
            DragNumber(a, min, max, step, def, fmt, { mix(k, id) }, { setMix(k, id, it) }).also {
                row.addView(it, LinearLayout.LayoutParams(d(w), -2).apply { marginEnd = d(4) })
            }
        val nums = listOf(
            num(3, 54, 1f, 16f, 1f, (k + 1).toFloat()) { "${it.roundToInt()}" },            // Mix::Channel
            num(4, 58, 0f, 127f, 1f, 0f) { note(it) },                                      // KeyLow
            num(5, 58, 0f, 127f, 1f, 127f) { note(it) },                                    // KeyHigh
            num(6, 58, -48f, 48f, 1f, 0f) { v -> v.roundToInt().let { if (it > 0) "+$it" else "$it" } },  // Transpose
            num(0, 58, 0f, 1f, 0.02f, 1f) { "${(it * 100).roundToInt()}" },                 // Volume
            num(1, 58, -1f, 1f, 0.05f, 0f) { v -> (v * 50).roundToInt().let { if (it == 0) "C" else if (it < 0) "L${-it}" else "R$it" } },  // Pan
            num(7, 58, 0f, 1f, 0.05f, 1f) { "${(it * 100).roundToInt()}" },                 // FxSend
        )
        val mute = ToggleSwitch(a, "", vertical = false).apply { onToggle = { on -> setMix(k, 2, if (on) 1f else 0f) } }
        row.addView(mute, LinearLayout.LayoutParams(d(56), -2))
        refreshers += {
            val multi = NativeEngine.nativeIsMulti()
            edit.lit = NativeEngine.nativeEditPart() == k
            sound.text = a.partNames[k].ifEmpty { "—" }
            nums.forEach { it.refresh() }
            mute.on = mix(k, 2) > 0.5f
            row.alpha = if (multi || k == 0) 1f else 0.4f
        }
        return row
    }

    private fun choosePreset(k: Int) {
        val list = a.libraryPanel.presetEntries()
        AlertDialog.Builder(a).setTitle(L10n.f("Sound for part %d", k + 1))
            .setItems(list.map { it.title }.toTypedArray()) { _, w ->
                if (!NativeEngine.nativeIsMulti() && k > 0) { NativeEngine.nativeSetMulti(true) }
                a.setEditPart(k)
                a.loadPreset(list[w].file) { refresh() }
            }.show()
    }

    // ------------------------------------------------------------ scenes

    private fun loadSceneNames() {
        for (i in 0 until 8) {
            val meta = File(sceneDir, "scene${i + 1}.names.json")
            names[i] = runCatching { JSONObject(meta.readText()).optString("name") }.getOrDefault("")
        }
        updateScenes()
    }

    private fun updateScenes() {
        sceneButtons.forEachIndexed { i, b ->
            b.text = if (names[i].isNotEmpty()) "${i + 1} ${names[i]}" else "${i + 1}"
            b.lit = i == currentScene
            b.alpha = if (File(sceneDir, "scene${i + 1}.json").exists()) 1f else 0.5f
        }
    }

    private fun sceneTapped(i: Int) {
        val file = File(sceneDir, "scene${i + 1}.json")
        if (storeMode) {
            val input = EditText(a).apply { setText(names[i].ifEmpty { "Scene ${i + 1}" }); setSelectAllOnFocus(true) }
            AlertDialog.Builder(a).setTitle(L10n.f("Store scene %d", i + 1)).setView(input)
                .setPositiveButton(L10n.t("Save")) { _, _ ->
                    val name = input.text.toString().trim().ifEmpty { "Scene ${i + 1}" }
                    NativeEngine.nativeSetMulti(true)
                    val err = NativeEngine.nativeSaveMulti(file.absolutePath, name, a.library.libraryRoot.absolutePath)
                    if (err.isNotEmpty()) { a.log("scene save failed: $err", Ui.ERR); return@setPositiveButton }
                    File(sceneDir, "scene${i + 1}.names.json").writeText(JSONObject().put("name", name)
                        .put("parts", JSONArray(a.partNames.toList())).toString())
                    names[i] = name
                    currentScene = i
                    storeMode = false
                    storeBtn.lit = false
                    a.log("scene ${i + 1} stored")
                    updateScenes()
                }.setNegativeButton(L10n.t("Cancel"), null).show()
            return
        }
        if (!file.exists()) return
        a.loader.execute {
            val r = JSONObject(NativeEngine.nativeLoadMulti(file.absolutePath, a.library.libraryRoot.absolutePath))
            a.runOnUiThread {
                if (!r.optBoolean("ok")) { a.log("scene load failed: ${r.optString("error")}", Ui.ERR); return@runOnUiThread }
                runCatching {
                    val p = JSONObject(File(sceneDir, "scene${i + 1}.names.json").readText()).optJSONArray("parts")
                    if (p != null) for (k in 0 until minOf(16, p.length())) a.partNames[k] = p.getString(k)
                }
                currentScene = i
                a.setEditPart(NativeEngine.nativeEditPart())
                a.log("scene ${i + 1} recalled")
                updateScenes()
                refresh()
            }
        }
    }

    fun refresh() {
        multiSwitch.on = NativeEngine.nativeIsMulti()
        refreshers.forEach { it() }
    }
}
