package com.forkmesh.nativeapp.navigation

import java.net.URI
import java.net.URLDecoder
import java.nio.charset.StandardCharsets

data class ParsedLink(
    val project: String? = null,
    val stack: List<Route>,
    val explicitProject: Boolean = true,
) {
    val destination: Route? get() = stack.lastOrNull()
}

object RouteParser {
    const val PRODUCTION_PROJECT = "app.forkmesh.com"
    const val CUSTOM_SCHEME = "forkmesh"

    val appLinkHosts: List<String> = listOf("app.forkmesh.com", "forkmesh.com")
    val retiredHosts: Map<String, String> = mapOf(
        "app.next.forkmesh.com" to PRODUCTION_PROJECT,
        "next.forkmesh.com" to PRODUCTION_PROJECT,
    )
    val universalLinkHosts: List<String> get() = appLinkHosts + retiredHosts.keys

    private val reservedOwners = setOf(
        "api", "dashboard", "assets", "static", ".well-known", "docs", "login", "logout", "signup", "auth",
        "admin", "world", "legal", "blog", "download", "downloads", "pricing", "help", "raw", "ws", "org", "orgs",
        "settings", "status", "ledger", "market", "search", "explore", "new", "privacy", "terms", "oauth",
    )
    private val safeSegment = Regex("^[A-Za-z0-9._-]+$")
    private val commitHash = Regex("^[0-9a-f]{7,64}$")

    fun isUniversalLinkHost(host: String): Boolean = host.lowercase() in universalLinkHosts

    fun projectId(host: String): String {
        val lowered = host.lowercase()
        retiredHosts[lowered]?.let { return it }
        if (lowered == "forkmesh.com") return PRODUCTION_PROJECT
        return if (lowered.startsWith("app.")) lowered else "app.$lowered"
    }

    fun browserOnly(url: String): Boolean {
        val uri = try {
            URI(url.trim())
        } catch (_: Exception) {
            return false
        }
        if (uri.scheme?.lowercase() != "https") return false
        val host = uri.host ?: return false
        if (!isUniversalLinkHost(host)) return false
        val parts = segments(uri.rawPath ?: "").map { it.lowercase() }
        return parts.firstOrNull() == "oauth" || (parts.size > 2 && parts[0] == "dashboard" && parts[1] == "apps")
    }

    fun parse(url: String, me: String = ""): ParsedLink? {
        val uri = try {
            URI(url.trim())
        } catch (_: Exception) {
            return null
        }
        val scheme = uri.scheme?.lowercase() ?: return null
        val query = queryItems(uri.rawQuery)
        val explicitProject = query["project"]?.trim()?.lowercase()?.takeIf { it.isNotEmpty() }
        val pathSegments = segments(uri.rawPath ?: "")
        if (scheme == CUSTOM_SCHEME) {
            val host = uri.host ?: uri.rawAuthority ?: return null
            if (host.isEmpty()) return null
            val stack = customRoutes(listOf(host) + pathSegments, query, me) ?: return null
            return ParsedLink(explicitProject, stack)
        }
        if (scheme != "https") return null
        val host = uri.host ?: return null
        if (!isUniversalLinkHost(host)) return null
        val stack = routes(pathSegments, query, me) ?: return null
        return ParsedLink(explicitProject ?: projectId(host), stack, explicitProject != null)
    }

    fun routes(segments: List<String>, query: Map<String, String> = emptyMap(), me: String = ""): List<Route>? {
        val first = segments.firstOrNull() ?: return null
        if (first.startsWith("@")) {
            val name = first.drop(1)
            if (segments.size != 1 || !isSafe(name)) return null
            return listOf(Route.Profile(name))
        }
        when (first.lowercase()) {
            "dashboard" -> return dashboard(segments.drop(1), query, me)
            "ledger.html" -> return if (segments.size == 1) listOf(Route.Ledger(null)) else null
            "market.html" -> return if (segments.size == 1) listOf(Route.Market(null)) else null
            "status.html" -> return if (segments.size == 1) listOf(Route.Status) else null
        }
        if (segments.size < 2) return null
        val owner = segments[0]
        val name = segments[1]
        if (!isSafe(owner) || !isSafe(name) || owner.lowercase() in reservedOwners) return null
        return repoRoutes(RepoRef(owner, name), segments.drop(2))
    }

