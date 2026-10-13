package com.forkmesh.nativeapp.insights

import com.forkmesh.nativeapp.api.ApiJsonResult

class UserActivitySync(
    private val log: UserActivityLog,
    private val kind: String,
    private val post: (Map<String, Any?>) -> ApiJsonResult,
    private val nowMs: () -> Long = System::currentTimeMillis,
) {
    private var account = ""
    private var sentRevision = -1L
    private var syncedMs = Long.MIN_VALUE / 2

    @Synchronized
    fun sync(account: String, force: Boolean = false): Boolean {
        if (account.isEmpty()) {
            reset()
            return false
        }
        if (!account.equals(this.account, ignoreCase = true)) {
            reset()
            this.account = account
        }
        val now = nowMs()
        val revision = log.revision
        val changed = revision != sentRevision
        if (!changed && now - syncedMs < if (force) FORCED_FLOOR_MS else READ_MS) return false
        val day = log.today()
        val payload = linkedMapOf<String, Any?>(
            "day" to day.toString(),
            "device" to log.deviceKey,
            "kind" to kind,
        )
        if (changed) payload["counts"] = log.localCounts(day).toList()
        val result = runCatching { post(payload) }.getOrNull() ?: return false
        if (!result.ok || !result.body["nodeName"]?.toString().orEmpty().equals(account, ignoreCase = true)) return false
        val others = (result.body["others"] as? List<*>)?.map { (it as? Number)?.toInt() ?: 0 } ?: return false
        if (others.size != ActivityGrid.SLOTS) return false
        if (changed) sentRevision = revision
        syncedMs = now
        val kinds = (result.body["sources"] as? List<*>).orEmpty()
            .mapNotNull { it as? Map<*, *> }
            .filter { it["self"] != true && ((it["total"] as? Number)?.toInt() ?: 0) > 0 }
            .map { it["kind"]?.toString().orEmpty() }
        log.setOthers(day, others.toIntArray(), kinds)
        return true
    }

    @Synchronized
    fun reset() {
        account = ""
        sentRevision = -1L
        syncedMs = Long.MIN_VALUE / 2
        log.clearOthers()
    }

    companion object {
        const val INTERVAL_MS = 2 * 60_000L
        const val READ_MS = 10 * 60_000L
        const val FORCED_FLOOR_MS = 10_000L
    }
}
