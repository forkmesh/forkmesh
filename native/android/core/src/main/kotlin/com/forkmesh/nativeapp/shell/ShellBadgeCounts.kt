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

    fun restored(repo: RepoRef, saved: String?): ShellRepoCounts {
        if (knows(repo)) return this
        val found = decode(saved.orEmpty())
        return if (found.isEmpty()) this else ShellRepoCounts(repo.slug.lowercase(), found)
    }

    companion object {
        fun key(repo: RepoRef): String = "shell/repoCounts/${repo.slug.lowercase()}"

        fun encode(counts: Map<RepoHubSection, Int>): String = counts.entries.map { "${it.key.name}:${it.value}" }.sorted().joinToString(",")

        fun decode(text: String): Map<RepoHubSection, Int> = text.split(',').mapNotNull { part ->
            val name = part.substringBefore(':', "")
            val count = part.substringAfter(':', "").toIntOrNull() ?: return@mapNotNull null
            val section = RepoHubSection.entries.firstOrNull { it.name == name } ?: return@mapNotNull null
            section to count
        }.toMap()
    }
}
