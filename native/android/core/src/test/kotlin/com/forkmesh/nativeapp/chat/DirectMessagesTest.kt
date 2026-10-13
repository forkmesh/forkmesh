package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.crypto.RoomCrypto
import com.forkmesh.nativeapp.crypto.RoomSeal
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class DirectMessagesTest {
    private val conversationId = "d".repeat(32)
    private val room = "chat-direct:$conversationId:v1"

    private fun preparedApi(): FakeAccountApi {
        val api = FakeAccountApi()
        api.replies["GET /api/chat/direct-messages"] = mapOf(
            "conversations" to listOf(mapOf("id" to conversationId, "otherUser" to "pat", "unreadCount" to 2L)),
            "nextCursor" to "",
        )
        api.replies["GET /api/chat/direct-messages/$conversationId/room-access"] = mapOf(
            "room" to room, "passphrase" to "direct-pass",
            "webSocketUrl" to "/api/chat/direct-messages/$conversationId/ws?ticket=abc",
        )
        return api
    }

    private fun service(
        api: FakeAccountApi,
        connector: OfficeSocketConnector,
        store: MemorySecretStore = MemorySecretStore(),
        loop: EventLoop = EventLoop("direct-messages-test", virtualTime = true),
    ) = DirectMessageService(
        api, connector, store, { "https://app.forkmesh.com" }, { "me-node" }, { "Me" }, loop = loop,
    )

    private val crypto = RoomCrypto.withPassphrase(room, "direct-pass")

    private fun mySeal(store: MemorySecretStore) =
        RoomSeal.loadOrCreate(room, storageScope = "direct|app.forkmesh.com", secureStore = store)

    private fun peerSeal() = RoomSeal.loadOrCreate(room, storageScope = "peer", secureStore = MemorySecretStore())

    private fun envelope(frame: Map<String, Any?>, vararg extra: Pair<String, Any?>): String =
        Json.stringify(crypto.encrypt(frame) + extra)

    private fun peerChat(id: String, text: String): Map<String, Any?> = mapOf(
        "type" to "chat", "id" to id, "senderId" to "pat-node", "sender" to "pat", "accountKind" to "user",
        "channel" to "direct:$conversationId", "text" to text, "ts" to System.currentTimeMillis(),
    )

    private fun sentFrames(socket: FakeOfficeSocket, from: Int = 0): List<Map<String, Any?>> =
        socket.sent.drop(from).map { Json.parseObject(it) }

    private fun receipts(socket: FakeOfficeSocket): List<Any?> =
        sentFrames(socket).filter { it["kind"] == "forkmesh-received" }.flatMap { it["refs"] as List<*> }

    private val historyEnd = """{"kind":"forkmesh-history-end","v":1}"""

    fun testConversationsAreParsedAndBounded() {
        val payload = mapOf(
            "conversations" to listOf(
                mapOf("id" to conversationId, "otherUser" to " Pat ", "updatedAt" to 9L, "keyVersion" to 2L, "unreadCount" to 3L),
                mapOf("id" to "not-an-id", "otherUser" to "bad"),
                mapOf("id" to "e".repeat(32), "otherUser" to ""),
                mapOf("id" to conversationId, "otherUser" to "dupe"),
            ),
        )
        val conversations = DirectMessageFrames.conversations(payload)
        assertEq(1, conversations.size)
        assertEq("pat", conversations[0].otherUser)
        assertEq(3, conversations[0].unreadCount)
        assertEq(2, conversations[0].keyVersion)
        assertEq("direct:$conversationId", conversations[0].conversation)
        expect(DirectMessageFrames.isDirect(conversations[0].conversation))
        expect(!DirectMessageFrames.isDirect("direct:short"))
        expect(!DirectMessageFrames.isDirect("#general"))
        assertEq(listOf("ann"), DirectMessageFrames.users(mapOf("users" to listOf(mapOf("name" to "Ann"), mapOf("name" to "ann"), mapOf("name" to "")))))
    }

    fun testGroupChannelNamesFitTheChannelRule() {
        for (names in listOf(listOf("alice", "bob", "carol"), listOf("Ann O'Really", "x", "very-long-name-exceeding"), listOf(""), emptyList())) {
            val name = DirectMessageFrames.groupChannelName(names)
            expect(OfficeFrames.validName(name))
            expect(name.length <= 40)
        }
        expect(DirectMessageFrames.groupChannelName(emptyList()).startsWith("group-"))
        assertEq("alice-bob-carol-3f2a", DirectMessageFrames.groupChannelName(listOf("alice", "bob", "carol"), "3f2a"))
    }

    fun testDirectRoomSendsOnlySealedPersistedFrames() {
        val api = preparedApi()
        val connector = FakeOfficeConnector()
        val dm = service(api, connector)
        dm.refresh()
        assertEq(listOf("direct:$conversationId"), dm.conversations)
        assertEq(2, dm.unreadTotal)
        val conversation = dm.conversations.single()
        expect(dm.isConnected(conversation))
        expect(dm.open(conversation))
        assertEq(2, dm.unreadTotal)
        assertEq("wss://app.forkmesh.com/api/chat/direct-messages/$conversationId/ws?ticket=abc", connector.urls.single())
        val socket = connector.last!!
        val crypto = RoomCrypto.withPassphrase(room, "direct-pass")
        expect(dm.send(conversation, "hello pat"))
        val frames = socket.sent.map { Json.parseObject(it) }
        val opened = frames.mapNotNull { crypto.decrypt(it) }
        expect(opened.any { RoomSeal.isKeyFrame(it) })
        val sealedEnvelope = frames.first { crypto.decrypt(it)?.let(RoomSeal::isSealedFrame) == true }
        assertEq(true, sealedEnvelope["persist"])
        assertEq(true, sealedEnvelope["notify"])
        expect(opened.none { it["text"] == "hello pat" })
        assertEq(listOf("hello pat"), dm.messages(conversation).map { it.text })
        expect(dm.messages(conversation).single().self)
    }

    fun testPeersExchangeSealedChatAndCallFrames() {
        val api = preparedApi()
        val connector = FakeOfficeConnector()
        val dm = service(api, connector)
        val callFrames = ArrayList<Pair<Map<String, Any?>, String>>()
        dm.onCallFrame = { plain, conversation -> callFrames.add(plain to conversation) }
        val liveMessages = ArrayList<ChatMessage>()
        dm.onMessage = { liveMessages.add(it) }
        dm.refresh()
        val conversation = dm.conversations.single()
        expect(dm.open(conversation))
        val socket = connector.last!!
        val crypto = RoomCrypto.withPassphrase(room, "direct-pass")
        val peerSeal = RoomSeal.loadOrCreate(room, storageScope = "peer", secureStore = MemorySecretStore())
        socket.onText(Json.stringify(crypto.encrypt(peerSeal.keyFrame())))
        for (text in socket.sent) {
            val frame = crypto.decrypt(Json.parseObject(text)) ?: continue
            if (RoomSeal.isKeyFrame(frame)) peerSeal.acceptKeyFrame(frame)
        }
        val plainChat = mapOf(
            "type" to "chat", "id" to "pat-1", "senderId" to "pat-node", "sender" to "pat", "accountKind" to "user",
            "channel" to conversation, "text" to "hi from pat", "ts" to 1000L,
        )
        socket.onText(Json.stringify(crypto.encrypt(peerSeal.seal(plainChat))))
        assertEq(listOf("hi from pat"), dm.messages(conversation).map { it.text })
        assertEq(listOf("hi from pat"), liveMessages.map { it.text })
        socket.onText(Json.stringify(crypto.encrypt(plainChat + ("id" to "pat-2") + ("text" to "leaky"))))
        socket.onText(Json.stringify(crypto.encrypt(peerSeal.seal(plainChat + ("id" to "pat-3") + ("channel" to "direct:${"f".repeat(32)}")))))
        val invite = mapOf(
            "type" to "call-invite", "id" to "pat-4", "senderId" to "pat-node", "sender" to "pat",
            "channel" to conversation, "call" to "c-1", "media" to "audio", "ts" to 1001L,
        )
        socket.onText(Json.stringify(crypto.encrypt(peerSeal.seal(invite))))
        assertEq(listOf("hi from pat"), dm.messages(conversation).map { it.text })
        assertEq(1, callFrames.size)
        assertEq("c-1", callFrames.single().first["call"])
        assertEq(conversation, callFrames.single().second)
        expect(dm.sendCallFrame(conversation, "call-accept", linkedMapOf("call" to "c-1", "to" to "pat-node")))
        val callEnvelope = Json.parseObject(socket.sent.last())
        assertEq(null, callEnvelope["persist"])
        assertEq(null, callEnvelope["notify"])
        val accepted = socket.sent
            .mapNotNull { crypto.decrypt(Json.parseObject(it)) }
            .filter { RoomSeal.isSealedFrame(it) }
            .mapNotNull { peerSeal.unseal(it) }
            .first { it["type"] == "call-accept" }
        assertEq("pat-node", accepted["to"])
        assertEq(conversation, accepted["channel"])
        dm.closeAll()
        expect(socket.closed)
    }

    fun testReceivedFramesAreAckedAndServerCopiesShown() {
        val api = preparedApi()
        val connector = FakeOfficeConnector()
        val store = MemorySecretStore()
        val dm = service(api, connector, store)
        dm.refresh()
        val hub = ChatHub(FakeChatRelay())
        hub.direct = dm
        val conversation = dm.conversations.single()
        expect(dm.open(conversation))
        val socket = connector.last!!
        assertEq(null, dm.serverCopies(conversation))
        assertEq("", hub.serverCopiesLine(conversation))
        socket.onText("""{"kind":"forkmesh-server-copies","v":1,"stored":2}""")
        assertEq(2, dm.serverCopies(conversation))
        assertEq("2 messages on the server", hub.serverCopiesLine(conversation))
        val peer = peerSeal()
        peer.acceptKeyFrame(mySeal(store).keyFrame())
        val keyRef = "a".repeat(40)
        val replayRef = "b".repeat(40)
        val liveKeyRef = "c".repeat(40)
        val liveRef = "e".repeat(40)
        socket.onText(envelope(peer.keyFrame(), "ref" to keyRef, "historyReplay" to true))
        socket.onText(envelope(peer.seal(peerChat("pat-1", "one")), "ref" to replayRef, "historyReplay" to true))
        socket.onText(envelope(peer.seal(peerChat("pat-2", "two")), "ref" to "not-a-ref", "historyReplay" to true))
        socket.onText(Json.stringify(mapOf("kind" to "cipher", "v" to 1, "iv" to "x", "data" to "y", "ref" to "f".repeat(40))))
        assertEq(emptyList<Any?>(), receipts(socket))
        socket.onText(historyEnd)
        socket.onText(envelope(peerSeal().keyFrame(), "ref" to liveKeyRef))
        socket.onText(envelope(peer.seal(peerChat("pat-3", "three")), "ref" to liveRef))
        assertEq(listOf(replayRef, liveRef), receipts(socket))
        assertEq(setOf("one", "two", "three"), dm.messages(conversation).map { it.text }.toSet())
        socket.onText("""{"kind":"forkmesh-server-copies","v":1,"stored":0}""")
        assertEq("Not stored on the server", hub.serverCopiesLine(conversation))
        dm.closeAll()
    }

    fun testReceiptsSentBeforeTheSocketIsSetAreQueued() {
        val store = MemorySecretStore()
        val peer = peerSeal()
        peer.acceptKeyFrame(mySeal(store).keyFrame())
        val replayRef = "a".repeat(40)
        val connector = ScriptedDirectConnector { onText ->
            onText(envelope(peer.seal(peerChat("pat-1", "early")), "ref" to replayRef, "historyReplay" to true))
            onText(historyEnd)
        }
        val dm = service(preparedApi(), connector, store)
        dm.refresh()
        val conversation = dm.conversations.single()
        expect(dm.isConnected(conversation))
        assertEq(listOf("early"), dm.messages(conversation).map { it.text })
        assertEq(listOf(replayRef), receipts(connector.sockets.single()))
    }

    fun testAFrameNotWrappedForThisDeviceAsksOnceForAResend() {
        val connector = FakeOfficeConnector()
        val dm = service(preparedApi(), connector)
        dm.refresh()
        val conversation = dm.conversations.single()
        val socket = connector.last!!
        val stranger = peerSeal()
        val before = socket.sent.size
        socket.onText(envelope(stranger.seal(peerChat("pat-1", "sealed to pat")), "ref" to "a".repeat(40), "historyReplay" to true))
        socket.onText(historyEnd)
        socket.onText(envelope(stranger.seal(peerChat("pat-2", "again")), "ref" to "b".repeat(40)))
        val adverts = sentFrames(socket, before).filter { crypto.decrypt(it)?.let(RoomSeal::isKeyFrame) == true }
        assertEq(1, adverts.size)
        assertEq(true, adverts.single()["persist"])
        assertEq(null, adverts.single()["notify"])
        assertEq(emptyList<Any?>(), receipts(socket))
        expect(dm.messages(conversation).isEmpty())
    }

    fun testOwnMessagesAreResealedWhenThePeerKeyArrives() {
        val connector = FakeOfficeConnector()
        val dm = service(preparedApi(), connector)
        dm.refresh()
        val conversation = dm.conversations.single()
        val socket = connector.last!!
        socket.onText(historyEnd)
        expect(dm.send(conversation, "before your key"))
        val firstRef = OfficeFrames.sentRef(socket.sent.last())
        val peer = peerSeal()
        assertEq(null, peer.unseal(crypto.decrypt(Json.parseObject(socket.sent.last()))!!))
        val before = socket.sent.size
        socket.onText(envelope(peer.keyFrame()))
        val after = sentFrames(socket, before)
        val resent = after.single { it["resend"] == true }
        assertEq(true, resent["persist"])
        assertEq(true, resent["notify"])
        val opened = peer.unseal(crypto.decrypt(resent)!!)!!
        assertEq("before your key", opened["text"])
        assertEq(dm.messages(conversation).single().id, opened["id"])
        val superseded = after.single { it["kind"] == "forkmesh-superseded" }
        assertEq(listOf(firstRef), superseded["refs"])
        expect(after.indexOf(superseded) > after.indexOf(resent))
        val settled = socket.sent.size
        socket.onText(envelope(peer.keyFrame()))
        assertEq(settled, socket.sent.size)
        socket.onText("""{"kind":"forkmesh-server-copies","v":1,"stored":0}""")
        val cleared = socket.sent.size
        socket.onText(envelope(peerSeal().keyFrame()))
        expect(sentFrames(socket, cleared).none { it["resend"] == true || it["kind"] == "forkmesh-superseded" })
    }

    fun testOwnFramesLeftOnTheServerAreResealedAfterTheReplay() {
        val store = MemorySecretStore()
        val mine = mySeal(store)
        val connector = FakeOfficeConnector()
        val dm = service(preparedApi(), connector, store)
        dm.refresh()
        val conversation = dm.conversations.single()
        val socket = connector.last!!
        val peer = peerSeal()
        val ownRef = "a".repeat(40)
        val own = mapOf(
            "type" to "chat", "id" to "me-node-1", "senderId" to "me-node", "sender" to "Me", "accountKind" to "user",
            "channel" to conversation, "text" to "sent while you were away", "ts" to System.currentTimeMillis(),
        )
        val stale = own + ("id" to "me-node-0") + ("ts" to System.currentTimeMillis() - 8L * 24 * 60 * 60 * 1000)
        socket.onText(envelope(peer.keyFrame(), "ref" to "b".repeat(40), "historyReplay" to true))
        socket.onText(envelope(mine.seal(stale), "ref" to "c".repeat(40), "historyReplay" to true, "notify" to true))
        socket.onText(envelope(mine.seal(own), "ref" to ownRef, "historyReplay" to true, "notify" to true))
        val before = socket.sent.size
        socket.onText(historyEnd)
        val after = sentFrames(socket, before)
        val resent = after.single { it["resend"] == true }
        assertEq(true, resent["notify"])
        assertEq("sent while you were away", peer.unseal(crypto.decrypt(resent)!!)!!["text"])
        assertEq(listOf(ownRef), after.single { it["kind"] == "forkmesh-superseded" }["refs"])
        expect(dm.messages(conversation).all { it.self })
    }

    fun testAResentMessageIsShownOnce() {
        val store = MemorySecretStore()
        val connector = FakeOfficeConnector()
        val dm = service(preparedApi(), connector, store)
        val live = ArrayList<ChatMessage>()
        dm.onMessage = { live.add(it) }
        dm.refresh()
        val conversation = dm.conversations.single()
        val socket = connector.last!!
        socket.onText(historyEnd)
        val peer = peerSeal()
        peer.acceptKeyFrame(mySeal(store).keyFrame())
        val chat = peerChat("pat-7", "once")
        socket.onText(envelope(peer.seal(chat), "ref" to "a".repeat(40), "persist" to true, "notify" to true))
        socket.onText(envelope(peer.seal(chat), "ref" to "b".repeat(40), "persist" to true, "notify" to true, "resend" to true))
        assertEq(listOf("once"), dm.messages(conversation).map { it.text })
        assertEq(1, live.size)
        assertEq(listOf("a".repeat(40), "b".repeat(40)), receipts(socket))
    }

    fun testWantedSocketsReconnectWithBackoff() {
        val loop = EventLoop("direct-messages-reconnect", virtualTime = true)
        val connector = ScriptedDirectConnector {}
        val dm = service(preparedApi(), connector, loop = loop)
        dm.refresh()
        val conversation = dm.conversations.single()
        assertEq(1, connector.sockets.size)
        connector.closers[0]()
        expect(!dm.isConnected(conversation))
        loop.advance(999)
        assertEq(1, connector.sockets.size)
        loop.advance(1)
        assertEq(2, connector.sockets.size)
        expect(dm.isConnected(conversation))
        dm.markRead(conversation)
        connector.closers[1]()
        loop.advance(10 * 60_000L)
        assertEq(2, connector.sockets.size)
        dm.focus(conversation)
        dm.keepOpen()
        assertEq(3, connector.sockets.size)
        dm.closeAll()
        loop.shutdown()
    }

    fun testSelectingAnUnknownDirectConversationRelistsFirst() {
        val api = preparedApi()
        val dm = service(api, FakeOfficeConnector())
        val hub = ChatHub(FakeChatRelay())
        hub.direct = dm
        val conversation = "direct:$conversationId"
        hub.select(conversation)
        expect(api.calls.any { it.first == "GET" && it.second.startsWith("/api/chat/direct-messages?") })
        expect(dm.isConnected(conversation))
        assertEq(0, dm.unreadTotal)
    }

    fun testStartAndReadMarksUseTheServerContract() {
        val api = FakeAccountApi()
        api.replies["POST /api/chat/direct-messages"] = mapOf(
            "ok" to true, "conversation" to mapOf("id" to conversationId, "otherUser" to "pat"),
        )
        val dm = service(api, FakeOfficeConnector())
        val started = dm.start(" Pat ")
        assertEq("pat", started.otherUser)
        assertEq(listOf("pat"), dm.records.map { it.otherUser })
        assertEq("pat", api.calls.first().third?.get("username"))
        dm.bumpUnread(started.conversation)
        assertEq(1, dm.unreadTotal)
        dm.markRead(started.conversation)
        assertEq(0, dm.unreadTotal)
        expect(api.calls.any { it.first == "POST" && it.second == "/api/chat/direct-messages/$conversationId/read" })
        api.failures["POST /api/chat/direct-messages"] = OrgApiException("rate_limited", "slow down")
        val refused = assertFails<DirectMessageException> { dm.start("sam") }
        assertEq("rate_limited", refused.code)
        assertEq("Too many new conversations. Try again later.", refused.message)
    }

    fun testHubRoutesDirectConversations() {
        val api = preparedApi()
        val connector = FakeOfficeConnector()
        val dm = service(api, connector)
        dm.refresh()
        val hub = ChatHub(FakeChatRelay())
        hub.direct = dm
        val conversation = dm.conversations.single()
        hub.select(conversation)
        expect(hub.isPrivateRoom(conversation))
        assertEq("end-to-end", hub.badge(conversation))
        expect(dm.isConnected(conversation))
        assertEq(ChatHubSend.Sent, hub.sendText(conversation, "hello there"))
        assertEq(listOf("hello there"), hub.messages(conversation).map { it.text })
        expect(hub.rows(conversation).none { it.deletable })
        expect(api.calls.any { it.first == "POST" && it.second == "/api/chat/direct-messages/$conversationId/read" })
    }

    fun testHubCountsDirectUnreadAndLiveMessages() {
        val api = preparedApi()
        val connector = FakeOfficeConnector()
        val dm = service(api, connector)
        dm.refresh()
        val hub = ChatHub(FakeChatRelay())
        hub.direct = dm
        assertEq(2, dm.unreadTotal)
        val conversation = dm.conversations.single()
        expect(dm.open(conversation))
        assertEq(2, dm.unreadTotal)
        val socket = connector.last!!
        val crypto = RoomCrypto.withPassphrase(room, "direct-pass")
        val peerSeal = RoomSeal.loadOrCreate(room, storageScope = "peer", secureStore = MemorySecretStore())
        socket.onText(Json.stringify(crypto.encrypt(peerSeal.keyFrame())))
        for (text in socket.sent) {
            val frame = crypto.decrypt(Json.parseObject(text)) ?: continue
            if (RoomSeal.isKeyFrame(frame)) peerSeal.acceptKeyFrame(frame)
        }
        val plainChat = mapOf(
            "type" to "chat", "id" to "pat-9", "senderId" to "pat-node", "sender" to "pat", "accountKind" to "user",
            "channel" to conversation, "text" to "unread line", "ts" to 1000L,
        )
        socket.onText(Json.stringify(crypto.encrypt(peerSeal.seal(plainChat))))
        assertEq(3, dm.unreadTotal)
        hub.select(conversation)
        socket.onText(Json.stringify(crypto.encrypt(peerSeal.seal(plainChat + ("id" to "pat-10") + ("text" to "read line")))))
        assertEq(0, dm.unreadTotal)
    }

    fun testHubSendsAndShowsImagesInDirectMessages() {
        val api = preparedApi()
        val connector = FakeOfficeConnector()
        val dm = service(api, connector)
        dm.refresh()
        val hub = ChatHub(FakeChatRelay())
        hub.direct = dm
        val conversation = dm.conversations.single()
        hub.select(conversation)
        val socket = connector.last!!
        val peerSeal = peerSeal()
        socket.onText(Json.stringify(crypto.encrypt(peerSeal.keyFrame())))
        for (text in socket.sent) {
            val frame = crypto.decrypt(Json.parseObject(text)) ?: continue
            if (RoomSeal.isKeyFrame(frame)) peerSeal.acceptKeyFrame(frame)
        }
        val image = byteArrayOf(-1, -40, -1, -32, 1, 2, 3, 4)
        expect(hub.sendAttachment(conversation, "photo.jpg", "image/jpeg", image))
        val mine = hub.rows(conversation).last()
        expect(mine.message.self)
        assertEq("photo.jpg", mine.message.fileName)
        expect(mine.attachment?.data?.contentEquals(image) == true)
        val delivered = socket.sent
            .mapNotNull { crypto.decrypt(Json.parseObject(it)) }
            .filter { RoomSeal.isSealedFrame(it) }
            .mapNotNull { peerSeal.unseal(it) }
            .first { it["id"] == mine.message.id }
        assertEq(null, delivered["text"])
        assertEq("image/jpeg", delivered["fileMime"])
        assertEq(java.util.Base64.getEncoder().encodeToString(image), delivered["file"])
        expect(!hub.sendAttachment(conversation, "big.jpg", "image/jpeg", ByteArray(DirectMessageFrames.MAX_FILE_BYTES + 1)))

        val pasted = byteArrayOf(-119, 80, 78, 71, 9, 9)
        val fromPat = mapOf(
            "type" to "chat", "id" to "pat-image", "senderId" to "pat-node", "sender" to "pat", "accountKind" to "user",
            "channel" to conversation, "fileName" to "pasted-image.png", "fileMime" to "image/png",
            "file" to java.util.Base64.getEncoder().encodeToString(pasted), "ts" to 2000L,
        )
        socket.onText(Json.stringify(crypto.encrypt(peerSeal.seal(fromPat))))
        val theirs = hub.rows(conversation).first { it.message.id == "pat-image" }
        assertEq("pasted-image.png", theirs.message.fileName)
        expect(theirs.attachment?.data?.contentEquals(pasted) == true)
    }
}

private class ScriptedDirectConnector(private val early: ((String) -> Unit) -> Unit) : OfficeSocketConnector {
    val sockets = ArrayList<FakeOfficeSocket>()
    val closers = ArrayList<() -> Unit>()

    override fun connect(url: String, onText: (String) -> Unit, onClosed: () -> Unit): OfficeSocket {
        val socket = FakeOfficeSocket()
        socket.onText = onText
        sockets.add(socket)
        closers.add(onClosed)
        early(onText)
        return socket
    }
}
