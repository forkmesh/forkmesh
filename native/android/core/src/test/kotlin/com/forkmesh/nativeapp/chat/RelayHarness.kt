package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.api.AccountSettings
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.crypto.Bytes
import com.forkmesh.nativeapp.crypto.ChatDeviceKeys
import com.forkmesh.nativeapp.crypto.ChatNodeKey
import com.forkmesh.nativeapp.crypto.RoomCrypto
import com.forkmesh.nativeapp.crypto.RoomSeal
import com.forkmesh.nativeapp.crypto.SealOpen
import com.forkmesh.nativeapp.crypto.SealPolicy
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.net.TestWebSocketServer
import com.forkmesh.nativeapp.security.Identity
import com.forkmesh.nativeapp.security.MemorySecretStore
import java.net.SocketTimeoutException
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

class RelayHarness(configure: (AccountSettings) -> Unit = {}) : AutoCloseable {
    companion object {
        const val PASSPHRASE = "relay-harness"
    }

    private val incoming = LinkedBlockingQueue<Map<String, Any?>>()

    @Volatile
    private var connection: TestWebSocketServer.Connection? = null

    val server = TestWebSocketServer { c ->
        c.readRequest().accept()
        connection = c
        while (true) {
            val frame = try {
                c.readFrame()
            } catch (_: SocketTimeoutException) {
                continue
            }
            when (frame.opcode) {
                1, 2 -> incoming.add(Json.parseObject(frame.text))
                8 -> return@TestWebSocketServer
                9 -> c.sendPong(frame.payload)
            }
        }
    }

    val settings: AccountSettings = AccountSettings.create(MemoryPrefStore(), MemorySecretStore())
    val crypto: RoomCrypto
    val room: RelayRoom
    val bundle: Map<String, Any?>

    init {
        settings.setServerUrl(server.baseUrl)
        configure(settings)
        crypto = RoomCrypto.withPassphrase(settings.room, PASSPHRASE)
        room = RelayRoom(settings, Identity.fromSeed(Bytes.random(32)), MemorySecretStore(), loop = EventLoop("relay-harness"))
        room.roomPassphraseProvider = { PASSPHRASE }
        room.connect()
        bundle = next { RoomSeal.isKeyFrame(it) } ?: throw AssertionError("no bundle from the client")
    }

    fun peer(acct: String): RoomSeal =
        RoomSeal.create(settings.room, settings.room, SealPolicy.OPEN, acct, ChatDeviceKeys.fromSeed(Bytes.random(32)), nodeKey = ChatNodeKey.generate())
            .also { it.acceptKeyFrame(bundle) }

    fun next(timeoutMs: Long = 3_000, match: (Map<String, Any?>) -> Boolean = { true }): Map<String, Any?>? {
        val deadline = System.currentTimeMillis() + timeoutMs
        while (true) {
            val left = deadline - System.currentTimeMillis()
            if (left <= 0) return null
            val envelope = incoming.poll(left, TimeUnit.MILLISECONDS) ?: return null
            val plain = crypto.decrypt(envelope) ?: continue
            if (match(plain)) return plain
        }
    }

    fun drain(): List<Map<String, Any?>> {
        val out = ArrayList<Map<String, Any?>>()
        while (true) {
            val envelope = incoming.poll() ?: return out
            crypto.decrypt(envelope)?.let { out.add(it) }
        }
    }

    fun opened(peer: RoomSeal, timeoutMs: Long = 3_000, match: (Map<String, Any?>) -> Boolean): Map<String, Any?>? {
        var found: Map<String, Any?>? = null
        next(timeoutMs) { frame ->
            if (!RoomSeal.isSealedFrame(frame)) return@next false
            val plain = (peer.open(frame) as? SealOpen.Opened)?.plain ?: return@next false
            if (match(plain)) {
                found = plain
                true
            } else {
                false
            }
        }
        return found
    }

    fun push(frame: Map<String, Any?>, extra: Map<String, Any?> = emptyMap()) {
        val envelope = LinkedHashMap(crypto.encrypt(frame))
        envelope.putAll(extra)
        val target = connection ?: throw AssertionError("not connected")
        target.sendText(Json.stringify(envelope))
    }

    fun join(peer: RoomSeal) {
        push(peer.keyFrame())
        next { RoomSeal.isKeyFrame(it) }
    }

    fun settle() {
        Thread.sleep(150)
        room.loop.awaitIdle()
    }

    override fun close() {
        room.dispose()
        server.close()
    }
}
