package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.control.UnifiedAgentRow
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.jsonLong
import com.forkmesh.nativeapp.model.AgentSession

data class AgentReadFacts(
    val key: String,
    val finishedAtMs: Long,
    val active: Boolean,
    val desktopUnread: Boolean? = null,
    val desktopId: Int = 0,
)

data class AgentReadState(val baselineMs: Long = 0, val seen: Map<String, Long> = emptyMap()) {
    fun isUnread(facts: AgentReadFacts?): Boolean {
        if (facts == null || facts.key.isEmpty()) return false
        val stamp = seen[facts.key]
        facts.desktopUnread?.let { flagged -> return flagged && (stamp == null || stamp < facts.finishedAtMs) }
        if (facts.active || facts.finishedAtMs <= 0) return false
        return facts.finishedAtMs > baselineMs && facts.finishedAtMs > (stamp ?: 0L)
    }

    fun markRead(facts: AgentReadFacts?): AgentReadState {
        if (facts == null || facts.key.isEmpty()) return this
        val before = seen[facts.key]
        val stamp = maxOf(before ?: 0L, facts.finishedAtMs, 0L)
        if (before == stamp) return this
        return copy(seen = capped(seen + (facts.key to stamp), MAX_ENTRIES))
    }

    fun encodeSeen(): String = Json.stringify(LinkedHashMap(seen))

    companion object {
        const val SEEN_KEY = "agents/seenFinishedAt"
        const val BASELINE_KEY = "agents/readBaselineMs"
        const val MAX_ENTRIES = 400
        const val UNREAD_LABEL = "Unread"

        fun load(storedBaselineMs: Long, seenJson: String?, nowMs: Long): AgentReadState =
            AgentReadState(if (storedBaselineMs > 0) storedBaselineMs else nowMs, decodeSeen(seenJson))

        fun decodeSeen(text: String?): Map<String, Long> {
            if (text.isNullOrBlank()) return emptyMap()
            val out = LinkedHashMap<String, Long>()
            for ((key, value) in Json.parseObject(text)) {
                if (key.isEmpty() || value !is Number) continue
                out[key] = maxOf(0L, jsonLong(value))
            }
            return capped(out, MAX_ENTRIES)
        }

        fun capped(seen: Map<String, Long>, max: Int): Map<String, Long> {
            if (seen.size <= max) return seen
            val kept = seen.entries.sortedByDescending { it.value }.take(maxOf(0, max)).map { it.key }.toSet()
            return seen.filterKeys { it in kept }
        }

        fun desktopKey(repoName: String, sessionId: Int): String = "desktop:${repoName.lowercase()}#$sessionId"

        fun runKey(sessionId: String): String = "run:$sessionId"

        fun runFinishedAt(session: AgentLaunchSession): Long = when {
            session.isActive -> 0L
            session.completedAtMs > 0 -> session.completedAtMs
            else -> session.updatedAtMs
        }

        fun facts(row: UnifiedAgentRow): AgentReadFacts? = when {
            row.outside -> null
            row.isDesktop -> if (row.localId <= 0) null else AgentReadFacts(
                key = desktopKey(row.repoName, row.localId),
                finishedAtMs = row.finishedAtMs,
                active = row.isActive,
                desktopUnread = row.unread,
                desktopId = row.localId,
            )
            row.cloudSessionId.isNotEmpty() -> AgentReadFacts(runKey(row.cloudSessionId), row.finishedAtMs, row.isActive)
            else -> null
        }

        fun facts(repoName: String, session: AgentSession, live: UnifiedAgentRow?): AgentReadFacts? {
            if (live != null) return facts(live)
            if (session.id <= 0) return null
            return AgentReadFacts(
                key = desktopKey(repoName, session.id),
                finishedAtMs = if (session.isActive) 0L else session.finishedAtMs,
                active = session.isActive,
                desktopId = session.id,
            )
        }

        fun facts(session: AgentLaunchSession): AgentReadFacts? =
            if (session.sessionId.isEmpty()) null
            else AgentReadFacts(runKey(session.sessionId), runFinishedAt(session), session.isActive)
    }
}
