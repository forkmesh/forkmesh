package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.control.LocalControlException
import com.forkmesh.nativeapp.crypto.RoomCrypto
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class FakeOfficeSocket : OfficeSocket {
    val sent = ArrayList<String>()
    var closed = false
    var onText: (String) -> Unit = {}

    override fun sendText(text: String): Boolean {
        sent.add(text)
        return true
    }

    override fun close() {
        closed = true
    }
}

class FakeOfficeConnector : OfficeSocketConnector {
    val urls = ArrayList<String>()
    var last: FakeOfficeSocket? = null

    override fun connect(url: String, onText: (String) -> Unit, onClosed: () -> Unit): OfficeSocket {
        urls.add(url)
        return FakeOfficeSocket().also {
            it.onText = onText
            last = it
        }
    }
}

class OfficeChannelsTest {
    private val roomId = "a".repeat(32)

    fun testRoomsAreParsedAndBounded() {
        val payload = mapOf(
            "channels" to listOf(
                mapOf(
                    "id" to roomId, "name" to "design", "visibility" to "private", "canManage" to true,
                    "topic" to "  pixels   and   type ", "members" to listOf("Alice", "alice", " bob "), "keyVersion" to 2L,
                ),
                mapOf("id" to "not-an-id", "name" to "bad"),
                mapOf("id" to "b".repeat(32), "name" to "design"),
                mapOf("id" to "c".repeat(32), "name" to "lobby", "visibility" to "public"),
            ),
        )
        val rooms = OfficeFrames.rooms(payload)
        assertEq(listOf("#office/design", "#office/lobby"), rooms.map { it.conversation })
        assertEq(listOf("alice", "bob"), rooms[0].members)
        assertEq("pixels and type", rooms[0].topic)
        assertEq(2, rooms[0].keyVersion)
        expect(rooms[0].isPrivate)
        expect(rooms[1].members.isEmpty())
        expect(OfficeFrames.validName("team-2"))
        expect(!OfficeFrames.validName("-x"))
        expect(!OfficeFrames.validName("Upper"))
    }

    fun testChatMessagesAreBoundedLikeTheDesktop() {
        val plain = mapOf("type" to "chat", "id" to "x1", "senderId" to "me", "text" to "hello", "ts" to 1000L)
        val message = OfficeFrames.chatMessage(plain, "#office/a", "me")
        assertEq("Office visitor", message?.senderName)
        assertEq(true, message?.self)
        assertEq(null, OfficeFrames.chatMessage(plain + ("text" to "x".repeat(ChatLimits.MAX_TEXT_CHARS + 1)), "#office/a", ""))
        assertEq(null, OfficeFrames.chatMessage(mapOf("type" to "typing", "id" to "x"), "#office/a", ""))
    }

    fun testRecipientsComeOnlyFromTheOuterEnvelope() {
        assertEq(3, OfficeFrames.recipients(mapOf("kind" to "cipher", "recipients" to 3L)))
        assertEq(0, OfficeFrames.recipients(mapOf("recipients" to 0L)))
        assertEq(null, OfficeFrames.recipients(mapOf("recipients" to -1L)))
        assertEq(null, OfficeFrames.recipients(mapOf("recipients" to "3")))
        assertEq(null, OfficeFrames.recipients(mapOf("recipients" to 2.5)))
        assertEq(null, OfficeFrames.recipients(mapOf("kind" to "cipher")))
        assertEq("ba7816bf8f01cfea414140de5dae2223b00361a3", OfficeFrames.sentRef("abc"))
        val ref = OfficeFrames.sentRef("frame")
        assertEq(ref to 2, OfficeFrames.recipientsAck(mapOf("kind" to "forkmesh-recipients", "v" to 1L, "ref" to ref, "recipients" to 2L)))
        assertEq(null, OfficeFrames.recipientsAck(mapOf("kind" to "cipher", "ref" to ref, "recipients" to 2L)))
        assertEq(null, OfficeFrames.recipientsAck(mapOf("kind" to "forkmesh-recipients", "ref" to "short", "recipients" to 2L)))
        assertEq("to 3", OfficeFrames.recipientsLabel(3))
        assertEq("Delivered to 1 person", OfficeFrames.recipientsDescription(1))
        assertEq("Delivered to 0 people", OfficeFrames.recipientsDescription(0))
        assertEq(null, OfficeFrames.recipientsLabel(null))
    }

