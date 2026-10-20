package com.forkmesh.nativeapp.shell

import com.forkmesh.nativeapp.code.RepoHubSection
import com.forkmesh.nativeapp.navigation.RepoRef
import com.forkmesh.nativeapp.tasks.TaskBoard

data class ShellBadgeCounts(
    val assignedTasks: Int = 0,
    val bugReports: Int? = null,
) {
    fun adoptTasks(board: TaskBoard): ShellBadgeCounts = copy(assignedTasks = board.mineOpen.size)

    fun adoptBugReports(counts: Map<String, Int>): ShellBadgeCounts {
        val fresh = counts["new"] ?: return this
        return copy(bugReports = fresh)
    }
}

data class ShellRepoCounts(
    val slug: String = "",
    val counts: Map<RepoHubSection, Int> = emptyMap(),
) {
    fun adopt(repo: RepoRef, more: Map<RepoHubSection, Int>): ShellRepoCounts {
        val key = repo.slug.lowercase()
        return ShellRepoCounts(key, if (key == slug) counts + more else more)
    }

    fun knows(repo: RepoRef): Boolean = repo.slug.lowercase() == slug

    fun of(current: RepoRef?, section: RepoHubSection): Int? = if (current != null && knows(current)) counts[section] else null
}