    fun stack(route: Route): List<Route> = when (route) {
        is Route.Repo -> if (route.section == null) listOf(route) else listOf(Route.Repo(route.repo, null), route)
        is Route.Tree -> listOf(Route.Repo(route.repo, null), route)
        is Route.CodeMap -> listOf(Route.Repo(route.repo, null), route)
        is Route.Commits -> listOf(Route.Repo(route.repo, null), route)
        is Route.File -> listOf(Route.Repo(route.repo, null), Route.Tree(route.repo, route.ref, parentPath(route.path)), route)
        is Route.FileHistory -> listOf(Route.Repo(route.repo, null), Route.File(route.repo, route.ref, route.path), route)
        is Route.Commit -> sectionStack(route.repo, RepoSection.Commits, route)
        is Route.Range -> sectionStack(route.repo, RepoSection.Branches, route)
        is Route.Compare -> sectionStack(route.repo, RepoSection.Branches, route)
        is Route.Conflicts -> sectionStack(route.repo, RepoSection.Branches, route)
        is Route.Issue -> sectionStack(route.repo, RepoSection.Issues, route)
        is Route.NewIssue -> sectionStack(route.repo, RepoSection.Issues, route)
        is Route.Board -> sectionStack(route.repo, RepoSection.Issues, route)
        is Route.Pull -> sectionStack(route.repo, RepoSection.Pulls, route)
        is Route.Review ->
            sectionStack(route.repo, RepoSection.Pulls, Route.Pull(route.repo, route.number, PullTab.Conversation)) + route
        is Route.Discussion -> sectionStack(route.repo, RepoSection.Discussions, route)
        is Route.Run -> sectionStack(route.repo, RepoSection.Actions, route)
        is Route.Release -> sectionStack(route.repo, RepoSection.Releases, route)
        is Route.Project -> sectionStack(route.repo, RepoSection.Projects, route)
        is Route.Milestones -> sectionStack(route.repo, RepoSection.Projects, route)
        is Route.RepoSettings -> sectionStack(route.repo, RepoSection.Settings, route)
        is Route.Thread -> listOf(Route.Channel(route.channel), route)
        is Route.Teammate -> listOf(Route.Teammates, route)
        is Route.TeammateEditor ->
            if (!route.id.isNullOrEmpty()) listOf(Route.Teammates, Route.Teammate(route.id), route)
            else listOf(Route.Teammates, route)
        is Route.Note -> listOf(Route.Notes, route)
        is Route.Task -> listOf(Route.Tasks(null), route)
        is Route.Host, is Route.Provision -> listOf(Route.Hosts, route)
        is Route.Node -> listOf(Route.Network(null), route)
        is Route.Package -> listOf(Route.Market(null), route)
        is Route.Contact, is Route.CrmOrg -> listOf(Route.Crm(null), route)
        is Route.Org -> listOf(Route.Orgs, route)
        is Route.ProjectAccess -> listOf(Route.Projects, route)
        is Route.Desktop -> listOf(Route.Desktops, route)
        is Route.LocalRepo -> listOf(Route.LocalRepos, route)
        is Route.Approval -> listOf(Route.Inbox(InboxFilter.Approvals), route)
        else -> listOf(route)
    }

    private fun sectionStack(repo: RepoRef, section: RepoSection, route: Route): List<Route> =
        listOf(Route.Repo(repo, null), Route.Repo(repo, section), route)

    private fun parentPath(path: String): String = if ('/' in path) path.substringBeforeLast('/') else ""

