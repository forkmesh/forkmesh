package com.forkmesh.nativeapp.hosts

import com.forkmesh.nativeapp.control.ControlErrorText
import com.forkmesh.nativeapp.control.ControlEvent
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings
import java.util.concurrent.CopyOnWriteArrayList

data class HostLogLine(val id: Int, val atMs: Long, val source: String, val text: String) {
    val copyText: String get() = if (source.isEmpty()) text else "[$source] $text"
}

class HostLogBuffer(limit: Int = 5000) {
    val limit: Int = maxOf(100, limit)
    private val shown = ArrayDeque<HostLogLine>()
    private val waiting = ArrayDeque<HostLogLine>()
    private var nextId = 0

    val lines: List<HostLogLine> get() = shown.toList()
    val held: List<HostLogLine> get() = waiting.toList()
    val size: Int get() = shown.size
    val heldCount: Int get() = waiting.size

    var paused: Boolean = false
        private set

    var dropped: Int = 0
        private set

    fun appendBacklog(raw: List<String>) {
        for (line in raw) {
            var source = ""
            var text = line
            val close = line.indexOf(']')
            if (line.startsWith("[") && close > 0) {
                source = line.substring(1, close)
                text = line.substring(close + 1).trim()
            }
            add(HostLogLine(takeId(), 0, source, text))
        }
    }

    fun append(batch: Map<String, Any?>) {
        dropped += maxOf(0, batch.int("dropped"))
        for (line in batch.maps("lines")) {
            add(HostLogLine(takeId(), line.int("t").toLong(), line.str("src"), line.str("text")))
        }
    }

    fun setPaused(value: Boolean) {
        if (paused == value) return
        paused = value
        if (!paused) {
            shown.addAll(waiting)
            waiting.clear()
            trim()
        }
    }

    fun clear() {
        shown.clear()
        waiting.clear()
    }

    fun filtered(query: String): List<HostLogLine> {
        val needle = query.trim()
        if (needle.isEmpty()) return lines
        return shown.filter { it.text.contains(needle, ignoreCase = true) || it.source.contains(needle, ignoreCase = true) }
    }

    fun copyText(query: String = ""): String = filtered(query).joinToString("\n") { it.copyText }

    private fun takeId(): Int = ++nextId

    private fun add(line: HostLogLine) {
        if (paused) {
            waiting.addLast(line)
            while (waiting.size > limit) {
                waiting.removeFirst()
                dropped++
            }
        } else {
            shown.addLast(line)
            trim()
        }
    }

    private fun trim() {
        while (shown.size > limit) {
            shown.removeFirst()
            dropped++
        }
    }
}

class HostLogStream(
    val channel: HostsChannel,
    val host: String,
    private val executor: HostsExecutor = HostsExecutor.Threads,
    limit: Int = 5000,
) {
    private val lock = Object()
    private val listeners = CopyOnWriteArrayList<() -> Unit>()
    private val buffer = HostLogBuffer(limit)
    private var tap: Any? = null
    private var stream = ""
    private val reachListener: () -> Unit = { onReachChanged() }

    @Volatile
    var live: Boolean = false
        private set

    @Volatile
    var error: String = ""
        private set

    @Volatile
    var revision: Int = 0
        private set

    fun <T> read(block: (HostLogBuffer) -> T): T = synchronized(lock) { block(buffer) }

    fun addListener(listener: () -> Unit) = listeners.add(listener)

    fun removeListener(listener: () -> Unit) = listeners.remove(listener)

    private fun changed() {
        revision++
        for (listener in listeners) runCatching { listener() }
    }

    fun start() {
        if (live) return
        synchronized(lock) {
            if (tap == null) {
                tap = channel.addEventTap(::onEvent)
                channel.addReachListener(reachListener)
            }
        }
        try {
            val result = channel.request("hosts.logs.start", mapOf("host" to host), 20_000)
            synchronized(lock) {
                stream = result.str("stream")
                buffer.appendBacklog(result.strings("backlog"))
                live = stream.isNotEmpty()
            }
            error = ""
        } catch (failure: Exception) {
            error = ControlErrorText.describe(failure)
        }
        changed()
    }

    fun stop() {
        val id: String
        synchronized(lock) {
            tap?.let { channel.removeEventTap(it) }
            if (tap != null) channel.removeReachListener(reachListener)
            tap = null
            id = stream
            stream = ""
            live = false
        }
        if (id.isNotEmpty()) runCatching { channel.request("hosts.logs.stop", mapOf("stream" to id), 10_000) }
        changed()
    }

    fun setPaused(paused: Boolean) {
        synchronized(lock) { buffer.setPaused(paused) }
        changed()
    }

    fun clear() {
        synchronized(lock) { buffer.clear() }
        changed()
    }

    private fun onEvent(event: ControlEvent) {
        when (event.event) {
            "hosts.log" -> onBatch(event.data)
            "__closed" -> onClosed()
        }
    }

    private fun onBatch(data: Map<String, Any?>) {
        val id: String
        synchronized(lock) {
            if (stream.isEmpty() || data.str("stream") != stream) return
            buffer.append(data)
            id = stream
        }
        changed()
        val seq = data.int("seq")
        executor.run { runCatching { channel.request("hosts.logs.ack", mapOf("stream" to id, "seq" to seq), 10_000) } }
    }

    private fun onReachChanged() {
        if (live && !channel.reach.isConnected) onClosed()
    }

    fun onClosed() {
        synchronized(lock) {
            if (!live && stream.isEmpty()) return
            live = false
            stream = ""
        }
        error = "The desktop connection closed. Reopen the log to continue."
        changed()
    }
}
