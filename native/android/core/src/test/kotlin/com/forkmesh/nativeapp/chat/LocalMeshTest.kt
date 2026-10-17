package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.api.AccountSettings
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.crypto.Bytes
import com.forkmesh.nativeapp.crypto.RoomCrypto
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.security.Identity
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.net.InetAddress
import java.util.concurrent.CopyOnWriteArrayList

class FakeBluetooth(val label: String) : MeshNativeTransport {
    lateinit var peer: FakeBluetooth
    private val listeners = CopyOnWriteArrayList<(MeshBluetoothEvent) -> Unit>()
    val closed = CopyOnWriteArrayList<Int>()

    @Volatile
    private var started = false

    override val listening: Boolean get() = started
    override val unavailable: Boolean get() = false

    override fun startBluetooth(): Boolean {
        started = true
        return true
    }

    override fun stopBluetooth() {
        started = false
    }

    override fun acquireMulticastLock(): Boolean = false

    override fun releaseMulticastLock() {}

    override fun addEventListener(listener: (MeshBluetoothEvent) -> Unit) {
        listeners.add(listener)
    }

    override fun removeEventListener(listener: (MeshBluetoothEvent) -> Unit) {
        listeners.remove(listener)
    }

    private fun emit(event: MeshBluetoothEvent) = listeners.forEach { it(event) }

    fun connect(linkId: Int = 1) {
        emit(MeshBluetoothEvent(MeshBluetoothEventKind.LINK_OPENED, linkId = linkId, outbound = true))
        peer.emit(MeshBluetoothEvent(MeshBluetoothEventKind.LINK_OPENED, linkId = linkId, outbound = false))
    }

    fun forget(linkId: Int) = emit(MeshBluetoothEvent(MeshBluetoothEventKind.LINK_CLOSED, linkId = linkId))

    override fun sendBluetooth(linkId: Int, frame: ByteArray): Boolean {
        peer.emit(MeshBluetoothEvent(MeshBluetoothEventKind.DATA, linkId = linkId, bytes = frame))
        return true
    }

    override fun closeBluetoothLink(linkId: Int) {
        closed.add(linkId)
    }

    companion object {
        fun pairOf(a: String, b: String): FakeBluetooth {
            val left = FakeBluetooth(a)
            val right = FakeBluetooth(b)
            left.peer = right
            right.peer = left
            return left
        }
    }
}

fun waitUntil(timeoutMs: Long = 8000, predicate: () -> Boolean): Boolean {
    val deadline = System.currentTimeMillis() + timeoutMs
    while (System.currentTimeMillis() < deadline) {
        if (predicate()) return true
        Thread.sleep(10)
    }
    return predicate()
}

class LocalMeshTest {
    private val loopback: InetAddress = InetAddress.getByName("127.0.0.1")

    private fun makeService(name: String, transport: MeshNativeTransport? = null): LocalMeshRoom {
        val settings = AccountSettings.create(MemoryPrefStore(mapOf("profile/name" to name)), MemorySecretStore())
        val room = LocalMeshRoom(
            settings,
            Identity.fromSeed(Bytes.random(32)),
            MemorySecretStore(),
            native = transport ?: MeshNativeTransport.UNAVAILABLE,
            platform = "linux",
        )
        room.listenAddress = loopback
        room.accountProvider = { name }
        return room
    }

    private fun addr(text: String) = InetAddress.getByName(text)

    fun testOnlyAddressesThatCannotLeaveTheLocalNetwork() {
        for (local in listOf("192.168.1.20", "10.4.4.4", "172.20.0.9", "169.254.7.7", "127.0.0.1", "::1", "fd12::1", "fe80::1")) {
            expect(isLocalNetworkAddress(addr(local)), "$local is local")
        }
    }

    fun testARoutableAddressIsNeverAPeer() {
        for (routable in listOf("8.8.8.8", "172.32.0.1", "100.64.0.1", "2606:4700::1")) {
            expect(!isLocalNetworkAddress(addr(routable)), "$routable is routable")
        }
    }

