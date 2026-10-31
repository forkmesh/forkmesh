package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.api.AccountSettings
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.crypto.Bytes
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.security.Identity
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.File
import java.net.InetAddress
import java.nio.file.Files

class LocalMeshQueueTest {
    private val loopback: InetAddress = InetAddress.getByName("127.0.0.1")

    private fun room(
        name: String,
        identity: Identity = Identity.fromSeed(Bytes.random(32)),
        history: File? = null,
        store: MemorySecretStore = MemorySecretStore(),
    ): LocalMeshRoom {
        val settings = AccountSettings.create(MemoryPrefStore(mapOf("profile/name" to name)), MemorySecretStore())
        val room = LocalMeshRoom(settings, identity, store, platform = "linux", historyFile = { history })
        room.listenAddress = loopback
        room.accountProvider = { name }
        room.multicastEnabled = false
        room.bluetoothEnabled = false
        room.addressSweepEnabled = false
        return room
    }

    fun testAReplyBeaconSaysSoAndAnOrdinaryOneDoesNot() {
        val reply = MeshBeacon(kid = "a", port = 1, reply = true).toJson()
        assertEq(true, reply["reply"])
        assertEq(true, MeshBeacon.parse(reply)!!.reply)
        val plain = MeshBeacon(kid = "a", port = 1).toJson()
        expect(!plain.containsKey("reply"))
        assertEq(false, MeshBeacon.parse(plain)!!.reply)
    }

    fun testAControlBeaconRoundTrips() {
        val beacon = MeshBeacon(kid = "desk", port = 48800, control = true, node = "desk-node").toJson()
        assertEq(true, MeshBeacon.parse(beacon)!!.control)
        assertEq("desk-node", MeshBeacon.parse(beacon)!!.node)
        assertEq(false, MeshBeacon.parse(MeshBeacon(kid = "desk", port = 1).toJson())!!.control)
    }

    fun testASubnetSweepStaysOnTheLocal24AndOffThisDevice() {
        val hosts = sweepAddresses(InetAddress.getByName("192.168.1.228"))
        assertEq(253, hosts.size)
        assertEq("192.168.1.1", hosts.first().hostAddress)
        assertEq("192.168.1.254", hosts.last().hostAddress)
        expect(hosts.none { it.hostAddress == "192.168.1.228" })
        assertEq(253, sweepAddresses(InetAddress.getByName("10.4.0.9")).size)
        expect(sweepAddresses(InetAddress.getByName("8.8.8.8")).isEmpty())
        expect(sweepAddresses(InetAddress.getByName("127.0.0.1")).isEmpty())
        expect(sweepAddresses(InetAddress.getByName("169.254.3.4")).isEmpty())
        expect(sweepAddresses(InetAddress.getByName("fe80::1")).isEmpty())
    }

    fun testTheBroadcastBeaconGoesToTheSwept24Only() {
        assertEq("192.168.1.255", sweepBroadcastAddress(InetAddress.getByName("192.168.1.228"))?.hostAddress)
        assertEq(null, sweepBroadcastAddress(InetAddress.getByName("8.8.8.8")))
        assertEq(null, sweepBroadcastAddress(InetAddress.getByName("127.0.0.1")))
        assertEq(null, sweepBroadcastAddress(InetAddress.getByName("169.254.3.4")))
    }

    fun testAMessageSentAloneWaitsSurvivesARestartAndReachesLaterDevices() {
        val dir = Files.createTempDirectory("mesh-history").toFile()
        val historyFile = File(dir, "history.json")
        val identity = Identity.fromSeed(Bytes.random(32))
        val keys = MemorySecretStore()
        val grace = room("grace")
        val carol = room("carol")
        var ada: LocalMeshRoom? = null
        try {
            val alone = room("ada", identity, historyFile, keys)
            expect(alone.start())
            val sent = alone.sendMessage("anyone out there?")
            expect(alone.isQueued(sent), "nobody is linked, so the message waits")
            alone.stop()
            alone.dispose()
            expect(historyFile.exists(), "history written at stop")

            val restarted = room("ada", identity, historyFile, keys)
            ada = restarted
            expect(restarted.start())
            assertEq(listOf(sent), restarted.messages.map { it.id })
            expect(restarted.isQueued(sent))
            assertEq(0, restarted.unread)

            expect(grace.start())
            expect(carol.start())
            expect(restarted.connectToPeer(loopback, grace.listenPort))
            expect(waitUntil { !restarted.isQueued(sent) && grace.messages.any { it.id == sent } }, "the first joiner takes it")
            expect(carol.connectToPeer(loopback, grace.listenPort))
            expect(waitUntil { carol.peerCount == 1 && grace.peerCount == 2 })
            expect(!waitUntil(1_500) { carol.messages.any { it.id == sent } }, "an earlier message is not re-sealed to a device that joined later")

            val line = restarted.diagnosticsLine()
            expect(line.contains("dial 1 ok 1 fail 0"), line)
            expect(line.contains("queued 0"), line)
            expect(grace.diagnosticsDetail().contains("peer ada"), grace.diagnosticsDetail())
        } finally {
            carol.stop()
            grace.stop()
            ada?.stop()
            carol.dispose()
            grace.dispose()
            ada?.dispose()
            dir.deleteRecursively()
        }
    }

