package com.forkmesh.nativeapp.navigation

import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.model.Repository

enum class GlobalSearchScope(val raw: String, val title: String) {
    All("all", "All"), Code("code", "Code"), Issues("issues", "Issues & PRs"), Chat("chat", "Chat"), People("people", "People");

    val routeScope: SearchScope
        get() = when (this) {
            All -> SearchScope.Everything
            Code -> SearchScope.Code
            Issues -> SearchScope.Issues
            Chat -> SearchScope.Chat
            People -> SearchScope.People
        }

    companion object {
        fun of(raw: String?): GlobalSearchScope? = entries.firstOrNull { it.raw == raw }

        fun from(scope: SearchScope): GlobalSearchScope = when (scope) {
            SearchScope.Code, SearchScope.Repos -> Code
            SearchScope.Issues, SearchScope.Pulls -> Issues
            SearchScope.Chat -> Chat
            SearchScope.People -> People
            SearchScope.Everything, SearchScope.Notes, SearchScope.Commands -> All
        }
    }
}

enum class SearchBucketKind(val raw: String, val title: String) {
    Repositories("repositories", "Repositories"),
    Destinations("destinations", "Go to"),
    People("people", "People"),
    Commits("commits", "Commits"),
    Issues("issues", "Issues"),
    Pulls("pulls", "Pull requests"),
    Code("code", "Code"),
    Chat("chat", "Chat");

    val scope: GlobalSearchScope
        get() = when (this) {
            Destinations -> GlobalSearchScope.All
            Issues, Pulls -> GlobalSearchScope.Issues
            Repositories, Commits, Code -> GlobalSearchScope.Code
            People -> GlobalSearchScope.People
            Chat -> GlobalSearchScope.Chat
        }
}

data class SearchHit(val id: String, val title: String, val subtitle: String = "", val icon: String, val route: Route)

data class SearchBucket(val kind: SearchBucketKind, val hits: List<SearchHit>, val total: Int) {
    val hasMore: Boolean get() = total > hits.size
}

interface GlobalSearchSource {
    val kind: SearchBucketKind
    fun search(query: String): List<SearchHit>
}

object SearchMatch {
    private const val SEPARATORS = " /-_.#@"

    fun score(text: String, query: String): Int? {
        val needle = query.trim().lowercase()
        if (needle.isEmpty()) return null
        val haystack = text.lowercase()
        if (haystack == needle) return 0
        if (haystack.startsWith(needle)) return 1
        if (haystack.split(*SEPARATORS.toCharArray()).any { it.isNotEmpty() && it.startsWith(needle) }) return 2
        if (haystack.contains(needle)) return 3
        val loose = compact(needle)
        if (loose.isNotEmpty() && compact(haystack).contains(loose)) return 4
        return null
    }

    fun compact(text: String): String = text.filter { it.isLetterOrDigit() }

    fun <T> rank(items: List<T>, query: String, text: (T) -> List<String>): List<T> =
        items.mapIndexedNotNull { offset, item ->
            text(item).mapNotNull { score(it, query) }.minOrNull()?.let { Triple(it, offset, item) }
        }.sortedWith(compareBy({ it.first }, { it.second })).map { it.third }
}

class DestinationsSearchSource(private val routes: List<Route> = defaultRoutes) : GlobalSearchSource {
    override val kind = SearchBucketKind.Destinations

    companion object {
        val defaultRoutes: List<Route>
            get() = AppTab.barOrder.map { it.root } + HubCatalog.routes + listOf(
                Route.Agents(AgentFilter.NeedsYou), Route.Agents(AgentFilter.Running), Route.Inbox(InboxFilter.Approvals),
                Route.Inbox(InboxFilter.Reviews), Route.Inbox(InboxFilter.Mentions), Route.LocalRepos, Route.PingSettings,
                Route.Projects,
            )

        fun routes(hidden: Set<Route.Kind>): List<Route> = if (hidden.isEmpty()) defaultRoutes else defaultRoutes.filter { it.kind !in hidden }
    }

    override fun search(query: String): List<SearchHit> =
        SearchMatch.rank(routes, query) { listOf(it.title, RouteCatalog.title(it.kind)) }.map {
            SearchHit("route:${it.kind.raw}:${it.title}", it.title, icon = RouteCatalog.icon(it.kind), route = it)
        }
}

