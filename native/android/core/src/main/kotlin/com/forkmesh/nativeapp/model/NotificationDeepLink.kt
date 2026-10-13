package com.forkmesh.nativeapp.model

import java.net.URLDecoder

enum class RepoDetailTab {
    Code, Issues, Pulls, Discussions, Commits, Mirrors, Releases, About, Actions, Agents, Worktrees,
}

data class NotificationDeepLink(
    val repo: Repository,
    val initialTab: RepoDetailTab,
    val number: Int = 0,
    val reference: String = "",
    val branch: String = "",
    val session: Int = 0,
) {
    val isBranchCommits: Boolean get() = initialTab == RepoDetailTab.Commits && reference.isEmpty() && branch.isNotEmpty()

    companion object {
        private val SAFE_SEGMENT = Regex("^[A-Za-z0-9._-]+$")
        private val SCHEME = Regex("^[A-Za-z][A-Za-z0-9+.-]*:")

        fun parse(href: String): NotificationDeepLink? {
            val trimmed = href.trim()
            if (trimmed.isEmpty()) return null
            if (SCHEME.containsMatchIn(trimmed) || trimmed.startsWith("//")) return null
            val path = trimmed.substringBefore('#').substringBefore('?')
            val query = trimmed.substringBefore('#').substringAfter('?', "")
            val parts = try {
                path.split('/').filter { it.isNotEmpty() }.map { decode(decode(it)) }
            } catch (_: IllegalArgumentException) {
                return null
            }
            if (parts.size < 2) return null
            val owner = parts[0]
            val name = parts[1]
            if (!SAFE_SEGMENT.matches(owner) || !SAFE_SEGMENT.matches(name)) return null
            if (owner == "api" || owner == "dashboard") return null

            var tab = RepoDetailTab.Code
            var number = 0
            var reference = ""
            var session = 0
            fun rest(from: Int): String = if (parts.size > from) parts.drop(from).joinToString("/") else ""
            var branch = (queryValue(query, "branch") ?: queryValue(query, "ref") ?: "").trim()
            if (parts.size >= 3) {
                when (parts[2]) {
                    "issues" -> { tab = RepoDetailTab.Issues; number = number(parts) }
                    "pulls" -> { tab = RepoDetailTab.Pulls; number = number(parts) }
                    "discussions" -> { tab = RepoDetailTab.Discussions; number = number(parts) }
                    "commit" -> {
                        tab = RepoDetailTab.Commits
                        reference = if (parts.size >= 4) parts[3] else ""
                    }
                    "commits" -> {
                        tab = RepoDetailTab.Commits
                        val named = rest(3)
                        if (COMMIT_HASH.matches(named)) {
                            reference = named
                        } else if (named.isNotEmpty()) {
                            branch = named
                        }
                    }
                    "mirrors" -> tab = RepoDetailTab.Mirrors
                    "releases" -> tab = RepoDetailTab.Releases
                    "about" -> tab = RepoDetailTab.About
                    "actions" -> tab = RepoDetailTab.Actions
                    "agents" -> {
                        tab = RepoDetailTab.Agents
                        session = number(parts)
                    }
                    "worktrees" -> tab = RepoDetailTab.Worktrees
                    "tree" -> {
                        tab = RepoDetailTab.Code
                        if (branch.isEmpty()) branch = rest(3)
                    }
                    "blob", "raw", "code" -> tab = RepoDetailTab.Code
                }
            }
            return NotificationDeepLink(Repository(owner = owner, name = name), tab, number, reference, branch, session)
        }

        private val COMMIT_HASH = Regex("^[0-9a-f]{7,64}$")

        private fun queryValue(query: String, name: String): String? {
            if (query.isEmpty()) return null
            for (pair in query.split('&')) {
                val key = pair.substringBefore('=')
                if (runCatching { decode(key) }.getOrNull() != name) continue
                return runCatching { URLDecoder.decode(pair.substringAfter('=', ""), "UTF-8") }.getOrNull()
            }
            return null
        }

        private fun number(parts: List<String>): Int = if (parts.size < 4) 0 else parts[3].toIntOrNull() ?: 0

        private fun decode(value: String): String = URLDecoder.decode(value.replace("+", "%2B"), "UTF-8")
    }
}