    fun testWebSocketUrlStaysOnTheOrigin() {
        val origin = "https://app.forkmesh.com/"
        assertEq(
            "wss://app.forkmesh.com/api/chat/channels/abc/ws?ticket=t%2B1",
            OfficeFrames.webSocketUrl("/api/chat/channels/abc/ws?ticket=t%2B1", origin),
        )
        assertEq(null, OfficeFrames.webSocketUrl("https://evil.example/ws", origin))
        assertEq(null, OfficeFrames.webSocketUrl("", origin))
        assertEq(null, OfficeFrames.webSocketUrl("ftp://app.forkmesh.com/x", origin))
    }

    fun testOfficeRoomSendsAndReceivesEncryptedChat() {
        val api = FakeAccountApi()
        api.replies["GET /api/chat/channels"] = mapOf(
            "channels" to listOf(mapOf("id" to roomId, "name" to "lobby", "visibility" to "public", "canManage" to true)),
        )
        val room = "chat-channel:$roomId:v1"
        api.replies["GET /api/chat/channels/$roomId/room-access"] = mapOf(
            "room" to room, "passphrase" to "office-pass", "webSocketUrl" to "/api/chat/channels/$roomId/ws?ticket=abc",
        )
        val crypto = RoomCrypto.withPassphrase(room, "office-pass")
        val earlier = crypto.encrypt(mapOf("type" to "chat", "id" to "old-1", "senderId" to "peer", "sender" to "Pat", "text" to "earlier", "ts" to 5L)) + ("recipients" to 2L)
        api.replies["GET /api/chat/channels/$roomId/history"] = mapOf(
            "room" to room, "passphrase" to "office-pass", "messages" to listOf(mapOf("ts" to 5L, "body" to Json.stringify(earlier))),
        )
        val connector = FakeOfficeConnector()
        val service = OfficeChannelService(api, connector, MemorySecretStore(), { "https://app.forkmesh.com" }, { "me-node" }, { "Me" })
        service.refresh()
        assertEq(listOf("#office/lobby"), service.conversations)
        service.loadHistory("#office/lobby")
        assertEq(listOf("earlier"), service.messages("#office/lobby").map { it.text })
        assertEq(2, service.messages("#office/lobby").first().recipients)
        expect(service.send("#office/lobby", "hi office"))
        assertEq("wss://app.forkmesh.com/api/chat/channels/$roomId/ws?ticket=abc", connector.urls.single())
        val socket = connector.last!!
        expect(socket.sent.size >= 2)
        val envelopes = socket.sent.map { Json.parseObject(it) }
        val chatEnvelope = envelopes.first { crypto.decrypt(it)?.get("type") == "chat" }
        val chat = crypto.decrypt(chatEnvelope)!!
        assertEq("hi office", chat["text"])
        assertEq("#lobby", chat["channel"])
        assertEq(true, chatEnvelope["persist"])
        val chatText = socket.sent.first { crypto.decrypt(Json.parseObject(it))?.get("type") == "chat" }
        assertEq(null, service.messages("#office/lobby").first { it.self }.recipients)
        socket.onText(Json.stringify(mapOf("kind" to "forkmesh-recipients", "v" to 1L, "ref" to OfficeFrames.sentRef(chatText), "recipients" to 4L)))
        assertEq(4, service.messages("#office/lobby").first { it.self }.recipients)
        socket.onText(
            Json.stringify(
                crypto.encrypt(mapOf("type" to "chat", "id" to "live-1", "senderId" to "peer", "sender" to "Pat", "text" to "welcome", "ts" to 9_999_999_999_999L)) +
                    ("recipients" to 3L),
            ),
        )
        assertEq(listOf("earlier", "hi office", "welcome"), service.messages("#office/lobby").map { it.text })
        assertEq(listOf(2, 4, 3), service.messages("#office/lobby").map { it.recipients })
        expect(service.isConnected("#office/lobby"))
        service.closeAll()
        expect(socket.closed)
        expect(!service.isConnected("#office/lobby"))
    }

