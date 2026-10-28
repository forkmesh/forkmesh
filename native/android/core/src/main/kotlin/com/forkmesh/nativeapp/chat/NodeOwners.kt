package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.crypto.ChatV3
import com.forkmesh.nativeapp.json.jsonText
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.TimeUnit

class NodeOwners(
    private val api: ChatAccountCalling,
    private val now: () -> Long = System::currentTimeMillis,
    private val run: (() -> Unit) -> Unit = { task -> Thread(task, "NodeOwners").apply { isDaemon = true }.start() },
    private val later: (Long, () -> Unit) -> Unit = { ms, task -> recheckTimer.schedule(Runnable { task() }, ms, TimeUnit.MILLISECONDS) },
) {
    companion object {
        const val CACHE_MS = 60 * 60 * 1000L
        const val RETRY_MS = 60 * 1000L
        const val MAX_NODES = 1024
        const val MAX_WATCHED = 256
        const val MAX_RECHECKS = 10
        const val RECHECK_MS = RETRY_MS + 1000L
        const val PATH = "/api/chat/node-owner"

        private val recheckTimer: ScheduledExecutorService by lazy {
            Executors.newSingleThreadScheduledExecutor { task -> Thread(task, "NodeOwners-recheck").apply { isDaemon = true } }
        }
    }

    private class Answer(val account: String, val expires: Long)

    private val lock = Any()
    private val answers = object : LinkedHashMap<String, Answer>(16, 0.75f, true) {
        override fun removeEldestEntry(eldest: MutableMap.MutableEntry<String, Answer>?): Boolean = size > MAX_NODES
    }
    private val pending = HashSet<String>()
    private val watched = HashMap<String, String>()
    private var generation = 0

    private val listeners = java.util.concurrent.CopyOnWriteArrayList<() -> Unit>()

    fun addListener(listener: () -> Unit) {
        listeners.add(listener)
    }

    fun verified(node: String, claimed: String): String {
        val name = claimed.trim().lowercase()
        if (name.isEmpty()) return ""
        val account = account(node) ?: return ""
        return if (account == name) account else ""
    }

    fun account(node: String): String? {
        if (ChatV3.nodeKeyBytes(node) == null) return null
        cached(node)?.let { return it.account.ifEmpty { null } }
        request(node)
        return cached(node)?.account?.ifEmpty { null }
    }

    private fun cached(node: String): Answer? = synchronized(lock) { answers[node]?.takeIf { it.expires > now() } }

    fun request(node: String) {
        if (ChatV3.nodeKeyBytes(node) == null) return
        val start = synchronized(lock) {
            val fresh = answers[node]?.let { it.expires > now() } == true
            !fresh && pending.add(node)
        }
        if (start) run { fetch(node) }
    }

    fun fetch(node: String): String? {
        val result = try {
            jsonText(api.accountApiCall("GET", "$PATH?node=$node", null)["account"]).trim().lowercase()
        } catch (_: Exception) {
            null
        }
        synchronized(lock) {
            pending.remove(node)
            answers[node] = if (result.isNullOrEmpty()) Answer("", now() + RETRY_MS) else Answer(result, now() + CACHE_MS)
            if (result != null && result.isNotEmpty() && watched[node] == result) watched.remove(node)
        }
        for (listener in listeners) runCatching { listener() }
        return result
    }

    fun watch(node: String, claimed: String) {
        val claim = claimed.trim().lowercase()
        if (claim.isEmpty() || ChatV3.nodeKeyBytes(node) == null) return
        val started = synchronized(lock) {
            if (answers[node]?.let { it.account == claim && it.expires > now() } == true) return
            val fresh = node !in watched
            if (fresh && watched.size >= MAX_WATCHED) return
            watched[node] = claim
            if (fresh) generation else return
        }
        schedule(node, started, 0)
    }

    fun isWatching(node: String): Boolean = synchronized(lock) { node in watched }

    private fun stillWatching(node: String, started: Int): Boolean = synchronized(lock) { started == generation && node in watched }

    private fun schedule(node: String, started: Int, done: Int) {
        later(RECHECK_MS) { run { recheck(node, started, done) } }
    }

    private fun recheck(node: String, started: Int, done: Int) {
        if (!stillWatching(node, started)) return
        fetch(node)
        if (done + 1 >= MAX_RECHECKS) {
            synchronized(lock) { if (started == generation) watched.remove(node) }
            return
        }
        if (stillWatching(node, started)) schedule(node, started, done + 1)
    }

    fun clear() {
        synchronized(lock) {
            answers.clear()
            pending.clear()
            watched.clear()
            generation++
        }
    }
}
