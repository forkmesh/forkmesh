package com.forkmesh.nativeapp.api

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.net.WebSocketClient
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.ScheduledThreadPoolExecutor
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger

class AccountEventLoop(
    private val api: ApiService,
    private val sessionToken: () -> String,
    private val connector: Connector = Connector.WEB_SOCKET,
    private val keepaliveMs: Long = 4 * 60_000L,
    private val initialReconnectDelayMs: Long = INITIAL_RECONNECT_DELAY_MS,
    private val maxReconnectDelayMs: Long = MAX_RECONNECT_DELAY_MS,
    private val nodeName: String = "",
    private val backoff: com.forkmesh.nativeapp.push.ReconnectBackoff? = null,
) {
    interface Socket {
        fun send(text: String): Boolean

        fun close()
    }

    interface Events {
        fun onText(text: String)

        fun onBinary(data: ByteArray)

        fun onClosed()
    }

    fun interface Connector {
        fun connect(url: String, events: Events): Socket

        companion object {
            val WEB_SOCKET = Connector { url, events ->
                val client = WebSocketClient.connect(
                    url,
                    object : WebSocketClient.Listener {
                        override fun onText(socket: WebSocketClient, text: String) = events.onText(text)

                        override fun onBinary(socket: WebSocketClient, data: ByteArray) = events.onBinary(data)

                        override fun onClosed(socket: WebSocketClient, code: Int, reason: String, error: Throwable?) =
                            events.onClosed()
                    },
                )
                object : Socket {
                    override fun send(text: String): Boolean = client.sendText(text)

                    override fun close() = client.close(WebSocketClient.NORMAL_CLOSURE)
                }
            }
        }
    }

    companion object {
        const val INITIAL_RECONNECT_DELAY_MS = 2_000L
        const val MAX_RECONNECT_DELAY_MS = 30_000L

        private val WORKERS = Executors.newCachedThreadPool { runnable ->
            Thread(runnable, "AccountEventLoop-io").apply { isDaemon = true }
        }
    }

    @Volatile
    var onTopic: ((String) -> Unit)? = null

    @Volatile
    var onEvent: ((String, Map<String, Any?>) -> Unit)? = null

    @Volatile
    var onConnected: (() -> Unit)? = null

    @Volatile
    var onFrame: ((Map<String, Any?>) -> Unit)? = null

    @Volatile
    var onSignedOut: (() -> Unit)? = null

    @Volatile
    var onStateChanged: (() -> Unit)? = null

    private val loop = ScheduledThreadPoolExecutor(1) { runnable ->
        Thread(runnable, "AccountEventLoop").apply { isDaemon = true }
    }.apply { removeOnCancelPolicy = true }

    @Volatile
    private var loopThread: Thread? = null
    private val pending = AtomicInteger()

    @Volatile
    private var socket: Socket? = null
    private var reconnectTimer: ScheduledFuture<*>? = null
    private var keepaliveTimer: ScheduledFuture<*>? = null
    private var reconnectDelayMs = initialReconnectDelayMs
    private var generation = 0
    private var paused = false
    private val earlyFrames = ArrayList<Pair<Int, String>>()

    @Volatile
    private var isStarted = false

    @Volatile
    private var isDisposed = false

    @Volatile
    private var isConnected = false

    val connected: Boolean get() = isConnected

    val started: Boolean get() = isStarted

    fun start() = runOnLoop { startNow() }

    fun stop() = runOnLoop { stopNow() }

    fun restart() = runOnLoop {
        if (isDisposed) return@runOnLoop
        val wasStarted = isStarted
        stopNow()
        if (wasStarted || sessionToken().isNotEmpty()) startNow()
    }

    fun dispose() {
        runOnLoop {
            isDisposed = true
            stopNow()
        }
        loop.shutdown()
    }

    fun setForeground(foreground: Boolean) = post {
        if (!foreground) {
            if (paused) return@post
            paused = true
            generation++
            cancelReconnect()
            teardownSocket()
            return@post
        }
        paused = false
        if (isDisposed || !isStarted) return@post
        cancelReconnect()
        reconnectDelayMs = initialReconnectDelayMs
        backoff?.reset()
        if (socket == null) {
            open()
        } else {
            sendKeepalive()
        }
    }

    fun awaitIdle(timeoutMs: Long = 5_000): Boolean {
        val deadline = System.currentTimeMillis() + timeoutMs
        while (System.currentTimeMillis() < deadline) {
            if (pending.get() == 0) return true
            Thread.sleep(2)
        }
        return pending.get() == 0
    }

    private fun post(block: () -> Unit) {
        pending.incrementAndGet()
        try {
            loop.execute {
                loopThread = Thread.currentThread()
                try {
                    block()
                } finally {
                    pending.decrementAndGet()
                }
            }
        } catch (rejected: java.util.concurrent.RejectedExecutionException) {
            pending.decrementAndGet()
        }
    }

    private fun runOnLoop(block: () -> Unit) {
        if (Thread.currentThread() === loopThread) {
            block()
            return
        }
        pending.incrementAndGet()
        val future = try {
            loop.submit {
                loopThread = Thread.currentThread()
                try {
                    block()
                } finally {
                    pending.decrementAndGet()
                }
            }
        } catch (rejected: java.util.concurrent.RejectedExecutionException) {
            pending.decrementAndGet()
            return
        }
        try {
            future.get()
        } catch (wrapped: java.util.concurrent.ExecutionException) {
            throw wrapped.cause ?: wrapped
        }
    }

    private fun onWorker(block: () -> Unit) {
        pending.incrementAndGet()
        WORKERS.execute {
            try {
                block()
            } finally {
                pending.decrementAndGet()
            }
        }
    }

    private fun stateChanged() {
        isConnected = socket != null
        onStateChanged?.invoke()
    }

    private fun startNow() {
        if (isDisposed || isStarted) return
        isStarted = true
        reconnectDelayMs = initialReconnectDelayMs
        backoff?.reset()
        stateChanged()
        open()
    }

    private fun stopNow() {
        isStarted = false
        generation++
        cancelReconnect()
        teardownSocket()
        stateChanged()
    }

    private fun open() {
        if (isDisposed || !isStarted || paused || socket != null) return
        if (sessionToken().isEmpty()) {
            isStarted = false
            stateChanged()
            return
        }
        val attempt = ++generation
        onWorker {
            val ticket = api.accountEventTicket()
            post { onTicket(attempt, ticket) }
        }
    }

    private fun live(attempt: Int): Boolean = attempt == generation && !isDisposed && isStarted && !paused

    private fun onTicket(attempt: Int, ticket: EventTicket) {
        if (!live(attempt)) return
        when (ticket.status) {
            EventTicketStatus.SIGNED_OUT -> {
                isStarted = false
                stateChanged()
                onSignedOut?.invoke()
                return
            }
            EventTicketStatus.UNAVAILABLE -> {
                scheduleReconnect()
                return
            }
            EventTicketStatus.OK -> Unit
        }
        val url = api.accountEventsUri(ticket.ticket, nodeName)
        onWorker {
            val opened: Socket? = try {
                connector.connect(url, eventsFor(attempt))
            } catch (_: Exception) {
                null
            }
            post { onSocket(attempt, opened) }
        }
    }

    private fun eventsFor(attempt: Int): Events = object : Events {
        override fun onText(text: String) {
            post { onFrameFor(attempt, text) }
        }

        override fun onBinary(data: ByteArray) {
            post { onFrameFor(attempt, String(data, Charsets.UTF_8)) }
        }

        override fun onClosed() {
            post { if (attempt == generation) this@AccountEventLoop.onSocketClosed() }
        }
    }

    private fun onFrameFor(attempt: Int, text: String) {
        if (attempt != generation) return
        if (socket == null) {
            if (live(attempt)) earlyFrames.add(attempt to text)
            return
        }
        onFrame(text)
    }

    private fun onSocket(attempt: Int, opened: Socket?) {
        if (opened == null) {
            earlyFrames.clear()
            scheduleReconnect()
            return
        }
        if (!live(attempt)) {
            earlyFrames.clear()
            onWorker { runCatching { opened.close() } }
            return
        }
        socket = opened
        reconnectDelayMs = initialReconnectDelayMs
        backoff?.reset()
        keepaliveTimer?.cancel(false)
        keepaliveTimer = loop.scheduleAtFixedRate({ sendKeepalive() }, keepaliveMs, keepaliveMs, TimeUnit.MILLISECONDS)
        stateChanged()
        onConnected?.invoke()
        val buffered = earlyFrames.filter { it.first == attempt }.map { it.second }
        earlyFrames.clear()
        for (frame in buffered) onFrame(frame)
    }

    private fun onFrame(raw: String) {
        val frame = try {
            Json.parse(raw) as? Map<*, *>
        } catch (_: Exception) {
            return
        }
        if (frame == null) return
        if (frame["type"] != "event") {
            @Suppress("UNCHECKED_CAST")
            onFrame?.invoke(frame as Map<String, Any?>)
            return
        }
        val topic = jsonText(frame["topic"])
        if (topic.isEmpty()) return
        @Suppress("UNCHECKED_CAST")
        onEvent?.invoke(topic, frame as Map<String, Any?>)
        onTopic?.invoke(topic)
    }

    fun send(frame: Map<String, Any?>): Boolean {
        val current = socket ?: return false
        val ok = try {
            current.send(Json.stringify(frame))
        } catch (_: Exception) {
            false
        }
        if (!ok) post { onSocketClosed() }
        return ok
    }

    private fun sendKeepalive() {
        val current = socket ?: return
        val ok = try {
            current.send("""{"type":"ping"}""")
        } catch (_: Exception) {
            false
        }
        if (!ok) onSocketClosed()
    }

    private fun onSocketClosed() {
        if (socket == null) return
        teardownSocket()
        stateChanged()
        if (isDisposed || !isStarted) return
        scheduleReconnect()
    }

    private fun teardownSocket() {
        keepaliveTimer?.cancel(false)
        keepaliveTimer = null
        val current = socket
        socket = null
        earlyFrames.clear()
        isConnected = false
        if (current != null) onWorker { runCatching { current.close() } }
    }

    private fun cancelReconnect() {
        reconnectTimer?.cancel(false)
        reconnectTimer = null
    }

    private fun scheduleReconnect() {
        if (isDisposed || !isStarted || paused) return
        if (reconnectTimer?.isDone == false) return
        val delay = backoff?.nextDelayMs() ?: reconnectDelayMs
        reconnectDelayMs = minOf(reconnectDelayMs * 2, maxReconnectDelayMs)
        reconnectTimer = loop.schedule({
            reconnectTimer = null
            if (!isDisposed && isStarted) open()
        }, delay, TimeUnit.MILLISECONDS)
    }
}
