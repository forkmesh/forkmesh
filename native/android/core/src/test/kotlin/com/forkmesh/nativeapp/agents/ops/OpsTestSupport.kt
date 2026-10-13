package com.forkmesh.nativeapp.agents.ops

import com.forkmesh.nativeapp.control.ControlCalling
import com.forkmesh.nativeapp.control.ControlEvent
import java.util.concurrent.ConcurrentHashMap

class FakeControlChannel : ControlCalling {
    val requests = ArrayList<Pair<String, Map<String, Any?>>>()
    val replies = HashMap<String, ArrayDeque<Map<String, Any?>>>()
    val failures = HashMap<String, Throwable>()
    private val taps = ConcurrentHashMap<Any, (ControlEvent) -> Unit>()

    fun reply(method: String, value: Map<String, Any?>) {
        replies.getOrPut(method) { ArrayDeque() }.addLast(value)
    }

    @Synchronized
    override fun request(method: String, params: Map<String, Any?>, timeoutMs: Long): Map<String, Any?> {
        requests.add(method to params)
        failures[method]?.let { throw it }
        val queue = replies[method] ?: return emptyMap()
        if (queue.isEmpty()) return emptyMap()
        return if (queue.size > 1) queue.removeFirst() else queue.first()
    }

    override fun addEventTap(tap: (ControlEvent) -> Unit): Any {
        val token = Any()
        taps[token] = tap
        return token
    }

    override fun removeEventTap(token: Any) {
        taps.remove(token)
    }

    val tapCount: Int get() = taps.size

    fun emit(event: String, data: Map<String, Any?> = emptyMap()) {
        for (tap in taps.values) tap(ControlEvent(event, data))
    }

    @Synchronized
    fun methods(): List<String> = requests.map { it.first }

    @Synchronized
    fun params(method: String): Map<String, Any?>? = requests.lastOrNull { it.first == method }?.second
}

class MutableClock(var nowMs: Long = 0) {
    val read: () -> Long = { nowMs }
}

val approvalA = "a".repeat(32)
val approvalB = "b".repeat(32)

fun approvalJson(id: String, session: Int, askedAt: Long = 0, command: String = ""): Map<String, Any?> = linkedMapOf(
    "id" to id,
    "sessionId" to session.toLong(),
    "title" to "Fix the build",
    "repo" to "me/app",
    "provider" to "codex",
    "capability" to "process.execute",
    "kind" to "command",
    "question" to if (command.isEmpty()) "Allow?" else "Run command:\n$command",
    "command" to command,
    "askedAtMs" to askedAt,
    "lifetimes" to listOf("once", "session", "worktree", "persistent"),
)

fun confirmationJson(action: String, token: String = "tok", typeToConfirm: String = ""): Map<String, Any?> = linkedMapOf(
    "token" to token,
    "action" to action,
    "summary" to "Do it",
    "destructive" to (action == "merge"),
    "typeToConfirm" to typeToConfirm,
    "expiresInMs" to 60_000L,
)
