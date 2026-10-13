package com.forkmesh.nativeapp.chat

class ChatReadMarks(
    private val api: ChatAccountCalling,
    private val now: () -> Long = System::currentTimeMillis,
    private val delayMs: Long = 1_000L,
    private val loop: EventLoop = sharedLoop,
) {
    companion object {
        const val ENDPOINT = "/api/chat/reads"
        const val TOPIC = "chat-read"
        const val POST_MAX = 50
        private const val FUTURE_SLACK_MS = 24L * 60 * 60 * 1000
        private val KEY = Regex("^(#[a-z0-9][a-z0-9_-]{0,63}|private:[0-9a-f]{32}|direct:[0-9a-f]{32})$")
        private val sharedLoop: EventLoop by lazy { EventLoop("chat-read-marks") }

        fun isSyncable(key: String): Boolean = KEY.matches(key)
    }

    private val lock = Any()
    private val marks = HashMap<String, Long>()
    private val pending = LinkedHashMap<String, Long>()
    private var timer: LoopTimer? = null

    fun mark(key: String): Long = synchronized(lock) { marks[key] ?: 0L }

    fun isRead(key: String, timestamp: Long): Boolean = timestamp > 0 && timestamp <= mark(key)

    fun queue(key: String, timestamp: Long) {
        if (!isSyncable(key) || timestamp > now() + FUTURE_SLACK_MS) return
        synchronized(lock) {
            if (timestamp <= (marks[key] ?: 0L) || timestamp <= (pending[key] ?: 0L)) return
            pending[key] = timestamp
            if (timer != null) return
            timer = loop.timer(delayMs) { loop.io { flush() } }
        }
    }

    fun flush() {
        val batch = synchronized(lock) {
            timer?.cancel()
            timer = null
            val taken = pending.entries.sortedBy { it.key }.take(POST_MAX).associate { it.key to it.value }
            pending.clear()
            for ((key, ts) in taken) marks[key] = maxOf(marks[key] ?: 0L, ts)
            taken
        }
        if (batch.isEmpty()) return
        runCatching { api.accountApiCall("POST", ENDPOINT, mapOf("marks" to LinkedHashMap<String, Any?>(batch))) }
    }

    fun fetch(): Boolean {
        val listed = runCatching { api.accountApiCall("GET", ENDPOINT, null)["marks"] as? Map<*, *> }
            .getOrNull() ?: return false
        synchronized(lock) {
            for ((key, value) in listed) {
                val name = key as? String ?: continue
                val ts = (value as? Number)?.toLong() ?: continue
                if (ts > (marks[name] ?: 0L)) marks[name] = ts
            }
        }
        return true
    }

    fun clear() = synchronized(lock) {
        timer?.cancel()
        timer = null
        pending.clear()
        marks.clear()
    }
}