    fun testAServerVisibleNoticesChannelShowsServerNotes() {
        val noticesId = "e".repeat(32)
        val api = FakeAccountApi()
        api.replies["GET /api/chat/channels"] = mapOf(
            "channels" to listOf(
                mapOf(
                    "id" to noticesId, "name" to "email-log", "visibility" to "private", "canManage" to false,
                    "kind" to "notices", "serverVisible" to true, "readOnly" to true,
                ),
                mapOf("id" to roomId, "name" to "secret", "visibility" to "private", "canManage" to true),
            ),
        )
        val noticesRoom = "chat-channel:$noticesId:v1"
        val secretRoom = "chat-channel:$roomId:v1"
        val note = mapOf(
            "type" to "chat", "id" to "note-1", "senderId" to "forkmesh-mail-log", "sender" to "ForkMesh mail",
            "ts" to 5L, "text" to "2026-09-28 17:35:58Z · Verification · accepted", "webhook" to true, "notice" to true,
        )
        val noticesCrypto = RoomCrypto.withPassphrase(noticesRoom, "notices-pass")
        api.replies["GET /api/chat/channels/$noticesId/history"] = mapOf(
            "room" to noticesRoom, "passphrase" to "notices-pass",
            "messages" to listOf(mapOf("ts" to 5L, "body" to Json.stringify(noticesCrypto.encrypt(note)))),
        )
        api.replies["GET /api/chat/channels/$noticesId/room-access"] = mapOf(
            "room" to noticesRoom, "passphrase" to "notices-pass", "webSocketUrl" to "/api/chat/channels/$noticesId/ws?ticket=n",
        )
        val secretCrypto = RoomCrypto.withPassphrase(secretRoom, "secret-pass")
        api.replies["GET /api/chat/channels/$roomId/history"] = mapOf(
            "room" to secretRoom, "passphrase" to "secret-pass",
            "messages" to listOf(mapOf("ts" to 5L, "body" to Json.stringify(secretCrypto.encrypt(note)))),
        )
        val connector = FakeOfficeConnector()
        val service = OfficeChannelService(api, connector, MemorySecretStore(), { "https://app.forkmesh.com" }, { "me-node" }, { "Me" })
        service.refresh()
        val notices = service.room("#office/email-log")!!
        expect(notices.isPrivate && notices.serverVisible && notices.readOnly)
        expect(!service.room("#office/secret")!!.serverVisible)
        service.loadHistory("#office/email-log")
        assertEq(listOf("2026-09-28 17:35:58Z · Verification · accepted"), service.messages("#office/email-log").map { it.text })
        service.loadHistory("#office/secret")
        expect(service.messages("#office/secret").isEmpty())
        expect(service.open("#office/email-log"))
        connector.last!!.onText(Json.stringify(noticesCrypto.encrypt(note + mapOf("id" to "note-2", "ts" to 6L, "text" to "delivered"))))
        assertEq(listOf("2026-09-28 17:35:58Z · Verification · accepted", "delivered"), service.messages("#office/email-log").map { it.text })
        val sentBefore = connector.last!!.sent.size
        expect(!service.send("#office/email-log", "hello"))
        assertEq(sentBefore, connector.last!!.sent.size)
        val (hub, relay) = hubWith()
        hub.office = service
        relay.switchConversation("#office/email-log")
        expect(hub.isReadOnly("#office/email-log"))
        assertEq("server-visible", hub.badge("#office/email-log"))
        assertEq(ChatHub.SERVER_VISIBLE_SEAL_SUMMARY, hub.sealSummary("#office/email-log"))
        assertEq("end-to-end", hub.badge("#office/secret"))
        expect(hub.sendText("#office/email-log", "hello") is ChatHubSend.Failed)
    }

