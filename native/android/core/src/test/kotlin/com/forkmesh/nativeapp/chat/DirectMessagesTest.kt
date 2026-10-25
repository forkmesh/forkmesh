package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.crypto.RoomCrypto
import com.forkmesh.nativeapp.crypto.RoomSeal
import com.forkmesh.nativeapp.crypto.SealOpen
import com.forkmesh.nativeapp.crypto.SealPolicy
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.model.ChatReceipt
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

    fun testARefreshThatChangesNothingDoesNotRedraw() {
        val api = FakeAccountApi()
        api.replies["GET /api/chat/direct-messages"] = mapOf(
            "conversations" to listOf(mapOf("id" to conversationId, "otherUser" to "pat", "unreadCount" to 0L)),
            "nextCursor" to "",
        )
        val dm = service(api, FakeOfficeConnector())
        var redraws = 0
        var loadingSeen = false
        dm.addListener {
            redraws++
            if (dm.loading) loadingSeen = true
        }
        dm.refresh()
        expect(loadingSeen)
        expect(!dm.loading)
        val first = redraws
        expect(first > 0)
        loadingSeen = false
        dm.refresh()
        assertEq(first, redraws)
        expect(!loadingSeen)
        api.replies["GET /api/chat/direct-messages"] = mapOf(
            "conversations" to listOf(mapOf("id" to conversationId, "otherUser" to "pat", "unreadCount" to 0L, "updatedAt" to 5L)),
            "nextCursor" to "",
        )
        dm.refresh()
        expect(redraws > first)
        expect(!loadingSeen)
    }

    private val crypto = RoomCrypto.withPassphrase(room, "direct-pass")

    private val scope = "direct:$conversationId"

    private fun mySeal(store: MemorySecretStore) =
        RoomSeal.loadOrCreate(room, scope = scope, policy = SealPolicy.STRICT, acct = "me-node", secureStore = store)

    private fun peerSeal() =
        RoomSeal.loadOrCreate(room, scope = scope, policy = SealPolicy.STRICT, acct = "pat", secureStore = MemorySecretStore(), node = "pat-node")

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

    private class Rig(
        val dm: DirectMessageService,
        val hub: ChatHub,
        val socket: FakeOfficeSocket,
        val peer: RoomSeal,
        val conversation: String,
        val store: MemorySecretStore,
    )

    private fun rig(): Rig {
        val store = MemorySecretStore()
        val connector = FakeOfficeConnector()
        val dm = service(preparedApi(), connector, store)
        dm.refresh()
        val hub = ChatHub(FakeChatRelay())
        hub.direct = dm
        val conversation = dm.conversations.single()
        hub.select(conversation)
        val socket = connector.last!!
        socket.onText(historyEnd)
        val peer = peerSeal()
        peer.acceptKeyFrame(mySeal(store).keyFrame())
        socket.onText(envelope(peer.keyFrame()))
        return Rig(dm, hub, socket, peer, conversation, store)
    }

    private fun Rig.fromPeer(frame: Map<String, Any?>, ref: String = "") {
        socket.onText(if (ref.isEmpty()) envelope(peer.seal(frame)) else envelope(peer.seal(frame), "ref" to ref))
    }

    private fun Rig.peerFrame(type: String, id: String, vararg extra: Pair<String, Any?>, sender: String = "pat-node"): Map<String, Any?> =
        mapOf(
            "type" to type, "id" to id, "senderId" to sender, "sender" to "pat", "accountKind" to "user",
            "channel" to conversation, "conversation" to conversation, "ts" to System.currentTimeMillis(),
        ) + extra

    private fun Rig.sealedSent(from: Int = 0): List<Pair<Map<String, Any?>, Map<String, Any?>>> =
        sentFrames(socket, from).mapNotNull { envelope ->
            val framed = crypto.decrypt(envelope) ?: return@mapNotNull null
            if (!RoomSeal.isRatchetFrame(framed)) return@mapNotNull null
            peer.unseal(framed)?.let { envelope to it }
        }

    fun testOwnDirectMessagesWalkThroughTheReceiptStates() {
        val r = rig()
        expect(r.dm.send(r.conversation, "tick me"))
        val ref = OfficeFrames.sentRef(r.socket.sent.last())
        val first = r.dm.messages(r.conversation).single().id
        assertEq(ChatReceipt.SENDING, r.hub.rows(r.conversation).single().receipt)
        expect(r.dm.receiptTime(first).isEmpty)
        r.socket.onText(Json.stringify(mapOf("kind" to "forkmesh-recipients", "v" to 1, "ref" to ref, "recipients" to 2)))
        assertEq(ChatReceipt.SENT, r.dm.receipt(first))
        expect(r.dm.receiptTime(first).isEmpty)
        r.socket.onText(Json.stringify(mapOf("kind" to "forkmesh-delivered", "v" to 1, "refs" to listOf("f".repeat(40)))))
        assertEq(ChatReceipt.SENT, r.dm.receipt(first))
        r.socket.onText(Json.stringify(mapOf("kind" to "forkmesh-delivered", "v" to 1, "refs" to listOf(ref, "nope"))))
        assertEq(ChatReceipt.DELIVERED, r.hub.rows(r.conversation).single().receipt)
        r.socket.onText(Json.stringify(mapOf("kind" to "forkmesh-recipients", "v" to 1, "ref" to ref, "recipients" to 2)))
        assertEq(ChatReceipt.DELIVERED, r.dm.receipt(first))
        val deliveredAt = r.dm.receiptTime(first).delivered!!
        assertEq(null, r.dm.receiptTime(first).read)
        expect(r.dm.send(r.conversation, "second"))
        val second = r.dm.messages(r.conversation).last().id
        r.socket.onText("""{"kind":"forkmesh-read","v":1}""")
        assertEq(ChatReceipt.READ, r.dm.receipt(first))
        assertEq(deliveredAt, r.dm.receiptTime(first).delivered, "reading keeps the delivered time")
        expect(r.dm.receiptTime(first).read!! >= deliveredAt)
        assertEq(r.dm.receiptTime(first), r.hub.receiptTimes(r.dm.messages(r.conversation).first()))
        assertEq(ChatReceipt.SENDING, r.dm.receipt(second))
        expect(r.dm.receiptTime(second).isEmpty)
        r.fromPeer(peerChat("pat-1", "hello"), "a".repeat(40))
        assertEq(null, r.hub.rows(r.conversation).first { it.message.id == "pat-1" }.receipt)
    }

    fun testMessagingAUsernameFromOutsideChatOpensTheirAccountDirectMessage() {
        val api = FakeAccountApi()
        api.replies["POST /api/chat/direct-messages"] = mapOf("ok" to true, "conversation" to mapOf("id" to conversationId, "otherUser" to "pat"))
        val dm = service(api, FakeOfficeConnector())
        val relay = FakeChatRelay()
        val hub = ChatHub(relay)
        hub.direct = dm
        hub.switchConversation("@Pat")
        assertEq("direct:$conversationId", hub.currentConversation)
        assertEq(1, hub.revealRequests)
        assertEq(listOf<Any?>("pat"), api.calls.filter { it.first == "POST" && it.second == "/api/chat/direct-messages" }.map { it.third?.get("username") })
        hub.select("#general")
        hub.switchConversation("@pat")
        assertEq("direct:$conversationId", hub.currentConversation)
        assertEq(1, api.calls.count { it.first == "POST" && it.second == "/api/chat/direct-messages" })
        relay.peers.add(com.forkmesh.nativeapp.model.Member(id = "peer-9", name = "Pat", online = true, owner = "pat"))
        hub.select("#general")
        hub.switchConversation("@peer-9")
        assertEq("direct:$conversationId", hub.currentConversation, "Message on a person opens their account's direct conversation")
        dm.closeAll()
    }

    fun testAServerMarkedUnsealedFrameNeverShowsInADirectMessage() {
        val rig = rig()
        rig.socket.onText(envelope(peerChat("srv-1", "from the server"), "srv" to true))
        rig.fromPeer(peerChat("p-1", "sealed hello"))
        assertEq(listOf("sealed hello"), rig.dm.messages(rig.conversation).map { it.text })
    }

    fun testOwnFramesComingBackFromTheRelayAreIgnored() {
        val store = MemorySecretStore()
        val connector = FakeOfficeConnector()
        val dm = service(preparedApi(), connector, store)
        dm.refresh()
        val conversation = dm.conversations.single()
        val socket = connector.last!!
        val own = mapOf(
            "type" to "chat", "id" to "me-node-old", "senderId" to "me-node", "sender" to "Me", "accountKind" to "user",
            "channel" to conversation, "text" to "from this phone", "ts" to System.currentTimeMillis(),
        )
        val frame = mySeal(store).seal(own)
        expect(mySeal(store).open(frame) is SealOpen.Own)
        socket.onText(envelope(frame, "ref" to "a".repeat(40), "historyReplay" to true))
        socket.onText(historyEnd)
        socket.onText(envelope(frame, "ref" to "a".repeat(40)))
        expect(dm.messages(conversation).isEmpty(), "an own frame is already in this phone's history")
        assertEq(emptyList<Any?>(), receipts(socket), "and is never acknowledged")
    }

    fun testRepliesAndReactionsWorkOnEitherPersonsMessages() {
        val r = rig()
        r.fromPeer(peerChat("pat-1", "root"), "a".repeat(40))
        r.fromPeer(r.peerFrame("thread-reply", "pat-2", "rootId" to "pat-1", "text" to "in thread"), "b".repeat(40))
        assertEq(listOf("in thread"), r.hub.threadRows("pat-1").map { it.text })
        assertEq(1, r.hub.rows(r.conversation).single().thread?.replyCount)
        assertEq("pat-1", r.hub.rootMessage("pat-1", r.conversation)?.id)
        expect(r.hub.canReplyInThread(r.conversation))
        assertEq("Thread with pat", r.hub.threadTitle(r.conversation))
        r.fromPeer(
            r.peerFrame("reaction", "pat-3", "target" to "pat-1", "emoji" to "\uD83D\uDC4D", "reactorId" to "pat-node", "reactorName" to "pat", "added" to true),
        )
        val chip = r.hub.rows(r.conversation).single().reactions.single()
        assertEq("like", chip.value)
        expect(!chip.mine)
        val root = r.hub.rows(r.conversation).single().message
        val before = r.socket.sent.size
        r.hub.toggleReaction(root, "like")
        val mine = r.hub.rows(r.conversation).single().reactions.single()
        assertEq(2, mine.count)
        expect(mine.mine)
        val (reactionEnvelope, reaction) = r.sealedSent(before).single()
        assertEq("reaction", reaction["type"])
        assertEq("pat-1", reaction["target"])
        assertEq(r.conversation, reaction["conversation"])
        assertEq(true, reaction["added"])
        assertEq("me-node", reaction["reactorId"])
        assertEq(true, reactionEnvelope["persist"])
        assertEq(null, reactionEnvelope["notify"])
        r.hub.toggleReaction(root, "like")
        assertEq(1, r.hub.rows(r.conversation).single().reactions.single().count)
        val replyFrom = r.socket.sent.size
        expect(r.hub.replyInThread(r.conversation, "pat-1", "my reply"))
        assertEq(listOf("in thread", "my reply"), r.hub.threadRows("pat-1").map { it.text })
        assertEq(ChatReceipt.SENDING, r.hub.threadRows("pat-1").last().receipt)
        val (replyEnvelope, reply) = r.sealedSent(replyFrom).single()
        assertEq("thread-reply", reply["type"])
        assertEq("pat-1", reply["rootId"])
        assertEq(r.conversation, reply["channel"])
        assertEq(true, replyEnvelope["notify"])
        expect(r.dm.send(r.conversation, "my own"))
        val own = r.dm.messages(r.conversation).last()
        r.hub.toggleReaction(own, "love")
        assertEq("love", r.hub.rows(r.conversation).last().reactions.single().value)
    }

    fun testStickersTravelBothWaysInADirectMessage() {
        val r = rig()
        r.fromPeer(peerChat("pat-1", "root"), "a".repeat(40))
        r.fromPeer(
            r.peerFrame(
                "reaction", "pat-2", "target" to "pat-1", "emoji" to "🎉", "sticker" to "party",
                "reactorId" to "pat-node", "reactorName" to "pat", "added" to true,
            ),
        )
        val row = r.hub.rows(r.conversation).single()
        assertEq(listOf("party"), row.stickers.map { it.slug })
        expect(row.reactions.isEmpty(), "a known sticker is not also a reaction")
        expect(r.hub.canSticker(r.conversation))
        val before = r.socket.sent.size
        r.hub.toggleSticker(row.message, "ship-it")
        assertEq(listOf("ship-it", "party"), r.hub.rows(r.conversation).single().stickers.map { it.slug })
        val (envelope, sent) = r.sealedSent(before).single()
        assertEq("reaction", sent["type"])
        assertEq("pat-1", sent["target"])
        assertEq("🚀", sent["emoji"])
        assertEq("ship-it", sent["sticker"])
        assertEq(true, sent["added"])
        assertEq("me-node", sent["reactorId"])
        assertEq(true, envelope["persist"])
        assertEq(null, envelope["notify"])
        val off = r.socket.sent.size
        r.hub.toggleSticker(row.message, "ship-it")
        assertEq(false, r.sealedSent(off).single().second["added"])
        assertEq(listOf("party"), r.hub.rows(r.conversation).single().stickers.map { it.slug })
        r.fromPeer(
            r.peerFrame(
                "reaction", "pat-3", "target" to "pat-1", "emoji" to "🎉", "sticker" to "party",
                "reactorId" to "pat-node", "added" to false,
            ),
        )
        expect(r.hub.rows(r.conversation).single().stickers.isEmpty())
    }

    fun testDirectThreadRepliesStayInTheThreadAndCarryTheirOwnMarks() {
        val r = rig()
        r.fromPeer(peerChat("pat-1", "root"), "a".repeat(40))
        r.fromPeer(r.peerFrame("thread-reply", "pat-2", "rootId" to "pat-1", "text" to "peer reply"), "b".repeat(40))
        r.fromPeer(r.peerFrame("thread-reply", "pat-2", "rootId" to "pat-1", "text" to "peer reply"), "b".repeat(40))
        assertEq(listOf("root"), r.dm.messages(r.conversation).map { it.text })
        assertEq(listOf("peer reply"), r.hub.threadRows("pat-1").map { it.text })
        assertEq(null, r.hub.threadRows("pat-1").single().receipt)
        expect(r.hub.replyInThread(r.conversation, "pat-1", "mine"))
        val ref = OfficeFrames.sentRef(r.socket.sent.last())
        val mine = r.hub.threadRows("pat-1").last()
        assertEq(ChatReceipt.SENDING, mine.receipt)
        expect(mine.deletable)
        expect(mine.editable)
        r.socket.onText(Json.stringify(mapOf("kind" to "forkmesh-recipients", "v" to 1, "ref" to ref, "recipients" to 2)))
        assertEq(ChatReceipt.SENT, r.hub.threadRows("pat-1").last().receipt)
        r.socket.onText(Json.stringify(mapOf("kind" to "forkmesh-delivered", "v" to 1, "refs" to listOf(ref))))
        assertEq(ChatReceipt.DELIVERED, r.hub.threadRows("pat-1").last().receipt)
        assertEq(2, r.hub.rows(r.conversation).single().thread?.replyCount)
        expect(r.hub.edit(mine.message, "mine, edited"))
        assertEq("mine, edited", r.hub.threadRows("pat-1").last().text)
        expect(r.hub.threadRows("pat-1").last().edited)
        r.fromPeer(r.peerFrame("edit", "x-1", "target" to mine.message.id, "text" to "forged"))
        assertEq("mine, edited", r.hub.threadRows("pat-1").last().text)
        val before = r.socket.sent.size
        expect(r.hub.deleteMessage(mine.message))
        val (_, delete) = r.sealedSent(before).single()
        assertEq("delete", delete["type"])
        assertEq(mine.message.id, delete["target"])
        assertEq(listOf("peer reply"), r.hub.threadRows("pat-1").map { it.text })
        assertEq(1, r.hub.rows(r.conversation).single().thread?.replyCount)
        r.fromPeer(r.peerFrame("delete", "pat-3", "target" to "pat-2"))
        expect(r.hub.threadRows("pat-1").isEmpty())
        assertEq(null, r.hub.rows(r.conversation).single().thread)
        assertEq("root", r.hub.listPreview(r.conversation)?.line)
    }

    fun testDirectListPreviewShowsTheNewestTopLevelLine() {
        val r = rig()
        assertEq(null, r.hub.listPreview(r.conversation))
        r.fromPeer(peerChat("pat-1", "hello\n**there**"), "a".repeat(40))
        assertEq("hello there", r.hub.listPreview(r.conversation)?.line)
        r.fromPeer(r.peerFrame("thread-reply", "pat-2", "rootId" to "pat-1", "text" to "aside"))
        r.fromPeer(r.peerFrame("reaction", "pat-3", "target" to "pat-1", "emoji" to "👍", "reactorId" to "pat-node", "added" to true))
        assertEq("hello there", r.hub.listPreview(r.conversation)?.line)
        r.fromPeer(r.peerFrame("edit", "pat-4", "target" to "pat-1", "text" to "hello again"))
        assertEq("hello again", r.hub.listPreview(r.conversation)?.line)
        expect(r.dm.send(r.conversation, "my turn"))
        assertEq("You: my turn", r.hub.listPreview(r.conversation)?.line)
        val (record, preview) = r.hub.directListRows().single()
        assertEq("pat", record.otherUser)
        assertEq("You: my turn", preview?.line)
        expect(r.hub.deleteMessage(r.dm.messages(r.conversation).last()))
        assertEq("hello again", r.hub.listPreview(r.conversation)?.line)
        expect(r.hub.sendAttachment(r.conversation, "plan.pdf", "application/pdf", byteArrayOf(1, 2, 3)))
        assertEq("You: 📎 plan.pdf", r.hub.listPreview(r.conversation)?.line)
    }

    fun testDirectListRowsAreNewestFirst() {
        val api = preparedApi()
        val other = "e".repeat(32)
        api.replies["GET /api/chat/direct-messages"] = mapOf(
            "conversations" to listOf(
                mapOf("id" to conversationId, "otherUser" to "pat", "updatedAt" to 5L),
                mapOf("id" to other, "otherUser" to "sam", "updatedAt" to 9L),
            ),
            "nextCursor" to "",
        )
        val dm = service(api, FakeOfficeConnector())
        dm.refresh()
        val hub = ChatHub(FakeChatRelay())
        hub.direct = dm
        assertEq(listOf("sam", "pat"), hub.directListRows().map { it.first.otherUser })
    }

    fun testDeleteForEveryoneRemovesTheMessageAndWithdrawsTheServerCopy() {
        val r = rig()
        expect(r.dm.send(r.conversation, "oops"))
        val ref = OfficeFrames.sentRef(r.socket.sent.last())
        val original = r.socket.sent.last()
        val mine = r.dm.messages(r.conversation).single()
        r.fromPeer(peerChat("pat-1", "theirs"), "a".repeat(40))
        val theirs = r.dm.messages(r.conversation).first { it.id == "pat-1" }
        expect(!r.hub.deleteMessage(theirs))
        val before = r.socket.sent.size
        expect(r.hub.deleteMessage(mine))
        assertEq(listOf("theirs"), r.dm.messages(r.conversation).map { it.text })
        val (deleteEnvelope, delete) = r.sealedSent(before).single()
        assertEq("delete", delete["type"])
        assertEq(mine.id, delete["target"])
        assertEq(r.conversation, delete["conversation"])
        assertEq(true, deleteEnvelope["persist"])
        assertEq(null, deleteEnvelope["notify"])
        val superseded = sentFrames(r.socket, before).single { it["kind"] == "forkmesh-superseded" }
        assertEq(listOf(ref), superseded["refs"])
        expect(r.hub.extras.isHidden(mine.id))
        r.socket.onText(original)
        expect(r.hub.rows(r.conversation).none { it.message.id == mine.id })
        val resealFrom = r.socket.sent.size
        r.socket.onText(envelope(peerSeal().keyFrame()))
        expect(r.sealedSent(resealFrom).none { it.second["id"] == mine.id })
    }

    fun testPeerEditsAndDeletesNeedTheAuthorAndWaitForTheirTarget() {
        val r = rig()
        r.fromPeer(peerChat("pat-1", "keep"), "a".repeat(40))
        r.fromPeer(r.peerFrame("delete", "x-1", "target" to "pat-1", sender = "mallory-node"))
        r.fromPeer(r.peerFrame("edit", "x-2", "target" to "pat-1", "text" to "forged", sender = "mallory-node"))
        val kept = r.hub.rows(r.conversation).single()
        assertEq("keep", kept.text)
        expect(!kept.edited)
        r.fromPeer(r.peerFrame("edit", "pat-2", "target" to "pat-1", "text" to "edited", "editedAt" to 5L))
        assertEq("edited", r.hub.rows(r.conversation).single().text)
        expect(r.hub.rows(r.conversation).single().edited)
        r.fromPeer(r.peerFrame("delete", "pat-3", "target" to "pat-1"))
        expect(r.dm.messages(r.conversation).isEmpty())
        expect(r.hub.extras.isHidden("pat-1"))
        r.fromPeer(r.peerFrame("delete", "pat-4", "target" to "pat-9"))
        r.fromPeer(r.peerFrame("edit", "pat-5", "target" to "pat-8", "text" to "late edit"))
        r.fromPeer(peerChat("pat-9", "gone before it came"))
        r.fromPeer(peerChat("pat-8", "first draft"))
        assertEq(listOf("late edit"), r.hub.rows(r.conversation).map { it.text })
        r.fromPeer(r.peerFrame("thread-reply", "pat-10", "rootId" to "pat-8", "text" to "reply"))
        r.fromPeer(r.peerFrame("delete", "pat-11", "target" to "pat-10"))
        expect(r.hub.threadRows("pat-8").isEmpty())
    }

    fun testOwnEditsGoToThePeerAndOnlyForOwnMessages() {
        val r = rig()
        expect(r.dm.send(r.conversation, "frist"))
        r.fromPeer(peerChat("pat-1", "theirs"), "a".repeat(40))
        val mine = r.dm.messages(r.conversation).first { it.self }
        val theirs = r.dm.messages(r.conversation).first { !it.self }
        val before = r.socket.sent.size
        expect(r.hub.edit(mine, "first"))
        val (envelope, edit) = r.sealedSent(before).single()
        assertEq("edit", edit["type"])
        assertEq(mine.id, edit["target"])
        assertEq("first", edit["text"])
        assertEq(null, envelope["notify"])
        assertEq("first", r.hub.rows(r.conversation).first { it.message.id == mine.id }.text)
        expect(!r.hub.edit(theirs, "mine now"))
        expect(!r.dm.edit(r.conversation, theirs.id, "mine now"))
    }

    fun testANewDeviceIsHeldUntilApprovedAndTheSafetyNumberChanges() {
        val r = rig()
        val before = r.dm.safetyNumber(r.conversation)!!
        assertEq(60, before.digits.length)
        assertEq(12, DirectMessageFrames.safetyGroups(before.digits).size)
        expect(before.qr.startsWith("forkmesh-safety:3:direct:$conversationId:"))
        expect(!r.dm.isVerified(r.conversation))
        expect(!r.dm.verifyScanned(r.conversation, "forkmesh-safety:3:direct:$conversationId:wrong"))
        expect(r.dm.verifyScanned(r.conversation, before.qr))
        expect(r.dm.isVerified(r.conversation))

        val phone = peerSeal()
        phone.acceptKeyFrame(mySeal(r.store).keyFrame())
        r.socket.onText(envelope(phone.keyFrame()))
        assertEq(listOf(phone.keyId), r.dm.pendingDevices(r.conversation).map { it.kid })
        val line = r.dm.messages(r.conversation).single { it.senderId == DirectMessageFrames.SYSTEM_SENDER }
        assertEq(DirectMessageFrames.pendingDeviceLine("pat"), line.text)
        expect(line.ephemeral)

        val from = r.socket.sent.size
        expect(r.dm.send(r.conversation, "not for the new phone yet"))
        val sealed = sentFrames(r.socket, from).mapNotNull { crypto.decrypt(it) }.single { RoomSeal.isRatchetFrame(it) }
        expect(phone.keyId !in RoomSeal.wrappedKeyIds(sealed), "a pending device gets nothing sealed to it")
        expect(r.peer.keyId in RoomSeal.wrappedKeyIds(sealed))

        r.socket.onText(envelope(phone.seal(peerChat("pat-new-1", "from my new phone"))))
        expect(r.dm.messages(r.conversation).none { it.text == "from my new phone" }, "held, not shown")
        expect(r.dm.approveDevice(r.conversation, phone.keyId))
        expect(r.dm.messages(r.conversation).any { it.text == "from my new phone" }, "shown once approved")
        expect(r.dm.messages(r.conversation).any { it.text == DirectMessageFrames.safetyChangedLine("pat") })
        expect(r.dm.pendingDevices(r.conversation).isEmpty())
        expect(!r.dm.isVerified(r.conversation), "a changed device set clears verified")
        val after = r.dm.safetyNumber(r.conversation)!!
        expect(after.digits != before.digits)
        expect(!r.dm.verifyScanned(r.conversation, before.qr))
        expect(r.dm.markVerified(r.conversation))
        expect(r.dm.isVerified(r.conversation))
    }

    fun testABlockedDeviceIsIgnored() {
        val r = rig()
        val phone = peerSeal()
        phone.acceptKeyFrame(mySeal(r.store).keyFrame())
        r.socket.onText(envelope(phone.keyFrame()))
        expect(r.dm.blockDevice(r.conversation, phone.keyId))
        r.socket.onText(envelope(phone.seal(peerChat("pat-b", "blocked"))))
        expect(r.dm.messages(r.conversation).none { it.text == "blocked" })
        expect(r.dm.pendingDevices(r.conversation).isEmpty())
        expect(r.dm.messages(r.conversation).none { it.text == DirectMessageFrames.safetyChangedLine("pat") })
        assertEq(listOf(r.peer.keyId), r.dm.deviceKeys(r.conversation))
    }

    fun testASenderIdThatIsNotTheDevicesAccountIsDropped() {
        val r = rig()
        r.fromPeer(peerChat("pat-1", "real"))
        r.fromPeer(peerChat("x-1", "pretending to be me") + ("senderId" to "me-node"))
        r.fromPeer(r.peerFrame("reaction", "x-2", "target" to "pat-1", "emoji" to "👍", "reactorId" to "me-node", "added" to true))
        assertEq(listOf("real"), r.dm.messages(r.conversation).map { it.text })
        expect(r.hub.rows(r.conversation).single().reactions.isEmpty(), "a reaction must come from its reactor")
    }

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
        val sealedEnvelope = frames.first { crypto.decrypt(it)?.let(RoomSeal::isRatchetFrame) == true }
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
        val peerSeal = peerSeal()
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
            .filter { RoomSeal.isRatchetFrame(it) }
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
        assertEq(setOf("one", "two", "three"), dm.messages(conversation).filter { !it.ephemeral }.map { it.text }.toSet())
        assertEq(1, dm.pendingDevices(conversation).size, "a second device for the same chat waits for approval")
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
            onText(envelope(peer.keyFrame(), "historyReplay" to true))
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
        val store = MemorySecretStore()
        val connector = FakeOfficeConnector()
        val dm = service(preparedApi(), connector, store)
        dm.refresh()
        val conversation = dm.conversations.single()
        val socket = connector.last!!
        socket.onText(historyEnd)
        expect(dm.send(conversation, "before your key"))
        val firstRef = OfficeFrames.sentRef(socket.sent.last())
        val peer = peerSeal()
        peer.acceptKeyFrame(mySeal(store).keyFrame())
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

    fun testTheOutboxSurvivesAReconnectAndIsResealedWhenThePeerKeyArrives() {
        val store = MemorySecretStore()
        val loop = EventLoop("direct-messages-outbox", virtualTime = true)
        val connector = ScriptedDirectConnector {}
        val dm = service(preparedApi(), connector, store, loop = loop)
        dm.refresh()
        val conversation = dm.conversations.single()
        val first = connector.sockets.single()
        first.onText(historyEnd)
        expect(dm.send(conversation, "sent while you were away"))
        val ownText = first.sent.last()
        val ownRef = OfficeFrames.sentRef(ownText)
        val ownFrame = crypto.decrypt(Json.parseObject(ownText))!!
        assertEq(emptyList<String>(), RoomSeal.wrappedKeyIds(ownFrame), "a frame with no entry is still sent")
        connector.closers[0]()
        loop.advance(1000)
        val socket = connector.sockets.last()
        expect(socket !== first, "reconnected")
        val peer = peerSeal()
        peer.acceptKeyFrame(mySeal(store).keyFrame())
        socket.onText(envelope(ownFrame, "ref" to ownRef, "historyReplay" to true, "notify" to true))
        socket.onText(envelope(peer.keyFrame(), "ref" to "b".repeat(40), "historyReplay" to true))
        val before = socket.sent.size
        socket.onText(historyEnd)
        val after = sentFrames(socket, before)
        val resent = after.single { it["resend"] == true }
        assertEq(true, resent["notify"])
        val sealed = crypto.decrypt(resent)!!
        assertEq(listOf(peer.keyId), RoomSeal.wrappedKeyIds(sealed), "complete without an entry for this phone")
        assertEq("sent while you were away", peer.unseal(sealed)!!["text"])
        assertEq(listOf(ownRef), after.single { it["kind"] == "forkmesh-superseded" }["refs"])
        expect(dm.messages(conversation).all { it.self })
        dm.closeAll()
        loop.shutdown()
    }

    private fun Rig.exchange() {
        fromPeer(peerChat("pat-1", "first"), "1".repeat(40))
        expect(dm.send(conversation, "my reply"))
        val reply = crypto.decrypt(Json.parseObject(socket.sent.last { crypto.decrypt(Json.parseObject(it))?.let(RoomSeal::isRatchetFrame) == true }))!!
        assertEq("my reply", peer.unseal(reply)!!["text"])
    }

    fun testALostSessionSendsOneResetAndOneKeyAdvertisement() {
        val r = rig()
        r.exchange()
        val myKid = mySeal(r.store).keyId
        mySeal(r.store).resetSessions(r.peer.keyId)
        val before = r.socket.sent.size
        r.fromPeer(peerChat("pat-2", "after the loss"), "2".repeat(40))
        r.fromPeer(peerChat("pat-3", "and again"), "3".repeat(40))
        val after = sentFrames(r.socket, before)
        val sealed = after.filter { crypto.decrypt(it)?.let(RoomSeal::isRatchetFrame) == true }
        val reset = sealed.single()
        assertEq(true, reset["persist"])
        assertEq(null, reset["notify"])
        val frame = crypto.decrypt(reset)!!
        assertEq(listOf(r.peer.keyId), RoomSeal.wrappedKeyIds(frame))
        val opened = r.peer.open(frame) as SealOpen.Opened
        assertEq(mapOf("type" to "e2ee-reset", "senderId" to "me-node"), opened.plain)
        val adverts = after.filter { crypto.decrypt(it)?.let(RoomSeal::isKeyFrame) == true }
        assertEq(1, adverts.size)
        assertEq(true, adverts.single()["persist"])
        expect(receipts(r.socket).none { it == "2".repeat(40) || it == "3".repeat(40) }, "a frame that cannot be opened is not acknowledged")
        expect(r.dm.messages(r.conversation).none { it.text == "after the loss" })
        r.fromPeer(peerChat("pat-4", "on the new session"), "4".repeat(40))
        expect(r.dm.messages(r.conversation).any { it.text == "on the new session" }, "the peer moved to the new session")
        expect(myKid !in RoomSeal.wrappedKeyIds(frame))
    }

    fun testAnOpenedResetResealsTheOutboxToThatDeviceOnly() {
        val r = rig()
        expect(r.dm.send(r.conversation, "hello"))
        val id = r.dm.messages(r.conversation).single().id
        val myKid = mySeal(r.store).keyId
        r.peer.resetSessions(myKid)
        val before = r.socket.sent.size
        r.socket.onText(envelope(r.peer.seal(mapOf("type" to "e2ee-reset", "senderId" to "pat-node"), only = listOf(myKid)), "ref" to "9".repeat(40)))
        expect(r.dm.messages(r.conversation).none { it.id != id }, "a reset is never shown")
        val after = sentFrames(r.socket, before)
        val resent = after.single { it["resend"] == true }
        assertEq(true, resent["persist"])
        assertEq(true, resent["notify"])
        val frame = crypto.decrypt(resent)!!
        assertEq(listOf(r.peer.keyId), RoomSeal.wrappedKeyIds(frame))
        val opened = r.peer.unseal(frame)!!
        assertEq(id, opened["id"])
        assertEq("hello", opened["text"])
        expect(after.none { it["kind"] == "forkmesh-superseded" }, "the original copy still serves the other devices")
        expect("9".repeat(40) in receipts(r.socket), "the reset frame is acknowledged")
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
        socket.onText(envelope(peer.keyFrame()))
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
        expect(hub.rows(conversation).all { it.deletable && it.editable && it.receipt == ChatReceipt.SENDING })
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
        val peerSeal = peerSeal()
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
            .filter { RoomSeal.isRatchetFrame(it) }
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
