package com.forkmesh.nativeapp.feature.chat

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothClass
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothServerSocket
import android.bluetooth.BluetoothSocket
import android.bluetooth.BluetoothStatusCodes
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelFileDescriptor
import android.os.ParcelUuid
import android.os.Process
import android.system.Os
import android.system.OsConstants
import com.forkmesh.nativeapp.chat.MESH_BLE_CCCD_UUID
import com.forkmesh.nativeapp.chat.MESH_BLE_INBOX_UUID
import com.forkmesh.nativeapp.chat.MESH_BLE_OUTBOX_UUID
import com.forkmesh.nativeapp.chat.MESH_BLE_SERVICE_UUID
import com.forkmesh.nativeapp.chat.MESH_BLUETOOTH_SERVICE_NAME
import com.forkmesh.nativeapp.chat.MESH_BLUETOOTH_SERVICE_UUID
import com.forkmesh.nativeapp.chat.MESH_KEEPALIVE_IDLE_S
import com.forkmesh.nativeapp.chat.MESH_KEEPALIVE_INTERVAL_S
import com.forkmesh.nativeapp.chat.MESH_KEEPALIVE_PROBES
import com.forkmesh.nativeapp.chat.MESH_UNACKED_TIMEOUT_MS
import com.forkmesh.nativeapp.chat.MeshBluetoothEvent
import com.forkmesh.nativeapp.chat.MeshBle
import com.forkmesh.nativeapp.chat.MeshBleOutbox
import com.forkmesh.nativeapp.chat.MeshBluetoothEventKind
import com.forkmesh.nativeapp.chat.MeshNativeTransport
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.Socket
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.atomic.AtomicInteger