data class LocalRepositoryItem(val name: String, val description: String = "", val publishedAs: String = "")

class RepositoriesSearchSource(
    private val load: () -> List<Repository>,
    private val local: () -> List<LocalRepositoryItem> = { emptyList() },
) : GlobalSearchSource {
    override val kind = SearchBucketKind.Repositories

    override fun search(query: String): List<SearchHit> {
        val device = local()
        val repos = try {
            load()
        } catch (error: Exception) {
            if (device.isEmpty()) throw error
            emptyList()
        }
        val network = repos.map { "${it.owner}/${it.name}".lowercase() }.toSet()
        val candidates = repos.map { repo ->
            listOf(repo.name, "${repo.owner}/${repo.name}", repo.description) to SearchHit(
                "repo:${repo.owner}/${repo.name}", repo.name, subtitle(repo.owner, repo.description),
                RouteCatalog.icon(Route.Kind.Repo), Route.Repo(RepoRef(repo.owner, repo.name), null),
            )
        } + device.filter { it.publishedAs.lowercase() !in network }.map { item ->
            listOf(item.name, item.publishedAs, item.description) to SearchHit(
                "local:${item.name}", item.name, subtitle("On this device", item.description),
                RouteCatalog.icon(Route.Kind.LocalRepo), Route.LocalRepo(item.name),
            )
        }
        return SearchMatch.rank(candidates, query) { it.first }.map { it.second }
    }

    private fun subtitle(place: String, description: String): String = if (description.isEmpty()) place else "$place · $description"

    companion object {
        fun named(query: String, catalog: List<Repository>, limit: Int): List<RepoRef> {
            val names = { repo: Repository -> listOf(repo.name, "${repo.owner}/${repo.name}") }
            val close = catalog.filter { repo -> names(repo).any { (SearchMatch.score(it, query) ?: Int.MAX_VALUE) <= 1 } }
            return SearchMatch.rank(close, query, names).take(limit).map { RepoRef(it.owner, it.name) }
        }
    }
}

data class SearchableItem(val number: Int, val title: String, val body: String = "", val status: String = "", val isPull: Boolean)

class IssuesSearchSource(
    pulls: Boolean,
    private val repos: (String) -> List<RepoRef>,
    private val items: (RepoRef) -> List<SearchableItem>,
) : GlobalSearchSource {
    override val kind = if (pulls) SearchBucketKind.Pulls else SearchBucketKind.Issues

    override fun search(query: String): List<SearchHit> {
        val wantsPulls = kind == SearchBucketKind.Pulls
        val hits = ArrayList<SearchHit>()
        val number = query.trim().trim('#', ' ').toIntOrNull()
        for (repo in repos(query)) {
            val wanted = runCatching { items(repo) }.getOrDefault(emptyList()).filter { it.isPull == wantsPulls }
            val ranked = SearchMatch.rank(wanted, query) { listOf(it.title, it.body) }
            val exact = if (number != null) wanted.filter { it.number == number } else emptyList()
            for (item in exact + ranked.filter { it !in exact }) {
                val route = if (wantsPulls) Route.Pull(repo, item.number, PullTab.Conversation) else Route.Issue(repo, item.number)
                val status = if (item.status.isEmpty()) "" else " · ${item.status}"
                hits += SearchHit(
                    "${kind.raw}:${repo.slug}#${item.number}", item.title, "${repo.slug} #${item.number}$status",
                    RouteCatalog.icon(route.kind), route,
                )
            }
        }
        return hits
    }
}

data class CommitItem(val sha: String, val subject: String, val author: String = "")

