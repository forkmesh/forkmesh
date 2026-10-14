package com.forkmesh.nativeapp.control

import com.forkmesh.nativeapp.json.jsonText
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit

class DesktopTranscriptStream(
    private val hub: DesktopControlHub,
    val sessionId: Int,
    since: Int = 0,
    private val onEnded: (String) -> Unit = {},
    private val onDelta: (AgentTranscriptDelta) -> Unit,
) {
    private val lock = Any()
    private val worker = Executors.newSingleThreadScheduledExecutor { Thread(it, "TranscriptStream").apply { isDaemon = true } }

    @Volatile
    var next: Int = since
        private set

    @Volatile
    var isStreaming: Boolean = false
        private set

    private var tap: Any? = null
    private var linkToken: Any? = null
    private var generation = 0
    private var stopped = false
    private var awaiting = false
    private val early = ArrayList<Map<String, Any?>>()
    private var followedLink: ControlLink? = null
    private var retry: ScheduledFuture<*>? = null
    private var retryDelayMs = 1_000L

    fun start(): Boolean {
        synchronized(lock) {
            stopped = false
            if (tap == null) tap = hub.addEventTap { event -> onEvent(event) }
            if (linkToken == null) linkToken = hub.addLinkListener { linkChanged() }
        }
        return follow()
    }

    fun refollow(): Boolean {
        synchronized(lock) {
            if (stopped) return false
            retry?.cancel(false)
            retry = null
        }
        return follow()
    }

    fun stop() {
        val link: ControlLink?
        synchronized(lock) {
            if (stopped) return
            stopped = true
            generation++
            isStreaming = false
            retry?.cancel(false)
            retry = null
            tap?.let { hub.removeEventTap(it) }
            linkToken?.let { hub.removeLinkListener(it) }
            tap = null
            linkToken = null
            link = hub.currentLink
        }
        if (link != null) {
            worker.execute { runCatching { link.request("agents.unfollow", mapOf("id" to sessionId), 5_000) } }
        }
        worker.shutdown()
    }

    private fun follow(): Boolean {
        val attempt = synchronized(lock) {
            if (stopped) return false
            generation++
            awaiting = true
            early.clear()
            generation
        }
        try {
            val link = hub.link()
            synchronized(lock) { followedLink = link }
            val answer = link.request("agents.follow", mapOf("id" to sessionId, "since" to next), 20_000)
            val queued = synchronized(lock) {
                if (stopped || attempt != generation) return isStreaming
                retryDelayMs = 1_000
                isStreaming = answer["streaming"] == true
                apply(answer)
                val held = ArrayList(early)
                early.clear()
                awaiting = false
                held
            }
            for (data in queued) onOutput(data)
        } catch (refusal: LocalControlException) {
            val current = synchronized(lock) { attempt == generation && !stopped }
            if (!current) return isStreaming
            isStreaming = false
            when (refusal.code) {
                "unknown_session" -> end(refusal.code)
                "unsupported", "unknown_method" -> {}
                else -> scheduleRetry()
            }
        } catch (_: Throwable) {
            val current = synchronized(lock) { attempt == generation && !stopped }
            if (!current) return isStreaming
            isStreaming = false
            scheduleRetry()
        } finally {
            synchronized(lock) {
                if (attempt == generation) {
                    awaiting = false
                    early.clear()
                }
            }
        }
        return isStreaming
    }

    private fun onEvent(event: ControlEvent) {
        if (event.event != "agents.output" || (event.data["id"] as? Number)?.toInt() != sessionId) return
        synchronized(lock) {
            if (stopped) return
            if (awaiting) {
                early.add(event.data)
                return
            }
        }
        onOutput(event.data)
    }

    private fun onOutput(data: Map<String, Any?>) {
        val error = jsonText(data["error"])
        if (error.isNotEmpty()) {
            end(error)
            return
        }
        synchronized(lock) {
            if (stopped || !isStreaming) return
            apply(data)
        }
    }

    private fun apply(data: Map<String, Any?>) {
        var delta = AgentTranscriptDelta.fromDesktop(data, next, sessionId)
        if (!delta.reset && delta.from < next) return
        if (!delta.reset && delta.from > next) delta = delta.copy(reset = true)
        next = delta.next
        onDelta(delta)
    }

    private fun end(reason: String) {
        isStreaming = false
        onEnded(reason)
        stop()
    }

    private fun linkChanged() {
        val refollow = synchronized(lock) {
            if (stopped || awaiting) return
            val current = hub.currentLink
            if (current != null && current === followedLink) return
            isStreaming = false
            current != null
        }
        if (refollow) worker.execute { follow() } else scheduleRetry()
    }

    private fun scheduleRetry() {
        synchronized(lock) {
            if (stopped || retry != null) return
            val delay = retryDelayMs
            retryDelayMs = minOf(retryDelayMs * 2, 15_000)
            retry = worker.schedule({
                synchronized(lock) { retry = null }
                follow()
            }, delay, TimeUnit.MILLISECONDS)
        }
    }
}