    fun testAnIpv4MappedAddressIsJudgedAsTheIpv4ItCarries() {
        expect(isLocalNetworkAddress(addr("::ffff:192.168.5.5")))
        expect(!isLocalNetworkAddress(addr("::ffff:8.8.8.8")))
        val mapped = ByteArray(16).also {
            it[10] = 0xff.toByte()
            it[11] = 0xff.toByte()
            it[12] = 192.toByte()
            it[13] = 168.toByte()
            it[14] = 1
            it[15] = 1
        }
        expect(isLocalNetworkAddress(java.net.Inet6Address.getByAddress(null, mapped, -1)))
    }

    fun testAFrameComesBackByteForByte() {
        val payload = """{"hello":"mesh"}""".toByteArray()
        val buffer = BytesBuilderBuffer()
        buffer.add(frameBytes(payload))
        buffer.add(frameBytes(payload))
        expect(takeFrame(buffer)!!.contentEquals(payload))
        expect(takeFrame(buffer)!!.contentEquals(payload))
        assertEq(null, takeFrame(buffer))
    }

    fun testAHalfArrivedFrameWaitsInTheBuffer() {
        val framed = frameBytes("""{"a":1}""".toByteArray())
        val buffer = BytesBuilderBuffer()
        buffer.add(framed.copyOfRange(0, framed.size - 2))
        assertEq(null, takeFrame(buffer))
        buffer.add(framed.copyOfRange(framed.size - 2, framed.size))
        expect(takeFrame(buffer) != null)
    }

    fun testAnImpossibleLengthIsRefusedRatherThanBuffered() {
        val buffer = BytesBuilderBuffer()
        buffer.add(byteArrayOf(0x7f, 0xff.toByte(), 0xff.toByte(), 0xff.toByte()))
        assertFails<MeshFrameException> { takeFrame(buffer) }
    }

    fun testABeaconCarriesOnlyTheDeviceKeyAndHowToReachIt() {
        val beacon = MeshBeacon(kid = "Wd2zKq0p_x7rB3aQ", port = 4242, timestampMs = 1750000000000)
        val json = beacon.toJson()
        assertEq(setOf("t", "v", "id", "port", "ts"), json.keys)
        assertEq(3L, json["v"])
        @Suppress("UNCHECKED_CAST")
        val parsed = MeshBeacon.parse(Json.parse(Json.stringify(json)) as Map<String, Any?>)!!
        assertEq("Wd2zKq0p_x7rB3aQ", parsed.kid)
        assertEq(4242, parsed.port)
        assertEq(1750000000000, parsed.timestampMs)
        val control = MeshBeacon(kid = "desk-kid", port = 48800, control = true, node = "desk-node").toJson()
        assertEq("desk-node", control["node"])
        assertEq("desk-node", MeshBeacon.parse(control)!!.node)
        expect(!MeshBeacon(kid = "a", port = 1, node = "ignored").toJson().containsKey("node"), "node travels only with control")
        assertEq(null, MeshBeacon.parse(json + ("v" to 1L)), "v1 beacons are ignored")
    }

    fun testAnythingThatIsNotABeaconWeSpeakIsDropped() {
        val good = MeshBeacon(kid = "a", port = 1).toJson()
        assertEq(null, MeshBeacon.parse(good + ("t" to "other")))
        assertEq(null, MeshBeacon.parse(good + ("v" to 99L)))
        assertEq(null, MeshBeacon.parse(good + ("port" to 0L)))
        assertEq(null, MeshBeacon.parse(emptyMap()))
    }

    fun testTheBeaconOnTheWireIsCiphertextNotAnAdvertisement() {
        val beacon = MeshBeacon(kid = "Wd2zKq0p_x7rB3aQ", port = 4242)
        val crypto = RoomCrypto.shared(MESH_ROOM_NAME)
        val envelope = crypto.encrypt(beacon.toJson())
        val datagram = Json.stringify(envelope)
        expect(!datagram.contains("Wd2zKq0p_x7rB3aQ"))
        expect(!datagram.contains(MESH_BEACON_TAG))
        val back = crypto.decrypt(envelope)!!
        assertEq("Wd2zKq0p_x7rB3aQ", MeshBeacon.parse(back)!!.kid)
    }

