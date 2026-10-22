package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.crypto.ChatV3
import com.forkmesh.nativeapp.json.jsonText

class NodeOwners(
    private val api: ChatAccountCalling,
    private val now: () -> Long = System::currentTimeMillis,
    private val run: (() -> Unit) -> Unit = { task -> Thread(task, "NodeOwners").apply { isDaemon = true }.start() },
) {
    companion object {
        const val CACHE_MS = 60 * 60 * 1000L
        const val RETRY_MS = 60 * 1000L
        const val MAX_NODES = 1024
        const val PATH = "/api/chat/node-owner"
    }

    private class Answer(val account: String, val expires: Long)

    private val lock = Any()
    private val answers = object : LinkedHashMap<String, Answer>(16, 0.75f, true) {
        override fun removeEldestEntry(eldest: MutableMap.MutableEntry<String, Answer>?): Boolean = size > MAX_NODES
    }
    private val pending = HashSet<String>()

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
            answers[node] = if (result == null) Answer("", now() + RETRY_MS) else Answer(result, now() + CACHE_MS)
        }
        for (listener in listeners) runCatching { listener() }
        return result
    }

    fun clear() = synchronized(lock) {
        answers.clear()
        pending.clear()
    }
}
