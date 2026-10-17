package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.crypto.RoomCrypto
import com.forkmesh.nativeapp.crypto.RoomSeal
import com.forkmesh.nativeapp.crypto.SealPolicy
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertBytes
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.io.File
import java.util.Base64

class FakeMediaServer(var role: String = "sender") : ChatAccountCalling {
    class Item(val kind: String, val size: Long, val parts: Int) {
        var state = "uploading"
        val data = HashMap<Int, ByteArray>()
    }

    val conversationId = "d".repeat(32)
    val items = LinkedHashMap<String, Item>()
    val calls = ArrayList<String>()
    val replies = HashMap<String, Map<String, Any?>>()
    var nextId = 1

    private fun gone(state: String): Nothing = throw OrgApiException("media_gone", "media gone").also { lastGoneState = state }

    var lastGoneState = ""

    private fun status(id: String, item: Item): Map<String, Any?> = mapOf(
        "id" to id, "state" to item.state, "role" to role, "size" to item.size, "parts" to item.parts,
    )

    override fun accountApiCall(method: String, path: String, body: Map<String, Any?>?): Map<String, Any?> {
        calls.add("$method ${path.substringBefore('?')}")
        val base = "/api/chat/direct-messages/$conversationId/media"
        if (!path.startsWith(base)) return replies["$method ${path.substringBefore('?')}"] ?: emptyMap()
        val rest = path.removePrefix(base)
        if (rest.isEmpty() && method == "POST") {
            val size = (body!!["size"] as Number).toLong()
            val parts = (body["parts"] as Number).toInt()
            if (parts.toLong() != (size + 524287) / 524288) throw OrgApiException("invalid_parts", "invalid parts")
            val id = "%032x".format(nextId++)
            items[id] = Item(body["kind"] as String, size, parts)
            return mapOf("ok" to true, "media" to status(id, items[id]!!))
        }
        if (rest.startsWith("?ids=") && method == "GET") {
            val ids = rest.removePrefix("?ids=").split(",")
            return mapOf("media" to ids.mapNotNull { id -> items[id]?.let { status(id, it) } })
        }
        val segments = rest.trim('/').split('/')
        val id = segments[0]
        val item = items[id] ?: throw OrgApiException("not_found", "not found")
        when {
            segments.size == 1 && method == "GET" -> return mapOf("media" to status(id, item))
            segments.size == 1 && method == "DELETE" -> {
                if (role != "sender") throw OrgApiException("sender_only", "sender only")
                if (item.state == "uploading" || item.state == "ready") {
                    item.state = "withdrawn"
                    item.data.clear()
                }
                return mapOf("ok" to true, "media" to status(id, item))
            }
            segments.size == 3 && method == "PUT" -> {
                val index = segments[2].toInt()
                val raw = Base64.getDecoder().decode(body!!["data"] as String)
                val expected = if (index < item.parts - 1) 524288L else item.size - 524288L * (item.parts - 1)
                if (raw.size.toLong() != expected) throw OrgApiException("invalid_part_size", "bad size")
                item.data[index] = raw
                return mapOf("ok" to true, "part" to index)
            }
            segments.size == 3 && method == "GET" -> {
                if (role != "recipient") throw OrgApiException("recipient_only", "recipient only")
                if (item.state != "ready") gone(item.state)
                val index = segments[2].toInt()
                return mapOf("index" to index, "parts" to item.parts, "data" to Base64.getEncoder().encodeToString(item.data[index]!!))
            }
            segments.size == 2 && segments[1] == "finish" -> {
                if (item.data.size != item.parts) throw OrgApiException("upload_incomplete", "incomplete")
                item.state = "ready"
                return mapOf("ok" to true, "media" to status(id, item))
            }
            segments.size == 2 && segments[1] == "consume" -> {
                if (role != "recipient") throw OrgApiException("recipient_only", "recipient only")
                if (item.state == "opened") return mapOf("ok" to true, "media" to status(id, item))
                if (item.state != "ready") gone(item.state)
                item.state = "opened"
                item.data.clear()
                return mapOf("ok" to true, "media" to status(id, item))
            }
        }
        throw OrgApiException("not_found", "not found")
    }
}

