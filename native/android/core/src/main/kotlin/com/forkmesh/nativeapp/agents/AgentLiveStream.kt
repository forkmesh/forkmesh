package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.net.HttpClient
import java.io.IOException
import java.io.InputStream
import java.net.HttpURLConnection
import java.net.URL
import java.nio.ByteBuffer
import java.nio.CharBuffer
import java.nio.charset.CodingErrorAction
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

sealed class AgentLiveFrame {
    data class Text(val atMs: Long, val text: String) : AgentLiveFrame()

    data class Append(val atMs: Long, val text: String) : AgentLiveFrame()

    data class End(val status: String) : AgentLiveFrame()
}

class AgentLiveParser {
    private val decoder = Charsets.UTF_8.newDecoder()
        .onMalformedInput(CodingErrorAction.REPLACE)
        .onUnmappableCharacter(CodingErrorAction.REPLACE)
    private var pending: ByteArray = ByteArray(0)
    private val line = StringBuilder()
    private val data = StringBuilder()
    private var event = ""
    private var hasData = false
    private var afterCr = false
    private var started = false

    fun feed(bytes: ByteArray, offset: Int = 0, length: Int = bytes.size - offset): List<AgentLiveFrame> {
        val input = ByteBuffer.allocate(pending.size + length)
        input.put(pending)
        input.put(bytes, offset, length)
        input.flip()
        val chars = CharBuffer.allocate(input.remaining() + 1)
        decoder.decode(input, chars, false)
        pending = ByteArray(input.remaining()).also { input.get(it) }
        chars.flip()
        return feed(chars)
    }

    fun feed(text: CharSequence): List<AgentLiveFrame> {
        val out = ArrayList<AgentLiveFrame>()
        for (ch in text) {
            if (!started) {
                started = true
                if (ch == '﻿') continue
            }
            if (afterCr) {
                afterCr = false
                if (ch == '\n') continue
            }
            when (ch) {
                '\r' -> {
                    afterCr = true
                    endLine(out)
                }
                '\n' -> endLine(out)
                else -> line.append(ch)
            }
        }
        return out
    }

    private fun endLine(out: MutableList<AgentLiveFrame>) {
        val text = line.toString()
        line.setLength(0)
        if (text.isEmpty()) {
            dispatch(out)
            return
        }
        if (text[0] == ':') return
        val colon = text.indexOf(':')
        val field = if (colon < 0) text else text.substring(0, colon)
        val value = if (colon < 0) "" else text.substring(colon + 1).removePrefix(" ")
        when (field) {
            "event" -> event = value
            "data" -> {
                if (hasData) data.append('\n')
                data.append(value)
                hasData = true
            }
        }
    }

    private fun dispatch(out: MutableList<AgentLiveFrame>) {
        val name = event
        val payload = data.toString()
        val had = hasData
        event = ""
        data.setLength(0)
        hasData = false
        if (had) frame(name, payload)?.let(out::add)
    }

    companion object {
        fun frame(event: String, data: String): AgentLiveFrame? {
            val json = try {
                Json.parse(data) as? Map<*, *>
            } catch (_: Exception) {
                null
            } ?: return null
            return when (event) {
                "live" -> {
                    val at = launchMs(json["at"])
                    val text = json["text"]
                    val append = json["append"]
                    when {
                        text is String -> AgentLiveFrame.Text(at, text)
                        append is String -> AgentLiveFrame.Append(at, append)
                        else -> null
                    }
                }
                "end" -> AgentLiveFrame.End(jsonText(json["status"]))
                else -> null
            }
        }
    }
}

class AgentLiveText {
    var text: String = ""
        private set
    var atMs: Long = 0
        private set
    var hasText: Boolean = false
        private set

    fun apply(frame: AgentLiveFrame): Boolean {
        val at = when (frame) {
            is AgentLiveFrame.Text -> {
                text = frame.text
                hasText = true
                frame.atMs
            }
            is AgentLiveFrame.Append -> {
                if (!hasText || frame.text.isEmpty()) return false
                text += frame.text
                frame.atMs
            }
            is AgentLiveFrame.End -> return false
        }
        if (at > 0) atMs = at
        return true
    }
}

fun AgentLaunchSession.withLive(text: String, atMs: Long): AgentLaunchSession {
    if (!isActive) return this
    if (atMs > 0 && liveAtMs > atMs) return this
    return copy(liveText = text, liveAtMs = if (atMs > 0) atMs else liveAtMs)
}

sealed class AgentLiveEvent {
    data class Text(val text: String, val atMs: Long) : AgentLiveEvent()

    data class Ended(val status: String) : AgentLiveEvent()

    data class Refused(val status: Int) : AgentLiveEvent()

    data class Dropped(val reason: String) : AgentLiveEvent()
}

interface AgentLiveConnection {
    fun status(): Int

    fun contentType(): String

    fun body(): InputStream

    fun close()
}

fun interface AgentLiveConnector {
    fun connect(request: HttpClient.Request): AgentLiveConnection

    companion object {
        val DEFAULT = AgentLiveConnector { request -> UrlLiveConnection(request) }
    }
}

