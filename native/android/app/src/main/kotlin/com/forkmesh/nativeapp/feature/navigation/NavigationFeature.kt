package com.forkmesh.nativeapp.feature.navigation

import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.chat.DirectMessageFrames
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountFeature
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.account.projects.ProjectsService
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.feature.agents.AgentOpsFeature
import com.forkmesh.nativeapp.feature.teammates.TeammatesFeature
import com.forkmesh.nativeapp.agents.ops.AgentListFilter
import com.forkmesh.nativeapp.feature.chat.ChatFeature
import com.forkmesh.nativeapp.feature.code.CodeFeature
import com.forkmesh.nativeapp.feature.code.collab.CollaborationScreens
import com.forkmesh.nativeapp.feature.hosts.HostsFeature
import com.forkmesh.nativeapp.feature.inbox.InboxFeature
import com.forkmesh.nativeapp.feature.market.MarketFeature
import com.forkmesh.nativeapp.feature.tools.ActivityScreen
import com.forkmesh.nativeapp.feature.tools.CaloriesScreen
import com.forkmesh.nativeapp.feature.tools.NoteEditorScreen
import com.forkmesh.nativeapp.feature.tools.NotesScreen
import com.forkmesh.nativeapp.feature.tools.TasksScreen
import com.forkmesh.nativeapp.feature.tools.vault.VaultScreen
import com.forkmesh.nativeapp.model.NotificationDeepLink
import com.forkmesh.nativeapp.model.RepoDetailTab
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.market.PackageMenu
import com.forkmesh.nativeapp.navigation.AppTab
import com.forkmesh.nativeapp.navigation.ComposeTarget
import com.forkmesh.nativeapp.navigation.GuestGate
import com.forkmesh.nativeapp.navigation.ClientVersionGate
import com.forkmesh.nativeapp.navigation.LinkOutcome
import com.forkmesh.nativeapp.navigation.ParsedLink
import com.forkmesh.nativeapp.navigation.RepoRef
import com.forkmesh.nativeapp.navigation.RepoSection
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.navigation.RouteParser
import com.forkmesh.nativeapp.navigation.RoutePlacement
import com.forkmesh.nativeapp.navigation.RouterState
import com.forkmesh.nativeapp.navigation.SavedSearchStore
import com.forkmesh.nativeapp.navigation.ShellQuickAction
import com.forkmesh.nativeapp.navigation.SurfaceLink
import com.forkmesh.nativeapp.navigation.UpdateRequirement
import com.forkmesh.nativeapp.navigation.root
import com.forkmesh.nativeapp.navigation.title
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.platform.ProjectLifecycle
import com.forkmesh.nativeapp.shell.HomeShell
import com.forkmesh.nativeapp.ui.navigation.AppNavigation
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

object NavigationFeature {
    const val ACTION_SHORTCUT = "com.forkmesh.shortcut.OPEN"
    const val EXTRA_SHORTCUT = "shortcut"
    const val EXTRA_PAYLOAD = "payload"
    const val ACTION_ROUTE = "com.forkmesh.route.OPEN"
    const val EXTRA_ROUTE = "route"
    private const val ARCHIVE_KEY = "nav/archive.v1"

    class Registration(val owner: String, val build: (Route) -> Screen?)

    private val registry = LinkedHashMap<Route.Kind, Registration>()
    private val conflictList = ArrayList<Triple<Route.Kind, String, String>>()
    private val router = RouterState(accountReady = false)
    private val updateState = MutableStateFlow<UpdateRequirement?>(null)
    private val recentsState = MutableStateFlow<List<Route>>(emptyList())
    private var app: App? = null
    private var defaultsInstalled = false
    var interceptor: ((Route) -> Boolean)? = null

    val updateRequired: StateFlow<UpdateRequirement?> = updateState.asStateFlow()
    private val linkProblemState = MutableStateFlow<String?>(null)
    val linkProblem: StateFlow<String?> = linkProblemState.asStateFlow()

    fun reportLinkProblem(problem: String) {
        linkProblemState.value = problem
    }

    fun clearLinkProblem() {
        linkProblemState.value = null
    }
    val recents: StateFlow<List<Route>> = recentsState.asStateFlow()
    val conflicts: List<Triple<Route.Kind, String, String>> get() = conflictList