class ViewOnceMediaTest {
    private val conversationId = "d".repeat(32)
    private val conversation = "direct:$conversationId"
    private val room = "chat-direct:$conversationId:v1"
    private val crypto = RoomCrypto.withPassphrase(room, "direct-pass")

    private fun mySeal(store: MemorySecretStore) =
        RoomSeal.loadOrCreate(room, scope = conversation, policy = SealPolicy.STRICT, acct = "me-node", secureStore = store)

    private fun peerSeal() =
        RoomSeal.loadOrCreate(room, scope = conversation, policy = SealPolicy.STRICT, acct = "pat", secureStore = MemorySecretStore(), node = "pat-node")

    private fun prepared(server: FakeMediaServer): FakeMediaServer {
        server.replies["GET /api/chat/direct-messages"] = mapOf(
            "conversations" to listOf(mapOf("id" to conversationId, "otherUser" to "pat")),
            "nextCursor" to "",
        )
        server.replies["GET /api/chat/direct-messages/$conversationId/room-access"] = mapOf(
            "room" to room, "passphrase" to "direct-pass",
            "webSocketUrl" to "/api/chat/direct-messages/$conversationId/ws?ticket=abc",
        )
        return server
    }

    private fun service(api: ChatAccountCalling, connector: OfficeSocketConnector, store: MemorySecretStore) = DirectMessageService(
        api, connector, store, { "https://app.forkmesh.com" }, { "me-node" }, { "Me" },
        loop = EventLoop("view-once-test", virtualTime = true),
    )

    private fun field(kind: String = "video", media: String = "a".repeat(32), size: Long = 1000, parts: Long = 1) = linkedMapOf<String, Any?>(
        "v" to 1L, "kind" to kind, "media" to media,
        "key" to ViewOnceCodec.encodeKey(ByteArray(32) { it.toByte() }),
        "iv" to Base64.getUrlEncoder().encodeToString(ByteArray(12) { (it + 1).toByte() }),
        "mime" to "video/mp4; codecs=avc1", "durationMs" to 12_000L, "size" to size, "parts" to parts,
    )

    fun testCryptoRoundTripAndTamper() {
        val key = ViewOnceCrypto.newKey()
        val iv = ViewOnceCrypto.newIv()
        val plain = ByteArray(1_200_000) { (it * 31).toByte() }
        val sealed = ViewOnceCrypto.encrypt(plain, key, iv)
        assertEq(plain.size + 16, sealed.size)
        assertBytes(plain, ViewOnceCrypto.decrypt(sealed, key, iv)!!)
        sealed[5] = (sealed[5].toInt() xor 1).toByte()
        assertEq(null, ViewOnceCrypto.decrypt(sealed, key, iv))
        assertEq(null, ViewOnceCrypto.decrypt(ByteArray(8), key, iv))
        assertEq(null, ViewOnceCrypto.decrypt(sealed, ByteArray(16), iv))
    }

    fun testKnownVectorMatchesWebCryptoLayout() {
        val key = ByteArray(32)
        val iv = ByteArray(12)
        val sealed = ViewOnceCrypto.encrypt(ByteArray(0), key, iv)
        assertEq("530f8afbc74536b9a963b4f1c4cb738b", sealed.joinToString("") { "%02x".format(it) })
    }

