package com.hsynth.app

import android.content.Context
import android.media.midi.MidiDevice
import android.media.midi.MidiDeviceInfo
import android.media.midi.MidiInputPort
import android.media.midi.MidiManager
import android.os.Build
import android.os.Handler
import android.os.Looper

/**
 * MIDI out.
 *
 * Legacy single port: the on-screen keyboard (notes, bend, mod wheel, sustain) is also sent to one chosen device,
 * e.g. an external synth over USB (byte-stream MIDI 1.0, the device's first input port).
 *
 * B3 multi-slot ports 1..4: the engine's external-zone routing (`ExtMidiPump`) writes to the device assigned to the
 * slot. The *slot number* is what the engine stores; Android `MidiDeviceInfo` ids change per session, so the user's
 * choice is persisted by device name (prefs `extPort1..4`) and re-opened by [ensureSlots].
 */
class MidiOutput(context: Context) {
    companion object {
        const val SLOTS = 4
    }

    private val manager = context.getSystemService(Context.MIDI_SERVICE) as? MidiManager
    private val handler = Handler(Looper.getMainLooper())
    private var device: MidiDevice? = null
    private var port: MidiInputPort? = null
    var channel = 0  // 0..15
    var deviceName = ""
        private set

    // slot 1..4 (index 0 unused)
    private val slotDevices = arrayOfNulls<MidiDevice>(SLOTS + 1)
    @Volatile private var slotPorts = arrayOfNulls<MidiInputPort>(SLOTS + 1)
    private val slotOpening = BooleanArray(SLOTS + 1)
    private val slotGen = IntArray(SLOTS + 1)

    /** Device name per slot (index 1..4) once the slot's port is open, "" otherwise. */
    val slotNames = Array(SLOTS + 1) { "" }

    private fun allDevices(): List<MidiDeviceInfo> {
        val m = manager ?: return emptyList()
        @Suppress("DEPRECATION")
        return if (Build.VERSION.SDK_INT >= 33) m.getDevicesForTransport(MidiManager.TRANSPORT_MIDI_BYTE_STREAM).toList() else m.devices.toList()
    }

    private fun nameOf(info: MidiDeviceInfo) = info.properties.getString(MidiDeviceInfo.PROPERTY_NAME) ?: "MIDI ${info.id}"

    /** Devices that accept MIDI (have an input port). */
    fun destinationInfos(): List<MidiDeviceInfo> = allDevices().filter { it.inputPortCount > 0 }

    /** Devices that accept MIDI (have an input port): id to name. */
    fun destinations(): List<Pair<Int, String>> = destinationInfos().map { it.id to nameOf(it) }

    fun open(id: Int, onDone: (Boolean) -> Unit = {}) {
        close()
        val m = manager ?: return onDone(false)
        val info = allDevices().firstOrNull { it.id == id && it.inputPortCount > 0 } ?: return onDone(false)
        m.openDevice(info, { dev ->
            device = dev
            port = dev?.openInputPort(0)
            deviceName = nameOf(info)
            onDone(port != null)
        }, handler)
    }

    fun close() {
        runCatching { port?.close() }
        runCatching { device?.close() }
        port = null
        device = null
        deviceName = ""
    }

    val active get() = port != null

    private fun send(vararg b: Int) {
        val p = port ?: return
        val bytes = ByteArray(b.size) { b[it].toByte() }
        runCatching { p.send(bytes, 0, bytes.size) }
    }

    fun noteOn(key: Int, velocity: Float) = send(0x90 or channel, key and 127, (velocity * 127).toInt().coerceIn(1, 127))
    fun noteOff(key: Int) = send(0x80 or channel, key and 127, 0)
    fun bend(v: Float) { val x = ((v + 1f) * 8192f).toInt().coerceIn(0, 16383); send(0xE0 or channel, x and 127, x shr 7) }
    fun cc(cc: Int, v: Float) = send(0xB0 or channel, cc and 127, (v * 127).toInt().coerceIn(0, 127))

    // ------------------------------------------------------------------ B3 slots 1..4

    fun slotActive(slot: Int): Boolean = slot in 1..SLOTS && slotPorts[slot] != null

    /** Opens [info]'s first input port as output slot [slot] (1..4), replacing whatever the slot had. */
    fun openSlot(slot: Int, info: MidiDeviceInfo, onDone: (Boolean) -> Unit = {}) {
        if (slot !in 1..SLOTS) return onDone(false)
        val m = manager ?: return onDone(false)
        closeSlot(slot)
        val gen = ++slotGen[slot]
        slotOpening[slot] = true
        val name = nameOf(info)
        m.openDevice(info, { dev ->
            slotOpening[slot] = false
            if (gen != slotGen[slot] || dev == null) {       // superseded by closeSlot / another openSlot, or failed
                runCatching { dev?.close() }
                return@openDevice onDone(false)
            }
            val p = runCatching { dev.openInputPort(0) }.getOrNull()
            if (p == null) {
                runCatching { dev.close() }
                return@openDevice onDone(false)
            }
            slotDevices[slot] = dev
            slotPorts[slot] = p
            slotNames[slot] = name
            onDone(true)
        }, handler)
    }

    /** Convenience for the spinner flow (ids from [destinations]). */
    fun openSlot(slot: Int, id: Int, onDone: (Boolean) -> Unit = {}) {
        val info = destinationInfos().firstOrNull { it.id == id } ?: return onDone(false)
        openSlot(slot, info, onDone)
    }

    fun closeSlot(slot: Int) {
        if (slot !in 1..SLOTS) return
        slotGen[slot]++
        slotOpening[slot] = false
        val p = slotPorts[slot]
        val d = slotDevices[slot]
        slotPorts[slot] = null
        slotDevices[slot] = null
        slotNames[slot] = ""
        runCatching { p?.close() }
        runCatching { d?.close() }
    }

    fun closeAllSlots() { for (s in 1..SLOTS) closeSlot(s) }

    /**
     * Re-opens every slot whose saved device [names] (index 1..4, "" = none) is not open yet and is now present.
     * Cheap when nothing is missing; call at startup and on MIDI hot-plug.
     */
    fun ensureSlots(names: Array<String>) {
        var devs: List<MidiDeviceInfo>? = null
        for (s in 1..SLOTS) {
            val n = names.getOrNull(s) ?: ""
            if (n.isEmpty()) continue
            if (slotPorts[s] != null && slotNames[s] == n) continue
            if (slotOpening[s]) continue
            val list = devs ?: destinationInfos().also { devs = it }
            val info = list.firstOrNull { nameOf(it) == n } ?: continue
            openSlot(s, info)
        }
    }

    /**
     * Writes [len] bytes of [bytes] (starting at [offset]) to slot [slot]'s port. Safe to call from the
     * `ExtMidiPump` thread; silently dropped when the slot has no open port.
     */
    fun sendSlot(slot: Int, bytes: ByteArray, len: Int = bytes.size, offset: Int = 0) {
        if (slot !in 1..SLOTS || len <= 0) return
        val p = slotPorts[slot] ?: return
        runCatching { p.send(bytes, offset, len) }
    }
}
