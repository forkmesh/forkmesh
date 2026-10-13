package com.forkmesh.nativeapp.code

import com.forkmesh.nativeapp.format.absoluteDate
import com.forkmesh.nativeapp.format.countOrNull
import com.forkmesh.nativeapp.format.relativeTime
import com.forkmesh.nativeapp.model.Repository

enum class RepoFilter(val label: String, val key: String) {
    All("All", "all"),
    Online("Online", "online"),
    Offline("Offline", "offline"),
    Public("Public", "public"),
    Private("Private", "private"),
}

enum class RepoSort(val buttonLabel: String, val menuLabel: String, val key: String) {
    RecentlyUpdated("Updated", "Recently updated first", "recentlyUpdated"),
    Name("Name", "Name A-Z", "name"),
    Mirrors("Mirrors", "Mirrors high-low", "mirrors"),
}

data class RepoMetaItem(
    val icon: String,
    val text: String,
    val tooltip: String? = null,
    val isLanguage: Boolean = false,
)

object RepoCatalog {
    fun isOnline(repo: Repository): Boolean = repo.cloneOnline || repo.liveHost

    fun normalizeQuery(raw: String): String = raw.trim().lowercase()

    fun matchesQuery(repo: Repository, query: String): Boolean {
        if (query.isEmpty()) return true
        return repo.fullName.lowercase().contains(query) ||
            repo.description.lowercase().contains(query) ||
            repo.language.lowercase().contains(query)
    }

    fun matchesFilter(repo: Repository, filter: RepoFilter): Boolean {
        val online = isOnline(repo)
        return when (filter) {
            RepoFilter.All -> true
            RepoFilter.Online -> online
            RepoFilter.Offline -> !online
            RepoFilter.Public -> !repo.isPrivate
            RepoFilter.Private -> repo.isPrivate
        }
    }

    fun compareNames(a: Repository, b: Repository): Int = a.fullName.lowercase().compareTo(b.fullName.lowercase())

    fun compare(a: Repository, b: Repository, sort: RepoSort): Int = when (sort) {
        RepoSort.RecentlyUpdated -> b.updatedMs.compareTo(a.updatedMs).let { if (it == 0) compareNames(a, b) else it }
        RepoSort.Name -> compareNames(a, b)
        RepoSort.Mirrors -> b.mirrors.compareTo(a.mirrors).let { if (it == 0) compareNames(a, b) else it }
    }

    fun filterAndSort(repos: List<Repository>, query: String, filter: RepoFilter, sort: RepoSort): List<Repository> =
        repos.filter { matchesQuery(it, query) && matchesFilter(it, filter) }.sortedWith { a, b -> compare(a, b, sort) }

    fun hasActiveControls(query: String, filter: RepoFilter): Boolean = query.isNotEmpty() || filter != RepoFilter.All

    fun emptyTitle(activeControls: Boolean): String =
        if (activeControls) "No matching repositories" else "No repositories"

    fun emptyMessage(activeControls: Boolean): String =
        if (activeControls) "Try another search term or filter." else "Repository mirrors will appear here when available."

    fun listStateKey(filter: RepoFilter, sort: RepoSort, query: String, visibleCount: Int): String =
        "list-${filter.key}-${sort.key}-$query-$visibleCount"

    fun metaItems(repo: Repository, nowMillis: Long = System.currentTimeMillis()): List<RepoMetaItem> {
        val items = ArrayList<RepoMetaItem>()
        if (repo.language.isNotEmpty()) items += RepoMetaItem("circle", repo.language, isLanguage = true)
        countOrNull(repo.stars)?.let { items += RepoMetaItem("star_border", it) }
        countOrNull(repo.forks)?.let { items += RepoMetaItem("call_split", it) }
        countOrNull(repo.mirrors, singular = "mirror")?.let { items += RepoMetaItem("dns_outlined", it) }
        items += RepoMetaItem(
            if (repo.isPrivate) "lock_outline" else "public",
            if (repo.isPrivate) "Private" else "Public",
        )
        if (repo.updatedMs > 0) {
            items += RepoMetaItem(
                "update",
                "Updated ${relativeTime(repo.updatedMs, nowMillis)}",
                tooltip = "Updated ${absoluteDate(repo.updatedMs)}",
            )
        }
        return items
    }
}