    fun testCodecParsesPaddedAndUnpaddedAndRejectsBadFields() {
        val parsed = ViewOnceCodec.parse(field())!!
        assertEq(ViewOnceKind.VIDEO, parsed.kind)
        assertEq("video/mp4", parsed.mime)
        assertEq(32, parsed.key.size)
        assertEq(12, parsed.iv.size)
        assertEq(1, parsed.parts)
        val rebuilt = ViewOnceCodec.parse(ViewOnceCodec.field(parsed))!!
        assertBytes(parsed.key, rebuilt.key)
        assertBytes(parsed.iv, rebuilt.iv)
        expect(!(ViewOnceCodec.field(parsed)["key"] as String).contains("="))
        assertEq(null, ViewOnceCodec.parse(field(media = "A".repeat(32))))
        assertEq(null, ViewOnceCodec.parse(field(media = "a".repeat(31))))
        assertEq(null, ViewOnceCodec.parse(field(kind = "photo")))
        assertEq(null, ViewOnceCodec.parse(field(parts = 2)))
        assertEq(null, ViewOnceCodec.parse(field(size = 524_289, parts = 1)))
        expect(ViewOnceCodec.parse(field(size = 524_289, parts = 2)) != null)
        assertEq(null, ViewOnceCodec.parse(field(size = 13L * 1024 * 1024, parts = 26)))
        assertEq(null, ViewOnceCodec.parse(field() + ("v" to 2L)))
        assertEq(null, ViewOnceCodec.parse(field() + ("key" to "short")))
        assertEq(null, ViewOnceCodec.parse(field() + ("mime" to "audio/mp4")))
        assertEq(null, ViewOnceCodec.parse(field() + ("durationMs" to 62_000L)))
        expect(ViewOnceCodec.parse(field(kind = "voice") + ("mime" to "audio/mp4") + ("durationMs" to 120_000L)) != null)
        assertEq(null, ViewOnceCodec.parse("nope"))
    }

    fun testPartsAndSplitting() {
        assertEq(1, ViewOnceCodec.partsFor(1))
        assertEq(1, ViewOnceCodec.partsFor(524_288))
        assertEq(2, ViewOnceCodec.partsFor(524_289))
        val bytes = ByteArray(524_288 * 2 + 7) { it.toByte() }
        val pieces = ViewOnceCodec.split(bytes)
        assertEq(listOf(524_288, 524_288, 7), pieces.map { it.size })
        assertBytes(bytes, pieces.reduce { a, b -> a + b })
        assertEq("1:05", ViewOnceCodec.durationLabel(64_600))
    }

    fun testUploadThenRecipientDownloadsAndConsumes() {
        val server = FakeMediaServer("sender")
        val client = ViewOnceClient(server)
        val key = ViewOnceCrypto.newKey()
        val iv = ViewOnceCrypto.newIv()
        val plain = ByteArray(1_100_000) { (it % 251).toByte() }
        val sealed = ViewOnceCrypto.encrypt(plain, key, iv)
        val progress = ArrayList<Int>()
        val id = client.upload(conversationId, ViewOnceKind.VIDEO, sealed, 5_000) { done, _ -> progress.add(done) }
        assertEq(listOf(0, 1, 2, 3), progress)
        assertEq("ready", server.items[id]!!.state)
        assertEq(3, server.calls.count { it.startsWith("PUT ") })
        assertFails<OrgApiException> { client.download(conversationId, id, 3, sealed.size.toLong()) }
        server.role = "recipient"
        val downloaded = client.download(conversationId, id, 3, sealed.size.toLong())
        assertBytes(plain, ViewOnceCrypto.decrypt(downloaded, key, iv)!!)
        assertEq("opened", client.consume(conversationId, id))
        assertEq("opened", client.consume(conversationId, id))
        val gone = assertFails<ViewOnceGone> { client.download(conversationId, id, 3, sealed.size.toLong()) }
        assertEq("opened", gone.state)
        assertEq(mapOf(id to "opened"), client.status(conversationId, listOf(id, "bad", id)))
    }

    fun testFailedUploadIsWithdrawn() {
        val server = object : ChatAccountCalling {
            val inner = FakeMediaServer("sender")
            override fun accountApiCall(method: String, path: String, body: Map<String, Any?>?): Map<String, Any?> {
                if (path.endsWith("/finish")) throw OrgApiException("upload_incomplete", "nope")
                return inner.accountApiCall(method, path, body)
            }
        }
        val client = ViewOnceClient(server)
        assertFails<OrgApiException> { client.upload(conversationId, ViewOnceKind.VOICE, ByteArray(100), 1_000) }
        assertEq("withdrawn", server.inner.items.values.single().state)
        assertFails<ViewOnceException> { client.upload(conversationId, ViewOnceKind.VOICE, ByteArray(5 * 1024 * 1024), 1_000) }
    }

