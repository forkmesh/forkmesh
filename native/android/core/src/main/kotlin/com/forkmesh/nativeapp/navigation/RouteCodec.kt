package com.forkmesh.nativeapp.navigation

import com.forkmesh.nativeapp.json.Json
import java.util.Base64

object RouteCodec {
    const val MAX_TOKEN_LENGTH = 2048

    fun toMap(route: Route): Map<String, Any?> {
        val out = linkedMapOf<String, Any?>("k" to route.kind.raw)
        fun put(key: String, value: Any?) {
            if (value != null) out[key] = value
        }
        route.repoRef?.let { if (route !is Route.Compose) put("repo", it.slug) }
        when (route) {
            is Route.Agents -> put("filter", route.filter.raw)
            is Route.Agent -> put("id", route.id)
            is Route.Approval -> put("id", route.id)
            is Route.Compose -> route.target.let {
                put("repo", it.repo?.slug)
                put("issue", it.issue?.toLong())
                put("pull", it.pull?.toLong())
                put("agent", it.agentId)
                put("channel", it.channel)
                put("text", it.text)
            }
            is Route.AgentLogin -> put("target", route.target)
            is Route.AgentWatch -> put("filter", route.filter.raw)
            is Route.LaunchedAgent -> put("session", route.sessionId)
            is Route.Teammate -> put("id", route.id)
            is Route.TeammateEditor -> put("id", route.id)
            is Route.Inbox -> put("filter", route.filter.raw)
            is Route.Repos -> put("scope", route.scope.raw)
            is Route.Repo -> put("section", route.section?.raw)
            is Route.Tree -> { put("ref", route.ref); put("path", route.path) }
            is Route.File -> { put("ref", route.ref); put("path", route.path) }
            is Route.FileHistory -> { put("ref", route.ref); put("path", route.path) }
            is Route.CodeMap -> put("ref", route.ref)
            is Route.Commits -> put("ref", route.ref)
            is Route.Commit -> put("sha", route.sha)
            is Route.Range -> { put("base", route.base); put("head", route.head) }
            is Route.Compare -> { put("base", route.base); put("head", route.head) }
            is Route.Conflicts -> put("branch", route.branch)
            is Route.Issue -> put("number", route.number.toLong())
            is Route.Pull -> { put("number", route.number.toLong()); put("tab", route.tab.raw) }
            is Route.Review -> put("number", route.number.toLong())
            is Route.Discussion -> put("number", route.number.toLong())
            is Route.Run -> put("id", route.id)
            is Route.Release -> put("tag", route.tag)
            is Route.Project -> put("id", route.id)
            is Route.RepoSettings -> put("section", route.section)
            is Route.LocalRepo -> put("id", route.id)
            is Route.QuickEdit -> { put("id", route.repoId); put("path", route.path) }
            is Route.Channel -> put("id", route.id)
            is Route.Thread -> { put("channel", route.channel); put("id", route.id) }
            is Route.Dm -> put("user", route.user)
            is Route.Discord -> { put("guild", route.guild); put("channel", route.channel) }
            is Route.Profile -> put("name", route.name)
            is Route.Search -> { put("q", route.query); put("scope", route.scope.raw) }
            is Route.Note -> put("id", route.id)
            is Route.Tasks -> put("filter", route.filter)
            is Route.Task -> put("id", route.id)
            is Route.Calendar -> { put("date", route.date); put("mode", route.mode.raw) }
            is Route.Event -> put("id", route.id)
            is Route.Desktop -> { put("id", route.id); put("section", route.section) }
            is Route.Host -> { put("name", route.name); put("section", route.section) }
            is Route.Provision -> put("id", route.id)
            is Route.Network -> put("section", route.section)
            is Route.Node -> put("name", route.name)
            is Route.Market -> put("filter", route.filter)
            is Route.Package -> put("id", route.id)
            is Route.Crm -> put("pipeline", route.pipeline)
            is Route.Contact -> put("id", route.id)
            is Route.CrmOrg -> put("id", route.id)
            is Route.Marketing -> put("section", route.section)
            is Route.Analytics -> put("section", route.section)
            is Route.Ledger -> put("section", route.section)
            is Route.Vault -> put("section", route.section)
            is Route.Cove -> put("id", route.id)
            is Route.Admin -> put("section", route.section)
            is Route.Settings -> put("section", route.section)
            is Route.Org -> put("name", route.name)
            is Route.ProjectAccess -> put("id", route.id)
            else -> Unit
        }
        return out
    }