    fun testAHubRoutesOfficeConversations() {
        val api = FakeAccountApi()
        api.replies["GET /api/chat/channels"] = mapOf(
            "channels" to listOf(mapOf("id" to roomId, "name" to "lobby", "visibility" to "public", "canManage" to true, "topic" to "old")),
        )
        val service = OfficeChannelService(api, FakeOfficeConnector(), MemorySecretStore(), { null }, { "me-node" }, { "Me" })
        service.refresh()
        val (hub, relay) = hubWith()
        hub.office = service
        relay.switchConversation("#office/lobby")
        expect(hub.inOffice)
        expect(hub.canSetTopic)
        assertEq(listOf("#office/lobby"), hub.officeChannels)
        expect("#office/lobby" !in hub.channels)
        expect(hub.sendText("#office/lobby", "/topic  new   topic") is ChatHubSend.Sent)
        val patch = api.calls.first { it.first == "PATCH" }
        assertEq("/api/chat/channels/$roomId", patch.second)
        assertEq("new topic", patch.third?.get("topic"))
        expect(hub.sendText("#office/lobby", "hello") is ChatHubSend.Failed)
    }

    fun testManagingChannelsMapsServerErrors() {
        val api = FakeAccountApi()
        api.failures["POST /api/chat/channels"] = OrgApiException("channel_name_taken", "taken")
        val service = OfficeChannelService(api, FakeOfficeConnector(), MemorySecretStore(), { null }, { "me" }, { "Me" })
        try {
            service.create("taken", isPrivate = true, members = listOf("Bob"))
            expect(false, "expected a refusal")
        } catch (error: OfficeException) {
            assertEq("channel_name_taken", error.code)
            assertEq("A channel with that name already exists.", error.message)
        }
        assertEq(listOf("bob"), api.calls.first().third?.get("members"))
        try {
            service.create("Bad Name", isPrivate = false, members = emptyList())
            expect(false, "expected a local refusal")
        } catch (error: OfficeException) {
            assertEq("invalid_channel_name", error.code)
        }
    }
}

class FakeDiscordControl : DiscordControl {
    val requests = ArrayList<Pair<String, Map<String, Any?>>>()
    val replies = HashMap<String, Map<String, Any?>>()
    val failures = HashMap<String, Exception>()
    private val taps = HashMap<Any, (String, Map<String, Any?>) -> Unit>()

    override fun call(suffix: String, params: Map<String, Any?>): Map<String, Any?> {
        requests.add(suffix to params)
        failures[suffix]?.let { throw it }
        return replies[suffix] ?: emptyMap()
    }

    override fun addEventTap(tap: (String, Map<String, Any?>) -> Unit): Any = Any().also { taps[it] = tap }

    override fun removeEventTap(token: Any) {
        taps.remove(token)
    }

    override fun isUnsupported(error: Throwable): Boolean =
        error is LocalControlException && (error.code == "unsupported" || error.code == "unknown_method")

    fun emit(suffix: String) = taps.values.toList().forEach { it(suffix, emptyMap()) }

    val tapCount: Int get() = taps.size
}

class DiscordBridgeTest {
    fun testChannelsAreParsedAndGrouped() {
        val reply = mapOf(
            "channels" to listOf(
                mapOf("id" to "1", "name" to "#general", "group" to "Discord · A", "guild" to true, "canSend" to true),
                mapOf("id" to "2", "name" to "#dev", "group" to "Discord · A", "guild" to true),
                mapOf("id" to "x", "name" to "bad"),
                mapOf("id" to "1", "name" to "dup"),
                mapOf("id" to "3", "name" to "Pat", "group" to "Discord · Direct messages", "direct" to true),
            ),
        )
        val channels = DiscordBridge.parseChannels(reply)
        assertEq(listOf("1", "2", "3"), channels.map { it.id })
        val guilds = DiscordBridge.guilds(channels)
        assertEq(listOf("Discord · A", "Discord · Direct messages"), guilds.map { it.name })
        assertEq(2, guilds[0].channels.size)
        expect(channels[0].canSend)
    }