    fun testExpiredMediaAnswersGoneWithState() {
        val server = FakeMediaServer("recipient")
        server.items["b".repeat(32)] = FakeMediaServer.Item("voice", 100, 1).also { it.state = "expired" }
        val client = ViewOnceClient(server)
        val gone = assertFails<ViewOnceGone> { client.download(conversationId, "b".repeat(32), 1, 100) }
        assertEq("expired", gone.state)
        val consumed = assertFails<ViewOnceGone> { client.consume(conversationId, "b".repeat(32)) }
        assertEq("expired", consumed.state)
    }

    fun testRelayFrameParsing() {
        assertEq("c".repeat(32) to "opened", ViewOnceFrames.relayState(mapOf("kind" to "forkmesh-media", "v" to 1L, "id" to "c".repeat(32), "state" to "opened")))
        assertEq(null, ViewOnceFrames.relayState(mapOf("kind" to "forkmesh-media", "v" to 2L, "id" to "c".repeat(32), "state" to "opened")))
        assertEq(null, ViewOnceFrames.relayState(mapOf("kind" to "forkmesh-media", "v" to 1L, "id" to "xyz", "state" to "opened")))
        assertEq(null, ViewOnceFrames.relayState(mapOf("kind" to "forkmesh-media", "v" to 1L, "id" to "c".repeat(32), "state" to "gone")))
        assertEq(null, ViewOnceFrames.relayState(mapOf("kind" to "forkmesh-received", "v" to 1L)))
    }

    fun testStoreDropsKeyOnceTerminalAndNeverRestoresIt() {
        val store = ViewOnceStore()
        val media = ViewOnceCodec.parse(field())!!
        val entry = store.accept("m1", conversation, media, self = false, sentAt = 1)
        expect(entry.playable)
        expect(store.keyFor("m1") != null)
        assertEq(listOf(media.media), store.pending(conversation))
        assertEq(ViewOnceState.OPENED, store.update(media.media, "opened")!!.state)
        expect(!store.entry("m1")!!.hasKey)
        assertEq(null, store.keyFor("m1"))
        assertEq(null, store.update(media.media, "ready"))
        store.accept("m1", conversation, media, self = false, sentAt = 1)
        expect(!store.entry("m1")!!.playable)
        assertEq(emptyList<String>(), store.pending(conversation))
        store.update("e".repeat(32), "expired")
        val late = store.accept("m2", conversation, ViewOnceCodec.parse(field(media = "e".repeat(32)))!!, self = false, sentAt = 2)
        assertEq(ViewOnceState.EXPIRED, late.state)
        expect(!late.hasKey)
        val mine = store.accept("m3", conversation, ViewOnceCodec.parse(field(media = "f".repeat(32)))!!, self = true, sentAt = 3)
        assertEq(ViewOnceState.SENT, mine.state)
        expect(!mine.hasKey)
        assertEq("Sent", mine.statusLabel)
        assertEq(ViewOnceState.SENT, ViewOnceState.fromServer("ready", true))
        assertEq(ViewOnceState.READY, ViewOnceState.fromServer("ready", false))
    }

