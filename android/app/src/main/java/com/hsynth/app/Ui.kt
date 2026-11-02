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

/** Small helpers for building the UI in code (no XML layouts needed). */
object Ui {
    val BG = Color.rgb(17, 20, 26)
    val PANEL = Color.rgb(28, 32, 41)
    val ACCENT = Color.rgb(79, 195, 247)
    val TEXT = Color.rgb(222, 226, 234)
    val DIM = Color.rgb(140, 148, 162)
    val WARN = Color.rgb(255, 183, 77)
    val ERR = Color.rgb(239, 83, 80)

    fun dp(ctx: Context, v: Int): Int = (v * ctx.resources.displayMetrics.density).toInt()

    fun text(ctx: Context, s: String, size: Float = 14f, color: Int = TEXT, bold: Boolean = false) = TextView(ctx).apply {
        text = s
        textSize = size
        setTextColor(color)
        if (bold) setTypeface(typeface, android.graphics.Typeface.BOLD)
        ellipsize = TextUtils.TruncateAt.END
    }

    fun button(ctx: Context, s: String, onClick: (View) -> Unit) = Button(ctx).apply {
        text = s
        isAllCaps = false
        textSize = 14f
        setTextColor(TEXT)
        background = rounded(Color.rgb(44, 50, 64), dp(ctx, 6))
        setPadding(dp(ctx, 12), dp(ctx, 4), dp(ctx, 12), dp(ctx, 4))
        minHeight = dp(ctx, 40)
        minimumHeight = dp(ctx, 40)
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

    fun column(ctx: Context) = LinearLayout(ctx).apply { orientation = LinearLayout.VERTICAL }

    fun lp(wrap: Boolean = false, weight: Float = 0f, height: Int = ViewGroup.LayoutParams.WRAP_CONTENT) =
        LinearLayout.LayoutParams(if (wrap) ViewGroup.LayoutParams.WRAP_CONTENT else if (weight > 0) 0 else ViewGroup.LayoutParams.MATCH_PARENT, height, weight)

    fun spinner(ctx: Context, items: List<String>, selected: Int, onSelect: (Int) -> Unit): Spinner {
        val sp = Spinner(ctx)
        val adapter = object : ArrayAdapter<String>(ctx, android.R.layout.simple_spinner_item, items) {
            override fun getView(position: Int, convertView: View?, parent: ViewGroup): View =
                (super.getView(position, convertView, parent) as TextView).apply { setTextColor(TEXT); textSize = 14f }
        }
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        sp.adapter = adapter
        sp.setSelection(selected.coerceIn(0, (items.size - 1).coerceAtLeast(0)), false)
        sp.background = rounded(Color.rgb(44, 50, 64), dp(ctx, 6))
        sp.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) = onSelect(position)
            override fun onNothingSelected(parent: AdapterView<*>?) {}
        }
        return sp
    }

    fun seek(ctx: Context, max: Int, progress: Int, onChange: (Int, Boolean) -> Unit) = SeekBar(ctx).apply {
        this.max = max
        this.progress = progress
        setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(sb: SeekBar, p: Int, fromUser: Boolean) = onChange(p, fromUser)
            override fun onStartTrackingTouch(sb: SeekBar) {}
            override fun onStopTrackingTouch(sb: SeekBar) {}
        })
    }
}
