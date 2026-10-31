package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.model.Member
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.nio.file.Files

class ChatHubHardeningTest {
    private val conversationId = "d".repeat(32)

    private fun directApi(): FakeAccountApi {
        val api = FakeAccountApi()
        api.replies["GET /api/chat/direct-messages"] = mapOf(
            "conversations" to listOf(mapOf("id" to conversationId, "otherUser" to "pat", "unreadCount" to 0L)),
            "nextCursor" to "",
        )
        return api
    }

    private fun hubWithDirect(): Triple<ChatHub, FakeChatRelay, ArrayList<String>> {
        val relay = FakeChatRelay()
        relay.peers.add(Member(id = "pat-node", name = "Pat", owner = "pat", online = true))
        relay.peers.add(Member(id = "guest-node", name = "Guest 1234", online = true))
        val hub = ChatHub(relay)
        val dm = DirectMessageService(
            directApi(), FakeOfficeConnector(), MemorySecretStore(), { "https://app.forkmesh.com" }, { "me-node" }, { "Me" },
            loop = EventLoop("hub-hardening", virtualTime = true),
        )
        dm.refresh()
        hub.direct = dm
        val notices = ArrayList<String>()
        hub.onNotice = { notices.add(it) }
        return Triple(hub, relay, notices)
    }

    fun testMessageOnAPersonOpensTheirAccountsDirectConversation() {
        val (hub, relay, notices) = hubWithDirect()
        hub.switchConversation("@pat-node")
        assertEq("direct:$conversationId", relay.currentConversation)
        relay.conversation = "#general"
        hub.switchConversation("@guest-node")
        assertEq("#general", relay.currentConversation, "a guest has no direct conversation")
        assertEq(ChatHub.GUEST_DIRECT_NOTICE, notices.last())
        assertEq(null, hub.messageAccount(relay.roster().single { it.id == "guest-node" }))
        assertEq("pat", hub.messageAccount(relay.roster().single { it.id == "pat-node" }))
        expect(relay.sent.none { it["type"] == "dm" })
    }

    fun testRelayPrivateHistoryIsDroppedOnStart() {
        val dir = Files.createTempDirectory("chat-hardening").toFile()
        try {
            fun message(id: String, conversation: String) =
                ChatMessage(id = id, conversation = conversation, senderId = "peer", senderName = "Peer", text = id, timestamp = 1_000L + id.length)
            val now = System.currentTimeMillis()
            val snapshot = ChatHistorySnapshot(
                messages = listOf(
                    message("pub", "#general").copy(timestamp = now),
                    message("at", "@peer").copy(timestamp = now),
                    message("vault", "#vault").copy(timestamp = now),
                    message("dm", "direct:$conversationId").copy(timestamp = now),
                ),
                replies = listOf(message("vault-reply", "#vault").copy(timestamp = now), message("pub-reply", "#general").copy(timestamp = now)),
                replyRoots = mapOf("vault-reply" to "vault", "pub-reply" to "pub"),
                reactions = mapOf("at" to mapOf("like" to mapOf("peer" to "Peer")), "pub" to mapOf("like" to mapOf("peer" to "Peer"))),
                reactionOrder = mapOf("at" to listOf("like"), "pub" to listOf("like")),
                edits = mapOf("vault" to "changed", "vault-reply" to "changed", "pub" to "kept"),
                privateChannels = listOf("#vault"),
            )
            val store = ChatHistoryStore(dir)
            store.save(snapshot, "scope-a")
            val (hub, relay) = hubWith()
            hub.attachHistory(store) { "scope-a" }
            assertEq(listOf("pub"), relay.messages("#general").map { it.id })
            expect(relay.messages("@peer").isEmpty() && relay.messages("#vault").isEmpty())
            expect("#vault" !in hub.channels)
            assertEq(1, hub.rows("#general").single().thread?.replyCount)
            assertEq("kept", hub.rows("#general").single().text)
            hub.saveHistory()
            val saved = (store.read("scope-a") as ChatHistoryLoad.Loaded).snapshot
            assertEq(emptyList<String>(), saved.privateChannels)
            expect(saved.messages.none { it.conversation.startsWith("@") || it.conversation == "#vault" })
            assertEq(setOf("pub"), saved.edits.keys)
            assertEq(setOf("pub"), saved.reactions.keys)
            assertEq(setOf("pub-reply"), saved.replyRoots.keys)
            val purged = hub.withoutRelayPrivate(snapshot)
            assertEq(listOf("pub", "dm"), purged.messages.map { it.id })
        } finally {
            dir.listFiles()?.forEach { it.delete() }
            dir.delete()
        }
    }