    fun testSenderUploadsSealsFrameAndLearnsOpenedFromRelay() {
        val server = prepared(FakeMediaServer("sender"))
        val connector = FakeOfficeConnector()
        val store = MemorySecretStore()
        val dm = service(server, connector, store)
        val hub = ChatHub(FakeChatRelay())
        hub.direct = dm
        dm.refresh()
        expect(dm.open(conversation))
        val socket = connector.last!!
        val peer = peerSeal()
        socket.onText(Json.stringify(crypto.encrypt(peer.keyFrame())))
        peer.acceptKeyFrame(mySeal(store).keyFrame())
        val recording = ByteArray(700_000) { (it % 97).toByte() }
        expect(hub.sendViewOnce(conversation, ViewOnceKind.VIDEO, "video/mp4", 9_000, recording))
        val mediaId = server.items.keys.single()
        assertEq("ready", server.items[mediaId]!!.state)
        val frame = socket.sent.mapNotNull { crypto.decrypt(Json.parseObject(it)) }
            .filter { RoomSeal.isSealedFrame(it) }
            .mapNotNull { peer.unseal(it) }
            .single { it["type"] == "chat" }
        assertEq("🎥 Video message", frame["text"])
        val sent = ViewOnceCodec.parse(frame["viewOnce"])!!
        assertEq(mediaId, sent.media)
        assertEq(2, sent.parts)
        val joined = server.items[mediaId]!!.data.let { it[0]!! + it[1]!! }
        assertBytes(recording, ViewOnceCrypto.decrypt(joined, sent.key, sent.iv)!!)
        val row = hub.rows(conversation).single()
        assertEq("", row.text)
        assertEq(ViewOnceState.SENT, row.viewOnce!!.state)
        expect(!row.viewOnce!!.hasKey)
        socket.onText("""{"kind":"forkmesh-media","v":1,"id":"$mediaId","state":"opened"}""")
        assertEq("Opened", hub.rows(conversation).single().viewOnce!!.statusLabel)
        dm.closeAll()
    }

    fun testSenderCanWithdrawAnUnopenedMessage() {
        val server = prepared(FakeMediaServer("sender"))
        val connector = FakeOfficeConnector()
        val dm = service(server, connector, MemorySecretStore())
        dm.refresh()
        expect(dm.open(conversation))
        expect(dm.sendViewOnce(conversation, ViewOnceKind.VOICE, "audio/mp4", 3_000, ByteArray(2_000) { 1 }))
        val messageId = dm.messages(conversation).single().id
        assertEq("🎤 Voice message", dm.messages(conversation).single().text)
        expect(dm.withdrawViewOnce(messageId))
        assertEq(ViewOnceState.WITHDRAWN, dm.viewOnce.entry(messageId)!!.state)
        expect(!dm.withdrawViewOnce(messageId))
        assertFails<ViewOnceException> { dm.sendViewOnce(conversation, ViewOnceKind.VOICE, "video/mp4", 3_000, ByteArray(10)) }
        dm.closeAll()
    }

    fun testRecipientPlaysOnceAndKeyIsGone() {
        val server = prepared(FakeMediaServer("sender"))
        val key = ViewOnceCrypto.newKey()
        val iv = ViewOnceCrypto.newIv()
        val recording = ByteArray(40_000) { (it % 13).toByte() }
        val sealed = ViewOnceCrypto.encrypt(recording, key, iv)
        val mediaId = ViewOnceClient(server).upload(conversationId, ViewOnceKind.VOICE, sealed, 4_000)
        server.role = "recipient"
        val connector = FakeOfficeConnector()
        val store = MemorySecretStore()
        val dm = service(server, connector, store)
        val hub = ChatHub(FakeChatRelay())
        hub.direct = dm
        dm.refresh()
        expect(dm.open(conversation))
        val socket = connector.last!!
        val peer = peerSeal()
        peer.acceptKeyFrame(mySeal(store).keyFrame())
        socket.onText(Json.stringify(crypto.encrypt(peer.keyFrame())))
        val media = ViewOnceMedia(ViewOnceKind.VOICE, mediaId, key, iv, "audio/mp4", 4_000, sealed.size.toLong(), 1)
        val chat = mapOf(
            "type" to "chat", "id" to "pat-1", "senderId" to "pat-node", "sender" to "pat", "accountKind" to "user",
            "channel" to conversation, "text" to "ignored text", "fileName" to "x.mp4", "file" to "AAAA", "ts" to 5L,
            "viewOnce" to ViewOnceCodec.field(media),
        )
        socket.onText(Json.stringify(crypto.encrypt(peer.seal(chat))))
        val message = dm.messages(conversation).single()
        assertEq("🎤 Voice message", message.text)
        assertEq("", message.fileName)
        val row = hub.rows(conversation).single()
        expect(row.viewOnce!!.playable)
        assertEq(null, row.attachment)
        assertBytes(recording, dm.downloadViewOnce("pat-1"))
        expect(dm.viewOnce.entry("pat-1")!!.playable)
        expect(dm.consumeViewOnce("pat-1"))
        assertEq("opened", server.items[mediaId]!!.state)
        val opened = dm.viewOnce.entry("pat-1")!!
        assertEq(ViewOnceState.OPENED, opened.state)
        expect(!opened.hasKey)
        socket.onText(Json.stringify(crypto.encrypt(peer.seal(chat)) + ("historyReplay" to true)))
        expect(!dm.viewOnce.entry("pat-1")!!.hasKey)
        assertFails<ViewOnceGone> { dm.downloadViewOnce("pat-1") }
        expect(!dm.consumeViewOnce("pat-1"))
        dm.closeAll()
    }