    fun register(kind: Route.Kind, owner: String = "", build: (Route) -> Screen?) {
        val previous = registry[kind]
        if (previous != null && previous.owner.isNotEmpty() && owner.isNotEmpty() && previous.owner != owner) {
            conflictList += Triple(kind, previous.owner, owner)
        }
        registry[kind] = Registration(owner, build)
    }

    fun unregister(kind: Route.Kind) {
        registry.remove(kind)
    }

    fun isRegistered(kind: Route.Kind): Boolean = registry.containsKey(kind)

    fun screen(route: Route): Screen {
        val guest = Services.get<AuthService>()?.isGuest?.value == true
        GuestGate.action(route, guest)?.let { return GuestGateScreen(route, it) }
        if (!guest) MarketFeature.reportOpened(route.kind)
        return registeredScreen(route)
    }

    fun registeredScreen(route: Route): Screen = (registry[route.kind]?.build?.invoke(route) ?: RoutePlaceholderScreen(route)).also { screen ->
        if (screen.pluginPackage == null) screen.pluginPackage = PackageMenu.packageFor(route.kind)
    }

    fun install(app: App) {
        this.app = app
        com.forkmesh.nativeapp.ui.kit.FmMessageHandoff.compose = { target -> compose(target) }
        if (!defaultsInstalled) {
            defaultsInstalled = true
            registerDefaults()
        }
        val prefs = Services.get<PrefStore>()
        val projectId = Services.get<ProjectsService>()?.directory?.active?.id.orEmpty()
        router.load(prefs?.getString(ARCHIVE_KEY))
        val pending = router.activate(projectId)
        recentsState.value = router.recents
        Services.put(SavedSearchStore::class.java, SavedSearchStore(prefs))
        val auth = Services.get<AuthService>()
        ProjectLifecycle.scope.launch {
            if (pending != null) {
                kotlinx.coroutines.yield()
                show(pending.stack)
            }
            auth?.hasAppAccess?.collect { ready ->
                val link = router.setAccountReady(ready)
                if (link != null) show(link.stack)
            }
        }
        ProjectLifecycle.onClose {
            prefs?.putString(ARCHIVE_KEY, router.archiveText())
            router.setAccountReady(false)
        }
        ProjectLifecycle.scope.launch { checkVersion() }
    }

    private suspend fun checkVersion() {
        val context = app ?: return
        val api = Services.get<ApiService>() ?: return
        val running = runCatching {
            context.packageManager.getPackageInfo(context.packageName, 0).versionName.orEmpty()
        }.getOrDefault("")
        val requirement = withContext(Dispatchers.IO) {
            try {
                ClientVersionGate.requirement(api.accountApiCall("GET", "/api/version"), running)
            } catch (error: com.forkmesh.nativeapp.model.OrgApiException) {
                val status = error.code.removePrefix("http_").toIntOrNull() ?: 0
                ClientVersionGate.requirement(status, running)
            } catch (_: Exception) {
                null
            }
        }
        updateState.value = requirement
    }

    fun shell(): HomeShell? = AppNavigation.navigator.stack.firstOrNull() as? HomeShell

    fun screenTitle(): String {
        val global = AppNavigation.navigator.stack.toList()
        val shell = global.firstOrNull() as? HomeShell ?: return global.lastOrNull()?.title ?: ""
        val tab = shell.selected
        val path = shell.navigator(tab).stack.toList().drop(1).map { it.title }
        return (listOf(tab.title) + path + global.drop(1).map { it.title }).filter { it.isNotEmpty() }.joinToString(" > ")
    }

    fun open(route: Route, placement: RoutePlacement = RoutePlacement.Automatic) {
        show(RouteParser.stack(route), placement)
    }

    fun openFromHub(route: Route) {
        remember(route)
        show(listOf(route), RoutePlacement.CurrentTab)
    }

    fun remember(route: Route) {
        router.remember(route)
        recentsState.value = router.recents
        Services.get<PrefStore>()?.putString(ARCHIVE_KEY, router.archiveText())
    }