private class UrlLiveConnection(request: HttpClient.Request) : AgentLiveConnection {
    private val connection = (URL(request.url).openConnection() as HttpURLConnection).apply {
        requestMethod = request.method
        connectTimeout = request.connectTimeoutMs
        readTimeout = request.readTimeoutMs
        useCaches = false
        instanceFollowRedirects = true
        for ((name, value) in request.headers) setRequestProperty(name, value)
    }

    override fun status(): Int = connection.responseCode

    override fun contentType(): String = connection.contentType ?: ""

    override fun body(): InputStream = connection.inputStream

    override fun close() = connection.disconnect()
}

class AgentLiveStream(
    private val request: () -> HttpClient.Request,
    private val connector: AgentLiveConnector = AgentLiveConnector.DEFAULT,
    private val onEvent: (AgentLiveEvent) -> Unit,
) {
    companion object {
        const val RECONNECT_MS = 1_000L
        const val FIRST_RETRY_MS = 2_000L
        const val MAX_RETRY_MS = 30_000L
        const val READ_TIMEOUT_MS = 45_000
        const val CONTENT_TYPE = "text/event-stream"
    }

    private sealed class Outcome {
        data class Ended(val status: String) : Outcome()

        data class Refused(val status: Int) : Outcome()

        data class Closed(val delivered: Boolean) : Outcome()

        data class Failed(val reason: String, val delivered: Boolean) : Outcome()
    }

    private val lock = Any()
    private val halt = CountDownLatch(1)
    private var thread: Thread? = null

    @Volatile
    private var stopped = false

    @Volatile
    private var connection: AgentLiveConnection? = null

    @Volatile
    var isLive: Boolean = false
        private set

    fun start() {
        synchronized(lock) {
            if (stopped || thread != null) return
            thread = Thread({ follow() }, "AgentLiveStream").apply {
                isDaemon = true
                start()
            }
        }
    }

    fun stop() {
        stopped = true
        isLive = false
        halt.countDown()
        connection?.let { runCatching { it.close() } }
    }

    fun follow() {
        var delay = FIRST_RETRY_MS
        while (!stopped) {
            val outcome = once()
            if (stopped) return
            when (outcome) {
                is Outcome.Ended -> {
                    isLive = false
                    emit(AgentLiveEvent.Ended(outcome.status))
                    return
                }
                is Outcome.Refused -> {
                    isLive = false
                    emit(AgentLiveEvent.Refused(outcome.status))
                    return
                }
                is Outcome.Closed -> {
                    if (outcome.delivered) {
                        delay = FIRST_RETRY_MS
                        halt.await(RECONNECT_MS, TimeUnit.MILLISECONDS)
                        continue
                    }
                    delay = drop("closed", delay)
                }
                is Outcome.Failed -> {
                    if (outcome.delivered) delay = FIRST_RETRY_MS
                    delay = drop(outcome.reason, delay)
                }
            }
        }
    }

    private fun drop(reason: String, delay: Long): Long {
        isLive = false
        emit(AgentLiveEvent.Dropped(reason))
        halt.await(delay, TimeUnit.MILLISECONDS)
        return minOf(delay * 2, MAX_RETRY_MS)
    }

    private fun emit(event: AgentLiveEvent) {
        if (!stopped) onEvent(event)
    }

    private fun once(): Outcome {
        val started = System.currentTimeMillis()
        val sent = request()
        var status = 0
        var bytesIn = 0L
        var failure: Throwable? = null
        var delivered = false
        try {
            val opened = connector.connect(sent)
            connection = opened
            if (stopped) return Outcome.Closed(false)
            status = opened.status()
            if (status >= 500 || status == 408 || status == 429) return Outcome.Failed("HTTP $status", false)
            if (status !in 200..299) return Outcome.Refused(status)
            if (!opened.contentType().lowercase().startsWith(CONTENT_TYPE)) return Outcome.Refused(status)
            val parser = AgentLiveParser()
            val live = AgentLiveText()
            val buffer = ByteArray(16 * 1024)
            opened.body().use { input ->
                while (!stopped) {
                    val read = input.read(buffer)
                    if (read < 0) break
                    bytesIn += read
                    for (frame in parser.feed(buffer, 0, read)) {
                        if (frame is AgentLiveFrame.End) return Outcome.Ended(frame.status)
                        if (live.apply(frame) && live.hasText) {
                            delivered = true
                            isLive = true
                            emit(AgentLiveEvent.Text(live.text, live.atMs))
                        }
                    }
                }
            }
            return Outcome.Closed(delivered)
        } catch (error: IOException) {
            failure = error
            return Outcome.Failed(error.message ?: error.toString(), delivered)
        } catch (error: RuntimeException) {
            failure = error
            return Outcome.Failed(error.message ?: error.toString(), delivered)
        } finally {
            connection?.let { runCatching { it.close() } }
            connection = null
            HttpClient.observer?.onExchange(
                sent.method, sent.url, status, 0, bytesIn, System.currentTimeMillis() - started, failure,
            )
        }
    }
}
