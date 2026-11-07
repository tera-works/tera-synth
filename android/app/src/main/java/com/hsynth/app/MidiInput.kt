package com.hsynth.app

import android.content.Context
import android.media.midi.MidiDevice
import android.media.midi.MidiDeviceInfo
import android.media.midi.MidiInputPort
import android.media.midi.MidiManager
import android.media.midi.MidiOutputPort
import android.media.midi.MidiReceiver
import android.os.Build
import android.os.Handler
import android.os.Looper
import java.util.concurrent.Executors

/**
 * USB / Bluetooth / virtual MIDI input using android.media.midi.
 *
 * MIDI 1.0 byte-stream devices are opened on every output port. On Android 13+
 * devices that expose the Universal MIDI Packet transport are opened as UMP
 * (MIDI 2.0) and their packets are forwarded unchanged; the C++ translator
 * decodes per-note pitch bend / per-note controllers into engine events.
 * Received bytes go straight to native code on the binder thread.
 */
class MidiInput(private val context: Context, private val onChange: () -> Unit) {
    private val manager = context.getSystemService(Context.MIDI_SERVICE) as? MidiManager
    private val handler = Handler(Looper.getMainLooper())
    private val opened = mutableMapOf<Int, Pair<MidiDevice, List<MidiOutputPort>>>()
    private val names = mutableMapOf<Int, String>()
    private val executor = Executors.newSingleThreadExecutor()
    val available get() = manager != null

    private inner class Receiver(private val ump: Boolean, private val source: Int) : MidiReceiver() {
        override fun onSend(msg: ByteArray, offset: Int, count: Int, timestamp: Long) {
            NativeEngine.nativeMidi(msg, offset, count, ump, source)
        }
    }

    // input ports of the opened devices, for SysEx remote replies (RemoteLink)
    private val replyPorts = mutableMapOf<Int, MidiInputPort>()

    /** Sends to the device with this id (its first input port; UMP devices get SysEx7 packets). */
    fun sendTo(id: Int, data: ByteArray): Boolean {
        val (port, ump) = synchronized(opened) {
            val dev = opened[id]?.first ?: return false
            val p = replyPorts[id] ?: (if (dev.info.inputPortCount > 0) dev.openInputPort(0) else null)?.also { replyPorts[id] = it }
            (p ?: return false) to (names[id]?.endsWith("(MIDI 2.0 UMP)") == true)
        }
        val bytes = if (ump) sysexToUmp(data) else data
        return runCatching { port.send(bytes, 0, bytes.size) }.isSuccess
    }

    // F0 .. F7 -> UMP SysEx7 packets (big-endian words), 6 data bytes per packet
    private fun sysexToUmp(m: ByteArray): ByteArray {
        val data = m.copyOfRange(1, m.size - 1)
        val out = java.io.ByteArrayOutputStream()
        val n = (data.size + 5) / 6
        for (k in 0 until maxOf(1, n)) {
            val chunk = data.copyOfRange(k * 6, minOf(data.size, k * 6 + 6))
            val status = when {
                n <= 1 -> 0
                k == 0 -> 1
                k == n - 1 -> 3
                else -> 2
            }
            val b = ByteArray(8)
            b[0] = 0x30
            b[1] = ((status shl 4) or chunk.size).toByte()
            for (i in chunk.indices) b[2 + i] = chunk[i]
            out.write(b)
        }
        return out.toByteArray()
    }

    private val callback = object : MidiManager.DeviceCallback() {
        override fun onDeviceAdded(device: MidiDeviceInfo) { open(device, isUmp(device)) }
        override fun onDeviceRemoved(device: MidiDeviceInfo) { close(device.id) }
    }

    private fun isUmp(info: MidiDeviceInfo): Boolean =
        Build.VERSION.SDK_INT >= 33 && info.defaultProtocol != MidiDeviceInfo.PROTOCOL_UNKNOWN

    fun start() {
        val m = manager ?: return
        if (Build.VERSION.SDK_INT >= 33) {
            for (d in m.getDevicesForTransport(MidiManager.TRANSPORT_UNIVERSAL_MIDI_PACKETS)) open(d, true)
            for (d in m.getDevicesForTransport(MidiManager.TRANSPORT_MIDI_BYTE_STREAM)) open(d, false)
            m.registerDeviceCallback(MidiManager.TRANSPORT_MIDI_BYTE_STREAM, executor, callback)
            m.registerDeviceCallback(MidiManager.TRANSPORT_UNIVERSAL_MIDI_PACKETS, executor, callback)
        } else {
            @Suppress("DEPRECATION")
            for (d in m.devices) open(d, false)
            @Suppress("DEPRECATION")
            m.registerDeviceCallback(callback, handler)
        }
    }

    fun stop() {
        manager?.unregisterDeviceCallback(callback)
        for (id in opened.keys.toList()) close(id)
    }

    private fun open(info: MidiDeviceInfo, ump: Boolean) {
        val m = manager ?: return
        if (info.outputPortCount == 0 || opened.containsKey(info.id)) return
        val name = info.properties.getString(MidiDeviceInfo.PROPERTY_NAME) ?: "MIDI ${info.id}"
        m.openDevice(info, { device ->
            if (device == null) return@openDevice
            val ports = (0 until info.outputPortCount).mapNotNull { p ->
                device.openOutputPort(p)?.also { it.connect(Receiver(ump, info.id)) }
            }
            synchronized(opened) {
                opened[info.id] = device to ports
                names[info.id] = name + if (ump) " (MIDI 2.0 UMP)" else " (MIDI 1.0)"
            }
            handler.post(onChange)
        }, handler)
    }

    private fun close(id: Int) {
        val entry = synchronized(opened) {
            names.remove(id)
            replyPorts.remove(id)?.let { runCatching { it.close() } }
            opened.remove(id)
        } ?: return
        entry.second.forEach { runCatching { it.close() } }
        runCatching { entry.first.close() }
        handler.post(onChange)
    }

    fun deviceNames(): List<String> = synchronized(opened) { names.values.toList() }
}
