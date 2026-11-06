package com.hsynth.app

import org.json.JSONArray
import kotlin.math.ln
import kotlin.math.pow
import kotlin.math.roundToInt

/** Parameter metadata exported by the engine (see hsynth/params.h). */
data class ParamMeta(
    val id: Int,
    val name: String,
    val group: String,
    val label: String,
    val kind: String,   // float | int | enum | bool
    val curve: String,  // linear | exp
    val min: Float,
    val max: Float,
    val def: Float,
    val unit: String,
    val options: List<String>,
) {
    /** Slider position 0..1 -> parameter value. */
    fun fromNormalized(t: Float): Float {
        val c = t.coerceIn(0f, 1f)
        val v = if (curve == "exp" && min > 0f) min * (max / min).pow(c) else min + c * (max - min)
        return if (kind == "float") v else v.roundToInt().toFloat()
    }

    fun toNormalized(v: Float): Float {
        if (max <= min) return 0f
        val c = v.coerceIn(min, max)
        return if (curve == "exp" && min > 0f) (ln(c / min) / ln(max / min)) else (c - min) / (max - min)
    }

    fun format(v: Float): String = when (kind) {
        "enum" -> options.getOrElse(v.roundToInt()) { v.roundToInt().toString() }
        "bool" -> L10n.t(if (v > 0.5f) "on" else "off")
        "int" -> v.roundToInt().toString() + if (unit.isNotEmpty()) " $unit" else ""
        else -> {
            val s = when {
                unit == "Hz" && v >= 1000f -> String.format("%.2f k", v / 1000f)
                unit == "s" && v < 1f -> String.format("%.0f m", v * 1000f)
                kotlin.math.abs(v) >= 100f -> String.format("%.0f ", v)
                kotlin.math.abs(v) >= 10f -> String.format("%.1f ", v)
                else -> String.format("%.3f ", v)
            }
            (s + unit).trim()
        }
    }

    companion object {
        fun parse(json: String): List<ParamMeta> {
            val arr = JSONArray(json)
            return List(arr.length()) { i ->
                val o = arr.getJSONObject(i)
                val opts = o.optJSONArray("options")
                ParamMeta(
                    id = o.getInt("id"),
                    name = o.getString("name"),
                    group = o.getString("group"),
                    label = o.getString("label"),
                    kind = o.getString("kind"),
                    curve = o.getString("curve"),
                    min = o.getDouble("min").toFloat(),
                    max = o.getDouble("max").toFloat(),
                    def = o.getDouble("def").toFloat(),
                    unit = o.optString("unit", ""),
                    options = if (opts == null) emptyList() else List(opts.length()) { opts.getString(it) },
                )
            }
        }
    }
}

/** Shared parameter model: metadata + last known values, observers for refresh. */
object Params {
    val meta: List<ParamMeta> by lazy { ParamMeta.parse(NativeEngine.nativeParamMetadata()) }
    private val byName: Map<String, ParamMeta> by lazy { meta.associateBy { it.name } }
    private val listeners = mutableListOf<() -> Unit>()

    fun id(name: String): Int = byName[name]?.id ?: -1
    fun get(name: String): Float = byName[name]?.let { NativeEngine.nativeGetParam(it.id) } ?: 0f
    fun set(name: String, v: Float) { byName[name]?.let { NativeEngine.setParam(it.id, v) } }

    fun addListener(l: () -> Unit) { listeners += l }
    /** Drop listeners of a previous Activity instance (they hold its views). */
    fun clearListeners() = listeners.clear()
    /** Call on the UI thread after a preset load. */
    fun notifyChanged() = listeners.forEach { it() }
}