    fun fromMap(map: Map<String, Any?>): Route? {
        val kind = Route.Kind.of(map["k"] as? String) ?: return null
        fun s(key: String): String? = map[key] as? String
        fun req(key: String): String? = s(key)
        fun n(key: String): Int? = (map[key] as? Number)?.toInt()
        val repo = RepoRef.parse(s("repo"))
        fun r(): RepoRef? = repo
        return when (kind) {
            Route.Kind.Agents -> Route.Agents(AgentFilter.of(s("filter")) ?: AgentFilter.All)
            Route.Kind.Agent -> req("id")?.let(Route::Agent)
            Route.Kind.Approval -> req("id")?.let(Route::Approval)
            Route.Kind.Compose -> Route.Compose(
                ComposeTarget(repo, n("issue"), n("pull"), s("agent"), s("channel"), s("text")),
            )
            Route.Kind.AgentLogin -> req("target")?.let(Route::AgentLogin)
            Route.Kind.AgentWatch -> Route.AgentWatch(AgentFilter.of(s("filter")) ?: AgentFilter.All)
            Route.Kind.AllAgents -> Route.AllAgents
            Route.Kind.LaunchedAgent -> r()?.let { rr -> req("session")?.let { Route.LaunchedAgent(rr, it) } }
            Route.Kind.Teammates -> Route.Teammates
            Route.Kind.Teammate -> req("id")?.let(Route::Teammate)
            Route.Kind.TeammateEditor -> Route.TeammateEditor(s("id"))
            Route.Kind.Shortcuts -> Route.Shortcuts(repo)
            Route.Kind.Inbox -> Route.Inbox(InboxFilter.of(s("filter")) ?: InboxFilter.All)
            Route.Kind.PingSettings -> Route.PingSettings
            Route.Kind.Repos -> Route.Repos(RepoScope.of(s("scope")) ?: RepoScope.All)
            Route.Kind.Repo -> r()?.let { Route.Repo(it, RepoSection.of(s("section"))) }
            Route.Kind.Tree -> r()?.let { Route.Tree(it, s("ref") ?: "", s("path") ?: "") }
            Route.Kind.File -> r()?.let { Route.File(it, s("ref") ?: "", s("path") ?: "") }
            Route.Kind.FileHistory -> r()?.let { Route.FileHistory(it, s("ref") ?: "", s("path") ?: "") }
            Route.Kind.CodeMap -> r()?.let { Route.CodeMap(it, s("ref") ?: "") }
            Route.Kind.Commits -> r()?.let { Route.Commits(it, s("ref") ?: "") }
            Route.Kind.Commit -> r()?.let { rr -> req("sha")?.let { Route.Commit(rr, it) } }
            Route.Kind.Range -> r()?.let { Route.Range(it, s("base") ?: "", s("head") ?: "") }
            Route.Kind.Compare -> r()?.let { Route.Compare(it, s("base") ?: "", s("head") ?: "") }
            Route.Kind.Conflicts -> r()?.let { Route.Conflicts(it, s("branch") ?: "") }
            Route.Kind.Issue -> r()?.let { rr -> n("number")?.let { Route.Issue(rr, it) } }
            Route.Kind.NewIssue -> r()?.let(Route::NewIssue)
            Route.Kind.Board -> r()?.let(Route::Board)
            Route.Kind.Pull -> r()?.let { rr -> n("number")?.let { Route.Pull(rr, it, PullTab.of(s("tab")) ?: PullTab.Conversation) } }
            Route.Kind.Review -> r()?.let { rr -> n("number")?.let { Route.Review(rr, it) } }
            Route.Kind.Discussion -> r()?.let { rr -> n("number")?.let { Route.Discussion(rr, it) } }
            Route.Kind.Run -> r()?.let { rr -> req("id")?.let { Route.Run(rr, it) } }
            Route.Kind.Release -> r()?.let { rr -> req("tag")?.let { Route.Release(rr, it) } }
            Route.Kind.Project -> r()?.let { rr -> req("id")?.let { Route.Project(rr, it) } }
            Route.Kind.Milestones -> r()?.let(Route::Milestones)
            Route.Kind.RepoSettings -> r()?.let { Route.RepoSettings(it, s("section")) }
            Route.Kind.LocalRepos -> Route.LocalRepos
            Route.Kind.LocalRepo -> req("id")?.let(Route::LocalRepo)
            Route.Kind.QuickEdit -> req("id")?.let { Route.QuickEdit(it, s("path") ?: "") }
            Route.Kind.Chat -> Route.Chat
            Route.Kind.Channel -> req("id")?.let(Route::Channel)
            Route.Kind.Thread -> req("channel")?.let { c -> req("id")?.let { Route.Thread(c, it) } }
            Route.Kind.Dm -> req("user")?.let(Route::Dm)
            Route.Kind.Discord -> req("guild")?.let { Route.Discord(it, s("channel")) }
            Route.Kind.Profile -> req("name")?.let(Route::Profile)
            Route.Kind.People -> Route.People
            Route.Kind.Docs -> Route.Docs
            Route.Kind.Search -> Route.Search(s("q") ?: "", SearchScope.of(s("scope")) ?: SearchScope.Everything)
            Route.Kind.Notes -> Route.Notes
            Route.Kind.Note -> Route.Note(s("id") ?: "")
            Route.Kind.Tasks -> Route.Tasks(s("filter"))
            Route.Kind.Task -> req("id")?.let(Route::Task)
            Route.Kind.Calendar -> Route.Calendar(s("date"), CalendarMode.of(s("mode")) ?: CalendarMode.Month)
            Route.Kind.Event -> req("id")?.let(Route::Event)
            Route.Kind.Timers -> Route.Timers
            Route.Kind.Desktops -> Route.Desktops
            Route.Kind.Desktop -> req("id")?.let { Route.Desktop(it, s("section")) }
            Route.Kind.Hosts -> Route.Hosts
            Route.Kind.Host -> req("name")?.let { Route.Host(it, s("section")) }
            Route.Kind.Provision -> req("id")?.let(Route::Provision)
            Route.Kind.Network -> Route.Network(s("section"))
            Route.Kind.Node -> req("name")?.let(Route::Node)
            Route.Kind.Market -> Route.Market(s("filter"))
            Route.Kind.Package -> req("id")?.let(Route::Package)
            Route.Kind.Crm -> Route.Crm(s("pipeline"))
            Route.Kind.Contact -> req("id")?.let(Route::Contact)
            Route.Kind.CrmOrg -> req("id")?.let(Route::CrmOrg)
            Route.Kind.Marketing -> Route.Marketing(s("section"))
            Route.Kind.Analytics -> Route.Analytics(s("section"))
            Route.Kind.Competitors -> Route.Competitors
            Route.Kind.Ads -> Route.Ads
            Route.Kind.Ledger -> Route.Ledger(s("section"))
            Route.Kind.Vault -> Route.Vault(s("section"))
            Route.Kind.Coves -> Route.Coves(repo)
            Route.Kind.Cove -> req("id")?.let(Route::Cove)
            Route.Kind.Calories -> Route.Calories
            Route.Kind.GlassesAlert -> Route.GlassesAlert
            Route.Kind.FaceChanger -> Route.FaceChanger
            Route.Kind.AppStoreStatus -> Route.AppStoreStatus
            Route.Kind.LocalAI -> Route.LocalAI
            Route.Kind.Activity -> Route.Activity
            Route.Kind.Stats -> Route.Stats
            Route.Kind.Status -> Route.Status
            Route.Kind.Admin -> Route.Admin(s("section"))
            Route.Kind.Settings -> Route.Settings(s("section"))
            Route.Kind.Orgs -> Route.Orgs
            Route.Kind.Org -> req("name")?.let(Route::Org)
            Route.Kind.InstalledApps -> Route.InstalledApps
            Route.Kind.Projects -> Route.Projects
            Route.Kind.ProjectAccess -> req("id")?.let(Route::ProjectAccess)
            Route.Kind.Tour -> Route.Tour
            Route.Kind.UpdateRequired -> Route.UpdateRequired
        }
    }

