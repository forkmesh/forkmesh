package com.forkmesh.nativeapp.control

import com.forkmesh.nativeapp.agents.RelayFrameChannel
import com.forkmesh.nativeapp.crypto.B64
import com.forkmesh.nativeapp.crypto.Bytes
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.security.DeviceIdentity
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit

class RelayControlConnection private constructor(
    private val channel: RelayFrameChannel,
    node: String,
    val desktopKey: String,
    val session: String,
) : ControlLink {
    private enum class Stage { Connecting, AwaitingWelcome, Open, Closed }

    companion object {
        const val CHUNK_CHARS = 9000
        const val MAX_FRAME_BYTES = 1024 * 1024

        fun newSessionId(): String = Bytes.hex(Bytes.random(16))

        private val keepaliveScheduler = Executors.newSingleThreadScheduledExecutor {
            Thread(it, "RelayControl-keepalive").apply { isDaemon = true }
        }

        fun open(
            channel: RelayFrameChannel,
            identity: DeviceIdentity,
            desktopKey: String,
            node: String,
            deviceName: String = "",
            user: String = "",
            platform: String = "",
            session: String = newSessionId(),
            timeoutMs: Long = 20_000,
            keepaliveMs: Long = 30_000,
        ): RelayControlConnection {
            val connection = RelayControlConnection(channel, node, desktopKey, session)
            try {
                connection.handshake(identity, deviceName, user, platform, timeoutMs)
            } catch (error: Exception) {
                connection.finish((error as? LocalControlException)?.code ?: "closed", notifyDesktop = true)
                runCatching { channel.dispose() }
                throw error
            }
            connection.startKeepalive(keepaliveMs)
            return connection
        }
    }

    val node: String = node.trim().lowercase()
    private val lock = Any()
    private var stage = Stage.Connecting
    private var keys = ControlKeys(ByteArray(0), ByteArray(0))
    private var sendSeq = 0
    private var receiveSeq = 0
    private var sendCounter = 0L
    private var receiveCounter = 0L
    private val pending = ByteChunks()
    private val dispatcher = ControlDispatcher()
    private val socketUp = CompletableFuture<Unit>()
    private val welcome = CompletableFuture<Map<String, Any?>>()
    private val sessionEvent = CompletableFuture<Map<String, Any?>>()
    private val closeListeners = CopyOnWriteArrayList<(String) -> Unit>()

    @Volatile
    private var lastHeardMs = System.currentTimeMillis()
    private var keepalive: ScheduledFuture<*>? = null

    val idleMs: Long get() = System.currentTimeMillis() - lastHeardMs

    @Volatile
    override var desktopName: String = ""
        private set

    @Volatile
    var desktopUser: String = ""
        private set

    @Volatile
    var closeReason: String = ""
        private set

    override val transport: ControlTransport get() = ControlTransport.Relay
    override val isOpen: Boolean get() = synchronized(lock) { stage == Stage.Open }

    fun onClose(listener: (String) -> Unit) {
        closeListeners.add(listener)
        val closed = synchronized(lock) { stage == Stage.Closed }
        if (closed) listener(closeReason)
    }

    private fun handshake(identity: DeviceIdentity, deviceName: String, user: String, platform: String, timeoutMs: Long) {
        channel.onFrame = { onFrame(it) }
        channel.onConnected = { onSocketConnected() }
        channel.start()
        awaitControl(socketUp, timeoutMs)
        val handshake = ControlHandshake(identity, deviceName, user, platform)
        synchronized(lock) { stage = Stage.AwaitingWelcome }
        if (!sendChunks("open", handshake.hello())) throw LocalControlException("relay_unavailable")
        val w = awaitControl(welcome, timeoutMs)
        val result = handshake.complete(w, desktopKey)
        synchronized(lock) {
            keys = result.keys
            stage = Stage.Open
        }
        desktopName = result.serverName
        desktopUser = result.serverUser
        if (!sendChunks("data", result.auth)) throw LocalControlException("relay_unavailable")
        val early = dispatcher.takeEarly { it.event == "session" || it.event == "pairing-denied" }
        if (early != null) {
            resolveSession(early)
        }
        val session = awaitControl(sessionEvent, timeoutMs)
        if (session["paired"] != true) throw LocalControlException("pair_on_lan")
    }

    private fun resolveSession(event: ControlEvent) {
        if (event.event == "pairing-denied") {
            sessionEvent.completeExceptionally(LocalControlException(jsonText(event.data["reason"]).ifEmpty { "pair_on_lan" }))
        } else {
            sessionEvent.complete(event.data)
        }
    }

    private fun onSocketConnected() {
        val current = synchronized(lock) { stage }
        when (current) {
            Stage.Connecting -> socketUp.complete(Unit)
            Stage.AwaitingWelcome, Stage.Open -> finish("relay_reconnected", notifyDesktop = true)
            Stage.Closed -> Unit
        }
    }

    internal fun onFrame(frame: Map<String, Any?>) {
        val payload: ByteArray
        synchronized(lock) {
            if (stage == Stage.Closed || frame["type"] != "control") return
            if (frame["session"] != session || frame["from"] != "node") return
            lastHeardMs = System.currentTimeMillis()
            when (frame["action"]) {
                "close", "error" -> {
                    val reason = jsonText(frame["reason"])
                    return finishLater(reason.ifEmpty { "closed" })
                }
                "data" -> Unit
                else -> return
            }
            val seq = (frame["seq"] as? Number)?.toInt()
            if (seq != receiveSeq) return finishLater("relay_out_of_order", true)
            receiveSeq++
            val chunk = B64.decodeOrNull(jsonText(frame["data"]))
            if (chunk == null || chunk.isEmpty()) return finishLater("bad_frame", true)
            pending.add(chunk)
            if (pending.size > MAX_FRAME_BYTES) return finishLater("frame_too_large", true)
            if (frame["more"] == true) return
            payload = pending.take()
        }
        handlePayload(payload)
    }

    private fun finishLater(reason: String, notifyDesktop: Boolean = false) {
        Thread({ finish(reason, notifyDesktop) }, "RelayControl-close").apply { isDaemon = true }.start()
    }

    private fun handlePayload(payload: ByteArray) {
        val current = synchronized(lock) { stage }
        when (current) {
            Stage.AwaitingWelcome -> {
                val decoded = try {
                    Json.parse(String(payload, Charsets.UTF_8))
                } catch (_: Exception) {
                    null
                }
                if (decoded is Map<*, *> && decoded["t"] == CONTROL_WELCOME_TAG) welcome.complete(decoded.asJsonMap())
            }
            Stage.Open -> {
                val plain = synchronized(lock) {
                    openControlFrame(keys.serverToClient, receiveCounter, payload)?.also { receiveCounter++ }
                }
                if (plain == null) {
                    finish("frame_did_not_authenticate", notifyDesktop = true)
                    return
                }
                val message = try {
                    Json.parse(String(plain, Charsets.UTF_8)) as? Map<*, *>
                } catch (_: Exception) {
                    null
                } ?: return
                val event = message["event"]
                if (event is String && !sessionEvent.isDone && (event == "session" || event == "pairing-denied")) {
                    resolveSession(ControlEvent(event, message["data"].asJsonMap()))
                    return
                }
                dispatcher.dispatch(message.asJsonMap())
            }
            else -> Unit
        }
    }

    private fun sendChunks(action: String, payload: ByteArray): Boolean {
        val encoded = B64.encode(payload)
        var at = 0
        do {
            val end = minOf(encoded.length, at + CHUNK_CHARS)
            val frame = synchronized(lock) {
                linkedMapOf<String, Any?>(
                    "type" to "control",
                    "action" to action,
                    "session" to session,
                    "node" to node,
                    "seq" to sendSeq++,
                    "data" to encoded.substring(at, end),
                    "more" to (end < encoded.length),
                )
            }
            if (!channel.send(frame)) return false
            at = end
        } while (at < encoded.length)
        return true
    }

    override fun request(method: String, params: Map<String, Any?>, timeoutMs: Long): Map<String, Any?> {
        if (!isOpen) throw LocalControlException("closed")
        val (id, future) = dispatcher.newRequest()
        val body = Json.stringify(linkedMapOf("id" to id, "method" to method, "params" to params)).toByteArray(Charsets.UTF_8)
        val sent = synchronized(lock) {
            val sealed = sealControlFrame(keys.clientToServer, sendCounter++, body)
            sendChunks("data", sealed)
        }
        if (!sent) {
            dispatcher.forget(id)
            finish("relay_unavailable", notifyDesktop = false)
            throw LocalControlException("relay_unavailable")
        }
        try {
            return awaitControl(future, timeoutMs)
        } finally {
            dispatcher.forget(id)
        }
    }

    fun confirmAlive(timeoutMs: Long = 8_000): Boolean {
        if (!isOpen) return false
        return try {
            request("ping", emptyMap(), timeoutMs)
            true
        } catch (answered: LocalControlException) {
            if (answered.code in setOf("timeout", "closed", "relay_unavailable") || !isOpen) {
                finish("desktop_unreachable", notifyDesktop = true)
                false
            } else {
                true
            }
        } catch (_: Exception) {
            finish("desktop_unreachable", notifyDesktop = true)
            false
        }
    }

    private fun startKeepalive(intervalMs: Long) {
        if (intervalMs <= 0) return
        val task = keepaliveScheduler.scheduleWithFixedDelay({
            if (isOpen && idleMs >= intervalMs) confirmAlive(minOf(intervalMs, 10_000))
        }, intervalMs, intervalMs, TimeUnit.MILLISECONDS)
        val closed = synchronized(lock) {
            if (stage == Stage.Closed) true else {
                keepalive = task
                false
            }
        }
        if (closed) task.cancel(false)
    }

    override fun addEventTap(tap: (ControlEvent) -> Unit): Any = dispatcher.addTap(tap)

    override fun removeEventTap(token: Any) = dispatcher.removeTap(token)

    fun close() {
        finish("closed", notifyDesktop = true)
        runCatching { channel.dispose() }
    }

    private fun finish(reason: String, notifyDesktop: Boolean) {
        val wasStarted: Boolean
        synchronized(lock) {
            if (stage == Stage.Closed) return
            wasStarted = stage != Stage.Connecting || sendSeq > 0
            stage = Stage.Closed
            closeReason = reason
            keepalive?.cancel(false)
            keepalive = null
        }
        if (notifyDesktop && wasStarted) {
            runCatching {
                channel.send(
                    linkedMapOf(
                        "type" to "control",
                        "action" to "close",
                        "session" to session,
                        "node" to node,
                        "reason" to reason.take(160),
                    ),
                )
            }
        }
        val error = LocalControlException(reason)
        socketUp.completeExceptionally(error)
        welcome.completeExceptionally(error)
        sessionEvent.completeExceptionally(error)
        dispatcher.failAll("closed")
        dispatcher.deliver(ControlEvent("__closed", mapOf("reason" to reason)))
        for (listener in closeListeners) runCatching { listener(reason) }
        closeListeners.clear()
    }
}
