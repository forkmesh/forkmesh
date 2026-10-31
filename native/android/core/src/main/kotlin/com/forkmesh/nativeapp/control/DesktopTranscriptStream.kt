package com.forkmesh.nativeapp.control

import com.forkmesh.nativeapp.json.jsonText
import java.lang.ref.WeakReference
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit

class DesktopTranscriptStream(
    private val hub: DesktopControlHub,
    val sessionId: Int,
    since: Int = 0,
    private val onEnded: (String) -> Unit = {},
    val desktopKey: String = "",
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
    private var filling = false
    private val held = ArrayList<Map<String, Any?>>()
    private var followedLink: ControlLink? = null
    private var retry: ScheduledFuture<*>? = null
    private var retryDelayMs = 1_000L

    fun start(): Boolean {
        hold(this)
        synchronized(lock) {
            stopped = false
            if (tap == null) tap = hub.addEventTap(desktopKey) { event -> onEvent(event) }
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
            filling = false
            held.clear()
            retry?.cancel(false)
            retry = null
            tap?.let { hub.removeEventTap(it) }
            linkToken?.let { hub.removeLinkListener(it) }
            tap = null
            linkToken = null
            link = hub.openLink(desktopKey)
        }
        if (!release(this) && link != null) {
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
            filling = false
            held.clear()
            generation
        }
        try {
            val link = hub.link(desktopKey)
            synchronized(lock) { followedLink = link }
            val answer = link.request("agents.follow", mapOf("id" to sessionId, "since" to next), 20_000)
            val queued = synchronized(lock) {
                if (stopped || attempt != generation) return isStreaming
                retryDelayMs = 1_000
                isStreaming = answer["streaming"] == true
                apply(answer)
                val replay = ArrayList(early)
                early.clear()
                awaiting = false
                replay
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

    private fun apply(data: Map<String, Any?>, fills: Boolean = true) {
        if (filling) {
            held.add(data)
            return
        }
        var delta = AgentTranscriptDelta.fromDesktop(data, next, sessionId)
        if (!delta.reset && delta.from > next) {
            if (fills) {
                held.add(data)
                fill()
                return
            }
            delta = delta.copy(reset = true)
        }
        if (!delta.reset && delta.from < next) delta = unseen(delta, next) ?: return
        next = delta.next
        onDelta(delta)
    }

    private fun fill() {
        filling = true
        val attempt = generation
        val since = next
        runCatching { worker.execute { fill(since, attempt) } }.onFailure { filling = false }
    }

    private fun fill(since: Int, attempt: Int) {
        val params: Map<String, Any?> = if (since > 0) mapOf("id" to sessionId, "since" to since) else mapOf("id" to sessionId)
        val answer = try {
            hub.request("agents.transcript", params, 20_000, desktopKey)
        } catch (_: Throwable) {
            synchronized(lock) {
                if (stopped || attempt != generation) return
                filling = false
                held.clear()
            }
            follow()
            return
        }
        synchronized(lock) {
            if (stopped || attempt != generation) return
            filling = false
            val queued = ArrayList(held)
            held.clear()
            apply(answer, fills = false)
            for (data in queued) {
                if (stopped || attempt != generation) break
                apply(data, fills = false)
            }
        }
    }

    private fun end(reason: String) {
        isStreaming = false
        onEnded(reason)
        stop()
    }

    private fun linkChanged() {
        val refollow = synchronized(lock) {
            if (stopped || awaiting) return
            val current = hub.openLink(desktopKey)
            if (current != null && current === followedLink) return
            isStreaming = false
            current != null
        }
        if (refollow) worker.execute { follow() } else scheduleRetry()
    }

    private companion object {
        val holders = ArrayList<WeakReference<DesktopTranscriptStream>>()

        fun hold(stream: DesktopTranscriptStream) {
            synchronized(holders) {
                holders.removeAll { it.get() == null }
                if (holders.none { it.get() === stream }) holders.add(WeakReference(stream))
            }
        }

        fun release(stream: DesktopTranscriptStream): Boolean = synchronized(holders) {
            holders.removeAll { it.get().let { other -> other == null || other === stream } }
            holders.any { it.get()?.let { other -> other.hub === stream.hub && other.sessionId == stream.sessionId && other.desktopKey == stream.desktopKey } == true }
        }

        fun unseen(delta: AgentTranscriptDelta, after: Int): AgentTranscriptDelta? {
            val skip = after - delta.from
            if (delta.next <= after || delta.text.length != delta.next - delta.from) return null
            if (Character.isLowSurrogate(delta.text[skip])) return null
            return delta.copy(text = delta.text.substring(skip), from = after)
        }
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