    fun testWakeTokensAreReadOnlyInTheirOwnShape() {
        assertEq(42L, meshWakeTokenExpiry("mw1.abc.42.sig"))
        assertEq(null, meshWakeTokenExpiry("mw2.abc.42.sig"))
        assertEq(null, meshWakeTokenExpiry("mw1.abc.42"))
        assertEq(null, meshWakeTokenExpiry("mw1.abc.soon.sig"))
        assertEq(null, meshWakeTokenExpiry("mw1.${"a".repeat(200)}.42.sig"))
    }

    fun testADeviceThatLeftIsWokenByTheNextMessage() {
        val dir = Files.createTempDirectory("mesh-wake").toFile()
        val historyFile = File(dir, "history.json")
        val now = System.currentTimeMillis()
        val phoneToken = "mw1.${"a".repeat(64)}.${now + 3_600_000}.sig"
        val staleToken = "mw1.${"c".repeat(64)}.${now - 1000}.sig"
        fun entry(id: String, sender: String, wake: String) = linkedMapOf<String, Any?>(
            "type" to "chat", "id" to id, "senderId" to sender, "sender" to sender,
            "conversation" to MESH_CONVERSATION, "ts" to now - 60_000, "text" to "hi", "wake" to wake,
        )
        historyFile.writeText(
            Json.stringify(
                linkedMapOf<String, Any?>(
                    "v" to 1,
                    "history" to listOf(entry("p-1", "phone", phoneToken), entry("o-1", "old", staleToken)),
                    "queued" to emptyList<String>(),
                ),
            ),
        )
        val desk = room("desk", history = historyFile)
        val woken = ArrayList<List<String>>()
        desk.wakeSender = { woken.add(it) }
        try {
            expect(desk.start())
            assertEq(listOf(phoneToken), desk.wakeTargets())
            expect(desk.sendMessage("are you there?").isNotEmpty())
            assertEq(listOf(listOf(phoneToken)), woken)
        } finally {
            desk.stop()
            desk.dispose()
            historyFile.delete()
            dir.delete()
        }
    }

    fun testAPeerMessageIsAnnouncedOnceAndOwnMessagesAreNot() {
        val ada = room("ada")
        val grace = room("grace")
        val heard = ArrayList<Pair<String, Int>>()
        val echoed = ArrayList<String>()
        grace.onIncoming = { message, unread -> synchronized(heard) { heard.add(message.text to unread) } }
        ada.onIncoming = { message, _ -> synchronized(echoed) { echoed.add(message.text) } }
        try {
            expect(ada.start())
            expect(grace.start())
            expect(ada.connectToPeer(loopback, grace.listenPort))
            expect(waitUntil { ada.peerCount > 0 && grace.peerCount > 0 }, "linked")
            expect(ada.sendMessage("lunch is here").isNotEmpty())
            expect(waitUntil { synchronized(heard) { heard.isNotEmpty() } }, "grace hears it")
            expect(grace.sendMessage("coming").isNotEmpty())
            expect(waitUntil { synchronized(echoed) { echoed.isNotEmpty() } }, "ada hears the reply")
            assertEq(listOf("lunch is here" to 1), synchronized(heard) { ArrayList(heard) })
            assertEq(listOf("coming"), synchronized(echoed) { ArrayList(echoed) })
        } finally {
            ada.stop()
            grace.stop()
            ada.dispose()
            grace.dispose()
        }
    }

    fun testAStoppedRoomSaysSo() {
        val idle = room("ada")
        try {
            assertEq("mesh not running", idle.diagnosticsLine())
            expect(idle.controlHosts().isEmpty())
            assertEq("ada", idle.displayName)
        } finally {
            idle.dispose()
        }
    }
}
