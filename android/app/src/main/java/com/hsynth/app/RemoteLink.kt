package com.hsynth.app

/**
 * SysEx remote control (docs/SYSEX.md): every setting and operation of the synth
 * can be read and changed over MIDI SysEx, from a controller or a PC (USB in
 * "MIDI" mode, Bluetooth MIDI, ...). The requests are handled in native code;
 * this thread sends the replies / change notifications back to the device the
 * last request came from.
 */
class RemoteLink(private val a: MainActivity) {
    @Volatile private var running = false
    private var thread: Thread? = null
    private var seenChanges = 0
    private var seenSongs = 0

    fun start() {
        if (running) return
        val lib = a.library
        val dirs = listOf(lib.factoryPresets, lib.userPresets).joinToString(";") { it.absolutePath }
        val songDir = (a.getExternalFilesDir("midi") ?: java.io.File(a.filesDir, "midi")).apply { mkdirs() }
        val version = runCatching { a.packageManager.getPackageInfo(a.packageName, 0).versionName ?: "" }.getOrDefault("")
        NativeEngine.nativeRemoteInit(lib.libraryRoot.absolutePath, dirs, lib.userPresets.absolutePath, songDir.absolutePath, version)
        running = true
        thread = Thread({
            while (running) {
                val m = NativeEngine.nativeRemoteNext(200) ?: continue
                val dev = NativeEngine.nativeRemoteSource()
                when {
                    dev >= BleMidi.PERIPHERAL_SOURCE -> a.ble.sendPeripheral(m)
                    dev >= BleMidi.CENTRAL_SOURCE -> a.ble.sendCentral(dev, m)
                    dev >= 0 -> a.midi.sendTo(dev, m)
                }
            }
        }, "HSynth SysEx").apply { isDaemon = true; start() }
    }

    fun stop() {
        running = false
        thread?.join(500)
        thread = null
    }

    /** UI thread, from the stats timer: refresh what a remote request changed. */
    fun poll() {
        val c = NativeEngine.nativeRemoteChanges()
        if (c != seenChanges) {
            seenChanges = c
            a.onRemoteChange()
        }
        val s = NativeEngine.nativeRemoteSongCount()
        if (s != seenSongs) {
            seenSongs = s
            a.log("remote: song ${java.io.File(NativeEngine.nativeRemoteSongPath()).name}")
        }
    }
}
