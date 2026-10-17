package com.forkmesh.nativeapp.push

import com.forkmesh.nativeapp.chat.FakeAccountApi
import com.forkmesh.nativeapp.crypto.ChatDeviceKeys
import com.forkmesh.nativeapp.crypto.RoomCrypto
import com.forkmesh.nativeapp.crypto.RoomSeal
import com.forkmesh.nativeapp.crypto.SealOpen
import com.forkmesh.nativeapp.crypto.SealPolicy
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class DirectMessagePreviewTest {
    private val id = "d".repeat(32)
    private val room = "chat-direct:$id:v1"
    private val ref = "a".repeat(40)
    private val crypto = RoomCrypto.withPassphrase(room, "direct-pass")
    private val event = DirectMessageEvent(id, ref, 1)

    private fun chat(vararg extra: Pair<String, Any?>): Map<String, Any?> = mapOf(
        "type" to "chat", "id" to "pat-1", "senderId" to "pat-node", "sender" to "pat", "accountKind" to "user",
        "channel" to "direct:$id", "text" to "see you at six", "ts" to 1234L,
    ) + extra

    private fun pending(frame: String, vararg extra: Pair<String, Any?>): Map<String, Any?> = mapOf(
        "conversation" to "direct:$id", "room" to room, "keyVersion" to 1L, "passphrase" to "direct-pass", "frame" to frame,
    ) + extra

    private val peers = java.util.IdentityHashMap<MemorySecretStore, RoomSeal>()

    private fun mine(store: MemorySecretStore): RoomSeal =
        RoomSeal.loadOrCreate(room, scope = "direct:$id", policy = SealPolicy.STRICT, acct = "me-node", secureStore = store)

    private fun device(acct: String = "pat"): RoomSeal =
        RoomSeal.loadOrCreate(room, scope = "direct:$id", policy = SealPolicy.STRICT, acct = acct, secureStore = MemorySecretStore(), node = "pat-node")

    private fun peerFor(store: MemorySecretStore): RoomSeal = peers.getOrPut(store) {
        val peer = device()
        peer.acceptKeyFrame(mine(store).keyFrame())
        mine(store).acceptKeyFrame(peer.keyFrame())
        peer
    }

    private fun wire(frame: Map<String, Any?>): String =
        Json.stringify(crypto.encrypt(frame) + ("ref" to ref) + ("persist" to true) + ("notify" to true))

    private fun sealedFor(store: MemorySecretStore, plain: Map<String, Any?>): String = wire(peerFor(store).seal(plain))

    fun testEventFramesNameTheConversationAndRef() {
        val frame = mapOf("type" to "event", "topic" to "direct-messages", "conversation" to "direct:$id", "ref" to ref, "keyVersion" to 3L)
        assertEq(DirectMessageEvent(id, ref, 3), DirectMessagePreviews.event(frame))
        assertEq("/api/chat/direct-messages/$id/pending?ref=$ref", DirectMessagePreviews.event(frame)?.pendingPath)
        assertEq("/api/chat/direct-messages/$id/delivered?ref=$ref", DirectMessagePreviews.event(frame)?.deliveredPath)
        assertEq(1, DirectMessagePreviews.event(frame - "keyVersion")?.keyVersion)
        assertEq(null, DirectMessagePreviews.event(frame - "ref"))
        assertEq(null, DirectMessagePreviews.event(frame + ("ref" to "A".repeat(40))))
        assertEq(null, DirectMessagePreviews.event(frame + ("conversation" to "direct:short")))
        assertEq(null, DirectMessagePreviews.event(frame + ("topic" to "pings")))
        assertEq(null, DirectMessagePreviews.event(mapOf("type" to "event", "topic" to "direct-messages")))
    }

    fun testThePendingFrameIsDecryptedOnThisPhone() {
        val store = MemorySecretStore()
        val api = FakeAccountApi()
        api.replies["GET /api/chat/direct-messages/$id/pending"] = pending(sealedFor(store, chat()))
        val preview = DirectMessagePreviews.fetch(api, event, store)!!
        assertEq("GET", api.calls.single().first)
        assertEq("/api/chat/direct-messages/$id/pending?ref=$ref", api.calls.single().second)
        assertEq("pat", preview.sender)
        assertEq("see you at six", preview.text)
        assertEq("pat-1", preview.messageId)
        assertEq(id, preview.conversationId)
        assertEq(1234L, preview.timestamp)
        expect(!preview.reply)
        val stranger = MemorySecretStore()
        assertEq(null, DirectMessagePreviews.fetch(api, event, stranger))
        assertEq(null, stranger.read(ChatDeviceKeys.IDENTITY_STORE_KEY))
        api.failures["GET /api/chat/direct-messages/$id/pending"] = OrgApiException("not_found", "gone")
        assertEq(null, DirectMessagePreviews.fetch(api, event, store))
    }

    fun testAPreviewLeavesTheFrameForTheAppAndSkipsUnapprovedDevices() {
        val store = MemorySecretStore()
        val frame = peerFor(store).seal(chat())
        expect(DirectMessagePreviews.open(event, pending(wire(frame)), store) != null)
        expect(mine(store).open(frame) is SealOpen.Opened, "the preview did not use up the frame's counter")
        val newPhone = device()
        newPhone.acceptKeyFrame(mine(store).keyFrame())
        mine(store).acceptKeyFrame(newPhone.keyFrame())
        val fromNewPhone = wire(newPhone.seal(chat("id" to "pat-2")))
        assertEq(null, DirectMessagePreviews.open(event, pending(fromNewPhone), store), "a pending device shows only New message")
        assertEq(0, mine(store).trust.heldCount(room), "the preview does not hold frames either")
    }

    fun testDeliveryIsConfirmedWithAPostThatReadsNothing() {
        val api = FakeAccountApi()
        expect(DirectMessagePreviews.confirmDelivery(api, event))
        assertEq(Triple("POST", "/api/chat/direct-messages/$id/delivered?ref=$ref", null), api.calls.single())
        api.failures["POST /api/chat/direct-messages/$id/delivered"] = OrgApiException("not_found", "gone")
        expect(!DirectMessagePreviews.confirmDelivery(api, event))
    }

    fun testOnlySealedFramesForThisConversationArePreviewed() {
        val store = MemorySecretStore()
        val sealed = sealedFor(store, chat())
        expect(DirectMessagePreviews.open(event, pending(sealed), store) != null)
        val plainFrame = Json.stringify(crypto.encrypt(chat()))
        assertEq(null, DirectMessagePreviews.open(event, pending(plainFrame), store))
        assertEq(null, DirectMessagePreviews.open(event, pending(sealed, "conversation" to "direct:${"e".repeat(32)}"), store))
        assertEq(null, DirectMessagePreviews.open(event, pending(sealed, "room" to "chat-direct:${"e".repeat(32)}:v1"), store))
        assertEq(null, DirectMessagePreviews.open(event, pending(sealed, "passphrase" to "wrong"), store))
        assertEq(null, DirectMessagePreviews.open(event, pending("not json"), store))
        val elsewhere = sealedFor(store, chat("channel" to "direct:${"e".repeat(32)}"))
        assertEq(null, DirectMessagePreviews.open(event, pending(elsewhere), store))
        val call = sealedFor(store, chat("type" to "call-invite"))
        assertEq(null, DirectMessagePreviews.open(event, pending(call), store))
        val reply = sealedFor(store, chat("type" to "thread-reply", "rootId" to "pat-0", "text" to "in a thread"))
        val opened = DirectMessagePreviews.open(event, pending(reply), store)!!
        expect(opened.reply)
        assertEq("in a thread", opened.text)
    }

    fun testTheContentSettingShapesTheNotification() {
        val preview = DirectMessagePreview("pat-1", id, "pat", "see you at six", "", 1L)
        assertEq(PreviewNotice("pat", "see you at six", "pat", "see you at six"), DirectMessagePreviews.notice(preview, NotificationContent.NAME_AND_MESSAGE))
        assertEq(PreviewNotice("pat", "New message", "pat", "New message"), DirectMessagePreviews.notice(preview, NotificationContent.NAME_ONLY))
        assertEq(PreviewNotice("ForkMesh", "New message", "", "New message"), DirectMessagePreviews.notice(preview, NotificationContent.NOTHING))
        val file = preview.copy(text = "", fileName = "plan.pdf", sender = "")
        assertEq(PreviewNotice("sam", "📎 plan.pdf", "sam", "📎 plan.pdf"), DirectMessagePreviews.notice(file, NotificationContent.NAME_AND_MESSAGE, "sam"))
        assertEq("📎 plan.pdf\nlook", DirectMessagePreviews.body(file.copy(text = "look")))
        assertEq(DirectMessagePreviews.MAX_BODY_CHARS, DirectMessagePreviews.body(preview.copy(text = "x".repeat(5000))).length)
        assertEq(NotificationContent.NAME_AND_MESSAGE, NotificationContent.of(null))
        assertEq(NotificationContent.NAME_ONLY, NotificationContent.of("name-only"))
        assertEq(NotificationContent.NOTHING, NotificationContent.of("none"))
        assertEq(NotificationContent.NAME_AND_MESSAGE, NotificationContent.of("bogus"))
        assertEq(listOf("Name and message", "Name only", "No name or message"), NotificationContent.entries.map { it.label })
    }

    fun testEachMessageIsListedOnceInItsConversation() {
        val first = PreviewLine("a", "pat", "one", 10)
        val second = PreviewLine("b", "pat", "two", 20)
        val lines = DirectMessagePreviews.merge(listOf(first), second)!!
        assertEq(listOf("one", "two"), lines.map { it.text })
        assertEq(null, DirectMessagePreviews.merge(lines, second))
        var many: List<PreviewLine> = emptyList()
        for (n in 1..12) many = DirectMessagePreviews.merge(many, PreviewLine("m$n", "pat", "line $n", n.toLong()))!!
        assertEq(DirectMessagePreviews.MAX_LINES, many.size)
        assertEq("line 12", many.last().text)
    }
}