class CommitsSearchSource(
    private val repos: (String) -> List<RepoRef>,
    private val commits: (RepoRef) -> List<CommitItem>,
) : GlobalSearchSource {
    override val kind = SearchBucketKind.Commits

    override fun search(query: String): List<SearchHit> {
        val needle = query.trim().lowercase()
        if (needle.length < 2) return emptyList()
        val hex = needle.length >= 4 && needle.all { it in '0'..'9' || it in 'a'..'f' }
        val hits = ArrayList<SearchHit>()
        for (repo in repos(query)) {
            val list = runCatching { commits(repo) }.getOrDefault(emptyList()).filter { it.sha.isNotEmpty() }
            val bySha = if (hex) list.filter { it.sha.lowercase().startsWith(needle) } else emptyList()
            val ranked = SearchMatch.rank(list, query) { listOf(it.subject, it.author) }
            for (item in bySha + ranked.filter { it !in bySha }) {
                val short = item.sha.take(7)
                val author = if (item.author.isEmpty()) "" else " · ${item.author}"
                hits += SearchHit(
                    "commit:${repo.slug}@${item.sha}", item.subject.ifEmpty { short }, "${repo.slug} $short$author",
                    RouteCatalog.icon(Route.Kind.Commit), Route.Commit(repo, item.sha),
                )
            }
        }
        return hits
    }
}

data class CodeMatchItem(val path: String, val line: Int, val text: String)

class CodeSearchSource(
    private val repos: (String) -> List<RepoRef>,
    private val searchRepo: (RepoRef, String) -> List<CodeMatchItem>,
) : GlobalSearchSource {
    override val kind = SearchBucketKind.Code

    override fun search(query: String): List<SearchHit> {
        if (query.trim().length < 2) return emptyList()
        val hits = ArrayList<SearchHit>()
        for (repo in repos(query)) {
            for (match in runCatching { searchRepo(repo, query) }.getOrDefault(emptyList())) {
                val line = if (match.line > 0) ":${match.line}" else ""
                hits += SearchHit(
                    "code:${repo.slug}:${match.path}:${match.line}", match.path, "${repo.slug}$line ${match.text.trim()}",
                    "description", Route.File(repo, "", match.path),
                )
            }
        }
        return hits
    }
}

class PeopleSearchSource(
    private val known: () -> List<String> = { emptyList() },
    private val accounts: (String) -> List<String> = { emptyList() },
) : GlobalSearchSource {
    override val kind = SearchBucketKind.People

    override fun search(query: String): List<SearchHit> {
        val name = query.trim().removePrefix("@")
        val valid = name.length >= 2 && name.all { it.isLetterOrDigit() || it in "-_." }
        val found = if (valid) accounts(name) else emptyList()
        val names = SearchMatch.rank(known() + found, name) { listOf(it) }.filter { it.isNotEmpty() }.distinctBy { it.lowercase() }
        return names.map { SearchHit("person:${it.lowercase()}", "@$it", icon = "account_circle", route = Route.Profile(it)) }
    }
}

class ChatSearchSource(private val conversations: () -> List<String>) : GlobalSearchSource {
    override val kind = SearchBucketKind.Chat

    override fun search(query: String): List<SearchHit> =
        SearchMatch.rank(conversations(), query) { listOf(it) }.map { conversation ->
            val route = if (conversation.startsWith("@")) Route.Dm(conversation.drop(1)) else Route.Channel(conversation)
            SearchHit("chat:$conversation", conversation, icon = RouteCatalog.icon(route.kind), route = route)
        }
}

object GlobalSearch {
    const val BUCKET_LIMIT = 5
    const val SEE_ALL_LIMIT = 50

    fun activeSources(
        sources: List<GlobalSearchSource>,
        scope: GlobalSearchScope,
        expanded: SearchBucketKind?,
    ): List<GlobalSearchSource> = sources.filter {
        (scope == GlobalSearchScope.All || it.kind.scope == scope) && (expanded == null || it.kind == expanded)
    }

    fun buckets(results: Map<SearchBucketKind, List<SearchHit>>, expanded: SearchBucketKind?): List<SearchBucket> {
        val limit = if (expanded == null) BUCKET_LIMIT else SEE_ALL_LIMIT
        return SearchBucketKind.entries.mapNotNull { kind ->
            val hits = results[kind]
            if (hits.isNullOrEmpty()) null else SearchBucket(kind, hits.take(limit), hits.size)
        }
    }

