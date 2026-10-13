package com.forkmesh.nativeapp.agents.ops

import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.model.AgentSession

enum class AgentListSort(val wire: String, val title: String) {
    Priority("priority", "Priority (highest first)"),
    NewestAdded("newestAdded", "Date added (newest first)"),
    OldestAdded("oldestAdded", "Date added (oldest first)"),
    RecentActivity("recentActivity", "Recent activity"),
    Status("status", "Status (active first)"),
    Title("title", "Title (A-Z)"),
    Provider("provider", "Provider and model"),
    IssueNumber("issueNumber", "Issue number"),
    TokenUsage("tokenUsage", "Token usage (highest first)"),
    Cost("cost", "Cost (highest first)"),
    Runtime("runtime", "Runtime (longest first)"),
    FilesChanged("filesChanged", "Files changed (most first)"),
    ;

    companion object {
        fun of(wire: String): AgentListSort? = entries.firstOrNull { it.wire == wire }
    }
}

enum class AgentListFilter(val wire: String, val title: String) {
    All("all", "All"),
    Desktop("desktop", "Desktop"),
    Cloud("cloud", "Cloud"),
    NeedsYou("needsYou", "Needs you"),
    Running("running", "Running"),
    Queued("queued", "Queued"),
    Recent("recent", "Recent"),
    ;

    companion object {
        fun of(wire: String): AgentListFilter? = entries.firstOrNull { it.wire == wire }
    }
}

data class AgentListItem(
    val session: AgentSession,
    val title: String = session.displayTitle,
    val priority: Int = session.priority,
    val needsYou: Boolean = false,
)

object AgentListOrdering {
    fun activity(session: AgentSession): Long =
        maxOf(maxOf(session.finishedAtMs, session.startedAtMs), maxOf(session.createdAtMs, session.mergedAtMs))

    fun runtime(session: AgentSession, nowMs: Long): Long {
        if (session.durationMs > 0) return session.durationMs
        if (session.startedAtMs <= 0) return 0
        val end = if (session.finishedAtMs > 0) session.finishedAtMs else nowMs
        return maxOf(0, end - session.startedAtMs)
    }

    private fun statusRank(item: AgentListItem): Int {
        if (item.needsYou) return 0
        return when (item.session.statusLabel) {
            "Waiting" -> 1
            "Running" -> 2
            "Queued" -> 3
            else -> if (item.session.isActive) 2 else 4
        }
    }

    fun sorted(items: List<AgentListItem>, order: AgentListSort, nowMs: Long = 0): List<AgentListItem> =
        sorted(items, order, nowMs) { it }

    fun <T> sorted(values: List<T>, order: AgentListSort, nowMs: Long = 0, item: (T) -> AgentListItem): List<T> {
        val items = values.map(item)
        val indices = values.indices.sortedWith { l, r ->
            when (compare(items[l], items[r], order, nowMs)) {
                true -> -1
                false -> 1
                null -> l.compareTo(r)
            }
        }
        return indices.map { values[it] }
    }

    private fun <C : Comparable<C>> desc(x: C, y: C): Boolean? = if (x == y) null else x > y

    private fun <C : Comparable<C>> asc(x: C, y: C): Boolean? = if (x == y) null else x < y

    private fun compare(a: AgentListItem, b: AgentListItem, order: AgentListSort, nowMs: Long): Boolean? {
        val s = a.session
        val t = b.session
        return when (order) {
            AgentListSort.Priority -> asc(a.priority, b.priority) ?: desc(activity(s), activity(t))
            AgentListSort.NewestAdded -> desc(s.createdAtMs, t.createdAtMs) ?: desc(s.id, t.id)
            AgentListSort.OldestAdded -> asc(s.createdAtMs, t.createdAtMs) ?: asc(s.id, t.id)
            AgentListSort.RecentActivity -> desc(activity(s), activity(t))
            AgentListSort.Status -> asc(statusRank(a), statusRank(b)) ?: desc(activity(s), activity(t))
            AgentListSort.Title -> {
                val x = a.title.lowercase()
                val y = b.title.lowercase()
                if (x == y) null else x < y
            }
            AgentListSort.Provider -> {
                val x = (s.providerLabel + " " + s.model).lowercase()
                val y = (t.providerLabel + " " + t.model).lowercase()
                if (x == y) desc(activity(s), activity(t)) else x < y
            }
            AgentListSort.IssueNumber -> {
                if ((s.issueNumber > 0) != (t.issueNumber > 0)) s.issueNumber > 0 else asc(s.issueNumber, t.issueNumber)
            }
            AgentListSort.TokenUsage -> desc(s.totalTokens, t.totalTokens)
            AgentListSort.Cost -> desc(s.costUsd, t.costUsd)
            AgentListSort.Runtime -> desc(runtime(s, nowMs), runtime(t, nowMs))
            AgentListSort.FilesChanged -> desc(s.filesChanged, t.filesChanged)
        }
    }

    fun matches(item: AgentListItem, filter: AgentListFilter, nowMs: Long = 0, recentWindowMs: Long = 86_400_000): Boolean {
        val session = item.session
        return when (filter) {
            AgentListFilter.All, AgentListFilter.Desktop, AgentListFilter.Cloud -> true
            AgentListFilter.NeedsYou -> item.needsYou || session.statusLabel == "Waiting"
            AgentListFilter.Running -> session.isActive && session.statusLabel != "Queued"
            AgentListFilter.Queued -> session.statusLabel == "Queued"
            AgentListFilter.Recent -> !session.isActive && (nowMs == 0L || nowMs - activity(session) <= recentWindowMs)
        }
    }

    fun apply(items: List<AgentListItem>, filter: AgentListFilter, sort: AgentListSort, nowMs: Long = 0): List<AgentListItem> =
        sorted(items.filter { matches(it, filter, nowMs) }, sort, nowMs)
}

data class AgentApprovalNotification(val approvalId: String, val sessionId: String = "") {
    enum class Action(val wire: String, val title: String) {
        Approve("agent-approval.approve", "Approve"),
        Deny("agent-approval.deny", "Deny"),
        Open("agent-approval.open", "Open"),
        ;

        val requiresAuthentication: Boolean get() = this == Approve
        val isDestructive: Boolean get() = this == Deny
        val opensApp: Boolean get() = this == Open

        companion object {
            fun of(wire: String): Action? = entries.firstOrNull { it.wire == wire }
        }
    }

    fun perform(action: Action, queue: ApprovalsQueue): Boolean = when (action) {
        Action.Approve -> queue.approve(approvalId, ApprovalLifetime.Once)
        Action.Deny -> queue.deny(approvalId)
        Action.Open -> false
    }

    companion object {
        const val CATEGORY = "agent-approval"

        fun from(payload: Map<String, Any?>): AgentApprovalNotification? {
            val layers = listOf(payload, payload.obj("alert"), payload.obj("data"), payload.obj("aps"))
            val category = layers.map { it.str("category") }.firstOrNull { it.isNotEmpty() } ?: ""
            if (category != CATEGORY) return null
            val approval = layers.map { it.str("approvalId") }.firstOrNull { it.isNotEmpty() } ?: ""
            if (approval.isEmpty() || approval.toByteArray(Charsets.UTF_8).size > 64) return null
            val session = layers.map { it.str("sessionId") }.firstOrNull { it.isNotEmpty() } ?: ""
            return AgentApprovalNotification(approval, session)
        }
    }
}
