package com.forkmesh.nativeapp.push

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.jsonLong
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.model.ForkNotification
import com.forkmesh.nativeapp.model.PingSettings

data class PingAlertDecision(
    val post: List<ForkNotification> = emptyList(),
    val cancel: List<String> = emptyList(),
    val unread: Int = 0,
)

class PingAlertLedger(
    baseline: Long = 0,
    seen: List<String> = emptyList(),
    shown: List<String> = emptyList(),
    private val capacity: Int = CAPACITY,
) {
    var baseline: Long = baseline
        private set

    private val seen = LinkedHashSet(seen.takeLast(capacity))
    private val shown = LinkedHashSet(shown.takeLast(capacity))

    val isPrimed: Boolean get() = baseline > 0

    val shownIds: List<String> get() = shown.toList()

    fun prime(nowMs: Long) {
        if (baseline <= 0) baseline = nowMs
    }

    fun restart(nowMs: Long) {
        baseline = nowMs
    }

    fun decide(
        rows: List<ForkNotification>,
        settings: PingSettings?,
        nowMs: Long,
        mutedKinds: Set<String> = settings?.pingMutedKinds ?: emptySet(),
    ): PingAlertDecision {
        val unread = rows.count { it.isUnread }
        if (baseline <= 0) {
            baseline = maxOf(nowMs, rows.maxOfOrNull { it.ts } ?: 0)
            rows.forEach { remember(seen, it.id) }
            return PingAlertDecision(unread = unread)
        }
        val post = ArrayList<ForkNotification>()
        for (row in rows.sortedBy { it.ts }) {
            if (row.id.isEmpty() || row.id in seen) continue
            remember(seen, row.id)
            if (!row.isUnread) continue
            if (row.ts in 1 until baseline) continue
            if (row.ts > 0 && nowMs - row.ts > MAX_AGE_MS) continue
            val reminder = jsonText(row.meta["context"]) == "reminder" || pingAlarm(row.kind, row.meta).isNotEmpty()
            if (!reminder && row.kind in mutedKinds) continue
            if (!reminder && pingAlertsHeld(settings, if (row.ts > 0) row.ts else nowMs)) continue
            post.add(row)
            remember(shown, row.id)
        }
        val byId = rows.associateBy { it.id }
        val cancel = shown.filter { id -> byId[id]?.let { !it.isUnread } ?: false }
        cancel.forEach { shown.remove(it) }
        return PingAlertDecision(post, cancel, unread)
    }

    fun forgetShown(id: String) {
        shown.remove(id)
    }

    private fun remember(set: LinkedHashSet<String>, id: String) {
        if (id.isEmpty()) return
        set.remove(id)
        set.add(id)
        while (set.size > capacity) set.remove(set.first())
    }

    fun toJson(): String = Json.stringify(
        linkedMapOf("baseline" to baseline, "seen" to seen.toList(), "shown" to shown.toList()),
    )

    companion object {
        const val CAPACITY = 300
        const val MAX_AGE_MS = 24 * 60 * 60_000L

        fun fromJson(raw: String?): PingAlertLedger {
            if (raw.isNullOrBlank()) return PingAlertLedger()
            val map = runCatching { Json.parse(raw) as? Map<*, *> }.getOrNull() ?: return PingAlertLedger()
            fun ids(key: String): List<String> = (map[key] as? List<*>)?.map(::jsonText)?.filter { it.isNotEmpty() } ?: emptyList()
            return PingAlertLedger(jsonLong(map["baseline"]).coerceAtLeast(0), ids("seen"), ids("shown"))
        }
    }
}