    fun testStatusReadOnOpenMarksExpiredAndGoneDownloadUpdatesRow() {
        val server = prepared(FakeMediaServer("recipient"))
        val first = "1".repeat(32)
        val second = "2".repeat(32)
        server.items[first] = FakeMediaServer.Item("video", 100, 1).also { it.state = "expired" }
        server.items[second] = FakeMediaServer.Item("video", 100, 1).also { it.state = "withdrawn" }
        val connector = FakeOfficeConnector()
        val store = MemorySecretStore()
        val dm = service(server, connector, store)
        dm.refresh()
        expect(dm.open(conversation))
        val socket = connector.last!!
        val peer = peerSeal()
        peer.acceptKeyFrame(mySeal(store).keyFrame())
        socket.onText(Json.stringify(crypto.encrypt(peer.keyFrame())))
        for ((index, id) in listOf(first, second).withIndex()) {
            val media = ViewOnceMedia(ViewOnceKind.VIDEO, id, ByteArray(32), ByteArray(12), "video/mp4", 1_000, 100, 1)
            val chat = mapOf(
                "type" to "chat", "id" to "pat-$index", "senderId" to "pat-node", "sender" to "pat",
                "channel" to conversation, "text" to "x", "ts" to index.toLong(), "viewOnce" to ViewOnceCodec.field(media),
            )
            socket.onText(Json.stringify(crypto.encrypt(peer.seal(chat))))
        }
        val gone = assertFails<ViewOnceGone> { dm.downloadViewOnce("pat-1") }
        assertEq("withdrawn", gone.state)
        assertEq(ViewOnceState.WITHDRAWN, dm.viewOnce.entry("pat-1")!!.state)
        server.calls.clear()
        dm.refreshViewOnce(conversation)
        assertEq(listOf("GET /api/chat/direct-messages/$conversationId/media"), server.calls)
        assertEq(ViewOnceState.EXPIRED, dm.viewOnce.entry("pat-0")!!.state)
        server.calls.clear()
        dm.refreshViewOnce(conversation)
        assertEq(emptyList<String>(), server.calls)
        dm.closeAll()
    }

    private fun receipts(socket: FakeOfficeSocket): List<Any?> =
        socket.sent.map { Json.parseObject(it) }.filter { it["kind"] == "forkmesh-received" }.flatMap { it["refs"] as List<*> }