    fun show(stack: List<Route>, placement: RoutePlacement = RoutePlacement.Automatic) {
        val last = stack.lastOrNull() ?: return
        if (interceptor?.invoke(last) == true) return
        if (intercepted(last)) return
        val global = AppNavigation.navigator
        val shell = shell()
        if (shell == null) {
            global.push(screen(last))
            return
        }
        global.popTo(shell)
        val plan = RouterState.plan(stack, placement, shell.selected, shell.routes(shell.selected)) ?: return
        shell.select(plan.tab, reselect = false)
        val tabNavigator = shell.navigator(plan.tab)
        val path = collapse(plan.path)
        if (plan.replace) {
            tabNavigator.popTo(tabNavigator.stack.first())
        } else {
            val existing = shell.routes(plan.tab)
            val shared = existing.zip(path).takeWhile { (a, b) -> a == b }.size
            val keep = path.drop(shared)
            keep.forEach { shell.push(plan.tab, it, screen(it)) }
            return
        }
        path.forEach { shell.push(plan.tab, it, screen(it)) }
    }

    private fun collapse(path: List<Route>): List<Route> {
        val last = path.lastOrNull() ?: return path
        val repo = last.repoRef ?: return path
        if (last is Route.Compose) return path
        return path.filterIndexed { index, route -> index == path.lastIndex || route.repoRef != repo }
    }

    private fun intercepted(route: Route): Boolean {
        val shell = shell()
        when (route) {
            is Route.Compose -> {
                if (shell == null) return false
                AppNavigation.navigator.popTo(shell)
                shell.openPrompt(route.target)
                return true
            }
            is Route.Channel, is Route.Dm, is Route.Thread -> {
                val conversation = when (route) {
                    is Route.Channel -> route.id
                    is Route.Dm -> if (route.user.startsWith("@") || DirectMessageFrames.isDirect(route.user)) route.user else "@${route.user}"
                    is Route.Thread -> route.channel
                    else -> ""
                }
                ChatFeature.openConversation(conversation)
                if (shell == null) return false
                AppNavigation.navigator.popTo(shell)
                shell.select(AppTab.Chat, reselect = false)
                shell.navigator(AppTab.Chat).let {
                    it.popTo(it.stack.first())
                    if (route is Route.Thread && route.id.isNotEmpty()) it.push(ChatFeature.threadScreen(route.channel, route.id))
                }
                return true
            }
            else -> {
                val tab = AppTab.entries.firstOrNull { it.root == route } ?: return false
                if (shell == null) return false
                AppNavigation.navigator.popTo(shell)
                shell.select(tab, reselect = false)
                shell.navigator(tab).let { it.popTo(it.stack.first()) }
                if (route is Route.Inbox) InboxFeature.selectFilter(route.filter)
                return true
            }
        }
    }

    fun openUri(context: Context, uri: String): Boolean {
        SurfaceLink.route(uri)?.let {
            open(it)
            return true
        }
        if (RouteParser.browserOnly(uri)) return openInBrowser(context, uri)
        val me = Services.get<AuthService>()?.session?.value?.nodeName.orEmpty()
        val link = RouteParser.parse(uri, me)
        if (link == null) {
            if (uri.startsWith("https://", ignoreCase = true) || uri.startsWith("http://", ignoreCase = true)) {
                return openInBrowser(context, uri)
            }
            return false
        }
        openLink(context, link, uri)
        return true
    }

    fun openLink(context: Context, link: ParsedLink, original: String = "") {
        val projects = Services.get<ProjectsService>()
        val known = projects?.directory?.projects?.map { it.id }?.toSet().orEmpty()
        when (val outcome = router.open(link, known)) {
            LinkOutcome.Opened -> show(link.stack)
            LinkOutcome.WaitingForSignIn, LinkOutcome.Ignored -> Unit
            is LinkOutcome.SwitchProject -> AppLifecycle.scope.launch {
                val result = projects?.switchTo(outcome.project)
                if (result?.ok != true) {
                    router.discardPending()
                    reportLinkProblem("Could not switch to ${outcome.project}.")
                }
            }
            is LinkOutcome.UnknownProject -> {
                reportLinkProblem("${outcome.project} is not one of your projects.")
                if (original.isNotEmpty()) openInBrowser(context, original, report = false)
            }
        }
    }

