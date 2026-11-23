package com.hsynth.app

import android.content.Context
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.text.TextUtils
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.LinearLayout
import android.widget.SeekBar
import android.widget.Spinner
import android.widget.TextView

/** Small helpers for building the Obsidian Instrument UI in code (no XML layouts needed). */
object Ui {
    val BG = Hw.CHASSIS
    val PANEL = Hw.PANEL
    val ACCENT = Hw.MOD_OSC
    val TEXT = Hw.SILK
    val DIM = Hw.ENGRAVE
    val WARN = Hw.MOD_FILTER
    val ERR = Hw.MOD_RED

    /** Narrow layouts: portrait phones (screens are rebuilt when this changes). */
    fun portrait(ctx: Context) = ctx.resources.configuration.orientation == android.content.res.Configuration.ORIENTATION_PORTRAIT

    fun dp(ctx: Context, v: Int): Int = (v * ctx.resources.displayMetrics.density).toInt()

    /** tr = false for names (presets, files, parameters) that must not be translated. */
    fun text(ctx: Context, s: String, size: Float = 13.5f, color: Int = TEXT, bold: Boolean = false, tr: Boolean = true) = TextView(ctx).apply {
        text = if (tr) L10n.t(s) else s
        textSize = size + 0.5f
        setTextColor(color)
        typeface = if (bold) Hw.BOLD else Hw.LABEL
        ellipsize = TextUtils.TruncateAt.END
    }

    fun button(ctx: Context, s: String, onClick: (View) -> Unit) = Button(ctx).apply {
        text = L10n.t(s)
        isAllCaps = false
        textSize = 13.5f
        typeface = Hw.BOLD
        setTextColor(TEXT)
        background = Hw.keyBg(ctx)
        setPadding(dp(ctx, 12), dp(ctx, 4), dp(ctx, 12), dp(ctx, 4))
        minHeight = dp(ctx, 36)
        minimumHeight = dp(ctx, 36)
        minWidth = 0
        minimumWidth = 0
        setOnClickListener(onClick)
    }

    fun rounded(color: Int, radius: Int) = GradientDrawable().apply {
        setColor(color)
        cornerRadius = radius.toFloat()
    }

    fun row(ctx: Context, vararg views: View) = LinearLayout(ctx).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        views.forEach { addView(it, lp(wrap = true).apply { marginEnd = dp(ctx, 6) }) }
    }

    /** Buttons sharing the row's width equally (narrow columns: nothing is pushed off-screen). */
    fun buttonRow(ctx: Context, vararg buttons: Button) = LinearLayout(ctx).apply {
        orientation = LinearLayout.HORIZONTAL
        buttons.forEachIndexed { i, b ->
            b.setPadding(dp(ctx, 4), dp(ctx, 4), dp(ctx, 4), dp(ctx, 4))
            b.textSize = 12f
            b.isSingleLine = true
            b.ellipsize = TextUtils.TruncateAt.END
            addView(b, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply {
                if (i < buttons.size - 1) marginEnd = dp(ctx, 5)
            })
        }
    }

    fun column(ctx: Context) = LinearLayout(ctx).apply { orientation = LinearLayout.VERTICAL }

    fun lp(wrap: Boolean = false, weight: Float = 0f, height: Int = ViewGroup.LayoutParams.WRAP_CONTENT) =
        LinearLayout.LayoutParams(if (wrap) ViewGroup.LayoutParams.WRAP_CONTENT else if (weight > 0) 0 else ViewGroup.LayoutParams.MATCH_PARENT, height, weight)

    fun spinner(ctx: Context, items: List<String>, selected: Int, onSelect: (Int) -> Unit): Spinner {
        val sp = Spinner(ctx)
        val adapter = object : ArrayAdapter<String>(ctx, android.R.layout.simple_spinner_item, items) {
            override fun getView(position: Int, convertView: View?, parent: ViewGroup): View =
                (super.getView(position, convertView, parent) as TextView).apply {
                    setTextColor(Hw.SILK)
                    textSize = 13f
                    typeface = Hw.MONO
                    setPadding(dp(ctx, 8), dp(ctx, 6), dp(ctx, 8), dp(ctx, 6))
                }
        }
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        sp.adapter = adapter
        sp.setSelection(selected.coerceIn(0, (items.size - 1).coerceAtLeast(0)), false)
        sp.background = Hw.lcdBg(ctx)
        sp.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) = onSelect(position)
            override fun onNothingSelected(parent: AdapterView<*>?) {}
        }
        return sp
    }

    fun seek(ctx: Context, max: Int, progress: Int, onChange: (Int, Boolean) -> Unit) = SeekBar(ctx).apply {
        this.max = max
        this.progress = progress
        progressTintList = android.content.res.ColorStateList.valueOf(Hw.MOD_OSC)
        thumbTintList = android.content.res.ColorStateList.valueOf(Color.WHITE)
        progressBackgroundTintList = android.content.res.ColorStateList.valueOf(Hw.PANEL_EDGE)
        setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(sb: SeekBar, p: Int, fromUser: Boolean) = onChange(p, fromUser)
            override fun onStartTrackingTouch(sb: SeekBar) {}
            override fun onStopTrackingTouch(sb: SeekBar) {}
        })
    }
}
