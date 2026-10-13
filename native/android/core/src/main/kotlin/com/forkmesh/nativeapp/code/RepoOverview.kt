package com.forkmesh.nativeapp.code

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.model.Issue
import com.forkmesh.nativeapp.model.PublishedPull
import com.forkmesh.nativeapp.model.RepoBranch
import com.forkmesh.nativeapp.model.RepoDetailTab
import com.forkmesh.nativeapp.model.RepoRelease
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.navigation.RepoRef

interface RepoOverviewClient {
    fun issues(owner: String, name: String): List<Issue>

    fun pulls(owner: String, name: String): List<PublishedPull>

    fun branches(owner: String, name: String): List<RepoBranch>

    fun releases(owner: String, name: String): List<RepoRelease>

    fun commits(owner: String, name: String): List<Map<String, Any?>>
}

class ApiRepoOverviewClient(private val api: ApiService) : RepoOverviewClient {
    override fun issues(owner: String, name: String): List<Issue> = api.publishedIssues(owner, name)

    override fun pulls(owner: String, name: String): List<PublishedPull> = api.publishedPulls(owner, name)

    override fun branches(owner: String, name: String): List<RepoBranch> = api.branches(owner, name)

    override fun releases(owner: String, name: String): List<RepoRelease> = api.releases(owner, name)

    override fun commits(owner: String, name: String): List<Map<String, Any?>> = api.commits(owner, name)
}

enum class RepoHubSection(val title: String, val icon: String, val tab: RepoDetailTab? = null) {
    Code("Code", "code", RepoDetailTab.Code),
    Pulls("Pull requests", "merge_type", RepoDetailTab.Pulls),
    Issues("Issues", "error_outline", RepoDetailTab.Issues),
    Commits("Commits", "history", RepoDetailTab.Commits),
    Branches("Branches", "account_tree"),
    Agents("Agents", "smart_toy", RepoDetailTab.Agents),
    Worktrees("Worktrees", "layers", RepoDetailTab.Worktrees),
    Actions("Actions", "play_circle_outline", RepoDetailTab.Actions),
    Releases("Releases", "new_releases_outlined", RepoDetailTab.Releases),
    Discussions("Discussions", "forum_outlined", RepoDetailTab.Discussions),
    Mirrors("Mirrors", "dns_outlined", RepoDetailTab.Mirrors),
    About("About", "info_outline", RepoDetailTab.About),
    Board("Issue board", "view_kanban_outlined"),
    Backlog("Backlog", "low_priority"),
    MergeQueue("Merge queue", "merge"),
    CodeMap("Code map", "grid_view"),
    Insights("Insights", "insights"),
    Security("Security", "shield_outlined"),
    Webhooks("Webhooks", "webhook"),
    Projects("Projects", "calendar_month_outlined"),
    Milestones("Milestones", "flag_outlined"),
    Archive("Download ZIP", "download"),
    Settings("Settings", "settings_outlined"),
    ;

    companion object {
        val counted: List<RepoHubSection> = listOf(Issues, Pulls, Commits, Branches, Releases)

        fun of(tab: RepoDetailTab): RepoHubSection = entries.firstOrNull { it.tab == tab } ?: Code
    }
}

data class RepoOverviewTile(val section: RepoHubSection, val count: Int?) {
    val title: String get() = section.title

    val icon: String get() = section.icon

    val countText: String? get() = count?.let { if (it > 999) "999+" else "$it" }

    val accessibilityLabel: String get() = count?.let { "$title, $it" } ?: title
}

class RepoOverviewModel(repo: Repository, private val client: RepoOverviewClient?) {
    var repo: Repository = repo
        private set

    private val known = LinkedHashMap<RepoHubSection, Int>()

    init {
        if (repo.mirrors > 0) known[RepoHubSection.Mirrors] = repo.mirrors
    }

    val counts: Map<RepoHubSection, Int> get() = synchronized(known) { LinkedHashMap(known) }

    fun tiles(hidden: Set<RepoHubSection> = emptySet()): List<RepoOverviewTile> {
        val now = counts
        return RepoHubSection.entries.filter { it !in hidden }.map { RepoOverviewTile(it, now[it]) }
    }

    fun adopt(catalog: List<Repository>): Boolean {
        val key = "${repo.owner}/${repo.name}".lowercase()
        val found = catalog.firstOrNull { "${it.owner}/${it.name}".lowercase() == key } ?: return false
        repo = found
        synchronized(known) {
            if (found.mirrors > 0) known[RepoHubSection.Mirrors] = found.mirrors else known.remove(RepoHubSection.Mirrors)
        }
        return true
    }

    fun count(section: RepoHubSection): Int? {
        val api = client ?: return null
        val owner = repo.owner
        val name = repo.name
        return try {
            when (section) {
                RepoHubSection.Issues -> api.issues(owner, name).count { it.isOpen }
                RepoHubSection.Pulls -> api.pulls(owner, name).count { it.status == "open" }
                RepoHubSection.Commits -> api.commits(owner, name).size
                RepoHubSection.Branches -> api.branches(owner, name).size
                RepoHubSection.Releases -> api.releases(owner, name).size
                else -> null
            }
        } catch (_: Exception) {
            null
        }
    }

    fun record(section: RepoHubSection, count: Int?) {
        if (count == null) return
        synchronized(known) { known[section] = count }
    }

    fun load(): Map<RepoHubSection, Int> {
        for (section in RepoHubSection.counted) record(section, count(section))
        return counts
    }
}

class CurrentRepoMemory(private val store: PrefStore?) {
    val slug: String get() = store?.getString(KEY).orEmpty()

    val ref: RepoRef? get() = RepoRef.parse(slug)

    fun remember(owner: String, name: String): Boolean {
        if (owner.isEmpty() || name.isEmpty()) return false
        val next = "$owner/$name"
        if (slug == next) return false
        store?.putString(KEY, next)
        return true
    }

    companion object {
        const val KEY = "code/current-repo"
    }
}