    private fun repoRoutes(repo: RepoRef, rest: List<String>): List<Route>? {
        val root = Route.Repo(repo, null)
        val head = rest.firstOrNull() ?: return listOf(root)
        val tail = rest.drop(1)
        fun section(value: RepoSection): List<Route> = listOf(root, Route.Repo(repo, value))
        fun number(index: Int): Int? = tail.getOrNull(index)?.toIntOrNull()?.takeIf { it > 0 }
        return when (head) {
            "tree" -> {
                val ref = tail.firstOrNull()
                if (ref.isNullOrEmpty()) section(RepoSection.Code)
                else listOf(root, Route.Tree(repo, ref, tail.drop(1).joinToString("/")))
            }
            "blob" -> if (tail.size < 2) null else stack(Route.File(repo, tail[0], tail.drop(1).joinToString("/")))
            "issues" -> when {
                tail.firstOrNull() == "new" -> section(RepoSection.Issues) + Route.NewIssue(repo)
                number(0) != null -> section(RepoSection.Issues) + Route.Issue(repo, number(0)!!)
                tail.isEmpty() -> section(RepoSection.Issues)
                else -> null
            }
            "pulls", "pull" -> {
                val value = number(0)
                when {
                    value != null -> {
                        val tab = if (tail.size > 1) PullTab.of(tail[1]) ?: PullTab.Conversation else PullTab.Conversation
                        section(RepoSection.Pulls) + Route.Pull(repo, value, tab)
                    }
                    tail.isEmpty() -> section(RepoSection.Pulls)
                    else -> null
                }
            }
            "discussions" -> when {
                number(0) != null -> section(RepoSection.Discussions) + Route.Discussion(repo, number(0)!!)
                tail.isEmpty() -> section(RepoSection.Discussions)
                else -> null
            }
            "commit" -> {
                val sha = tail.firstOrNull()
                if (sha.isNullOrEmpty()) section(RepoSection.Commits) else section(RepoSection.Commits) + Route.Commit(repo, sha)
            }
            "commits" -> {
                val named = tail.joinToString("/")
                when {
                    named.isEmpty() -> section(RepoSection.Commits)
                    commitHash.matches(named) -> section(RepoSection.Commits) + Route.Commit(repo, named)
                    else -> listOf(root, Route.Commits(repo, named))
                }
            }
            "actions" ->
                if (tail.size >= 2 && tail[0] == "runs" && tail[1].isNotEmpty()) section(RepoSection.Actions) + Route.Run(repo, tail[1])
                else section(RepoSection.Actions)
            "releases" ->
                if (tail.size >= 2 && tail[0] == "tag") section(RepoSection.Releases) + Route.Release(repo, tail.drop(1).joinToString("/"))
                else section(RepoSection.Releases)
            "compare" -> {
                val parts = tail.joinToString("/").split("...")
                if (parts.size != 2 || parts[0].isEmpty() || parts[1].isEmpty()) section(RepoSection.Branches)
                else section(RepoSection.Branches) + Route.Compare(repo, parts[0], parts[1])
            }
            "settings" -> {
                val first = tail.firstOrNull()
                if (!first.isNullOrEmpty()) section(RepoSection.Settings) + Route.RepoSettings(repo, first)
                else section(RepoSection.Settings)
            }
            "projects" -> {
                val id = tail.firstOrNull()
                if (!id.isNullOrEmpty()) section(RepoSection.Projects) + Route.Project(repo, id) else section(RepoSection.Projects)
            }
            "milestones" -> section(RepoSection.Projects) + Route.Milestones(repo)
            "board" -> section(RepoSection.Issues) + Route.Board(repo)
            "shortcuts" -> if (tail.isEmpty()) listOf(Route.Shortcuts(repo)) else null
            else -> {
                val value = RepoSection.of(head)
                if (value == null || tail.isNotEmpty()) null else section(value)
            }
        }
    }

    private fun dashboard(rest: List<String>, query: Map<String, String>, me: String): List<Route>? {
        val page = rest.firstOrNull() ?: return null
        val first = rest.getOrNull(1)?.takeIf { it.isNotEmpty() }
        return when (page) {
            "chat" -> listOf(Route.Chat)
            "notes" -> if (first != null) listOf(Route.Notes, Route.Note(first)) else listOf(Route.Notes)
            "tasks" -> if (first != null) listOf(Route.Tasks(null), Route.Task(first)) else listOf(Route.Tasks(query["filter"]))
            "calendar" -> listOf(Route.Calendar(query["date"], CalendarMode.of(query["mode"]) ?: CalendarMode.Month))
            "settings" -> listOf(Route.Settings(first))
            "network" -> listOf(Route.Network(first))
            "ads" -> listOf(Route.Ads)
            "repos" -> listOf(Route.Repos(RepoScope.All))
            "profile" -> if (me.isEmpty()) listOf(Route.Settings("profile")) else listOf(Route.Profile(me))
            "agents" -> listOf(Route.Agents(AgentFilter.All))
            "inbox", "pings", "notifications" -> listOf(Route.Inbox(InboxFilter.All))
            "apps" -> if (first == null) listOf(Route.InstalledApps) else null
            else -> null
        }
    }