    fun openInBrowser(context: Context, url: String, report: Boolean = true): Boolean {
        val uri = Uri.parse(url)
        val intent = Intent(Intent.ACTION_VIEW, uri).addCategory(Intent.CATEGORY_BROWSABLE).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        val manager = context.packageManager
        val probe = Intent(Intent.ACTION_VIEW, Uri.parse("https://example.com/")).addCategory(Intent.CATEGORY_BROWSABLE)
        val handlers = if (Build.VERSION.SDK_INT >= 33) {
            manager.queryIntentActivities(probe, PackageManager.ResolveInfoFlags.of(PackageManager.MATCH_DEFAULT_ONLY.toLong()))
        } else {
            @Suppress("DEPRECATION")
            manager.queryIntentActivities(probe, PackageManager.MATCH_DEFAULT_ONLY)
        }
        val browser = handlers.map { it.activityInfo.packageName }.firstOrNull { it != context.packageName }
        if (browser != null) {
            intent.setPackage(browser)
        } else {
            intent.selector = Intent.makeMainSelectorActivity(Intent.ACTION_MAIN, Intent.CATEGORY_APP_BROWSER)
        }
        return try {
            context.startActivity(intent)
            true
        } catch (_: ActivityNotFoundException) {
            if (report) reportLinkProblem("No browser can open $url")
            false
        }
    }

    fun onIntent(navigator: Navigator, intent: Intent?): Boolean {
        val context = app ?: return false
        when (intent?.action) {
            Intent.ACTION_VIEW -> {
                val data = intent.dataString ?: return false
                intent.action = null
                return openUri(context, data)
            }
            ACTION_SHORTCUT -> {
                val action = ShellQuickAction.of(intent.getStringExtra(EXTRA_SHORTCUT))
                val route = action?.route(intent.getStringExtra(EXTRA_PAYLOAD).orEmpty())
                intent.action = null
                if (route != null) open(route)
                return true
            }
            ACTION_ROUTE -> {
                val route = com.forkmesh.nativeapp.navigation.RouteCodec.fromToken(intent.getStringExtra(EXTRA_ROUTE))
                intent.action = null
                if (route != null) open(route)
                return true
            }
        }
        return false
    }

    fun routeIntent(context: Context, route: Route): Intent? {
        val token = com.forkmesh.nativeapp.navigation.RouteCodec.token(route) ?: return null
        return Intent(context, com.forkmesh.nativeapp.MainActivity::class.java).apply {
            action = ACTION_ROUTE
            putExtra(EXTRA_ROUTE, token)
            flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP
        }
    }

    fun compose(target: ComposeTarget) = open(Route.Compose(target))

    private fun repository(ref: RepoRef): Repository = Repository(ref.owner, ref.name)

    private fun detailTab(section: RepoSection?): RepoDetailTab = when (section) {
        RepoSection.Issues -> RepoDetailTab.Issues
        RepoSection.Pulls -> RepoDetailTab.Pulls
        RepoSection.Discussions -> RepoDetailTab.Discussions
        RepoSection.Commits, RepoSection.Branches -> RepoDetailTab.Commits
        RepoSection.Mirrors -> RepoDetailTab.Mirrors
        RepoSection.Releases -> RepoDetailTab.Releases
        RepoSection.About -> RepoDetailTab.About
        RepoSection.Actions -> RepoDetailTab.Actions
        RepoSection.Agents -> RepoDetailTab.Agents
        RepoSection.Worktrees -> RepoDetailTab.Worktrees
        else -> RepoDetailTab.Code
    }

    private fun repoTarget(ref: RepoRef, tab: RepoDetailTab, number: Int = 0, reference: String = "", branch: String = ""): Screen {
        val repo = repository(ref)
        val link = NotificationDeepLink(repo, tab, number, reference, branch)
        return CodeFeature.repoDetailScreen(repo, tab, link)
    }

    private fun agentSession(id: String): Screen? {
        val parts = id.split('/', '#', ':')
        if (parts.size == 3) {
            val number = parts[2].toIntOrNull() ?: return null
            return AgentsFeature.sessionScreen(Repository(parts[0], parts[1]), number)
        }
        return null
    }

