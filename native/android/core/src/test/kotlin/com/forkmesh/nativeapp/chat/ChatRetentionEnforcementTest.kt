package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.api.AccountSettings
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.crypto.Bytes
import com.forkmesh.nativeapp.crypto.RoomCrypto
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.model.ChatRetention
import com.forkmesh.nativeapp.security.Identity
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.nio.file.Files

class ChatRetentionEnforcementTest {
    private val hour = 3_600_000L
    private val day = 24 * hour

    private fun relay(): RelayRoom {
        val settings = AccountSettings.create(MemoryPrefStore(), MemorySecretStore())
        return RelayRoom(settings, Identity.fromSeed(Bytes.random(32)), MemorySecretStore(), loop = EventLoop("retention", virtualTime = true))
    }

    private fun message(id: String, conversation: String, ts: Long) =
        ChatMessage(id = id, conversation = conversation, senderId = "peer", senderName = "Pat", text = id, timestamp = ts)

    fun testBoundedHistoryDropsExpiredMessagesPerChat() {
        val now = 30 * day
        val conversations = mapOf(
            "#general" to listOf(message("g-old", "#general", now - 2 * day), message("g-new", "#general", now - hour)),
            "#dev" to listOf(message("t-mid", "#dev", now - 2 * hour), message("t-new", "#dev", now - 60_000L)),
            "@peer" to listOf(message("d-old", "@peer", now - 8 * day), message("d-new", "@peer", now - 6 * day)),
        )
        val rule = ChatRetention(defaultMs = day, chats = mapOf("#dev" to hour, "@peer" to 7 * day))
        assertEq(listOf("t-new", "g-new", "d-new"), ChatHistoryCodec.bounded(conversations, rule, now).map { it.id })
        assertEq(6, ChatHistoryCodec.bounded(conversations).size)
    }

    fun testRelayRoomDropsExpiredFramesAndPrunesOnDemand() {
        val room = relay()
        try {
            val now = System.currentTimeMillis()
            room.setChatRetention(ChatRetention(defaultMs = hour, chats = mapOf("#team" to day)))
            room.applyFrame(mapOf("type" to "chat", "id" to "old", "senderId" to "peer", "sender" to "Pat", "channel" to "#general", "text" to "old", "ts" to now - 2 * hour))
            room.applyFrame(mapOf("type" to "chat", "id" to "fresh", "senderId" to "peer", "sender" to "Pat", "channel" to "#general", "text" to "fresh", "ts" to now - 60_000L))
            room.applyFrame(mapOf("type" to "chat", "id" to "team", "senderId" to "peer", "sender" to "Pat", "channel" to "#team", "text" to "team", "ts" to now - 2 * hour))
            room.applyFrame(mapOf("type" to "dm", "id" to "dm", "senderId" to "peer", "sender" to "Pat", "to" to room.selfId, "text" to "dm", "ts" to now - 3 * hour))
            assertEq(listOf("fresh"), room.messages("#general").map { it.id })
            assertEq(listOf("team"), room.messages("#team").map { it.id })
            expect(room.messages("@peer").isEmpty())

            room.restoreHistory(listOf(message("restored-old", "#random", now - 5 * hour), message("restored-new", "#random", now - 10_000L)))
            assertEq(listOf("restored-new"), room.messages("#random").map { it.id })

            expect(room.pruneExpired(now + 2 * hour))
            expect(room.messages("#general").isEmpty())
            expect(room.messages("#random").isEmpty())
            assertEq(listOf("team"), room.messages("#team").map { it.id })
            assertEq(0, room.unreadCountFor("#general"))
            expect(!room.pruneExpired(now + 2 * hour))
        } finally {
            room.dispose()
        }
    }

    fun testHubRestoresAndSavesOnlyLiveMessagesAndPrunesThreads() {
        val dir = Files.createTempDirectory("chat-retention").toFile()
        try {
            val now = System.currentTimeMillis()
            val (hub, relay) = hubWith()
            hub.attachHistory(ChatHistoryStore(dir)) { "scope-r" }
            relay.handle(peerFrame("chat", mapOf("channel" to "#general", "text" to "old", "ts" to now - 3 * hour), id = "p-old"))
            relay.handle(peerFrame("chat", mapOf("channel" to "#general", "text" to "new", "ts" to now - 60_000L), id = "p-new"))
            relay.handle(peerFrame("thread-reply", mapOf("channel" to "#general", "rootId" to "p-new", "text" to "stale", "ts" to now - 2 * hour), id = "r-old"))
            relay.handle(peerFrame("thread-reply", mapOf("channel" to "#general", "rootId" to "p-new", "text" to "live", "ts" to now - 30_000L), id = "r-new"))
            hub.saveHistory()

            val (other, _) = hubWith(clock = TestClock(now))
            other.setRetention(ChatRetention(defaultMs = hour))
            other.attachHistory(ChatHistoryStore(dir)) { "scope-r" }
            assertEq(listOf("new"), other.rows("#general").map { it.text })
            assertEq(listOf("live"), other.extras.replies("p-new").map { it.text })

            other.extras.apply(peerFrame("thread-reply", mapOf("channel" to "#general", "rootId" to "p-new", "text" to "late", "ts" to now - 5 * hour), id = "r-late"))
            assertEq(listOf("live"), other.extras.replies("p-new").map { it.text })

            expect(other.pruneExpired(now + 2 * hour))
            expect(other.extras.replies("p-new").isEmpty())
            expect(other.historySnapshot(now + 2 * hour).replies.isEmpty())

            hub.setRetention(ChatRetention(defaultMs = hour))
            assertEq(listOf("p-new"), hub.historySnapshot().messages.map { it.id })
        } finally {
            dir.listFiles()?.forEach { it.delete() }
            dir.delete()
        }
    }

