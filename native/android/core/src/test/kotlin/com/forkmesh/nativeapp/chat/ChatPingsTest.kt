package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.inbox.InboxSource
import com.forkmesh.nativeapp.inbox.LocalPingLog
import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class ChatPingsTest {
    private val now = 1_000_000_000L

    private fun hub(prefs: MemoryPrefStore = MemoryPrefStore()): Triple<ChatHub, FakeChatRelay, LocalPingLog> {
        val relay = FakeChatRelay()
        val hub = ChatHub(relay)
        val log = LocalPingLog(prefs, { now })
        hub.pings = log
        return Triple(hub, relay, log)
    }

    private fun message(id: String, conversation: String, text: String = "hello", ts: Long = now - 1_000, self: Boolean = false, fileName: String = "") =
        ChatMessage(id, conversation, "pat-node", "Pat", text, ts, self, fileName)

    fun testIncomingMessagesBecomeChatPings() {
        val (hub, relay, log) = hub()
        relay.conversation = "#random"
        var changed = 0
        hub.onPingsChanged = { changed++ }
        hub.filePing(message("m1", "#general"), now)
        hub.filePing(message("m2", "@pat-node", text = "  are you there?  "), now)
        hub.filePing(message("m3", "direct:" + "d".repeat(32), text = "", fileName = "plan.pdf"), now)
        assertEq(3, changed)
        val items = log.items
        assertEq(listOf("local:chat:m1", "local:chat:m2", "local:chat:m3"), items.map { it.id })
        assertEq(listOf("Pat in #general", "Pat sent you a message", "Pat sent you a message"), items.map { it.title })
        assertEq(listOf("hello", "are you there?", "📎 plan.pdf"), items.map { it.body })
        expect(items.all { it.source == InboxSource.Local && it.kind == "chat" && it.unread })
        assertEq(Route.Channel("#general"), items[0].route)
        assertEq("Chat", items[0].kindLabel)
        assertEq("chat_bubble_outline", items[0].icon)
    }

    fun testOwnStaleAndRepeatedMessagesAreSkipped() {
        val (hub, _, log) = hub()
        hub.filePing(message("m1", "#general", self = true), now)
        hub.filePing(message("m2", "#general", ts = now - ChatHub.PING_FRESH_MS), now)
        hub.filePing(message("", "#general"), now)
        hub.filePing(message("m3", "#general"), now)
        hub.filePing(message("m3", "#general"), now)
        assertEq(listOf("chat:m3"), log.pings.map { it.id })
        hub.pings = null
        hub.filePing(message("m4", "#general"), now)
        assertEq(1, log.pings.size)
    }

    fun testWelcomeMessagesReadAsNewUsers() {
        val (hub, _, log) = hub()
        hub.filePing(message("w1", "#welcome", text = "hi all"), now)
        assertEq("New user joined", log.pings[0].title)
        assertEq("Pat said hello in #welcome", log.pings[0].message)
    }

    fun testTheOpenConversationFilesReadAndOpeningMarksRead() {
        val prefs = MemoryPrefStore()
        val (hub, relay, log) = hub(prefs)
        relay.conversation = "#general"
        hub.filePing(message("m1", "#general"), now)
        hub.filePing(message("m2", "#random"), now)
        assertEq(listOf(false, true), log.items.map { it.unread })
        var changed = 0
        hub.onPingsChanged = { changed++ }
        hub.select("#random")
        assertEq(1, changed)
        assertEq(listOf(false, false), log.items.map { it.unread })
        hub.select("#random")
        assertEq(1, changed)
        val reloaded = LocalPingLog(prefs).pings
        assertEq(listOf("chat", "chat"), reloaded.map { it.kind })
        assertEq(listOf("#general", "#random"), reloaded.map { it.conversation })
    }

    fun testOlderFailurePingsStillLoad() {
        val prefs = MemoryPrefStore()
        prefs.putString(LocalPingLog.KEY, "[{\"id\":\"a\",\"surface\":\"git\",\"title\":\"Push failed\",\"message\":\"x\",\"tsMs\":5,\"read\":false}]")
        val item = LocalPingLog(prefs).items.single()
        assertEq("local_failure", item.kind)
        assertEq(null, item.route)
        assertEq("On this phone", item.kindLabel)
    }
}
