package com.hsynth.app

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothGattServer
import android.bluetooth.BluetoothGattServerCallback
import android.bluetooth.BluetoothGattService
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.AdvertiseCallback
import android.bluetooth.le.AdvertiseData
import android.bluetooth.le.AdvertiseSettings
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.content.pm.PackageManager
import android.media.midi.MidiDevice
import android.media.midi.MidiManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import java.util.UUID

/**
 * Bluetooth LE MIDI.
 *
 * Central: scans for BLE MIDI devices (keyboards, controllers, a PC / Mac offering
 * BLE MIDI) and opens them through MidiManager.openBluetoothDevice; from then on they
 * are ordinary MIDI devices (notes, SysEx remote control) handled by MidiInput.
 * Connected devices are remembered and reconnected at start.
 *
 * Peripheral: advertises the phone as a BLE MIDI device "HSynth" (GATT server with the
 * standard MIDI service), so a PC / Mac / tablet can connect to it and play or
 * remote-control the synth without cables. Packets are decoded / encoded in native
 * code (engine ble_midi.h).
 */
@SuppressLint("MissingPermission")  // every entry point checks hasPermissions() first
class BleMidi(private val a: MainActivity) {
    companion object {
        val SERVICE: UUID = UUID.fromString("03b80e5a-ede8-4b33-a751-6ce34ec4c700")
        val CHARACTERISTIC: UUID = UUID.fromString("7772e5db-3868-4112-a1a9-f2669d106bf3")
        val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
        /** MIDI source id of the peripheral connection (replies to it go through notify()). */
        const val PERIPHERAL_SOURCE = 0x7000_0000
    }

    private val bt = a.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
    private val adapter: BluetoothAdapter? get() = bt?.adapter
    private val midiManager = a.getSystemService(Context.MIDI_SERVICE) as? MidiManager
    private val handler = Handler(Looper.getMainLooper())
    val available get() = adapter != null && midiManager != null &&
        a.packageManager.hasSystemFeature(PackageManager.FEATURE_BLUETOOTH_LE)

    var onChange: () -> Unit = {}

    fun permissions(): Array<String> =
        if (Build.VERSION.SDK_INT >= 31) arrayOf(android.Manifest.permission.BLUETOOTH_SCAN,
            android.Manifest.permission.BLUETOOTH_CONNECT, android.Manifest.permission.BLUETOOTH_ADVERTISE)
        else arrayOf(android.Manifest.permission.ACCESS_FINE_LOCATION)

    fun hasPermissions() = permissions().all { a.checkSelfPermission(it) == PackageManager.PERMISSION_GRANTED }

    // ------------------------------------------------------------ central