    private fun registerDefaults() {
        val base = "WS8"
        register(Route.Kind.AllAgents, base) { AgentsFeature.allAgentsScreen() }
        register(Route.Kind.Agent, base) { route -> agentSession((route as Route.Agent).id) ?: AgentsFeature.agentScreen(route.id) }
        register(Route.Kind.LaunchedAgent, base) { route ->
            route as Route.LaunchedAgent
            route.sessionId.toIntOrNull()?.let { AgentsFeature.sessionScreen(repository(route.repo), it) } ?: AgentsFeature.allAgentsScreen()
        }
        register(Route.Kind.Desktops, base) { AgentsFeature.localDesktopScreen() }
        register(Route.Kind.PingSettings, base) { AccountFeature.pingSettingsScreen() }
        register(Route.Kind.Repo, base) { route ->
            route as Route.Repo
            CodeFeature.repoDetailScreen(repository(route.repo), detailTab(route.section))
        }
        register(Route.Kind.File, base) { route -> (route as Route.File).let { CodeFeature.repoFileScreen(repository(it.repo), it.path, it.ref) } }
        register(Route.Kind.Tree, base) { route -> CodeFeature.repoDetailScreen(repository((route as Route.Tree).repo)) }
        register(Route.Kind.Commit, base) { route -> (route as Route.Commit).let { repoTarget(it.repo, RepoDetailTab.Commits, reference = it.sha) } }
        register(Route.Kind.Commits, base) { route -> (route as Route.Commits).let { repoTarget(it.repo, RepoDetailTab.Commits, branch = it.ref) } }
        register(Route.Kind.Compare, base) { route -> (route as Route.Compare).let { CodeFeature.branchDiffScreen(repository(it.repo), it.head, it.base) } }
        register(Route.Kind.Range, base) { route -> (route as Route.Range).let { CodeFeature.branchDiffScreen(repository(it.repo), it.head, it.base) } }
        register(Route.Kind.Issue, base) { route -> (route as Route.Issue).let { CollaborationScreens.issueDetailScreen(repository(it.repo), it.number) } }
        register(Route.Kind.NewIssue, base) { route -> CollaborationScreens.newIssueScreen(repository((route as Route.NewIssue).repo)) }
        register(Route.Kind.Pull, base) { route -> (route as Route.Pull).let { CollaborationScreens.pullDetailScreen(repository(it.repo), it.number) } }
        register(Route.Kind.Review, base) { route -> (route as Route.Review).let { CollaborationScreens.pullDetailScreen(repository(it.repo), it.number) } }
        register(Route.Kind.Discussion, base) { route ->
            (route as Route.Discussion).let { CollaborationScreens.discussionDetailScreen(repository(it.repo), it.number) }
        }
        register(Route.Kind.Run, base) { route ->
            (route as Route.Run).let { run ->
                val id = run.id.toIntOrNull() ?: 0
                if (id > 0) CodeFeature.actionRunScreen(repository(run.repo), id) else repoTarget(run.repo, RepoDetailTab.Actions)
            }
        }
        register(Route.Kind.Release, base) { route -> repoTarget((route as Route.Release).repo, RepoDetailTab.Releases) }
        register(Route.Kind.Notes, base) { NotesScreen() }
        register(Route.Kind.Note, base) { route -> (route as Route.Note).let { NoteEditorScreen(it.id.ifEmpty { null }, it.comment) } }
        register(Route.Kind.Tasks, base) { TasksScreen() }
        register(Route.Kind.Task, base) { TasksScreen() }
        register(Route.Kind.Vault, base) { VaultScreen() }
        register(Route.Kind.Calories, base) { CaloriesScreen() }
        register(Route.Kind.Activity, base) { ActivityScreen() }
        register(Route.Kind.Hosts, base) { HostsFeature.hostsScreen() }
        register(Route.Kind.Host, base) { route -> HostsFeature.hostScreen((route as Route.Host).name) }
        register(Route.Kind.Settings, base) { AccountFeature.settingsTab() }
        register(Route.Kind.Projects, base) { com.forkmesh.nativeapp.feature.account.projects.ProjectsScreen() }
        register(Route.Kind.Orgs, base) { AccountFeature.orgsScreen() }
        register(Route.Kind.Org, base) { route -> com.forkmesh.nativeapp.feature.account.orgs.OrgDetailScreen((route as Route.Org).name) }
        register(Route.Kind.InstalledApps, base) { AccountFeature.installedAppsScreen() }
        register(Route.Kind.Search, base) { route -> (route as Route.Search).let { GlobalSearchScreen(it.query, it.scope) } }
        register(Route.Kind.Tour, base) { TourScreen() }
        register(Route.Kind.UpdateRequired, base) { UpdateRequiredScreen() }
        register(Route.Kind.Inbox, base) { route -> InboxFeature.screen((route as Route.Inbox).filter) }
        register(Route.Kind.Approval, base) { route -> ApprovalRouteScreen((route as Route.Approval).id) }
        register(Route.Kind.Agents, base) { AgentsFeature.tab() }
        register(Route.Kind.Repos, base) { CodeFeature.tab() }
        register(Route.Kind.LocalRepo, base) { route ->
            val store = Services.get<com.forkmesh.nativeapp.localrepos.LocalRepoStore>()
            val record = store?.records?.firstOrNull { it.name == (route as Route.LocalRepo).id }
            if (store != null && record != null) com.forkmesh.nativeapp.feature.localrepos.LocalRepoScreen(store, record) else null
        }
        register(Route.Kind.Chat, base) { ChatFeature.tab() }
        registerAreaRoutes()
    }

