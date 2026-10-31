package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.api.AccountSettings
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.crypto.Bytes
import com.forkmesh.nativeapp.crypto.ChatDeviceKeys
import com.forkmesh.nativeapp.crypto.RoomCrypto
import com.forkmesh.nativeapp.crypto.RoomSeal
import com.forkmesh.nativeapp.crypto.SealPolicy
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.model.ChatReceipt
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

    private fun service(api: FakeAccountApi, connector: OfficeSocketConnector, store: MemorySecretStore, self: String, loop: EventLoop) =
        DirectMessageService(
            api, connector, store, { "https://app.forkmesh.com" }, { "$self-node" }, { self },
            loop = loop,
            account = { self },
        )

    private fun dmSeal(store: MemorySecretStore, acct: String) =
        RoomSeal.loadOrCreate(room, scope = channel, policy = SealPolicy.STRICT, acct = acct, secureStore = store, node = "$acct-node")

    private fun learn(connector: DroppableConnector, otherStore: MemorySecretStore, otherAcct: String) {
        connector.last!!.onText(Json.stringify(crypto.encrypt(dmSeal(otherStore, otherAcct).keyFrame())))
    }

    private fun relayChats(socket: FakeOfficeSocket, from: Int): Int =
        socket.sent.drop(from).count { text ->
            val framed = crypto.decrypt(Json.parseObject(text)) ?: return@count false
            RoomSeal.isRatchetFrame(framed)
        }

    fun testDirectMessagesUseAuthenticatedLinksAndTheFocusRule() {
        val annStore = MemorySecretStore()
        val patStore = MemorySecretStore()
        val eveStore = MemorySecretStore()
        val annMesh = mesh("ann", annStore)
        val patMesh = mesh("pat", patStore)
        val eveMesh = mesh("eve", eveStore)
        val annLoop = EventLoop("mesh-direct-ann", virtualTime = true)
        val patLoop = EventLoop("mesh-direct-pat", virtualTime = true)
        try {
            val annApi = api("pat")
            val annConnector = DroppableConnector()
            val ann = service(annApi, annConnector, annStore, "ann", annLoop)
            val patConnector = DroppableConnector()
            val pat = service(api("ann"), patConnector, patStore, "pat", patLoop)
            ann.refresh()
            pat.refresh()
            expect(ann.open(channel) && pat.open(channel))
            learn(annConnector, patStore, "pat")
            learn(patConnector, annStore, "ann")
            val annKid = ChatDeviceKeys.forStore(annStore).kid
            val patKid = ChatDeviceKeys.forStore(patStore).kid
            val eveKid = ChatDeviceKeys.forStore(eveStore).kid
            assertEq(listOf(patKid), ann.deviceKeys(channel), "ann's conversation approved pat's one device")

            ann.mesh = annMesh
            pat.mesh = patMesh
            annMesh.onDirect = { kid, frame -> ann.acceptMeshFrame(kid, frame) }
            patMesh.onDirect = { kid, frame -> pat.acceptMeshFrame(kid, frame) }
            annMesh.onLinksChanged = { ann.meshLinksChanged() }
            val eveSaw = ArrayList<Map<String, Any?>>()
            eveMesh.onDirect = { _, frame -> if (frame["type"] != DirectMessageFrames.FOCUS) synchronized(eveSaw) { eveSaw.add(frame) } }
            val patAlerts = ArrayList<ChatMessage>()
            pat.onMessage = { synchronized(patAlerts) { patAlerts.add(it) } }

            expect(annMesh.start() && patMesh.start() && eveMesh.start())
            assertEq(annKid, annMesh.sealKeyId, "the mesh and the conversation share one device identity")
            expect(annMesh.connectToPeer(loopback, patMesh.listenPort))
            expect(eveMesh.connectToPeer(loopback, annMesh.listenPort))
            expect(eveMesh.connectToPeer(loopback, patMesh.listenPort))
            expect(waitUntil { annMesh.peerCount == 2 && patMesh.peerCount == 2 && eveMesh.peerCount == 2 }, "the three phones link")
            expect(waitUntil { annMesh.linkedKids().keys == setOf(patKid, eveKid) }, "links carry authenticated device ids")
            assertEq("Direct · Wi-Fi", ann.routeLabel(channel))

            annApi.failures["GET /api/chat/direct-messages/$conversationId/room-access"] = IllegalStateException("offline")
            annConnector.drop()
            expect(!ann.isConnected(channel), "ann's relay socket is gone")
            expect(ann.send(channel, "the router is down"), "the send still succeeds over the mesh")
            assertEq(listOf("the router is down"), ann.messages(channel).map { it.text })
            expect(waitUntil { pat.messages(channel).any { it.text == "the router is down" } }, "pat's phone shows it")
            val shown = pat.messages(channel).single { it.text == "the router is down" }
            assertEq("ann", shown.senderName)
            expect(!shown.self)
            expect(waitUntil { synchronized(patAlerts) { patAlerts.size == 1 } }, "and raises one alert")
            val firstId = ann.messages(channel).single().id
            expect(waitUntil { ann.receipt(firstId) == ChatReceipt.DELIVERED }, "pat's direct-ack marks it delivered")
            expect(synchronized(eveSaw) { eveSaw.isEmpty() }, "the third phone on the mesh is never sent it")
            expect(eveMesh.messages.isEmpty() && patMesh.messages.isEmpty(), "and it never lands in #mesh")

            val forged = dmSeal(eveStore, "ann").seal(
                linkedMapOf(
                    "type" to "chat", "id" to "eve-1", "senderId" to "ann-node", "sender" to "ann",
                    "channel" to channel, "text" to "it is ann, honest", "ts" to System.currentTimeMillis(),
                ),
            )
            expect(eveMesh.sendDirect(patKid, DirectMessageFrames.directSealed(channel, forged)), "eve can reach pat's link")
            Thread.sleep(500)
            expect(pat.messages(channel).none { it.text.contains("honest") }, "but pat never shows a device the chat did not approve")

            annApi.failures.remove("GET /api/chat/direct-messages/$conversationId/room-access")
            expect(ann.open(channel))
            val relay = annConnector.last!!
            ann.screenFocus(channel)
            pat.screenFocus(channel)
            expect(waitUntil { annMesh.peerFocused(patKid, channel) }, "pat's focus reaches ann over the link")
            expect(waitUntil { patMesh.peerFocused(annKid, channel) })
            expect(!eveMesh.peerFocused(annKid, channel) && !eveMesh.peerFocused(patKid, channel), "a phone outside the conversation is not told who is looking at it")
            assertEq(DirectMessageFrames.ROUTE_DIRECT_ONLY, ann.routeLabel(channel))
            val before = relay.sent.size
            expect(ann.send(channel, "only between us"))
            expect(waitUntil { pat.messages(channel).any { it.text == "only between us" } })
            val onlyId = ann.messages(channel).last().id
            expect(waitUntil { ann.receipt(onlyId) == ChatReceipt.DELIVERED })
            annLoop.advance(DirectMessageFrames.DIRECT_ACK_TIMEOUT_MS + 1)
            Thread.sleep(300)
            assertEq(0, relayChats(relay, before), "while both look at the chat nothing reaches the server")

            patMesh.onDirect = { kid, frame -> if (frame["type"] != DirectMessageFrames.DIRECT_SEALED) pat.acceptMeshFrame(kid, frame) }
            val silent = relay.sent.size
            expect(ann.send(channel, "is anyone there"))
            assertEq(0, relayChats(relay, silent))
            annLoop.advance(DirectMessageFrames.DIRECT_ACK_TIMEOUT_MS + 1)
            expect(waitUntil { relayChats(relay, silent) == 1 }, "no ack within 3 s sends the relay copy")
            patMesh.onDirect = { kid, frame -> pat.acceptMeshFrame(kid, frame) }

            pat.screenFocus("")
            expect(waitUntil { !annMesh.peerFocused(patKid, channel) })
            assertEq("Direct · Wi-Fi", ann.routeLabel(channel))
            val both = relay.sent.size
            expect(ann.send(channel, "back to normal"))
            assertEq(1, relayChats(relay, both), "without the other side's focus the relay copy goes too")
            expect(waitUntil { pat.messages(channel).count { it.text == "back to normal" } == 1 })
        } finally {
            for (node in listOf(annMesh, patMesh, eveMesh)) {
                node.stop()
                node.dispose()
            }
            annLoop.shutdown()
            patLoop.shutdown()
        }
    }

    fun testNoLinkedRecipientMeansTheSendStillNeedsTheRelay() {
        val store = MemorySecretStore()
        val api = api("pat")
        api.failures["GET /api/chat/direct-messages/$conversationId/room-access"] = IllegalStateException("offline")
        val dm = service(api, DroppableConnector(), store, "ann", EventLoop("mesh-direct-none", virtualTime = true))
        dm.refresh()
        var asked = 0
        dm.mesh = object : DirectMeshLinks {
            override fun linkedKids(): Map<String, MeshRoute> {
                asked++
                return emptyMap()
            }

            override fun peerFocused(kid: String, channel: String): Boolean = false

            override fun sendDirect(kid: String, frame: Map<String, Any?>): Boolean = error("nothing is linked")

            override fun setFocus(channel: String, on: Boolean, kids: Set<String>) {}
        }
        expect(!dm.send(channel, "hello?"), "nothing reached anyone")
        expect(asked > 0)
        expect(dm.messages(channel).isEmpty())
        assertEq("", dm.routeLabel(channel))
    }
}
