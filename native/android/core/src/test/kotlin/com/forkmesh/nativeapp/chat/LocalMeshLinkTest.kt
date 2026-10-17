package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.api.AccountSettings
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.crypto.Bytes
import com.forkmesh.nativeapp.crypto.ChatDeviceKeys
import com.forkmesh.nativeapp.security.Identity
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.net.InetAddress

class LocalMeshLinkTest {
    private val loopback: InetAddress = InetAddress.getByName("127.0.0.1")

    private fun room(
        name: String,
        transport: MeshNativeTransport,
        store: MemorySecretStore = MemorySecretStore(),
        network: Boolean = false,
    ): LocalMeshRoom {
        val settings = AccountSettings.create(MemoryPrefStore(mapOf("profile/name" to name)), MemorySecretStore())
        val room = LocalMeshRoom(
            settings,
            Identity.fromSeed(Bytes.random(32)),
            store,
            native = transport,
            platform = "linux",
            loop = EventLoop("mesh-$name", virtualTime = true),
        )
        room.listenAddress = loopback
        room.accountProvider = { name }
        room.multicastEnabled = false
        room.addressSweepEnabled = false
        room.networkEnabled = network
        return room
    }

    private fun lowAndHigh(): Pair<MemorySecretStore, MemorySecretStore> {
        val first = MemorySecretStore()
        val second = MemorySecretStore()
        val firstKid = ChatDeviceKeys.forStore(first).kid
        val secondKid = ChatDeviceKeys.forStore(second).kid
        return if (firstKid < secondKid) first to second else second to first
    }

    private fun closeAll(vararg rooms: LocalMeshRoom) {
        for (room in rooms) room.stop()
        for (room in rooms) room.dispose()
    }

    fun testOnlyLanInterfacesCarryTheMesh() {
        for (name in listOf("wlan0", "wlan1", "eth0", "en0", "ap0", "swlan0", "rndis0", "p2p-wlan0-0")) {
            expect(isMeshInterfaceName(name), name)
        }
        for (name in listOf("rmnet_data0", "rmnet0", "ccmni1", "tun0", "v4-rmnet_data0", "clat4", "ppp0", "docker0", "br-1f2e", "veth12", "wg0")) {
            expect(!isMeshInterfaceName(name), name)
        }
    }

    fun testALinkThePeerHasLostIsReplacedByItsNewDial() {
        val (low, high) = lowAndHigh()
        val phoneLink = FakeBluetooth.pairOf("phone", "laptop")
        val laptopLink = phoneLink.peer
        val phone = room("phone", phoneLink, store = high)
        val laptop = room("laptop", laptopLink, store = low)
        try {
            expect(phone.start())
            expect(laptop.start())
            phoneLink.connect(1)
            expect(waitUntil { phone.peerCount == 1 && laptop.peerCount == 1 }, "the first link comes up")

            phone.loop.advance(11_000)
            laptop.loop.advance(11_000)
            laptopLink.forget(1)
            expect(waitUntil { laptop.peerCount == 0 }, "the laptop loses the link")
            assertEq(1, phone.peerCount)

            laptopLink.connect(2)
            expect(waitUntil { laptop.peerCount == 1 && 1 in phoneLink.closed }, "the phone lets the dead link go")
            expect(waitUntil { phone.diagnosticsDetail().contains(": replaced") }, phone.diagnosticsDetail())
            expect(2 !in phoneLink.closed && 2 !in laptopLink.closed, "the new link is kept")
            assertEq(1, phone.peerCount)

            laptop.sendMessage("back again")
            expect(waitUntil { phone.messages.any { it.text == "back again" } }, "the new link carries the room")
        } finally {
            closeAll(phone, laptop)
        }
    }

    fun testCrossingDialsStillKeepTheLinkTheHigherIdOpened() {
        val (low, high) = lowAndHigh()
        val lowLink = FakeBluetooth.pairOf("low", "high")
        val highLink = lowLink.peer
        val lower = room("low", lowLink, store = low)
        val higher = room("high", highLink, store = high)
        try {
            expect(lower.start())
            expect(higher.start())
            lowLink.connect(1)
            highLink.connect(2)
            expect(waitUntil { 1 in lowLink.closed && 1 in highLink.closed }, "both ends drop the link the lower id opened")
            expect(waitUntil { lower.peerCount == 1 && higher.peerCount == 1 })
            expect(2 !in lowLink.closed && 2 !in highLink.closed, "the higher id's link survives")

            lower.sendMessage("one link left")
            expect(waitUntil { higher.messages.any { it.text == "one link left" } })
        } finally {
            closeAll(lower, higher)
        }
    }