    fun testAReplyToAPersonGoesThroughTheirDirectConversation() {
        val (hub, relay, _) = hubWithDirect()
        try {
            hub.reply("@guest-node", "hello")
            throw AssertionError("a guest cannot be messaged")
        } catch (_: ChatReplyException) {
        }
        expect(relay.sent.none { it["type"] == "dm" || it["to"] != null })
    }

    fun testBlocksAreKeptByTheServer() {
        val api = FakeAccountApi()
        val prefs = MemoryPrefStore()
        val blocks = ChatBlocks(api, prefs)
        expect(blocks.set("@Mallory", true))
        assertEq(Triple("PUT", ChatBlocks.PATH, mapOf<String, Any?>("accounts" to listOf("mallory"))), api.calls.last())
        expect(!blocks.set("mallory", true), "blocking twice changes nothing")
        expect(blocks.isBlocked("MALLORY"))
        api.failures["PUT ${ChatBlocks.PATH}"] = RuntimeException("offline")
        expect(blocks.set("eve", true))
        api.failures.clear()
        api.replies["GET ${ChatBlocks.PATH}"] = mapOf("accounts" to listOf("trent"))
        val restarted = ChatBlocks(api, prefs)
        assertEq(listOf("mallory", "eve"), restarted.accounts)
        expect(restarted.signedIn())
        assertEq(Triple("PUT", ChatBlocks.PATH, mapOf<String, Any?>("accounts" to listOf("mallory", "eve"))), api.calls.last(), "an unsent change is pushed at sign-in")
        expect(restarted.signedIn())
        assertEq("GET", api.calls.last().first)
        assertEq(listOf("trent"), restarted.accounts, "at sign-in the server's list wins")
        expect(restarted.set("trent", false))
        assertEq(mapOf<String, Any?>("accounts" to emptyList<String>()), api.calls.last().third)
    }

    fun testABlockedAccountsMessagesAndConversationsAreHidden() {
        val (hub, relay) = hubWith()
        relay.peers.add(Member(id = "mallory-node", name = "Mallory", owner = "mallory", online = true))
        relay.handle(peerFrame("chat", mapOf("channel" to "#general", "text" to "spam"), id = "s-1", sender = "mallory-node"))
        relay.handle(peerFrame("chat", mapOf("channel" to "#general", "text" to "fine"), id = "s-2"))
        hub.blocks = ChatBlocks(FakeAccountApi())
        expect(hub.setBlocked("mallory", true))
        assertEq(listOf("fine"), hub.rows("#general").map { it.text })
        expect(hub.setBlocked("mallory", false))
        assertEq(2, hub.rows("#general").size)
    }

    fun testADeleteForAMessageWithNoKnownAuthorChangesNothing() {
        val (hub, relay) = hubWith()
        hub.extras.restore(emptyList(), emptyMap(), mapOf("ghost" to mapOf("like" to mapOf("peer-1" to "Pat"))), mapOf("ghost" to listOf("like")), mapOf("ghost" to "kept"))
        relay.handle(peerFrame("delete", mapOf("target" to "ghost", "conversation" to "#general"), id = "d-1", sender = "mallory-node"))
        assertEq(mapOf("ghost" to "kept"), hub.extras.snapshotEdits(), "only the authenticated author may delete")
        assertEq(setOf("ghost"), hub.extras.snapshotReactions().keys)
    }
}
