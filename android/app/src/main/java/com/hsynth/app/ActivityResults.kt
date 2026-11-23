package com.hsynth.app

import android.content.Intent
import android.util.SparseArray

/**
 * Request-code router for `startActivityForResult` (SAF pickers etc.).
 *
 * `MainActivity.onActivityResult` becomes:
 * ```
 * if (resultCode == RESULT_OK) ActivityResults.dispatch(requestCode, data)
 * ```
 * and every feature does `ActivityResults.register(REQ_X) { data -> ... }` once when its UI is built.
 * Handlers only run for RESULT_OK (a cancelled picker is silently dropped, as before).
 * UI thread only.
 */
object ActivityResults {
    // New request codes (existing 101–107, 120–121, 201–202 keep working through register() as well).
    const val REQ_SCL = 130
    const val REQ_PHRASE_EXPORT = 131
    const val REQ_BACKUP_EXPORT = 140
    const val REQ_BACKUP_IMPORT = 141
    const val REQ_BACKUP_TREE = 142
    const val REQ_MEMO_TXT = 150
    const val REQ_AUDIO_FILE = 210
    const val REQ_PAD_FILE = 211

    private val handlers = SparseArray<(Intent?) -> Unit>()

    /** Registers (or replaces) the handler for [requestCode]. */
    fun register(requestCode: Int, handler: (Intent?) -> Unit) {
        handlers.put(requestCode, handler)
    }

    fun unregister(requestCode: Int) {
        handlers.remove(requestCode)
    }

    /** Drop every handler; call when the Activity UI is rebuilt so handlers do not leak old views. */
    fun clear() {
        handlers.clear()
    }

    fun isRegistered(requestCode: Int): Boolean = handlers.indexOfKey(requestCode) >= 0

    /** @return true if a handler consumed the result; false lets the caller fall back to its legacy chain. */
    fun dispatch(requestCode: Int, data: Intent?): Boolean {
        val h = handlers.get(requestCode) ?: return false
        h(data)
        return true
    }
}
