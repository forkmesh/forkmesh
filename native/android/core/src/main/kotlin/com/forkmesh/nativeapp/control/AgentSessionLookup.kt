package com.forkmesh.nativeapp.control

object AgentSessionLookup {
    fun find(rows: List<UnifiedAgentRow>, sessionId: String): UnifiedAgentRow? {
        val wanted = sessionId.trim()
        if (wanted.isEmpty()) return null
        rows.firstOrNull { it.id == wanted }?.let { return it }
        val bare = wanted.substringAfter(':')
        val local = bare.toIntOrNull()
        if (local != null && local > 0) rows.firstOrNull { it.isDesktop && it.localId == local }?.let { return it }
        return rows.firstOrNull { !it.isDesktop && it.cloudSessionId == bare }
    }

    fun forPull(rows: List<UnifiedAgentRow>, owner: String, name: String, number: Int, head: String): UnifiedAgentRow? =
        rows.firstOrNull { row ->
            !row.outside && !row.merged && row.repoName == name && (row.repoOwner.isEmpty() || row.repoOwner == owner) &&
                ((number > 0 && row.prNumber == number) || (head.isNotEmpty() && row.branchName == head))
        }

    fun desktopSessionNumber(sessionId: String): Int? = sessionId.trim().substringAfter(':').toIntOrNull()?.takeIf { it > 0 }
}
