package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.api.AccountSettings
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.crypto.Bytes
import com.forkmesh.nativeapp.crypto.RoomCrypto
import com.forkmesh.nativeapp.crypto.RoomSeal
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.security.Identity
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.net.InetAddress

class MeshDirectMessagesTest {
    private val loopback: InetAddress = InetAddress.getByName("127.0.0.1")
    private val conversationId = "f".repeat(32)
    private val room = "chat-direct:$conversationId:v1"
    private val channel = "direct:$conversationId"
    private val crypto = RoomCrypto.withPassphrase(room, "direct-pass")

    private class DroppableConnector : OfficeSocketConnector {
        var last: FakeOfficeSocket? = null
        var drop: () -> Unit = {}

        override fun connect(url: String, onText: (String) -> Unit, onClosed: () -> Unit): OfficeSocket =
            FakeOfficeSocket().also {
                it.onText = onText
                last = it
                drop = onClosed
            }
    }

    private fun mesh(name: String, store: MemorySecretStore): LocalMeshRoom {
        val settings = AccountSettings.create(MemoryPrefStore(mapOf("profile/name" to name)), MemorySecretStore())
        val mesh = LocalMeshRoom(settings, Identity.fromSeed(Bytes.random(32)), store, platform = "linux")
        mesh.listenAddress = loopback
        mesh.accountProvider = { name }
        mesh.multicastEnabled = false
        mesh.bluetoothEnabled = false
        mesh.addressSweepEnabled = false
        return mesh
    }

    private fun api(other: String): FakeAccountApi {
        val api = FakeAccountApi()
        api.replies["GET /api/chat/direct-messages"] = mapOf(
            "conversations" to listOf(mapOf("id" to conversationId, "otherUser" to other)),
            "nextCursor" to "",
        )
        api.replies["GET /api/chat/direct-messages/$conversationId/room-access"] = mapOf(
            "room" to room, "passphrase" to "direct-pass",
            "webSocketUrl" to "/api/chat/direct-messages/$conversationId/ws?ticket=abc",
        )
        return api
    }

    private fun service(api: FakeAccountApi, connector: OfficeSocketConnector, store: MemorySecretStore, self: String) =
        DirectMessageService(
            api, connector, store, { "https://app.forkmesh.com" }, { "$self-node" }, { self },
            loop = EventLoop("mesh-direct-$self", virtualTime = true),
        )

    private fun learn(connector: DroppableConnector, otherStore: MemorySecretStore) {
        val other = RoomSeal.loadOrCreate(room, storageScope = "peer", secureStore = otherStore)
        connector.last!!.onText(Json.stringify(crypto.encrypt(other.keyFrame())))
    }

    fun testADirectMessageReachesTheOtherPhoneOverTheMeshWithTheRelayDown() {
        val annStore = MemorySecretStore()
        val patStore = MemorySecretStore()
        val eveStore = MemorySecretStore()
        val annMesh = mesh("ann", annStore)
        val patMesh = mesh("pat", patStore)
        val eveMesh = mesh("eve", eveStore)
        try {
            val annApi = api("pat")
            val annConnector = DroppableConnector()
            val ann = service(annApi, annConnector, annStore, "ann")
            val patConnector = DroppableConnector()
            val pat = service(api("ann"), patConnector, patStore, "pat")
            ann.refresh()
            pat.refresh()
            expect(ann.open(channel) && pat.open(channel))
            learn(annConnector, patStore)
            learn(patConnector, annStore)

            expect(annMesh.start() && patMesh.start() && eveMesh.start())
            assertEq(listOf(patMesh.sealKeyId), ann.deviceKeys(channel), "ann's room knows pat's one device key")
            expect(patMesh.sealKeyId != eveMesh.sealKeyId)
            expect(annMesh.connectToPeer(loopback, patMesh.listenPort))
            expect(eveMesh.connectToPeer(loopback, annMesh.listenPort))
            expect(eveMesh.connectToPeer(loopback, patMesh.listenPort))
            expect(waitUntil { annMesh.peerCount == 2 && patMesh.peerCount == 2 && eveMesh.peerCount == 2 }, "the three phones link")
            Thread.sleep(300)

            ann.meshSender = { plain, keys -> annMesh.sendDirect(plain, keys) }
            patMesh.onDirect = { wire, keyId, plain -> pat.acceptMeshDirect(wire, keyId, plain) }
            val eveSaw = ArrayList<Map<String, Any?>>()
            eveMesh.onDirect = { _, _, plain -> synchronized(eveSaw) { eveSaw.add(plain) } }
            val patAlerts = ArrayList<ChatMessage>()
            pat.onMessage = { synchronized(patAlerts) { patAlerts.add(it) } }

            annApi.failures["GET /api/chat/direct-messages/$conversationId/room-access"] = IllegalStateException("offline")
            annConnector.drop()
            expect(!ann.isConnected(channel), "ann's relay socket is gone")
            expect(ann.send(channel, "the router is down"), "the send still succeeds over the mesh")
            assertEq(listOf("the router is down"), ann.messages(channel).map { it.text })
            expect(waitUntil { pat.messages(channel).any { it.text == "the router is down" } }, "pat's phone shows it")
            val shown = pat.messages(channel).single()
            assertEq("ann", shown.senderName)
            expect(!shown.self)
            expect(waitUntil { synchronized(patAlerts) { patAlerts.size == 1 } }, "and raises one alert")
            Thread.sleep(300)
            expect(synchronized(eveSaw) { eveSaw.isEmpty() }, "the third phone on the mesh is never sent it")
            expect(eveMesh.messages.isEmpty() && patMesh.messages.isEmpty(), "and it never lands in #mesh")

            val forged = linkedMapOf<String, Any?>(
                "type" to "direct", "id" to "eve-1", "senderId" to "ann-node", "sender" to "ann",
                "channel" to channel, "text" to "it is ann, honest", "ts" to System.currentTimeMillis(),
            )
            expect(eveMesh.sendDirect(forged, listOf(patMesh.sealKeyId)) == 1, "eve can address pat's device key")
            Thread.sleep(500)
            expect(pat.messages(channel).none { it.text.contains("honest") }, "but pat refuses a key the conversation never saw")

            pat.closeAll()
            expect(!pat.isConnected(channel))
            val replay = pat.messages(channel).single()
            expect(!pat.acceptMeshDirect(channel, annMesh.sealKeyId, mapOf("channel" to channel, "id" to replay.id, "text" to replay.text)),
                "a message already shown is not shown twice")
        } finally {
            for (node in listOf(annMesh, patMesh, eveMesh)) {
                node.stop()
                node.dispose()
            }
        }
    }

    fun testNoLinkedRecipientMeansTheSendStillNeedsTheRelay() {
        val store = MemorySecretStore()
        val api = api("pat")
        api.failures["GET /api/chat/direct-messages/$conversationId/room-access"] = IllegalStateException("offline")
        val dm = service(api, DroppableConnector(), store, "ann")
        dm.refresh()
        var asked = 0
        dm.meshSender = { _, _ -> asked++; 0 }
        expect(!dm.send(channel, "hello?"), "nothing reached anyone")
        assertEq(0, asked, "no device key is known, so the mesh is not even asked")
        expect(dm.messages(channel).isEmpty())
    }
}
