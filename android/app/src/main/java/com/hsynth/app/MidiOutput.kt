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
 * MIDI out: the on-screen keyboard (notes, bend, mod wheel, sustain) is also
 * sent to one chosen device, e.g. an external synth over USB. Byte-stream
 * MIDI 1.0 on the device's first input port.
 */
class MidiOutput(context: Context) {
    private val manager = context.getSystemService(Context.MIDI_SERVICE) as? MidiManager
    private val handler = Handler(Looper.getMainLooper())
    private var device: MidiDevice? = null
    private var port: MidiInputPort? = null
    var channel = 0  // 0..15
    var deviceName = ""
        private set

    /** Devices that accept MIDI (have an input port): id to name. */
    fun destinations(): List<Pair<Int, String>> {
        val m = manager ?: return emptyList()
        @Suppress("DEPRECATION")
        val all = if (Build.VERSION.SDK_INT >= 33) m.getDevicesForTransport(MidiManager.TRANSPORT_MIDI_BYTE_STREAM).toList() else m.devices.toList()
        return all.filter { it.inputPortCount > 0 }
            .map { it.id to (it.properties.getString(MidiDeviceInfo.PROPERTY_NAME) ?: "MIDI ${it.id}") }
    }

    fun open(id: Int, onDone: (Boolean) -> Unit = {}) {
        close()
        val m = manager ?: return onDone(false)
        @Suppress("DEPRECATION")
        val all = if (Build.VERSION.SDK_INT >= 33) m.getDevicesForTransport(MidiManager.TRANSPORT_MIDI_BYTE_STREAM).toList() else m.devices.toList()
        val info = all.firstOrNull { it.id == id && it.inputPortCount > 0 } ?: return onDone(false)
        m.openDevice(info, { dev ->
            device = dev
            port = dev?.openInputPort(0)
            deviceName = info.properties.getString(MidiDeviceInfo.PROPERTY_NAME) ?: ""
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
}