    fun testReceiptIsHeldUntilOpenedExpiredOrWithdrawn() {
        val server = prepared(FakeMediaServer("sender"))
        val key = ViewOnceCrypto.newKey()
        val iv = ViewOnceCrypto.newIv()
        val sealedMedia = ViewOnceCrypto.encrypt(ByteArray(3_000) { 7 }, key, iv)
        val playId = ViewOnceClient(server).upload(conversationId, ViewOnceKind.VOICE, sealedMedia, 2_000)
        server.role = "recipient"
        val expireId = "3".repeat(32)
        val withdrawId = "4".repeat(32)
        server.items[expireId] = FakeMediaServer.Item("voice", 100, 1).also { it.state = "ready" }
        server.items[withdrawId] = FakeMediaServer.Item("voice", 100, 1).also { it.state = "ready" }
        val connector = FakeOfficeConnector()
        val store = MemorySecretStore()
        val dm = service(server, connector, store)
        dm.refresh()
        expect(dm.open(conversation))
        val socket = connector.last!!
        val peer = peerSeal()
        peer.acceptKeyFrame(mySeal(store).keyFrame())
        socket.onText(Json.stringify(crypto.encrypt(peer.keyFrame())))
        fun chat(id: String, media: ViewOnceMedia?) = mapOf(
            "type" to "chat", "id" to id, "senderId" to "pat-node", "sender" to "pat",
            "channel" to conversation, "text" to "plain $id", "ts" to 1L,
        ) + (if (media != null) mapOf("viewOnce" to ViewOnceCodec.field(media)) else emptyMap())
        val playRef = "a".repeat(40)
        val expireRef = "b".repeat(40)
        val withdrawRef = "c".repeat(40)
        val textRef = "e".repeat(40)
        val replayRef = "f".repeat(40)
        socket.onText(Json.stringify(crypto.encrypt(peer.seal(chat("v-play", ViewOnceMedia(ViewOnceKind.VOICE, playId, key, iv, "audio/mp4", 2_000, sealedMedia.size.toLong(), 1)))) + ("ref" to playRef)))
        socket.onText(Json.stringify(crypto.encrypt(peer.seal(chat("v-expire", ViewOnceMedia(ViewOnceKind.VOICE, expireId, key, iv, "audio/mp4", 1_000, 100, 1)))) + ("ref" to expireRef)))
        socket.onText(Json.stringify(crypto.encrypt(peer.seal(chat("v-withdraw", ViewOnceMedia(ViewOnceKind.VOICE, withdrawId, key, iv, "audio/mp4", 1_000, 100, 1)))) + ("ref" to withdrawRef) + ("historyReplay" to true)))
        socket.onText(Json.stringify(crypto.encrypt(peer.seal(chat("t-1", null))) + ("ref" to textRef)))
        socket.onText("""{"kind":"forkmesh-history-end","v":1}""")
        assertEq(listOf(textRef), receipts(socket))
        assertEq(3, dm.heldReceiptCount())
        socket.onText(Json.stringify(crypto.encrypt(peer.seal(chat("v-play", ViewOnceMedia(ViewOnceKind.VOICE, playId, key, iv, "audio/mp4", 2_000, sealedMedia.size.toLong(), 1)))) + ("ref" to replayRef) + ("resend" to true)))
        assertEq(listOf(textRef), receipts(socket))
        dm.downloadViewOnce("v-play")
        expect(dm.consumeViewOnce("v-play"))
        assertEq(listOf(textRef, playRef, replayRef), receipts(socket))
        socket.onText("""{"kind":"forkmesh-media","v":1,"id":"$expireId","state":"expired"}""")
        assertEq(listOf(textRef, playRef, replayRef, expireRef), receipts(socket))
        server.items[withdrawId]!!.state = "withdrawn"
        dm.refreshViewOnce(conversation)
        assertEq(listOf(textRef, playRef, replayRef, expireRef, withdrawRef), receipts(socket))
        assertEq(0, dm.heldReceiptCount())
        val lateRef = "9".repeat(40)
        socket.onText(Json.stringify(crypto.encrypt(peer.seal(chat("v-expire", ViewOnceMedia(ViewOnceKind.VOICE, expireId, key, iv, "audio/mp4", 1_000, 100, 1)))) + ("ref" to lateRef)))
        assertEq(lateRef, receipts(socket).last())
        dm.closeAll()
    }

    fun testCaptureFiltersArePassThrough() {
        val file = File("recording.mp4")
        expect(PassThroughCaptureFilters.apply(ViewOnceKind.VIDEO, file, "video/mp4") === file)
        expect(PassThroughCaptureFilters.apply(ViewOnceKind.VOICE, file, "audio/mp4") === file)
        expect(!PassThroughCaptureFilters.face.active)
        expect(!PassThroughCaptureFilters.voice.active)
    }
}