    fun testTwoDevicesOnThisNetworkTalkWithNoRelay() {
        val ada = makeService("ada")
        val grace = makeService("grace")
        try {
            for (service in listOf(ada, grace)) {
                service.multicastEnabled = false
                service.bluetoothEnabled = false
                expect(service.start(), "start")
            }
            expect(ada.listenPort != 0)

            expect(ada.connectToPeer(loopback, grace.listenPort))
            expect(waitUntil { ada.peerCount == 1 && grace.peerCount == 1 }, "both devices end up in the same room")

            val sent = ada.sendMessage("anyone here?")
            expect(sent.isNotEmpty())
            expect(waitUntil { grace.messages.isNotEmpty() }, "the message crosses the local link")
            assertEq("anyone here?", grace.messages.first().text)
            assertEq(MESH_CONVERSATION, grace.messages.first().conversation)
            assertEq(false, grace.messages.first().self)
            assertEq(2, grace.members().size)
            expect(grace.members()[1].online)
            assertEq(1, ada.messages.count { it.id == sent })
            expect(grace.hasUnread)
            grace.markRead()
            assertEq(0, grace.unread)
        } finally {
            ada.stop()
            grace.stop()
            ada.dispose()
            grace.dispose()
        }
    }

    fun testALateJoinerIsBackfilledAndReachedWithNoDirectLink() {
        val ada = makeService("ada")
        val grace = makeService("grace")
        val linus = makeService("linus")
        val all = listOf(ada, grace, linus)
        try {
            for (service in all) {
                service.multicastEnabled = false
                service.bluetoothEnabled = false
                service.start()
            }
            ada.connectToPeer(loopback, grace.listenPort)
            waitUntil { grace.peerCount == 1 }
            ada.sendMessage("anyone here?")
            waitUntil { grace.messages.isNotEmpty() }

            linus.connectToPeer(loopback, grace.listenPort)
            expect(waitUntil { linus.messages.isNotEmpty() }, "the devices already here backfill a late joiner")
            assertEq("anyone here?", linus.messages.first().text)

            ada.sendMessage("second")
            expect(waitUntil { linus.messages.size == 2 }, "a message crosses the mesh to a device with no direct link")
            assertEq("second", linus.messages[1].text)
            assertEq(2, grace.messages.size)
        } finally {
            for (service in all) {
                service.stop()
                service.dispose()
            }
        }
    }

    fun testTheSameRoomRunsOverBluetoothWithNoNetworkAtAll() {
        val phoneLink = FakeBluetooth.pairOf("phone", "laptop")
        val laptopLink = phoneLink.peer
        val phone = makeService("phone", transport = phoneLink)
        val laptop = makeService("laptop", transport = laptopLink)
        try {
            for (service in listOf(phone, laptop)) {
                service.multicastEnabled = false
                service.networkEnabled = false
                expect(service.start())
            }
            assertEq(false, phone.networkActive)
            assertEq(true, phone.bluetoothActive)

            phoneLink.connect()
            expect(waitUntil { phone.peerCount == 1 && laptop.peerCount == 1 }, "a Bluetooth link puts both devices in the room")

            phone.sendMessage("no wifi here")
            expect(waitUntil { laptop.messages.isNotEmpty() }, "the message crosses Bluetooth")
            assertEq("no wifi here", laptop.messages.first().text)
            assertEq("phone", laptop.messages.first().senderName)
        } finally {
            phone.stop()
            laptop.stop()
            phone.dispose()
            laptop.dispose()
        }
    }

    fun testARoomWithNeitherTransportWaitsAndOpensWhenANetworkAppears() {
        val lonely = makeService("lonely")
        try {
            lonely.multicastEnabled = false
            lonely.networkEnabled = false
            assertEq(true, lonely.start())
            assertEq(true, lonely.running)
            assertEq(false, lonely.networkActive)
            expect(lonely.diagnosticsLine().startsWith("waiting for a local network or Bluetooth"), lonely.diagnosticsLine())
            val sent = lonely.sendMessage("nobody")
            expect(sent.isNotEmpty())
            expect(lonely.isQueued(sent), "the message waits for a link")

            lonely.networkEnabled = true
            lonely.announceNow()
            expect(waitUntil { lonely.networkActive }, "the network side opens once there is a network")
            expect(lonely.listenPort != 0)
        } finally {
            lonely.stop()
            lonely.dispose()
        }
    }
}