    private fun customRoutes(all: List<String>, query: Map<String, String>, me: String): List<Route>? {
        val head = all.firstOrNull()?.lowercase() ?: return null
        val tail = all.drop(1)
        val rest = tail.joinToString("/")
        return when (head) {
            "agents" ->
                if (rest.isNotEmpty()) listOf(Route.Agents(AgentFilter.All), Route.Agent(rest))
                else listOf(Route.Agents(AgentFilter.of(query["filter"]) ?: AgentFilter.All))
            "approvals" ->
                if (rest.isNotEmpty()) listOf(Route.Inbox(InboxFilter.Approvals), Route.Approval(rest))
                else listOf(Route.Inbox(InboxFilter.Approvals))
            "compose" -> listOf(
                Route.Compose(
                    ComposeTarget(
                        repo = RepoRef.parse(query["repo"]),
                        issue = query["issue"]?.toIntOrNull(),
                        pull = query["pull"]?.toIntOrNull(),
                        agentId = query["agent"]?.ifEmpty { null },
                        channel = query["channel"]?.ifEmpty { null },
                        text = query["text"]?.ifEmpty { null },
                    ),
                ),
            )
            "search" -> listOf(Route.Search(query["q"] ?: "", searchScope(query["scope"])))
            "inbox" -> listOf(Route.Inbox(InboxFilter.of(tail.firstOrNull()) ?: InboxFilter.All))
            "cove" ->
                if (tail.size == 3 && tail.all { it.isNotBlank() }) listOf(Route.Coves(null), Route.Cove(rest))
                else listOf(Route.Coves(null))
            "coves" -> listOf(Route.Coves(null))
            "home" -> listOf(AppTab.barOrder.first().root)
            else -> dashboard(all, query, me) ?: routes(all, query, me) ?: named(head, tail.firstOrNull()) ?: listOf(AppTab.barOrder.first().root)
        }
    }

    private fun named(head: String, first: String?): List<Route>? = when (head) {
        "vault" -> listOf(Route.Vault(first))
        "calories" -> listOf(Route.Calories)
        "glasses-alert" -> listOf(Route.GlassesAlert)
        "activity" -> listOf(Route.Activity)
        "timers" -> listOf(Route.Timers)
        "hosts" -> listOf(Route.Hosts)
        "desktops" -> listOf(Route.Desktops)
        "market" -> listOf(Route.Market(first))
        "ledger" -> listOf(Route.Ledger(first))
        "stats" -> listOf(Route.Stats)
        "status" -> listOf(Route.Status)
        "orgs" -> listOf(Route.Orgs)
        "projects" -> listOf(Route.Projects)
        "teammates" -> listOf(Route.Teammates)
        "tour" -> listOf(Route.Tour)
        else -> null
    }

    fun searchScope(raw: String?): SearchScope {
        val lowered = raw?.lowercase()
        if (lowered.isNullOrEmpty() || lowered == "all") return SearchScope.Everything
        return SearchScope.of(lowered) ?: SearchScope.Everything
    }

    private fun queryItems(raw: String?): Map<String, String> {
        val result = LinkedHashMap<String, String>()
        if (raw.isNullOrEmpty()) return result
        for (pair in raw.split('&')) {
            if (pair.isEmpty()) continue
            val name = decode(pair.substringBefore('='))
            val value = if ('=' in pair) decode(pair.substringAfter('=')) else ""
            if (name !in result) result[name] = value
        }
        return result
    }

    private fun decode(text: String): String = try {
        URLDecoder.decode(text.replace("+", "%2B"), StandardCharsets.UTF_8)
    } catch (_: IllegalArgumentException) {
        text
    }

    private fun segments(encodedPath: String): List<String> =
        encodedPath.split('/').filter { it.isNotEmpty() }.map(::decode)

    private fun isSafe(segment: String): Boolean = segment != "." && segment != ".." && safeSegment.matches(segment)
}
