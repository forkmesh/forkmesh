package com.forkmesh.nativeapp.agentwatch

import com.forkmesh.nativeapp.control.AgentControlCenter
import com.forkmesh.nativeapp.control.AgentTranscriptDelta
import com.forkmesh.nativeapp.control.TranscriptFollower
import com.forkmesh.nativeapp.control.UnifiedAgentRow

interface WatchTranscriptStream {
    val isStreaming: Boolean

    fun start(): Boolean

    fun stop()
}

data class ControlWatchText(val text: String, val status: String)

class ControlWatchFeeds(
    private val limit: Int = DEFAULT_LIMIT,
    private val starter: (Runnable) -> Unit = { Thread(it, "WatchFollow").apply { isDaemon = true }.start() },
    private val open: (UnifiedAgentRow, (String) -> Unit, (AgentTranscriptDelta) -> Unit) -> WatchTranscriptStream?,
) {
    companion object {
        const val DEFAULT_LIMIT = 6

        fun forControl(control: AgentControlCenter, limit: Int = DEFAULT_LIMIT): ControlWatchFeeds =
            ControlWatchFeeds(limit) { row, onEnded, onDelta ->
                control.stream(row, 0, onEnded, onDelta)?.let { stream ->
                    object : WatchTranscriptStream {
                        override val isStreaming: Boolean get() = stream.isStreaming

                        override fun start(): Boolean = stream.start()

                        override fun stop() = stream.stop()
                    }
                }
            }
    }

    private class Feed {
        val follower = TranscriptFollower()
        var status = ""
        var streamed = false
        var ended = false
        var revision = 0
        var stream: WatchTranscriptStream? = null
    }

    private val lock = Any()
    private val feeds = HashMap<String, Feed>()
    private var opened = 0
    private var stopped = false

    @Volatile
    var onChange: (() -> Unit)? = null

    fun follow(rows: List<UnifiedAgentRow>) {
        val starting = ArrayList<WatchTranscriptStream>()
        synchronized(lock) {
            if (stopped) return
            for (row in rows) {
                if (!row.isDesktop || row.localId == 0 || feeds.containsKey(row.id)) continue
                val id = row.id
                val feed = Feed()
                feeds[id] = feed
                if (opened < limit) {
                    feed.stream = open(row, { reason -> ended(id, reason) }, { delta -> apply(id, delta) })
                    feed.stream?.let {
                        opened++
                        starting += it
                    }
                }
            }
        }
        for (stream in starting) starter(Runnable { runCatching { stream.start() } })
    }

    fun text(id: String): ControlWatchText? = synchronized(lock) {
        val feed = feeds[id] ?: return null
        if (!feed.streamed && !feed.ended) return null
        ControlWatchText(feed.follower.text, if (feed.ended) "done" else feed.status)
    }

    fun revision(id: String): Int = synchronized(lock) { feeds[id]?.revision ?: -1 }

    fun streaming(id: String): Boolean {
        val feed = synchronized(lock) { feeds[id] } ?: return false
        return feed.ended || feed.stream?.isStreaming == true
    }

    val lagging: Boolean
        get() {
            val all = synchronized(lock) { feeds.values.toList() }
            return all.any { !it.ended && it.stream?.isStreaming != true }
        }

    fun stop() {
        val streams = synchronized(lock) {
            if (stopped) return
            stopped = true
            val held = feeds.values.mapNotNull { it.stream }
            feeds.clear()
            held
        }
        onChange = null
        for (stream in streams) runCatching { stream.stop() }
    }

    private fun apply(id: String, delta: AgentTranscriptDelta) {
        synchronized(lock) {
            if (stopped) return
            val feed = feeds[id] ?: return
            feed.follower.apply(delta)
            if (delta.status.isNotEmpty()) feed.status = delta.status
            feed.streamed = true
            feed.revision++
        }
        onChange?.invoke()
    }

    private fun ended(id: String, reason: String) {
        synchronized(lock) {
            if (stopped) return
            val feed = feeds[id] ?: return
            if (reason == "unknown_session") feed.ended = true
            feed.revision++
        }
        onChange?.invoke()
    }
}

class ControlWatchRoster(
    private val control: AgentControlCenter,
    val feeds: ControlWatchFeeds = ControlWatchFeeds.forControl(control),
    private val limit: Int = 24,
    private val rows: (AgentControlCenter) -> List<UnifiedAgentRow>,
) {
    private var listener: (() -> Unit)? = null

    fun targets(): List<AgentWatchTarget> {
        val chosen = rows(control).take(limit)
        feeds.follow(chosen)
        return chosen.map { controlWatchTarget(control, it, feeds) }
    }

    fun watch(onTargets: (List<AgentWatchTarget>) -> Unit) {
        if (listener != null) return
        val made: () -> Unit = { onTargets(targets()) }
        listener = made
        control.addListener(made)
    }

    fun refresh() = control.refreshAsync()

    fun stop() {
        listener?.let { control.removeListener(it) }
        listener = null
        feeds.stop()
    }
}
