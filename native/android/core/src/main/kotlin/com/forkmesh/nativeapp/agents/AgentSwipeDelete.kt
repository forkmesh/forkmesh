package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.control.UnifiedAgentRow
import com.forkmesh.nativeapp.control.liveDesktopRow

sealed class AgentDeleteTarget {
    data class Desktop(val sessionId: Int, val repoName: String, val desktopKey: String = "") : AgentDeleteTarget()

    data class Outside(val agentId: Int, val repoName: String, val desktopKey: String = "") : AgentDeleteTarget()

    data class Run(val row: UnifiedAgentRow) : AgentDeleteTarget()

    val onDesktop: Boolean get() = this is Desktop || this is Outside

    val desktopId: Int
        get() = when (this) {
            is Desktop -> sessionId
            is Outside -> agentId
            is Run -> 0
        }

    val needsConfirmation: Boolean get() = this is Outside

    val onDesktopKey: String
        get() = when (this) {
            is Desktop -> desktopKey
            is Outside -> desktopKey
            is Run -> ""
        }
}

object AgentSwipeDelete {
    const val DESKTOP_OUT_OF_REACH = "The desktop is not in reach."
    const val OUTSIDE_CONFIRM_TITLE = "Stop and delete this agent?"
    const val OUTSIDE_CONFIRM_TEXT = "It was started outside ForkMesh. Deleting it ends that process on the desktop."

    fun forRow(row: UnifiedAgentRow): AgentDeleteTarget? = when {
        row.outside -> if (row.localId != 0) AgentDeleteTarget.Outside(row.localId, row.repoName, if (row.elsewhere) row.desktopKey else "") else null
        row.isDesktop -> if (row.localId > 0) AgentDeleteTarget.Desktop(row.localId, row.repoName, if (row.elsewhere) row.desktopKey else "") else null
        row.cloudSessionId.isNotEmpty() -> AgentDeleteTarget.Run(row)
        else -> null
    }

    fun forEntry(entry: AgentEntry, liveRows: List<UnifiedAgentRow>): AgentDeleteTarget? =
        when (entry) {
            is AgentEntry.Desktop ->
                liveDesktopRow(liveRows, entry.repo.name, entry.session.id)?.let(::forRow)
                    ?: if (entry.session.id > 0) AgentDeleteTarget.Desktop(entry.session.id, entry.repo.name) else null
            is AgentEntry.Launched ->
                forRow(UnifiedAgentRow.fromCloud(entry.session.copy(owner = entry.repo.owner, repo = entry.repo.name)))
        }

    fun forget(groups: List<AgentRepoGroup>, target: AgentDeleteTarget): List<AgentRepoGroup> = groups.map { group ->
        when (target) {
            is AgentDeleteTarget.Desktop ->
                if (target.desktopKey.isNotEmpty() || !group.repo.name.equals(target.repoName, ignoreCase = true)) group
                else group.copy(sessions = group.sessions.filter { it.id != target.sessionId })
            is AgentDeleteTarget.Outside -> group
            is AgentDeleteTarget.Run ->
                if (group.repo.owner != target.row.repoOwner || group.repo.name != target.row.repoName) group
                else group.copy(launched = group.launched.filter { it.sessionId != target.row.cloudSessionId })
        }
    }.let(AgentsList::visible)
}