    data class Found(val address: String, val name: String)
    val found = linkedMapOf<String, Found>()
    private val connected = linkedMapOf<String, Pair<String, MidiDevice>>()  // address -> name, device
    var scanning = false
        private set

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val addr = result.device.address
            val name = result.scanRecord?.deviceName ?: runCatching { result.device.name }.getOrNull() ?: addr
            if (found[addr]?.name != name) {
                found[addr] = Found(addr, name)
                handler.post(onChange)
            }
        }
        override fun onScanFailed(errorCode: Int) {
            scanning = false
            a.log("Bluetooth MIDI scan failed ($errorCode)", Ui.WARN)
            handler.post(onChange)
        }
    }

    /** Looks for BLE MIDI devices for 10 s. */
    fun scan() {
        val sc = adapter?.bluetoothLeScanner ?: return a.log("Bluetooth is off", Ui.WARN)
        if (!hasPermissions() || scanning) return
        found.clear()
        scanning = true
        val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(SERVICE)).build()
        val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        sc.startScan(listOf(filter), settings, scanCallback)
        handler.postDelayed({ stopScan() }, 10_000)
        onChange()
    }

    fun stopScan() {
        if (!scanning) return
        scanning = false
        runCatching { adapter?.bluetoothLeScanner?.stopScan(scanCallback) }
        onChange()
    }

    fun connect(address: String, name: String = address) {
        val m = midiManager ?: return
        val ad = adapter ?: return
        if (!hasPermissions() || connected.containsKey(address)) return
        stopScan()
        val dev = runCatching { ad.getRemoteDevice(address) }.getOrNull() ?: return
        m.openBluetoothDevice(dev, { md ->
            if (md == null) {
                a.log("Bluetooth MIDI: cannot connect to $name", Ui.WARN)
                return@openBluetoothDevice
            }
            connected[address] = name to md
            a.midi.attach(md.info)  // ports for notes / SysEx (MidiInput also sees it as a new device)
            remember()
            a.log("Bluetooth MIDI connected: $name")
            onChange()
        }, handler)
    }

    fun disconnect(address: String) {
        val (name, md) = connected.remove(address) ?: return
        runCatching { md.close() }
        remember()
        a.log("Bluetooth MIDI disconnected: $name")
        onChange()
    }

    fun connectedDevices(): List<Pair<String, String>> = connected.map { it.key to it.value.first }

    private fun remember() {
        a.prefs.edit().putString("bleMidi", connected.entries.joinToString("\n") { "${it.key}\t${it.value.first}" }).apply()
    }

    /** Reconnects the devices of the last session (no scan needed). */
    fun reconnect() {
        if (!available || !hasPermissions()) return
        val saved = a.prefs.getString("bleMidi", "") ?: ""
        for (line in saved.lines()) {
            val parts = line.split('\t')
            if (parts.size == 2 && parts[0].isNotEmpty()) connect(parts[0], parts[1])
        }
        if (a.prefs.getBoolean("blePeripheral", false)) startPeripheral()
    }

    // ------------------------------------------------------------ peripheral

    private var server: BluetoothGattServer? = null
    private var characteristic: BluetoothGattCharacteristic? = null
    private val centrals = linkedMapOf<String, BluetoothDevice>()  // subscribed to notifications
    private val mtu = mutableMapOf<String, Int>()
    private val outQueue = ArrayDeque<Pair<BluetoothDevice, ByteArray>>()
    private var sending = false
    private val t0 = System.currentTimeMillis()
    var peripheralOn = false
        private set

    fun peripheralClients(): List<String> = synchronized(centrals) {
        centrals.values.map { runCatching { it.name }.getOrNull() ?: it.address }
    }

    private val advertiseCallback = object : AdvertiseCallback() {
        override fun onStartFailure(errorCode: Int) {
            a.log("Bluetooth MIDI: advertising failed ($errorCode)", Ui.WARN)
        }
    }

    private val serverCallback = object : BluetoothGattServerCallback() {
        override fun onConnectionStateChange(device: BluetoothDevice, status: Int, newState: Int) {
            if (newState != BluetoothProfile.STATE_CONNECTED) {
                synchronized(centrals) { centrals.remove(device.address) }
                mtu.remove(device.address)
                NativeEngine.nativeBleReset(PERIPHERAL_SOURCE + (device.address.hashCode() and 0xFFFF))
                handler.post { a.log("Bluetooth MIDI: ${device.address} disconnected"); onChange() }
            } else {
                handler.post { a.log("Bluetooth MIDI: ${device.address} connected to HSynth"); onChange() }
            }
        }

        override fun onMtuChanged(device: BluetoothDevice, m: Int) { mtu[device.address] = m }

        override fun onCharacteristicReadRequest(device: BluetoothDevice, requestId: Int, offset: Int, c: BluetoothGattCharacteristic) {
            server?.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, 0, ByteArray(0))  // reads are empty by the spec
        }

        override fun onCharacteristicWriteRequest(device: BluetoothDevice, requestId: Int, c: BluetoothGattCharacteristic,
                                                  preparedWrite: Boolean, responseNeeded: Boolean, offset: Int, value: ByteArray?) {
            if (value != null && c.uuid == CHARACTERISTIC)
                NativeEngine.nativeBlePacket(value, PERIPHERAL_SOURCE + (device.address.hashCode() and 0xFFFF))
            synchronized(centrals) { centrals.putIfAbsent(device.address, device) }
            if (responseNeeded) server?.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, 0, null)
        }

        override fun onDescriptorWriteRequest(device: BluetoothDevice, requestId: Int, d: BluetoothGattDescriptor,
                                              preparedWrite: Boolean, responseNeeded: Boolean, offset: Int, value: ByteArray?) {
            if (d.uuid == CCCD) {
                val on = value != null && value.isNotEmpty() && (value[0].toInt() and 1) != 0
                synchronized(centrals) { if (on) centrals[device.address] = device else centrals.remove(device.address) }
                handler.post(onChange)
            }
            if (responseNeeded) server?.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, 0, null)
        }

        override fun onDescriptorReadRequest(device: BluetoothDevice, requestId: Int, offset: Int, d: BluetoothGattDescriptor) {
            val on = synchronized(centrals) { centrals.containsKey(device.address) }
            server?.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, 0,
                if (on) BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE else BluetoothGattDescriptor.DISABLE_NOTIFICATION_VALUE)
        }

        override fun onNotificationSent(device: BluetoothDevice, status: Int) {
            synchronized(outQueue) { sending = false }
            pump()
        }
    }

    fun startPeripheral() {
        if (peripheralOn || !available || !hasPermissions()) return
        val ad = adapter ?: return
        if (!ad.isEnabled) return a.log("Bluetooth is off", Ui.WARN)
        val s = bt?.openGattServer(a, serverCallback) ?: return a.log("Bluetooth MIDI: no GATT server", Ui.WARN)
        val ch = BluetoothGattCharacteristic(CHARACTERISTIC,
            BluetoothGattCharacteristic.PROPERTY_READ or BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE or
                BluetoothGattCharacteristic.PROPERTY_NOTIFY,
            BluetoothGattCharacteristic.PERMISSION_READ or BluetoothGattCharacteristic.PERMISSION_WRITE)
        ch.addDescriptor(BluetoothGattDescriptor(CCCD,
            BluetoothGattDescriptor.PERMISSION_READ or BluetoothGattDescriptor.PERMISSION_WRITE))
        val service = BluetoothGattService(SERVICE, BluetoothGattService.SERVICE_TYPE_PRIMARY)
        service.addCharacteristic(ch)
        s.addService(service)
        server = s
        characteristic = ch
        val adv = ad.bluetoothLeAdvertiser ?: return a.log("Bluetooth MIDI: this phone cannot advertise", Ui.WARN)
        adv.startAdvertising(
            AdvertiseSettings.Builder().setConnectable(true).setAdvertiseMode(AdvertiseSettings.ADVERTISE_MODE_LOW_LATENCY)
                .setTxPowerLevel(AdvertiseSettings.ADVERTISE_TX_POWER_HIGH).build(),
            AdvertiseData.Builder().addServiceUuid(ParcelUuid(SERVICE)).build(),
            AdvertiseData.Builder().setIncludeDeviceName(true).build(),
            advertiseCallback)
        peripheralOn = true
        a.log("Bluetooth MIDI: advertising as \"${runCatching { ad.name }.getOrNull() ?: "HSynth"}\"")
        onChange()
    }

    fun stopPeripheral() {
        if (!peripheralOn) return
        runCatching { adapter?.bluetoothLeAdvertiser?.stopAdvertising(advertiseCallback) }
        runCatching { server?.close() }
        server = null
        characteristic = null
        synchronized(centrals) { centrals.clear() }
        synchronized(outQueue) { outQueue.clear(); sending = false }
        peripheralOn = false
        onChange()
    }

    /** Sends a complete MIDI message (e.g. a SysEx reply) to the centrals connected to the phone. */
    fun sendPeripheral(msg: ByteArray) {
        val devices = synchronized(centrals) { centrals.values.toList() }
        if (devices.isEmpty()) return
        val now = (System.currentTimeMillis() - t0).toInt()
        for (d in devices) {
            val payload = (mtu[d.address] ?: 23) - 3
            val packets = NativeEngine.nativeBleEncode(msg, payload, now)
            synchronized(outQueue) { for (p in packets) outQueue.addLast(d to p) }
        }
        pump()
    }

    // one notification at a time (the stack holds only one); onNotificationSent sends the next
    private fun pump() {
        val (dev, data) = synchronized(outQueue) {
            if (sending || outQueue.isEmpty()) return
            sending = true
            outQueue.removeFirst()
        }
        val s = server
        val ch = characteristic
        if (s == null || ch == null) { synchronized(outQueue) { sending = false }; return }
        val ok = if (Build.VERSION.SDK_INT >= 33) {
            s.notifyCharacteristicChanged(dev, ch, false, data) == BluetoothGatt.GATT_SUCCESS
        } else {
            @Suppress("DEPRECATION")
            ch.value = data
            @Suppress("DEPRECATION")
            s.notifyCharacteristicChanged(dev, ch, false)
        }
        if (!ok) {  // try again shortly (queue full) rather than dropping a SysEx chunk
            synchronized(outQueue) { outQueue.addFirst(dev to data); sending = false }
            handler.postDelayed({ pump() }, 5)
        }
    }

    fun stop() {
        stopScan()
        stopPeripheral()
        for (addr in connected.keys.toList()) {
            val md = connected.remove(addr)?.second
            runCatching { md?.close() }
        }
    }
}