class AndroidMeshTransport(
    private val context: Context,
    private val requestBluetoothPermissions: (Array<String>) -> Boolean,
) : MeshNativeTransport {
    companion object {
        private val SERVICE_UUID: UUID = UUID.fromString(MESH_BLUETOOTH_SERVICE_UUID)
        private val BLE_SERVICE: UUID = UUID.fromString(MESH_BLE_SERVICE_UUID)
        private val BLE_INBOX: UUID = UUID.fromString(MESH_BLE_INBOX_UUID)
        private val BLE_OUTBOX: UUID = UUID.fromString(MESH_BLE_OUTBOX_UUID)
        private val BLE_CCCD: UUID = UUID.fromString(MESH_BLE_CCCD_UUID)

        private const val MAX_FRAME_BYTES = 24 * 1024 * 1024

        private const val SEARCH_INTERVAL_MS = 120_000L
        private const val INQUIRY_TIMEOUT_MS = 15_000L

        private const val TCP_KEEPIDLE = 4
        private const val TCP_KEEPINTVL = 5
        private const val TCP_KEEPCNT = 6
        private const val TCP_USER_TIMEOUT = 18

        fun runtimePermissions(): Array<String> =
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                arrayOf(Manifest.permission.BLUETOOTH_CONNECT, Manifest.permission.BLUETOOTH_SCAN)
            } else {
                emptyArray()
            }
    }

    private sealed class Link {
        abstract val address: String?
    }

    private class RfcommLink(val socket: BluetoothSocket, val output: OutputStream) : Link() {
        override val address: String?
            get() = try {
                socket.remoteDevice?.address
            } catch (_: SecurityException) {
                null
            }
    }

    private class BleLink(val id: Int, override val address: String) : Link() {
        @Volatile var gatt: BluetoothGatt? = null
        @Volatile var inbox: BluetoothGattCharacteristic? = null
        var discovering = false
        var open = false
        var closed = false
        var writing = false
        var chunk = MeshBle.MIN_CHUNK_BYTES
        val queue = MeshBleOutbox()
    }

    private val main = Handler(Looper.getMainLooper())
    private val links = ConcurrentHashMap<Int, Link>()
    private val bleLinks = ConcurrentHashMap<String, BleLink>()
    private val quietUntil = ConcurrentHashMap<String, Long>()
    private val nextId = AtomicInteger(1)
    private val listeners = CopyOnWriteArrayList<(MeshBluetoothEvent) -> Unit>()

    @Volatile private var serverSocket: BluetoothServerSocket? = null
    @Volatile private var accepting = false
    @Volatile private var multicastLock: WifiManager.MulticastLock? = null
    @Volatile private var permissionsRequested = false
    @Volatile private var permissionPromptQueued = false
    @Volatile private var noAdapter = false
    @Volatile private var rescanRequested = false
    @Volatile var onAdapterReady: (() -> Unit)? = null
    private var adapterWatch: BroadcastReceiver? = null
    private val searchLock = Object()

    private val adapter: BluetoothAdapter?
        get() = (context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)?.adapter

    override val listening: Boolean get() = accepting

    override val unavailable: Boolean get() = noAdapter

    override fun addEventListener(listener: (MeshBluetoothEvent) -> Unit) {
        listeners.add(listener)
    }

    override fun removeEventListener(listener: (MeshBluetoothEvent) -> Unit) {
        listeners.remove(listener)
    }

    private fun emit(event: MeshBluetoothEvent) {
        for (listener in listeners) {
            try {
                listener(event)
            } catch (_: Exception) {
            }
        }
    }

    private fun hasPermission(permission: String): Boolean =
        context.checkPermission(permission, Process.myPid(), Process.myUid()) == PackageManager.PERMISSION_GRANTED

    private fun canUseBluetooth(): Boolean =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            hasPermission(Manifest.permission.BLUETOOTH_CONNECT)
        } else {
            hasPermission(Manifest.permission.BLUETOOTH)
        }

    @Synchronized
    override fun startBluetooth(): Boolean {
        if (accepting) return true
        val adapter = adapter ?: run {
            noAdapter = true
            return false
        }
        watchAdapter()
        if (!adapter.isEnabled) return false
        if (!canUseBluetooth()) {
            askForPermissions()
            return false
        }
        val server = try {
            adapter.listenUsingInsecureRfcommWithServiceRecord(MESH_BLUETOOTH_SERVICE_NAME, SERVICE_UUID)
        } catch (_: IOException) {
            return false
        } catch (_: SecurityException) {
            return false
        }
        serverSocket = server
        accepting = true
        Thread({ acceptLoop(server) }, "forkmesh-mesh-accept").apply {
            isDaemon = true
            start()
        }
        Thread({ searchLoop(server) }, "forkmesh-mesh-search").apply {
            isDaemon = true
            start()
        }
        return true
    }

    @Synchronized
    override fun stopBluetooth() {
        synchronized(searchLock) {
            accepting = false
            searchLock.notifyAll()
        }
        try {
            serverSocket?.close()
        } catch (_: IOException) {
        }
        serverSocket = null
        for (id in links.keys.toList()) closeBluetoothLink(id)
        for (link in bleLinks.values.toList()) dropBle(link)
    }

    override fun dispose() {
        onAdapterReady = null
        stopBluetooth()
        releaseMulticastLock()
        listeners.clear()
        synchronized(this) {
            adapterWatch?.let { receiver ->
                try {
                    context.unregisterReceiver(receiver)
                } catch (_: RuntimeException) {
                }
            }
            adapterWatch = null
        }
    }

    override fun rescanBluetooth() {
        synchronized(searchLock) {
            rescanRequested = true
            searchLock.notifyAll()
        }
    }

    override fun tuneLinkSocket(socket: Socket) {
        val descriptor = try {
            ParcelFileDescriptor.fromSocket(socket)
        } catch (_: Exception) {
            null
        } ?: return
        try {
            val fd = descriptor.fileDescriptor
            Os.setsockoptInt(fd, OsConstants.SOL_SOCKET, OsConstants.SO_KEEPALIVE, 1)
            Os.setsockoptInt(fd, OsConstants.IPPROTO_TCP, TCP_KEEPIDLE, MESH_KEEPALIVE_IDLE_S)
            Os.setsockoptInt(fd, OsConstants.IPPROTO_TCP, TCP_KEEPINTVL, MESH_KEEPALIVE_INTERVAL_S)
            Os.setsockoptInt(fd, OsConstants.IPPROTO_TCP, TCP_KEEPCNT, MESH_KEEPALIVE_PROBES)
            Os.setsockoptInt(fd, OsConstants.IPPROTO_TCP, TCP_USER_TIMEOUT, MESH_UNACKED_TIMEOUT_MS)
        } catch (_: Exception) {
        } finally {
            try {
                descriptor.close()
            } catch (_: IOException) {
            }
        }
    }

    private fun askForPermissions() {
        if (permissionsRequested || permissionPromptQueued) return
        val wanted = runtimePermissions()
        if (wanted.isEmpty()) return
        permissionPromptQueued = true
        main.post {
            permissionPromptQueued = false
            if (!permissionsRequested && requestBluetoothPermissions(wanted)) permissionsRequested = true
        }
    }

    private fun watchAdapter() {
        if (adapterWatch != null) return
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context?, intent: Intent?) {
                if (intent == null || intent.action != BluetoothAdapter.ACTION_STATE_CHANGED) return
                when (intent.getIntExtra(BluetoothAdapter.EXTRA_STATE, BluetoothAdapter.ERROR)) {
                    BluetoothAdapter.STATE_TURNING_OFF, BluetoothAdapter.STATE_OFF ->
                        Thread({ stopBluetooth() }, "forkmesh-mesh-bt-off").apply {
                            isDaemon = true
                            start()
                        }
                    BluetoothAdapter.STATE_ON -> onAdapterReady?.invoke()
                }
            }
        }
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                context.registerReceiver(receiver, IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED), Context.RECEIVER_EXPORTED)
            } else {
                @Suppress("UnspecifiedRegisterReceiverFlag")
                context.registerReceiver(receiver, IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED))
            }
            adapterWatch = receiver
        } catch (_: RuntimeException) {
        }
    }

    private fun acceptLoop(server: BluetoothServerSocket) {
        while (accepting && serverSocket === server) {
            val socket = try {
                server.accept()
            } catch (_: IOException) {
                null
            } catch (_: SecurityException) {
                null
            }
            if (socket == null) {
                serverDied(server)
                return
            }
            if (!accepting) {
                closeQuietly(socket)
                return
            }
            adopt(socket, outbound = false)
        }
    }

    private fun serverDied(server: BluetoothServerSocket) {
        synchronized(searchLock) {
            if (serverSocket !== server) return
            serverSocket = null
            accepting = false
            searchLock.notifyAll()
        }
        try {
            server.close()
        } catch (_: IOException) {
        }
    }

    private fun searchLoop(server: BluetoothServerSocket) {
        try {
            while (accepting && serverSocket === server) {
                rescanRequested = false
                val bonded: Set<BluetoothDevice> = try {
                    adapter?.bondedDevices ?: emptySet()
                } catch (_: SecurityException) {
                    emptySet()
                }
                dial(bonded)
                dial(discoverNearby() - bonded)
                scanBle()
                pause(SEARCH_INTERVAL_MS, server, wakeOnRescan = true)
            }
        } catch (_: InterruptedException) {
        }
    }

    private fun pause(ms: Long, server: BluetoothServerSocket?, wakeOnRescan: Boolean) {
        val deadline = System.currentTimeMillis() + ms
        synchronized(searchLock) {
            while (accepting && (server == null || serverSocket === server) && !(wakeOnRescan && rescanRequested)) {
                val left = deadline - System.currentTimeMillis()
                if (left <= 0) return
                searchLock.wait(left)
            }
        }
    }

    private fun canScan(): Boolean =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            hasPermission(Manifest.permission.BLUETOOTH_SCAN)
        } else {
            hasPermission(Manifest.permission.BLUETOOTH_ADMIN)
        }

    private fun discoverNearby(): Set<BluetoothDevice> {
        val adapter = adapter ?: return emptySet()
        if (!accepting || !canScan()) return emptySet()
        val found = LinkedHashSet<BluetoothDevice>()
        val finished = Object()
        var done = false
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context?, intent: Intent?) {
                when (intent?.action) {
                    BluetoothDevice.ACTION_FOUND -> {
                        val device = foundDevice(intent) ?: return
                        val major = try {
                            device.bluetoothClass?.majorDeviceClass
                        } catch (_: SecurityException) {
                            null
                        }
                        if (major == BluetoothClass.Device.Major.COMPUTER ||
                            major == BluetoothClass.Device.Major.PHONE ||
                            major == BluetoothClass.Device.Major.UNCATEGORIZED
                        ) {
                            synchronized(found) { found.add(device) }
                        }
                    }
                    BluetoothAdapter.ACTION_DISCOVERY_FINISHED -> synchronized(finished) {
                        done = true
                        finished.notifyAll()
                    }
                }
            }
        }
        val filter = IntentFilter().apply {
            addAction(BluetoothDevice.ACTION_FOUND)
            addAction(BluetoothAdapter.ACTION_DISCOVERY_FINISHED)
        }
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                context.registerReceiver(receiver, filter, Context.RECEIVER_EXPORTED)
            } else {
                @Suppress("UnspecifiedRegisterReceiverFlag")
                context.registerReceiver(receiver, filter)
            }
        } catch (_: RuntimeException) {
            return emptySet()
        }
        try {
            if (!adapter.startDiscovery()) return emptySet()
            synchronized(finished) {
                val deadline = System.currentTimeMillis() + INQUIRY_TIMEOUT_MS
                while (!done && accepting) {
                    val left = deadline - System.currentTimeMillis()
                    if (left <= 0) break
                    finished.wait(left)
                }
            }
        } catch (_: SecurityException) {
            return emptySet()
        } catch (_: InterruptedException) {
            return emptySet()
        } finally {
            try {
                adapter.cancelDiscovery()
            } catch (_: SecurityException) {
            }
            try {
                context.unregisterReceiver(receiver)
            } catch (_: RuntimeException) {
            }
        }
        return synchronized(found) { found.toSet() }
    }

    @Suppress("DEPRECATION")
    private fun foundDevice(intent: Intent): BluetoothDevice? =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice::class.java)
        } else {
            intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE)
        }

    private fun isLinked(device: BluetoothDevice): Boolean = links.values.any { it.address == device.address }

    private fun isQuiet(address: String): Boolean {
        val until = quietUntil[address] ?: return false
        if (until > System.currentTimeMillis()) return true
        quietUntil.remove(address, until)
        return false
    }

    private fun dial(devices: Set<BluetoothDevice>) {
        for (device in devices) {
            if (!accepting) return
            if (isLinked(device) || isQuiet(device.address)) continue
            val socket = try {
                device.createInsecureRfcommSocketToServiceRecord(SERVICE_UUID)
            } catch (_: IOException) {
                continue
            } catch (_: SecurityException) {
                return
            }
            try {
                socket.connect()
            } catch (_: IOException) {
                closeQuietly(socket)
                continue
            } catch (_: SecurityException) {
                return
            }
            adopt(socket, outbound = true)
        }
    }

    private fun adopt(socket: BluetoothSocket, outbound: Boolean) {
        val id = nextId.getAndIncrement()
        val output = try {
            socket.outputStream
        } catch (_: IOException) {
            closeQuietly(socket)
            return
        }
        links[id] = RfcommLink(socket, output)
        emit(MeshBluetoothEvent(MeshBluetoothEventKind.LINK_OPENED, linkId = id, outbound = outbound))
        Thread({ readLoop(id, socket) }, "forkmesh-mesh-link-$id").apply {
            isDaemon = true
            start()
        }
    }

    private fun readLoop(id: Int, socket: BluetoothSocket) {
        val input: InputStream = try {
            socket.inputStream
        } catch (_: IOException) {
            closeBluetoothLink(id)
            return
        }
        val chunk = ByteArray(8192)
        while (links.containsKey(id)) {
            val read = try {
                input.read(chunk)
            } catch (_: IOException) {
                break
            }
            if (read <= 0 || read > MAX_FRAME_BYTES) break
            emit(MeshBluetoothEvent(MeshBluetoothEventKind.DATA, linkId = id, bytes = chunk.copyOf(read)))
        }
        closeBluetoothLink(id)
    }

    override fun sendBluetooth(linkId: Int, frame: ByteArray): Boolean {
        val link = when (val found = links[linkId]) {
            is RfcommLink -> found
            is BleLink -> {
                synchronized(found) {
                    if (!found.open) return false
                    found.queue.enqueue(frame)
                }
                pump(found)
                return true
            }
            null -> return false
        }
        return try {
            synchronized(link.output) {
                link.output.write(frame)
                link.output.flush()
            }
            true
        } catch (_: IOException) {
            closeBluetoothLink(linkId)
            false
        }
    }

    override fun closeBluetoothLink(linkId: Int) = closeBluetoothLink(linkId, false)

    override fun closeBluetoothLink(linkId: Int, duplicate: Boolean) {
        val link = links[linkId] ?: return
        val address = link.address
        if (duplicate && address != null) {
            quietUntil[address] = System.currentTimeMillis() + MeshBle.QUIET_AFTER_DUPLICATE_MS
        }
        when (link) {
            is BleLink -> dropBle(link)
            is RfcommLink -> {
                if (!links.remove(linkId, link)) return
                closeQuietly(link.socket)
                emit(MeshBluetoothEvent(MeshBluetoothEventKind.LINK_CLOSED, linkId = linkId))
            }
        }
    }

    private fun scanBle() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) return
        val adapter = adapter ?: return
        if (!accepting || !canScan()) return
        val scanner = try {
            adapter.bluetoothLeScanner
        } catch (_: SecurityException) {
            null
        } ?: return
        val found = LinkedHashMap<String, BluetoothDevice>()
        val callback = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                val device = result.device ?: return
                synchronized(found) { found[device.address] = device }
            }

            override fun onBatchScanResults(results: MutableList<ScanResult>) {
                for (result in results) onScanResult(ScanSettings.CALLBACK_TYPE_ALL_MATCHES, result)
            }
        }
        val filters = listOf(ScanFilter.Builder().setServiceUuid(ParcelUuid(BLE_SERVICE)).build())
        val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        try {
            scanner.startScan(filters, settings, callback)
        } catch (_: SecurityException) {
            return
        } catch (_: IllegalStateException) {
            return
        }
        try {
            pause(MeshBle.SCAN_MS, null, wakeOnRescan = false)
        } catch (_: InterruptedException) {
        } finally {
            try {
                scanner.stopScan(callback)
            } catch (_: SecurityException) {
            } catch (_: IllegalStateException) {
            }
        }
        val devices = synchronized(found) { found.values.toList() }
        for (device in devices) dialBle(device)
    }

    @Suppress("DEPRECATION")
    private fun dialBle(device: BluetoothDevice) {
        if (!accepting) return
        val address = device.address ?: return
        if (bleLinks.containsKey(address) || isQuiet(address)) return
        val link = BleLink(nextId.getAndIncrement(), address)
        if (bleLinks.putIfAbsent(address, link) != null) return
        val gatt = try {
            device.connectGatt(context, false, GattCallback(link), BluetoothDevice.TRANSPORT_LE)
        } catch (_: SecurityException) {
            null
        }
        if (gatt == null) {
            bleLinks.remove(address, link)
            return
        }
        link.gatt = gatt
        main.postDelayed({ if (synchronized(link) { !link.open }) dropBle(link) }, MeshBle.OPEN_TIMEOUT_MS)
    }

    private fun dropBle(link: BleLink) {
        val wasOpen = synchronized(link) {
            if (link.closed) return
            link.closed = true
            val open = link.open
            link.open = false
            link.queue.clear()
            open
        }
        bleLinks.remove(link.address, link)
        links.remove(link.id, link)
        val gatt = link.gatt
        if (gatt != null) {
            try {
                gatt.disconnect()
            } catch (_: SecurityException) {
            }
            try {
                gatt.close()
            } catch (_: SecurityException) {
            }
        }
        if (wasOpen) emit(MeshBluetoothEvent(MeshBluetoothEventKind.LINK_CLOSED, linkId = link.id))
    }

    private fun pump(link: BleLink) {
        val gatt = link.gatt ?: return
        val inbox = link.inbox ?: return
        val chunk = synchronized(link) {
            if (!link.open || link.writing) return
            val next = link.queue.peek(link.chunk) ?: return
            link.writing = true
            next
        }
        if (!writeCharacteristic(gatt, inbox, chunk)) dropBle(link)
    }

    @Suppress("DEPRECATION")
    private fun writeCharacteristic(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic, value: ByteArray): Boolean =
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                gatt.writeCharacteristic(characteristic, value, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) ==
                    BluetoothStatusCodes.SUCCESS
            } else {
                characteristic.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                characteristic.value = value
                gatt.writeCharacteristic(characteristic)
            }
        } catch (_: SecurityException) {
            false
        }

    @Suppress("DEPRECATION")
    private fun writeDescriptor(gatt: BluetoothGatt, descriptor: BluetoothGattDescriptor, value: ByteArray): Boolean =
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                gatt.writeDescriptor(descriptor, value) == BluetoothStatusCodes.SUCCESS
            } else {
                descriptor.value = value
                gatt.writeDescriptor(descriptor)
            }
        } catch (_: SecurityException) {
            false
        }

    private inner class GattCallback(private val link: BleLink) : BluetoothGattCallback() {
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS || newState != BluetoothProfile.STATE_CONNECTED) {
                link.gatt = gatt
                dropBle(link)
                return
            }
            link.gatt = gatt
            val asked = try {
                gatt.requestMtu(MeshBle.REQUESTED_MTU)
            } catch (_: SecurityException) {
                false
            }
            if (!asked) discover(gatt)
        }

        override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS) synchronized(link) { link.chunk = MeshBle.chunkBytes(mtu) }
            discover(gatt)
        }

        private fun discover(gatt: BluetoothGatt) {
            synchronized(link) {
                if (link.discovering || link.closed) return
                link.discovering = true
            }
            val started = try {
                gatt.discoverServices()
            } catch (_: SecurityException) {
                false
            }
            if (!started) dropBle(link)
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            val service = gatt.getService(BLE_SERVICE)
            val inbox = service?.getCharacteristic(BLE_INBOX)
            val outbox = service?.getCharacteristic(BLE_OUTBOX)
            val cccd = outbox?.getDescriptor(BLE_CCCD)
            if (status != BluetoothGatt.GATT_SUCCESS || inbox == null || outbox == null || cccd == null) {
                dropBle(link)
                return
            }
            link.inbox = inbox
            val subscribed = try {
                gatt.setCharacteristicNotification(outbox, true)
            } catch (_: SecurityException) {
                false
            }
            if (!subscribed || !writeDescriptor(gatt, cccd, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)) {
                dropBle(link)
            }
        }

        override fun onDescriptorWrite(gatt: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) {
            if (descriptor.uuid != BLE_CCCD) return
            if (status != BluetoothGatt.GATT_SUCCESS) {
                dropBle(link)
                return
            }
            synchronized(link) {
                if (link.open || link.closed) return
                link.open = true
            }
            if (!accepting) {
                dropBle(link)
                return
            }
            links[link.id] = link
            emit(MeshBluetoothEvent(MeshBluetoothEventKind.LINK_OPENED, linkId = link.id, outbound = true))
            pump(link)
        }

        @Deprecated("Deprecated in Java")
        @Suppress("DEPRECATION")
        override fun onCharacteristicChanged(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) return
            received(characteristic.uuid, characteristic.value ?: ByteArray(0))
        }

        override fun onCharacteristicChanged(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic, value: ByteArray) {
            received(characteristic.uuid, value)
        }

        private fun received(uuid: UUID, value: ByteArray) {
            if (uuid != BLE_OUTBOX) return
            if (synchronized(link) { !link.open }) return
            if (value.isEmpty()) {
                dropBle(link)
                return
            }
            emit(MeshBluetoothEvent(MeshBluetoothEventKind.DATA, linkId = link.id, bytes = value.copyOf()))
        }

        override fun onCharacteristicWrite(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) {
            if (characteristic.uuid != BLE_INBOX) return
            if (status != BluetoothGatt.GATT_SUCCESS) {
                dropBle(link)
                return
            }
            synchronized(link) {
                link.writing = false
                link.queue.advance()
            }
            pump(link)
        }
    }

    override fun acquireMulticastLock(): Boolean {
        if (multicastLock?.isHeld == true) return true
        val wifi = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager ?: return false
        return try {
            val lock = wifi.createMulticastLock("forkmesh-local-mesh")
            lock.setReferenceCounted(false)
            lock.acquire()
            multicastLock = lock
            true
        } catch (_: SecurityException) {
            false
        }
    }

    override fun releaseMulticastLock() {
        val lock = multicastLock ?: return
        multicastLock = null
        try {
            if (lock.isHeld) lock.release()
        } catch (_: RuntimeException) {
        }
    }

    private fun closeQuietly(socket: BluetoothSocket) {
        try {
            socket.close()
        } catch (_: IOException) {
        }
    }
}
