package com.forkmesh.nativeapp.control

import java.util.concurrent.Executor
import java.util.concurrent.Executors

data class DesktopTokenReading(val totals: List<Long>, val agents: Map<Int, List<Long>>) {
    companion object {
        const val EVENT = "diag.vitals"

        private fun whole(value: Any?): Long? = (value as? Number)?.toLong()

        fun parse(data: Map<String, Any?>): DesktopTokenReading? {
            val into = whole(data["tokensIn"]) ?: return null
            val out = whole(data["tokensOut"]) ?: return null
            val agents = HashMap<Int, List<Long>>()
            for ((key, value) in (data["agentTokens"] as? Map<*, *>).orEmpty()) {
                val session = key?.toString()?.toIntOrNull() ?: continue
                val pair = (value as? List<*>).orEmpty().mapNotNull(::whole)
                if (pair.size == 2) agents[session] = pair
            }
            return DesktopTokenReading(listOf(into, out), agents)
        }
    }
}

class DesktopTokenFeed(
    private val hub: DesktopControlHub,
    private val worker: Executor = Executors.newSingleThreadExecutor { Thread(it, "DesktopTokenFeed").apply { isDaemon = true } },
) {
    private val lock = Any()
    private var users = 0
    private var tap: Any? = null
    private var linkToken: Any? = null
    private var generation = 0
    private var watchedLink: ControlLink? = null

    @Volatile
    var watching = false
        private set

    @Volatile
    var reading: DesktopTokenReading? = null
        private set

    val totals: List<Long> get() = if (watching) reading?.totals.orEmpty() else emptyList()

    fun agentTokens(sessionId: Int): List<Long>? = if (watching && sessionId > 0) reading?.agents?.get(sessionId) else null

    fun acquire() {
        synchronized(lock) {
            users++
            if (users > 1) return
            tap = hub.addEventTap { event -> onEvent(event) }
            linkToken = hub.addLinkListener { worker.execute { watch() } }
        }
        worker.execute { watch() }
    }

    fun release() {
        val link = synchronized(lock) {
            if (users == 0) return
            users--
            if (users > 0) return
            tap?.let(hub::removeEventTap)
            linkToken?.let(hub::removeLinkListener)
            tap = null
            linkToken = null
            generation++
            watching = false
            reading = null
            watchedLink.also { watchedLink = null }
        }
        if (link != null && link.isOpen) {
            worker.execute { runCatching { link.request("diag.unwatch", emptyMap(), 10_000) } }
        }
    }

    private fun watch() {
        val mine = synchronized(lock) {
            if (users == 0) return
            ++generation
        }
        val link = hub.currentLink
        if (link == null) {
            synchronized(lock) {
                if (mine != generation) return
                watching = false
                reading = null
                watchedLink = null
            }
            return
        }
        try {
            link.request("diag.watch", emptyMap(), 15_000)
            synchronized(lock) {
                if (mine != generation) return
                watchedLink = link
                reading = null
                watching = true
            }
        } catch (_: Throwable) {
            synchronized(lock) {
                if (mine != generation) return
                watching = false
                reading = null
            }
        }
    }

    private fun onEvent(event: ControlEvent) {
        if (event.event != DesktopTokenReading.EVENT || !watching) return
        DesktopTokenReading.parse(event.data)?.let { reading = it }
    }
}
