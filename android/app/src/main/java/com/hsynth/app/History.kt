package com.hsynth.app

/**
 * Undo / redo / compare for the sound being edited (all parameters of the
 * edit part). A checkpoint is taken when a control is first touched, so one
 * knob turn is one undo step. Compare switches between the sound as it was
 * loaded and the edited one.
 */
object History {
    private const val kMax = 64
    private val undo = ArrayDeque<FloatArray>()
    private val redo = ArrayDeque<FloatArray>()
    private var loaded: FloatArray? = null
    private var edited: FloatArray? = null   // kept while comparing
    var comparing = false
        private set
    var onChange: (() -> Unit)? = null

    /** After a preset load or a part change: the new starting point. */
    fun reset() {
        undo.clear()
        redo.clear()
        comparing = false
        edited = null
        loaded = NativeEngine.nativeGetAllParams()
        onChange?.invoke()
    }

    /** Before an edit. */
    fun checkpoint() {
        if (comparing) endCompare(keepEdited = true)
        val now = NativeEngine.nativeGetAllParams()
        if (undo.lastOrNull()?.contentEquals(now) == true) return
        undo.addLast(now)
        while (undo.size > kMax) undo.removeFirst()
        redo.clear()
        onChange?.invoke()
    }

    fun canUndo() = undo.isNotEmpty()
    fun canRedo() = redo.isNotEmpty()

    fun undo() {
        if (comparing) endCompare(keepEdited = true)
        val prev = undo.removeLastOrNull() ?: return
        redo.addLast(NativeEngine.nativeGetAllParams())
        NativeEngine.nativeSetAllParams(prev)
        Params.notifyChanged()
        onChange?.invoke()
    }

    fun redo() {
        if (comparing) endCompare(keepEdited = true)
        val next = redo.removeLastOrNull() ?: return
        undo.addLast(NativeEngine.nativeGetAllParams())
        NativeEngine.nativeSetAllParams(next)
        Params.notifyChanged()
        onChange?.invoke()
    }

    fun toggleCompare() {
        if (comparing) {
            endCompare(keepEdited = true)
        } else {
            val l = loaded ?: return
            edited = NativeEngine.nativeGetAllParams()
            NativeEngine.nativeSetAllParams(l)
            comparing = true
            Params.notifyChanged()
        }
        onChange?.invoke()
    }

    private fun endCompare(keepEdited: Boolean) {
        val e = edited
        comparing = false
        edited = null
        if (keepEdited && e != null) {
            NativeEngine.nativeSetAllParams(e)
            Params.notifyChanged()
        }
    }
}