    fun encode(route: Route): String = Json.stringify(toMap(route))

    fun decode(text: String?): Route? {
        if (text.isNullOrEmpty()) return null
        val parsed = try {
            Json.parse(text)
        } catch (_: Exception) {
            return null
        }
        @Suppress("UNCHECKED_CAST")
        return (parsed as? Map<String, Any?>)?.let(::fromMap)
    }

    fun token(route: Route): String? {
        val token = Base64.getUrlEncoder().withoutPadding().encodeToString(encode(route).toByteArray(Charsets.UTF_8))
        return token.takeIf { it.length <= MAX_TOKEN_LENGTH }
    }

    fun fromToken(token: String?): Route? {
        if (token.isNullOrEmpty() || token.length > MAX_TOKEN_LENGTH) return null
        val bytes = try {
            Base64.getUrlDecoder().decode(token)
        } catch (_: IllegalArgumentException) {
            return null
        }
        return decode(String(bytes, Charsets.UTF_8))
    }

    fun encodeList(routes: List<Route>): String = Json.stringify(routes.map(::toMap))

    fun decodeList(text: String?): List<Route> {
        if (text.isNullOrEmpty()) return emptyList()
        val parsed = try {
            Json.parse(text)
        } catch (_: Exception) {
            return emptyList()
        }
        @Suppress("UNCHECKED_CAST")
        return (parsed as? List<Any?>)?.mapNotNull { (it as? Map<String, Any?>)?.let(::fromMap) } ?: emptyList()
    }
}

object SurfaceLink {
    const val SCHEME = "forkmesh"
    const val HOST = "surface"
    const val ROUTE_ITEM = "route"

    fun url(route: Route): String? = RouteCodec.token(route)?.let { "$SCHEME://$HOST/open?$ROUTE_ITEM=$it" }

    fun isSurfaceLink(url: String): Boolean {
        val lowered = url.trim().lowercase()
        return lowered.startsWith("$SCHEME://$HOST/") || lowered == "$SCHEME://$HOST"
    }

    fun route(url: String): Route? {
        if (!isSurfaceLink(url)) return null
        val query = url.substringAfter('?', "")
        val token = query.split('&').firstOrNull { it.startsWith("$ROUTE_ITEM=") }?.substringAfter('=')
        return RouteCodec.fromToken(token)
    }
}
