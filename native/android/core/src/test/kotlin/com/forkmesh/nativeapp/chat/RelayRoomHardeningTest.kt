package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.crypto.ChatV3
import com.forkmesh.nativeapp.crypto.RoomSeal
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class RelayRoomHardeningTest {
    private fun peerChat(peer: RoomSeal, type: String, fields: Map<String, Any?>, id: String): Map<String, Any?> {
        val plain = linkedMapOf<String, Any?>("type" to type, "id" to id, "senderId" to peer.node, "sender" to peer.acct, "ts" to System.currentTimeMillis())
        plain.putAll(fields)
        return peer.seal(plain)
    }

    fun testTheBundleIsSignedByTheNodeKeyAndEveryFrameLeavesSealed() {
        RelayHarness { it.setSolanaAddress("So1anaTipAddress") }.use { h ->
            assertEq(h.room.selfId, h.bundle["node"])
            expect(ChatV3.verifyNodeBinding(h.bundle, h.settings.room), "the relay bundle carries a valid nsig")
            val alice = h.peer("alice")
            h.join(alice)
            expect(h.room.sendMessage("public hello", "#general"))
            val chat = h.opened(alice) { it["type"] == "chat" && it["text"] == "public hello" }
            expect(chat != null, "a public channel message is sealed to the room")
            assertEq(h.room.selfId, chat!!["senderId"])
            expect(!chat.containsKey("solana"), "no Solana address unless tips are on")
            expect(!chat.containsKey("timeZone") && !chat.containsKey("dg"))
            for (frame in h.drain()) expect(RoomSeal.isSealedFrame(frame) || RoomSeal.isKeyFrame(frame), "nothing leaves in the clear: ${frame["type"]}")
            h.settings.setChatTipsEnabled(true)
            expect(h.room.sendMessage("tip me", "#general"))
            assertEq("So1anaTipAddress", h.opened(alice) { it["text"] == "tip me" }?.get("solana"))
        }
    }

    fun testPrivateFramesAreNeitherSentNorAccepted() {
        RelayHarness().use { h ->
            val alice = h.peer("alice")
            h.join(alice)
            h.drain()
            expect(!h.room.sendMessage("psst", "@${alice.node}"), "no relay @peer messages")
            for (frame in listOf(
                linkedMapOf<String, Any?>("type" to "dm", "to" to alice.node, "text" to "x"),
                linkedMapOf<String, Any?>("type" to "chat", "channel" to "#plans", "private" to true, "text" to "x"),
                linkedMapOf<String, Any?>("type" to "invite", "to" to alice.node, "channel" to "#plans"),
                linkedMapOf<String, Any?>("type" to "cove-invite", "to" to alice.node),
                linkedMapOf<String, Any?>("type" to "cove-open", "to" to alice.node),
                linkedMapOf<String, Any?>("type" to "reaction", "conversation" to "@${alice.node}", "target" to "m", "emoji" to "x", "added" to true),
            )) {
                val made = h.room.makeFrame(jsonText(frame["type"]))
                made.putAll(frame)
                expect(!h.room.sendFrame(made), "refused: ${frame["type"]}")
            }
            expect(h.room.sendMessage("marker", "#general"))
            val leaked = ArrayList<Map<String, Any?>>()
            h.opened(alice) { plain ->
                if (plain["text"] != "marker") leaked.add(plain)
                plain["text"] == "marker"
            }
            assertEq(emptyList<Map<String, Any?>>(), leaked.filter { it["type"] != "hello" && it["type"] != "presence" })

            val observed = ArrayList<Map<String, Any?>>()
            h.room.frameObserver = { observed.add(it) }
            h.push(peerChat(alice, "dm", mapOf("to" to h.room.selfId, "text" to "secret dm"), "a-1"))
            h.push(peerChat(alice, "chat", mapOf("conversation" to "@${h.room.selfId}", "text" to "at dm"), "a-2"))
            h.push(peerChat(alice, "chat", mapOf("channel" to "#plans", "private" to true, "text" to "private"), "a-3"))
            h.push(peerChat(alice, "invite", mapOf("to" to h.room.selfId, "channel" to "#plans"), "a-4"))
            h.push(peerChat(alice, "cove-invite", mapOf("to" to h.room.selfId, "cove" to "c"), "a-5"))
            h.push(peerChat(alice, "chat", mapOf("channel" to "#general", "text" to "public"), "a-6"))
            h.settle()
            expect(h.room.messages("#general").any { it.text == "public" }, "the public frame arrives")
            expect(h.room.historySnapshot().keys.none { it.startsWith("@") || it == "#plans" })
            assertEq(listOf("a-6"), observed.map { jsonText(it["id"]) }.filter { it.startsWith("a-") })
        }
    }

    fun testUnsealedFramesNeedTheServerMarkAndShowAsTheIntegration() {
        RelayHarness().use { h ->
            val alice = h.peer("alice")
            h.join(alice)
            h.push(peerChat(alice, "chat", mapOf("channel" to "#general", "text" to "original"), "m-1"))
            h.settle()
            val observed = ArrayList<Map<String, Any?>>()
            h.room.frameObserver = { observed.add(it) }
            val forged = linkedMapOf<String, Any?>("type" to "chat", "id" to "u-1", "senderId" to alice.node, "sender" to "alice", "ts" to 5L, "channel" to "#general", "text" to "forged")
            h.push(forged)
            h.push(linkedMapOf("type" to "edit", "id" to "u-2", "senderId" to alice.node, "target" to "m-1", "conversation" to "#general", "text" to "edited"), mapOf("srv" to true))
            h.push(linkedMapOf("type" to "delete", "id" to "u-3", "senderId" to alice.node, "target" to "m-1", "conversation" to "#general"), mapOf("srv" to true))
            h.push(linkedMapOf("type" to "reaction", "id" to "u-4", "senderId" to alice.node, "reactorId" to alice.node, "target" to "m-1", "emoji" to "x", "added" to true), mapOf("srv" to true))
            h.push(
                linkedMapOf("type" to "chat", "id" to "w-1", "senderId" to alice.node, "sender" to "Deploys", "ownerUser" to "alice", "ts" to 6L, "channel" to "#general", "text" to "build passed"),
                mapOf("srv" to true),
            )
            h.push(linkedMapOf("type" to "chat", "id" to "w-2", "senderId" to "webhook:abc", "sender" to "CI", "ts" to 7L, "channel" to "#general", "text" to "deployed"), mapOf("srv" to true))
            h.settle()
            val texts = h.room.messages("#general").associateBy { it.id }
            expect("u-1" !in texts, "an unmarked unsealed frame is dropped")
            assertEq("original", texts["m-1"]?.text, "server frames cannot delete a person's message")
            assertEq(listOf("w-1", "w-2"), observed.map { jsonText(it["id"]) })
            val integration = texts["w-1"]!!
            assertEq(RelayFrames.SERVER_PREFIX + alice.node, integration.senderId, "an integration never takes a person's node id")
            assertEq("Deploys", integration.senderName)
            assertEq("webhook:abc", texts["w-2"]?.senderId)
            assertEq(RelayFrames.INTEGRATION_KIND, observed.first()["accountKind"])
            expect(observed.none { it.containsKey("ownerUser") })
            expect(h.room.roster().none { it.id.contains(':') }, "integrations are not people in the roster")
        }
    }

    fun testOnlyTheAuthenticatedAuthorEditsOrDeletes() {
        RelayHarness().use { h ->
            val hub = ChatHub(RelayRoomChat(h.room))
            h.room.frameObserver = { hub.extras.apply(it) }
            val alice = h.peer("alice")
            val mallory = h.peer("mallory")
            h.join(alice)
            h.join(mallory)
            h.push(peerChat(alice, "chat", mapOf("channel" to "#general", "text" to "alice wrote this"), "m-1"))
            h.push(peerChat(mallory, "edit", mapOf("target" to "m-1", "conversation" to "#general", "channel" to "#general", "text" to "mallory edit"), "e-1"))
            val claimed = linkedMapOf<String, Any?>("type" to "edit", "id" to "e-2", "senderId" to alice.node, "target" to "m-1", "conversation" to "#general", "text" to "claimed", "ts" to 1L)
            h.push(mallory.seal(claimed))
            h.push(peerChat(mallory, "delete", mapOf("target" to "m-1", "conversation" to "#general", "channel" to "#general"), "d-1"))
            h.push(peerChat(mallory, "reaction", mapOf("target" to "m-1", "conversation" to "#general", "reactorId" to alice.node, "emoji" to "👍", "added" to true), "r-1"))
            h.settle()
            val row = hub.rows("#general").single { it.message.id == "m-1" }
            assertEq("alice wrote this", row.text)
            expect(row.reactions.isEmpty(), "a reaction cannot be made in someone else's name")
            h.push(peerChat(alice, "edit", mapOf("target" to "m-1", "conversation" to "#general", "channel" to "#general", "text" to "alice edit"), "e-3"))
            h.settle()
            assertEq("alice edit", hub.rows("#general").single { it.message.id == "m-1" }.text)
            h.push(peerChat(alice, "delete", mapOf("target" to "m-1", "conversation" to "#general", "channel" to "#general"), "d-2"))
            h.settle()
            expect(hub.rows("#general").none { it.message.id == "m-1" })
        }
    }

    fun testFramesWithoutASenderIdAreTheSealedSenders() {
        val authenticated = RelayFrames.authenticated(mapOf("type" to "chat", "channel" to "#general"), "node-a")!!
        assertEq("node-a", authenticated["senderId"])
        assertEq(null, RelayFrames.authenticated(mapOf("type" to "chat", "senderId" to "node-b"), "node-a"))
        assertEq(null, RelayFrames.authenticated(mapOf("type" to "chat", "integration" to true, "private" to true), "node-a"))
        expect(!RelayFrames.authenticated(mapOf("type" to "chat", "integration" to true), "node-a")!!.containsKey("integration"))
        assertEq(null, RelayFrames.serverFrame(emptyMap(), mapOf("type" to "chat", "channel" to "#general")))
        assertEq(null, RelayFrames.serverFrame(mapOf("srv" to true), mapOf("type" to "thread-reply", "channel" to "#general")))
        expect(Json.stringify(RelayFrames.serverFrame(mapOf("srv" to true), mapOf("type" to "chat", "channel" to "#general", "senderId" to "x"))!!).contains("srv:x"))
    }
}