    fun testHubKeysOfficeChannelsByRoomId() {
        val roomId = "b".repeat(32)
        val api = FakeAccountApi()
        api.replies["GET /api/chat/channels"] = mapOf("channels" to listOf(mapOf("id" to roomId, "name" to "design", "visibility" to "public")))
        var clock = 100 * day
        val service = OfficeChannelService(api, FakeOfficeConnector(), MemorySecretStore(), { "https://app.forkmesh.com" }, { "me" }, { "Me" }, now = { clock })
        service.refresh()
        val (hub, _) = hubWith()
        hub.office = service
        val rule = ChatRetention(defaultMs = 7 * day, chats = mapOf("channel:$roomId" to hour, "#general" to day))
        hub.setRetention(rule)
        assertEq("channel:$roomId", hub.retentionKey("#office/design"))
        assertEq("#general", hub.retentionKey("#general"))
        assertEq("@peer", hub.retentionKey("@peer"))
        assertEq(hour, hub.retentionMs("#office/design"))
        assertEq(hour, hub.retentionOverrideMs("#office/design"))
        assertEq(null, hub.retentionOverrideMs("#random"))
        assertEq(7 * day, hub.retentionMs("#random"))

        val room = "chat-channel:$roomId:v1"
        val crypto = RoomCrypto.withPassphrase(room, "pass")
        fun entry(id: String, ts: Long) = mapOf(
            "ts" to ts,
            "body" to Json.stringify(crypto.encrypt(mapOf("type" to "chat", "id" to id, "senderId" to "peer", "sender" to "Pat", "channel" to "#design", "text" to id, "ts" to ts)) + ("srv" to true)),
        )
        api.replies["GET /api/chat/channels/$roomId/history"] = mapOf(
            "room" to room, "passphrase" to "pass", "messages" to listOf(entry("stale", clock - 2 * hour), entry("fresh", clock - 60_000L)),
        )
        service.loadHistory("#office/design")
        assertEq(listOf("fresh"), hub.messages("#office/design").map { it.id })
        clock += hour
        expect(hub.pruneExpired(clock))
        expect(hub.messages("#office/design").isEmpty())
    }

    fun testMeshRoomSkipsExpiredHistoryAndPrunesItsSavedCopy() {
        val dir = Files.createTempDirectory("mesh-retention").toFile()
        val file = java.io.File(dir, "history.json")
        val now = System.currentTimeMillis()
        fun plain(id: String, ts: Long) = mapOf(
            "type" to "chat", "conversation" to MESH_CONVERSATION, "id" to id, "senderId" to "peer", "sender" to "Pat", "text" to id, "ts" to ts,
        )
        file.writeText(Json.stringify(mapOf("v" to 1L, "history" to listOf(plain("stale", now - 2 * hour), plain("fresh", now - 60_000L)), "queued" to emptyList<String>())))
        val settings = AccountSettings.create(MemoryPrefStore(), MemorySecretStore())
        val room = LocalMeshRoom(settings, Identity.fromSeed(Bytes.random(32)), MemorySecretStore(), platform = "linux", historyFile = { file })
        room.listenAddress = java.net.InetAddress.getByName("127.0.0.1")
        room.multicastEnabled = false
        room.bluetoothEnabled = false
        room.addressSweepEnabled = false
        try {
            room.setChatRetention(ChatRetention(defaultMs = hour))
            room.start()
            assertEq(listOf("fresh"), room.messages.map { it.id })
            expect(room.pruneExpired(now))
            expect(room.pruneExpired(now + hour))
            expect(room.messages.isEmpty())
            room.stop()
            val deadline = System.currentTimeMillis() + 5_000
            fun savedHistory() = Json.parseObject(file.readText())["history"] as List<*>
            while (savedHistory().isNotEmpty() && System.currentTimeMillis() < deadline) Thread.sleep(50)
            expect(savedHistory().isEmpty(), file.readText())
        } finally {
            room.dispose()
            dir.deleteRecursively()
        }
    }
}