    fun testTheLanLinkIsKeptWhenBluetoothLinksTheSamePeerAfterIt() {
        val adaLink = FakeBluetooth.pairOf("ada", "grace")
        val graceLink = adaLink.peer
        val ada = room("ada", adaLink, network = true)
        val grace = room("grace", graceLink, network = true)
        try {
            expect(ada.start())
            expect(grace.start())
            expect(ada.connectToPeer(loopback, grace.listenPort))
            expect(waitUntil { ada.peerCount == 1 && grace.peerCount == 1 }, "the LAN link comes up")

            adaLink.connect(1)
            expect(waitUntil { 1 in adaLink.closed && 1 in graceLink.closed }, "both ends drop the Bluetooth link")
            expect(waitUntil { ada.diagnosticsLine().contains("links 1 ") }, ada.diagnosticsLine())
            assertEq(1, grace.peerCount)

            ada.sendMessage("still on the lan")
            expect(waitUntil { grace.messages.any { it.text == "still on the lan" } })
        } finally {
            closeAll(ada, grace)
        }
    }

    fun testTheLanLinkReplacesABluetoothLinkToTheSamePeer() {
        val adaLink = FakeBluetooth.pairOf("ada", "grace")
        val graceLink = adaLink.peer
        val ada = room("ada", adaLink, network = true)
        val grace = room("grace", graceLink, network = true)
        try {
            expect(ada.start())
            expect(grace.start())
            adaLink.connect(1)
            expect(waitUntil { ada.peerCount == 1 && grace.peerCount == 1 }, "the Bluetooth link comes up")

            expect(grace.connectToPeer(loopback, ada.listenPort))
            expect(waitUntil { 1 in adaLink.closed && 1 in graceLink.closed }, "both ends drop the Bluetooth link")
            expect(waitUntil { grace.diagnosticsLine().contains("links 1 ") }, grace.diagnosticsLine())
            expect(grace.diagnosticsLine().contains("dial 1 ok 1"), grace.diagnosticsLine())
            assertEq(1, ada.peerCount)

            grace.sendMessage("moved to the lan")
            expect(waitUntil { ada.messages.any { it.text == "moved to the lan" } })
        } finally {
            closeAll(ada, grace)
        }
    }

    fun testALinkThatNeverSaysHelloIsDroppedAfterTheHandshakeTimeout() {
        val link = FakeBluetooth.pairOf("phone", "silent")
        val phone = room("phone", link)
        try {
            expect(phone.start())
            link.connect(7)
            expect(waitUntil { phone.diagnosticsLine().contains("links 1 ") }, phone.diagnosticsLine())
            phone.loop.advance(14_000)
            expect(7 !in link.closed, "still inside the handshake window")
            phone.loop.advance(2_000)
            expect(waitUntil { 7 in link.closed }, "the silent link is closed")
            expect(phone.diagnosticsDetail().contains("handshake timeout"), phone.diagnosticsDetail())
            expect(phone.diagnosticsLine().contains("links 0 "), phone.diagnosticsLine())
        } finally {
            closeAll(phone)
        }
    }

    fun testALinkThatSaidHelloOutlivesTheHandshakeTimeout() {
        val phoneLink = FakeBluetooth.pairOf("phone", "laptop")
        val phone = room("phone", phoneLink)
        val laptop = room("laptop", phoneLink.peer)
        try {
            expect(phone.start())
            expect(laptop.start())
            phoneLink.connect(1)
            expect(waitUntil { phone.peerCount == 1 && laptop.peerCount == 1 })
            phone.loop.advance(20_000)
            laptop.loop.advance(20_000)
            assertEq(1, phone.peerCount)
            assertEq(1, laptop.peerCount)
            expect(phoneLink.closed.isEmpty() && phoneLink.peer.closed.isEmpty(), "no link was dropped")
        } finally {
            closeAll(phone, laptop)
        }
    }
}