    fun testMessagesAreBounded() {
        val rows = ArrayList<Map<String, Any?>>()
        for (index in 0 until 150) rows.add(mapOf("id" to "discord-message:$index", "sender" to "Pat", "text" to "m$index", "ts" to index.toLong()))
        rows.add(mapOf("id" to "", "text" to "no id"))
        rows[149] = mapOf("id" to "discord-message:149", "sender" to "Pat", "text" to "last", "ts" to 149L, "reactions" to mapOf("👍" to 2L, "zero" to 0L))
        val messages = DiscordBridge.parseMessages(mapOf("messages" to rows))
        assertEq(DiscordBridge.MAX_MESSAGES - 1, messages.size)
        assertEq("last", messages.last().text)
        assertEq(listOf(DiscordReaction("like", 2)), messages.last().reactions)
    }

    fun testSendNeedsAPreparedToken() {
        val desktop = FakeDiscordControl()
        desktop.replies["prepare"] = mapOf("token" to "tok", "summary" to "Send to #general: “hi”", "expiresInMs" to 60000L)
        val bridge = DiscordBridge(desktop)
        val confirmation = bridge.prepareSend("1", "  hi  ")
        assertEq("hi", confirmation.text)
        assertEq("Send to #general: “hi”", confirmation.summary)
        bridge.send(confirmation)
        assertEq(listOf("prepare", "send", "messages"), desktop.requests.map { it.first })
        assertEq("tok", desktop.requests[1].second["token"])
        assertEq("hi", desktop.requests[1].second["text"])
        try {
            bridge.prepareSend("1", "x".repeat(2_001))
            expect(false, "too long")
        } catch (error: DiscordBridgeException) {
            assertEq(DiscordBridgeException.Kind.INVALID, error.kind)
            assertEq(DiscordBridge.errorMessage("bad_text"), error.message)
        }
        val clock = TestClock()
        val stale = DiscordBridge(desktop, clock::now)
        val old = stale.prepareSend("1", "hi")
        clock.ms += 61_000
        try {
            stale.send(old)
            expect(false, "expired")
        } catch (error: DiscordBridgeException) {
            assertEq("confirmation_expired", error.code)
        }
    }

    fun testAnOlderDesktopIsReported() {
        val desktop = FakeDiscordControl()
        desktop.failures["status"] = LocalControlException("unsupported")
        val bridge = DiscordBridge(desktop)
        bridge.refresh()
        assertEq(DiscordBridgeException(DiscordBridgeException.Kind.DESKTOP_TOO_OLD).message, bridge.lastError)
        desktop.failures["status"] = LocalControlException("discord_unavailable")
        bridge.refresh()
        assertEq("The Discord package is not running on your desktop.", bridge.lastError)
    }

    fun testRefreshListsAndTheBellReloads() {
        val desktop = FakeDiscordControl()
        desktop.replies["status"] = mapOf("available" to true, "configured" to true, "connected" to true, "bot" to "helper")
        desktop.replies["list"] = mapOf("channels" to listOf(mapOf("id" to "9", "name" to "#x", "canSend" to true)))
        val bridge = DiscordBridge(desktop)
        bridge.refresh()
        assertEq("helper", bridge.status.bot)
        assertEq(listOf("9"), bridge.channels.map { it.id })
        var bells = 0
        bridge.startWatching { bells++ }
        bridge.startWatching { bells += 100 }
        desktop.emit("changed")
        desktop.emit("other")
        assertEq(1, bells)
        bridge.stopWatching()
        assertEq(0, desktop.tapCount)
    }
}