    fun runBlocking(
        sources: List<GlobalSearchSource>,
        query: String,
        scope: GlobalSearchScope,
        expanded: SearchBucketKind? = null,
    ): Pair<List<SearchBucket>, List<SearchBucketKind>> {
        val text = query.trim()
        if (text.isEmpty()) return emptyList<SearchBucket>() to emptyList()
        val results = LinkedHashMap<SearchBucketKind, MutableList<SearchHit>>()
        val failed = ArrayList<SearchBucketKind>()
        for (source in activeSources(sources, scope, expanded)) {
            try {
                results.getOrPut(source.kind) { ArrayList() }.addAll(source.search(text))
            } catch (_: Exception) {
                failed += source.kind
            }
        }
        return buckets(results, expanded) to failed.sortedBy { it.raw }
    }
}

data class SavedSearch(val query: String, val scope: GlobalSearchScope, val lastCount: Int = 0, val lastRunAtMs: Long = 0) {
    val id: String get() = "${scope.raw}:${query.lowercase()}"
    val route: Route get() = Route.Search(query, scope.routeScope)
}

data class RecentSearch(val query: String, val scope: GlobalSearchScope) {
    val id: String get() = "${scope.raw}:${query.lowercase()}"
}

class SavedSearchStore(private val store: PrefStore?, private val nowMs: () -> Long = System::currentTimeMillis) {
    companion object {
        const val SAVED_KEY = "fm.search.saved.v1"
        const val RECENT_KEY = "fm.search.recent.v1"
        const val MAX_SAVED = 20
        const val MAX_RECENT = 10
    }

    var saved: List<SavedSearch> = readSaved()
        private set
    var recent: List<RecentSearch> = readRecent()
        private set

    fun contains(query: String, scope: GlobalSearchScope): Boolean {
        val id = SavedSearch(query, scope).id
        return saved.any { it.id == id }
    }

    fun save(query: String, scope: GlobalSearchScope, count: Int) {
        if (query.isEmpty()) return
        val entry = SavedSearch(query, scope, count, nowMs())
        saved = (listOf(entry) + saved.filter { it.id != entry.id }).take(MAX_SAVED)
        persist()
    }

    fun remove(search: SavedSearch) {
        saved = saved.filter { it.id != search.id }
        persist()
    }

    fun recordRun(query: String, scope: GlobalSearchScope, count: Int) {
        val id = SavedSearch(query, scope).id
        if (saved.none { it.id == id }) return
        saved = saved.map { if (it.id == id) it.copy(lastCount = count, lastRunAtMs = nowMs()) else it }
        persist()
    }

    fun commit(query: String, scope: GlobalSearchScope) {
        val trimmed = query.trim()
        if (trimmed.isEmpty()) return
        val entry = RecentSearch(trimmed, scope)
        recent = (listOf(entry) + recent.filter { it.id != entry.id }).take(MAX_RECENT)
        persist()
    }

    fun clearRecent() {
        recent = emptyList()
        persist()
    }

    private fun persist() {
        val prefs = store ?: return
        prefs.putString(
            SAVED_KEY,
            Json.stringify(saved.map { mapOf("query" to it.query, "scope" to it.scope.raw, "lastCount" to it.lastCount.toLong(), "lastRunAt" to it.lastRunAtMs) }),
        )
        prefs.putString(RECENT_KEY, Json.stringify(recent.map { mapOf("query" to it.query, "scope" to it.scope.raw) }))
    }

    private fun readList(key: String): List<Map<String, Any?>> {
        val text = store?.getString(key) ?: return emptyList()
        return try {
            (Json.parse(text) as? List<*>).orEmpty().map { it.asJsonMap() }
        } catch (_: Exception) {
            emptyList()
        }
    }

    private fun readSaved(): List<SavedSearch> = readList(SAVED_KEY).mapNotNull {
        val scope = GlobalSearchScope.of(it.str("scope")) ?: return@mapNotNull null
        SavedSearch(it.str("query"), scope, it.int("lastCount"), it.long("lastRunAt"))
    }.filter { it.query.isNotEmpty() }.take(MAX_SAVED)

    private fun readRecent(): List<RecentSearch> = readList(RECENT_KEY).mapNotNull {
        val scope = GlobalSearchScope.of(it.str("scope")) ?: return@mapNotNull null
        RecentSearch(it.str("query"), scope)
    }.filter { it.query.isNotEmpty() }.take(MAX_RECENT)
}
