package com.hsynth.app

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothGattServer
import android.bluetooth.BluetoothGattServerCallback
import android.bluetooth.BluetoothGattService
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.bluetooth.le.AdvertiseCallback
import android.bluetooth.le.AdvertiseData
import android.bluetooth.le.AdvertiseSettings
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.os.Process
import android.os.SystemClock
import android.util.Log
import java.util.UUID
import java.util.concurrent.atomic.AtomicInteger

/**
 * Bluetooth LE MIDI.
 *
 * Central: scans for BLE MIDI devices and opens them as GATT clients. Notifications
 * go straight to the native decoder. Connected devices are remembered for startup.
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
        const val CENTRAL_SOURCE = 0x6000_0000
        /** MIDI source id of the peripheral connection (replies to it go through notify()). */
        const val PERIPHERAL_SOURCE = 0x7000_0000
        private val nextCentralSource = AtomicInteger(CENTRAL_SOURCE)
        private val tunedBleThread = ThreadLocal<Boolean>()
        private const val DEFAULT_MTU = 23
        private const val MTU_TIMEOUT_MS = 2_000L
        private const val SETUP_TIMEOUT_MS = 45_000L
        private const val WRITE_TIMEOUT_MS = 5_000L
    }

    private val bt = a.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
    private val adapter: BluetoothAdapter? get() = bt?.adapter
    private val handler = Handler(Looper.getMainLooper())
    val available get() = adapter != null &&
        a.packageManager.hasSystemFeature(PackageManager.FEATURE_BLUETOOTH_LE)

    var onChange: () -> Unit = {}
    @Volatile var onMidiActivity: (() -> Unit)? = null

    fun permissions(): Array<String> =
        if (Build.VERSION.SDK_INT >= 31) arrayOf(android.Manifest.permission.BLUETOOTH_SCAN,
            android.Manifest.permission.BLUETOOTH_CONNECT, android.Manifest.permission.BLUETOOTH_ADVERTISE)
        else arrayOf(android.Manifest.permission.ACCESS_FINE_LOCATION)

    fun hasPermissions() = permissions().all { a.checkSelfPermission(it) == PackageManager.PERMISSION_GRANTED }

    // ------------------------------------------------------------ central

    data class Found(val address: String, val name: String, val device: BluetoothDevice? = null)
    val found = linkedMapOf<String, Found>()
    private val connections = linkedMapOf<String, CentralConnection>()
    private val remembered = linkedMapOf<String, String>()
    private var autoReconnectScan = false
    private val scanTimeout = Runnable { stopScan() }
    var scanning = false
        private set

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val dev = result.device
            val addr = dev.address
            val name = result.scanRecord?.deviceName ?: runCatching { dev.name }.getOrNull() ?: addr
            if (found[addr]?.name != name || found[addr]?.device == null) {
                found[addr] = Found(addr, name, dev)
                handler.post(onChange)
            }
            if (autoReconnectScan && addr in remembered && addr !in connections) {
                handler.post {
                    if (addr in remembered && addr !in connections) connectDevice(dev, remembered[addr] ?: name)
                }
            }
        }
        override fun onScanFailed(errorCode: Int) {
            scanning = false
            autoReconnectScan = false
            handler.removeCallbacks(scanTimeout)
            a.log("Bluetooth MIDI scan failed ($errorCode)", Ui.WARN)
            handler.post(onChange)
        }
    }

    /** Looks for BLE MIDI devices for 10 s. */
    fun scan(autoReconnect: Boolean = false) {
        if (!hasPermissions() || scanning) return
        val sc = adapter?.bluetoothLeScanner ?: return a.log("Bluetooth is off", Ui.WARN)
        if (!autoReconnect) found.clear()
        autoReconnectScan = autoReconnect
        scanning = true
        val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(SERVICE)).build()
        val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        sc.startScan(listOf(filter), settings, scanCallback)
        handler.postDelayed(scanTimeout, 10_000)
        onChange()
    }

    fun stopScan() {
        if (!scanning) return
        scanning = false
        autoReconnectScan = false
        handler.removeCallbacks(scanTimeout)
        runCatching { adapter?.bluetoothLeScanner?.stopScan(scanCallback) }
        onChange()
    }

    fun connect(address: String, name: String = address) {
        if (!hasPermissions()) return
        val ad = adapter ?: return
        val dev = found[address]?.device ?: runCatching { ad.getRemoteDevice(address) }.getOrNull() ?: return
        connectDevice(dev, name)
    }

    private fun connectDevice(dev: BluetoothDevice, name: String) {
        val addr = dev.address
        if (addr in connections) return
        stopScan()
        releaseFastInterval(addr)
        val link = CentralConnection(addr, name, nextCentralSource.getAndIncrement())
        connections[addr] = link
        link.gatt = runCatching { dev.connectGatt(a, false, link, BluetoothDevice.TRANSPORT_LE) }.getOrNull()
        if (link.gatt == null) {
            link.fail("connectGatt failed")
            return
        }
        handler.postDelayed(link.setupTimeout, SETUP_TIMEOUT_MS)
        onChange()
    }

    fun disconnect(address: String) {
        val link = connections[address] ?: return
        remembered.remove(address)
        remember()
        link.close()
        a.log("Bluetooth MIDI disconnected: ${link.name}")
    }

    fun connectedDevices(): List<Pair<String, String>> =
        connections.values.filter { it.ready }.map { it.address to it.name }

    fun connectingDevices(): List<Pair<String, String>> =
        connections.values.filterNot { it.ready }.map { it.address to it.name }

    private fun remember() {
        a.prefs.edit().putString("bleMidi", remembered.entries.joinToString("\n") { "${it.key}\t${it.value}" }).apply()
    }

    /** Reconnects the devices of the last session (scans briefly so random-address devices are found after BT restart). */
    fun reconnect() {
        if (available && hasPermissions()) {
            if (remembered.isNotEmpty()) scan(autoReconnect = true)
            if (a.prefs.getBoolean("blePeripheral", false)) startPeripheral()
        }
    }

    init {
        val saved = a.prefs.getString("bleMidi", "") ?: ""
        for (line in saved.lines()) {
            val parts = line.split('\t', limit = 2)
            if (parts.size == 2 && parts[0].isNotEmpty()) remembered[parts[0]] = parts[1]
        }
    }

    private enum class Setup { CONNECTING, MTU, SERVICES, READ, CCCD, READY }

    // Setup, UI state and writes are serialized on the main thread. Notifications
    // enter native code on the GATT callback thread, under the same lock as reset.
    private inner class CentralConnection(val address: String, val name: String, val source: Int) :
        BluetoothGattCallback() {
        var gatt: BluetoothGatt? = null
        private var setup = Setup.CONNECTING
        val ready get() = setup == Setup.READY
        private var midiCharacteristic: BluetoothGattCharacteristic? = null
        private var negotiatedMtu = DEFAULT_MTU
        private val receiveLock = Any()
        private var receiving = false
        private var disconnected = false  // guarded by receiveLock
        private val writes = ArrayDeque<ByteArray>()
        private var writePending = false
        private var writeRetries = 0
        private val retryWrite = Runnable { pumpWrites() }
        val setupTimeout = Runnable { fail("setup timed out at $setup") }
        private val mtuTimeout = Runnable { discoverServices() }
        private val writeTimeout = Runnable { fail("MIDI write timed out") }

        private fun onMain(client: BluetoothGatt, action: () -> Unit) {
            handler.post { if (gatt === client) action() }
        }

        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_DISCONNECTED || status != BluetoothGatt.GATT_SUCCESS) {
                synchronized(receiveLock) { disconnected = true }
                onMain(gatt) {
                    if (ready && status == BluetoothGatt.GATT_SUCCESS) {
                        close()
                        a.log("Bluetooth MIDI disconnected: $name")
                    } else {
                        fail("disconnected (status $status)")
                    }
                }
            } else if (newState == BluetoothProfile.STATE_CONNECTED) {
                onMain(gatt) {
                    if (setup != Setup.CONNECTING) return@onMain
                    requestCentralPriority(gatt, name)
                    if (Build.VERSION.SDK_INT >= 26) {
                        val phyOk = runCatching {
                            gatt.setPreferredPhy(
                                BluetoothDevice.PHY_LE_2M_MASK,
                                BluetoothDevice.PHY_LE_2M_MASK,
                                BluetoothDevice.PHY_OPTION_NO_PREFERRED
                            )
                        }.isSuccess
                        Log.i("HSynth", "Bluetooth MIDI: $name 2M PHY requested ($phyOk)")
                    }
                    setup = Setup.MTU
                    if (runCatching { gatt.requestMtu(517) }.getOrDefault(false)) {
                        handler.postDelayed(mtuTimeout, MTU_TIMEOUT_MS)
                    } else {
                        discoverServices()
                    }
                }
            }
        }

        override fun onPhyUpdate(gatt: BluetoothGatt, txPhy: Int, rxPhy: Int, status: Int) {
            val msg = "Bluetooth MIDI: $name PHY updated tx=$txPhy rx=$rxPhy status=$status"
            Log.i("HSynth", msg)
            onMain(gatt) { a.log(msg) }
        }

        override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) = onMain(gatt) {
            if (status == BluetoothGatt.GATT_SUCCESS && mtu >= DEFAULT_MTU) negotiatedMtu = mtu
            discoverServices()
        }

        private fun discoverServices() {
            if (setup != Setup.MTU) return
            handler.removeCallbacks(mtuTimeout)
            setup = Setup.SERVICES
            if (!runCatching { gatt?.discoverServices() == true }.getOrDefault(false)) fail("service discovery failed")
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) = onMain(gatt) {
            if (setup != Setup.SERVICES) return@onMain
            if (status != BluetoothGatt.GATT_SUCCESS) return@onMain fail("service discovery failed ($status)")
            val ch = gatt.getService(SERVICE)?.getCharacteristic(CHARACTERISTIC)
                ?: return@onMain fail("MIDI characteristic missing")
            midiCharacteristic = ch
            setup = Setup.READ
            // The initial read lets Android start pairing/encryption when required.
            if (!runCatching { gatt.readCharacteristic(ch) }.getOrDefault(false)) {
                enableNotifications(gatt, ch)
            }
        }

        @Suppress("OVERRIDE_DEPRECATION")
        override fun onCharacteristicRead(gatt: BluetoothGatt, ch: BluetoothGattCharacteristic, status: Int) {
            onMidiRead(gatt, ch)
        }

        override fun onCharacteristicRead(
            gatt: BluetoothGatt, ch: BluetoothGattCharacteristic, value: ByteArray, status: Int
        ) = onMidiRead(gatt, ch)

        private fun onMidiRead(gatt: BluetoothGatt, ch: BluetoothGattCharacteristic) = onMain(gatt) {
            // A read failure still allows peripherals that support notifications to work.
            if (setup == Setup.READ && ch === midiCharacteristic) enableNotifications(gatt, ch)
        }

        @Suppress("DEPRECATION")
        private fun enableNotifications(gatt: BluetoothGatt, ch: BluetoothGattCharacteristic) {
            val descriptor = ch.getDescriptor(CCCD) ?: return fail("MIDI notification descriptor missing")
            setup = Setup.CCCD
            val enabled = runCatching {
                if (!gatt.setCharacteristicNotification(ch, true)) return@runCatching false
                synchronized(receiveLock) { receiving = true }
                val value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                if (Build.VERSION.SDK_INT >= 33) {
                    gatt.writeDescriptor(descriptor, value) == BluetoothStatusCodes.SUCCESS
                } else {
                    descriptor.setValue(value) && gatt.writeDescriptor(descriptor)
                }
            }.getOrDefault(false)
            if (!enabled) fail("could not enable MIDI notifications")
        }

        override fun onDescriptorWrite(gatt: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) =
            onMain(gatt) {
                if (setup != Setup.CCCD || descriptor.uuid != CCCD || descriptor.characteristic !== midiCharacteristic)
                    return@onMain
                if (status != BluetoothGatt.GATT_SUCCESS) return@onMain fail("MIDI subscription failed ($status)")
                setup = Setup.READY
                handler.removeCallbacks(setupTimeout)
                remembered[address] = name
                remember()
                a.log("Bluetooth MIDI connected: $name")
                onChange()
                pumpWrites()
            }

        @Suppress("DEPRECATION", "OVERRIDE_DEPRECATION")
        override fun onCharacteristicChanged(gatt: BluetoothGatt, ch: BluetoothGattCharacteristic) {
            // Android 13+ frameworks can call both overloads for a single notification.
            if (Build.VERSION.SDK_INT < 33) ch.value?.let { receive(ch, it) }
        }

        override fun onCharacteristicChanged(gatt: BluetoothGatt, ch: BluetoothGattCharacteristic, value: ByteArray) {
            receive(ch, value)
        }

        private fun receive(ch: BluetoothGattCharacteristic, value: ByteArray) {
            synchronized(receiveLock) {
                if (!receiving || disconnected || ch !== midiCharacteristic) return
                tuneBleThread()
                NativeEngine.nativeBlePacket(value, source)
            }
            onMidiActivity?.invoke()
        }

        fun send(msg: ByteArray) {
            if (setup != Setup.CCCD && !ready) return
            val payload = negotiatedMtu - 3
            val time = SystemClock.elapsedRealtime().toInt()
            for (packet in NativeEngine.nativeBleEncode(msg, payload, time)) writes.addLast(packet)
            pumpWrites()
        }

        @Suppress("DEPRECATION")
        private fun pumpWrites() {
            if (!ready || writePending || writes.isEmpty()) return
            val client = gatt ?: return
            val ch = midiCharacteristic ?: return
            val data = writes.first()
            val type = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
            val result = runCatching {
                if (Build.VERSION.SDK_INT >= 33) {
                    client.writeCharacteristic(ch, data, type)
                } else {
                    ch.writeType = type
                    if (ch.setValue(data) && client.writeCharacteristic(ch)) BluetoothStatusCodes.SUCCESS
                    else BluetoothStatusCodes.ERROR_GATT_WRITE_REQUEST_BUSY
                }
            }.getOrDefault(BluetoothStatusCodes.ERROR_UNKNOWN)
            when {
                result == BluetoothStatusCodes.SUCCESS -> {
                    writes.removeFirst()
                    writePending = true
                    writeRetries = 0
                    handler.postDelayed(writeTimeout, WRITE_TIMEOUT_MS)
                }
                result == BluetoothStatusCodes.ERROR_GATT_WRITE_REQUEST_BUSY && writeRetries++ < 5 -> {
                    handler.removeCallbacks(retryWrite)
                    handler.postDelayed(retryWrite, 10)
                }
                else -> writeFailed(result)
            }
        }

        override fun onCharacteristicWrite(gatt: BluetoothGatt, ch: BluetoothGattCharacteristic, status: Int) =
            onMain(gatt) {
                if (!writePending || ch !== midiCharacteristic) return@onMain
                handler.removeCallbacks(writeTimeout)
                writePending = false
                if (status == BluetoothGatt.GATT_SUCCESS) pumpWrites() else writeFailed(status)
            }

        private fun writeFailed(status: Int) {
            handler.removeCallbacks(retryWrite)
            writes.clear()
            writeRetries = 0
            a.log("Bluetooth MIDI: $name MIDI write failed ($status)", Ui.WARN)
        }

        fun fail(reason: String) {
            close()
            a.log("Bluetooth MIDI: $name ($reason)", Ui.WARN)
        }

        fun close() {
            connections.remove(address)
            handler.removeCallbacks(setupTimeout)
            handler.removeCallbacks(mtuTimeout)
            handler.removeCallbacks(writeTimeout)
            handler.removeCallbacks(retryWrite)
            synchronized(receiveLock) {
                receiving = false
                NativeEngine.nativeBleReset(source)
            }
            writes.clear()
            val client = gatt
            gatt = null
            runCatching { client?.disconnect() }
            runCatching { client?.close() }
            onChange()
        }
    }

    /** Enqueues a complete MIDI message for the Central connection that received the request. */
    fun sendCentral(source: Int, msg: ByteArray) {
        handler.post { connections.values.firstOrNull { it.source == source }?.send(msg) }
    }

    // ------------------------------------------------------------ connection interval

    private fun preserveInitialInterval(): Boolean {
        val aggressive = NativeEngine.nativeBleAggressiveInterval()
        return aggressive == 6 ||
            // SELinux can hide Bluetooth properties. Keep the tuned Pixel 7 link
            // unless the app can verify that the phone is booting a stock image.
            (aggressive == 0 && Build.DEVICE == "panther" && !NativeEngine.nativeBleVerifiedBoot())
    }

    private fun requestCentralPriority(gatt: BluetoothGatt, name: String) {
        val result = when {
            // On a rooted device tuned for 7.5 ms initial interval (interval=6), avoid
            // calling HIGH (9..12 = 11.25-15 ms on stock BT config) unless needed.
            // Note: CONNECTION_PRIORITY_DCK is 30 ms (24/24) in Android 15/16 AOSP, so never use it.
            preserveInitialInterval() -> "preserving the initial connection interval"
            runCatching {
                gatt.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH)
            }.getOrDefault(false) -> "HIGH priority requested"
            else -> "connection priority request refused"
        }
        // Acceptance means the request was queued. BluetoothGatt logs the actual update.
        val message = "Bluetooth MIDI: $name ($result)"
        Log.i("HSynth", message)
        a.log(message)
    }

    private fun tuneBleThread() {
        if (tunedBleThread.get() == true) return
        tunedBleThread.set(true)
        runCatching { Thread.currentThread().name = "hsynth-ble" }
        runCatching { Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO) }
    }

    // Peripheral mode uses a separate GATT client to ask its central for a faster interval.
    private val priorityGatts = mutableMapOf<String, BluetoothGatt>()

    private fun requestFastInterval(dev: BluetoothDevice) {
        if (!hasPermissions() || dev.address in connections || preserveInitialInterval()) return
        synchronized(priorityGatts) { if (priorityGatts.containsKey(dev.address)) return }
        val cb = object : BluetoothGattCallback() {
            override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
                if (newState != BluetoothProfile.STATE_CONNECTED) return
                val ok = runCatching { gatt.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH) }.getOrDefault(false)
                handler.post { a.log("Bluetooth MIDI: high connection priority for ${dev.address} ${if (ok) "requested" else "refused"}") }
            }
        }
        val g = runCatching { dev.connectGatt(a, false, cb, BluetoothDevice.TRANSPORT_LE) }.getOrNull() ?: return
        synchronized(priorityGatts) { priorityGatts.put(dev.address, g) }?.let { runCatching { it.close() } }
    }

    private fun releaseFastInterval(address: String) {
        val g = synchronized(priorityGatts) { priorityGatts.remove(address) } ?: return
        runCatching { g.disconnect(); g.close() }
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
                releaseFastInterval(device.address)
                synchronized(centrals) { centrals.remove(device.address) }
                mtu.remove(device.address)
                NativeEngine.nativeBleReset(PERIPHERAL_SOURCE + (device.address.hashCode() and 0xFFFF))
                handler.post { a.log("Bluetooth MIDI: ${device.address} disconnected"); onChange() }
            } else {
                handler.post { a.log("Bluetooth MIDI: ${device.address} connected to HSynth"); onChange() }
                handler.postDelayed({ if (peripheralOn) requestFastInterval(device) }, 1000)  // after the central's own setup
            }
        }

        override fun onMtuChanged(device: BluetoothDevice, m: Int) { mtu[device.address] = m }

        override fun onCharacteristicReadRequest(device: BluetoothDevice, requestId: Int, offset: Int, c: BluetoothGattCharacteristic) {
            server?.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, 0, ByteArray(0))  // reads are empty by the spec
        }

        override fun onCharacteristicWriteRequest(device: BluetoothDevice, requestId: Int, c: BluetoothGattCharacteristic,
                                                  preparedWrite: Boolean, responseNeeded: Boolean, offset: Int, value: ByteArray?) {
            if (value != null && c.uuid == CHARACTERISTIC) {
                tuneBleThread()
                NativeEngine.nativeBlePacket(value, PERIPHERAL_SOURCE + (device.address.hashCode() and 0xFFFF))
                onMidiActivity?.invoke()
            }
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
        for (addr in synchronized(priorityGatts) { priorityGatts.keys.toList() }) releaseFastInterval(addr)
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
        for (link in connections.values.toList()) link.close()
    }
}