    private fun registerAreaRoutes() {
        val ops = "WS10"
        register(Route.Kind.Approval, ops) { route -> AgentOpsFeature.approvalScreen((route as Route.Approval).id) }
        register(Route.Kind.Agents, ops) { route ->
            val filter = AgentListFilter.entries.firstOrNull { it.wire == (route as Route.Agents).filter.raw } ?: AgentListFilter.All
            AgentOpsFeature.agentsScreen(filter)
        }
        val team = "WS17"
        register(Route.Kind.Teammates, team) { TeammatesFeature.teammatesScreen() }
        register(Route.Kind.Teammate, team) { route -> TeammatesFeature.teammateScreen((route as Route.Teammate).id) }
        register(Route.Kind.TeammateEditor, team) { route -> TeammatesFeature.teammateEditorScreen((route as Route.TeammateEditor).id) }
        val code = "WS11"
        register(Route.Kind.Repo, code) { route ->
            route as Route.Repo
            val repo = repository(route.repo)
            when (route.section) {
                RepoSection.Branches -> CodeFeature.branchesScreen(repo)
                RepoSection.Security -> CodeFeature.securityScreen(repo)
                RepoSection.Settings -> CodeFeature.repoSettingsScreen(repo)
                RepoSection.Actions -> CodeFeature.actionsScreen(repo)
                RepoSection.Releases -> CodeFeature.releasesScreen(repo)
                RepoSection.Worktrees -> AgentOpsFeature.worktreesScreen(repo.owner, repo.name)
                else -> CodeFeature.repoDetailScreen(repo, detailTab(route.section))
            }
        }
        register(Route.Kind.Review, code) { route -> (route as Route.Review).let { CodeFeature.pullReviewScreen(repository(it.repo), it.number) } }
        register(Route.Kind.Board, code) { route -> CodeFeature.issueBoardScreen(repository((route as Route.Board).repo)) }
        register(Route.Kind.Conflicts, code) { route -> (route as Route.Conflicts).let { CodeFeature.conflictsScreen(repository(it.repo), it.branch) } }
        register(Route.Kind.CodeMap, code) { route -> CodeFeature.codeMapScreen(repository((route as Route.CodeMap).repo)) }
        register(Route.Kind.RepoSettings, code) { route -> CodeFeature.repoSettingsScreen(repository((route as Route.RepoSettings).repo)) }
        register(Route.Kind.FileHistory, code) { route -> (route as Route.FileHistory).let { CodeFeature.fileToolsScreen(repository(it.repo), it.path, it.ref) } }
        val ci = "WS14"
        register(Route.Kind.Run, ci) { route ->
            (route as Route.Run).let { run ->
                val id = run.id.toIntOrNull() ?: 0
                if (id > 0) CodeFeature.actionRunScreen(repository(run.repo), id) else CodeFeature.actionsScreen(repository(run.repo))
            }
        }
        register(Route.Kind.Release, ci) { route -> CodeFeature.releasesScreen(repository((route as Route.Release).repo)) }
    }
}
