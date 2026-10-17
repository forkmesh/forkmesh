package com.forkmesh.nativeapp.navigation

import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.market.PackageGate
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.File

class NavigationTest {
    private val repo = RepoRef("fm", "app")

    private fun sampleRoutes(): List<Route> = listOf(
        Route.Agents(AgentFilter.Running), Route.Agent("desktop:1"), Route.Approval("7/a1"),
        Route.Compose(ComposeTarget(repo, 3, null, "s1", null, "fix it")), Route.AgentLogin("node"),
        Route.AgentWatch(AgentFilter.Cloud), Route.AllAgents, Route.LaunchedAgent(repo, "12"),
        Route.Teammates, Route.Teammate("t1"), Route.TeammateEditor(null), Route.TeammateEditor("t2"),
        Route.Shortcuts(repo), Route.Shortcuts(null),
        Route.Inbox(InboxFilter.Mentions), Route.PingSettings, Route.Repos(RepoScope.Network), Route.Repo(repo, null),
        Route.Repo(repo, RepoSection.Pulls), Route.Tree(repo, "main", "src"), Route.File(repo, "main", "src/a.kt"),
        Route.FileHistory(repo, "main", "a"), Route.CodeMap(repo, "main"), Route.Commits(repo, "dev"),
        Route.Commit(repo, "abcdef1"), Route.Range(repo, "a", "b"), Route.Compare(repo, "main", "dev"),
        Route.Conflicts(repo, "dev"), Route.Issue(repo, 4), Route.NewIssue(repo), Route.Board(repo),
        Route.Pull(repo, 9, PullTab.Files), Route.Review(repo, 9), Route.Discussion(repo, 2), Route.Run(repo, "r1"),
        Route.Release(repo, "v1/x"), Route.Project(repo, "p"), Route.Milestones(repo), Route.RepoSettings(repo, "git"),
        Route.LocalRepos, Route.LocalRepo("l1"), Route.QuickEdit("l1", "a.txt"), Route.Chat, Route.Channel("general"),
        Route.Thread("general", "m1"), Route.Dm("bob"), Route.Discord("g", null), Route.Profile("bob"), Route.People, Route.Docs,
        Route.Search("q", SearchScope.Issues), Route.Notes, Route.Note(""), Route.Tasks("mine"), Route.Task("t"),
        Route.Calendar("2026-09-24", CalendarMode.Week), Route.Event("e"), Route.Timers, Route.Desktops,
        Route.Desktop("d", "logs"), Route.Hosts, Route.Host("mirror35", null), Route.Provision("v1"), Route.Network(null),
        Route.Node("n"), Route.Market("installed"), Route.Package("vault"), Route.Crm(null), Route.Contact("c"),
        Route.CrmOrg("o"), Route.Marketing(null), Route.Analytics("funnels"), Route.Competitors, Route.Ads,
        Route.Ledger(null), Route.Vault(null), Route.Coves(repo), Route.Coves(null), Route.Cove("c"), Route.Calories,
        Route.GlassesAlert, Route.PhoneSecurity, Route.FaceChanger, Route.AppStoreStatus, Route.RssReader, Route.Workflows, Route.LocalAI, Route.Activity, Route.Stats, Route.Status, Route.Admin("users"), Route.Settings("pings"),
        Route.Orgs, Route.Org("acme"), Route.InstalledApps, Route.Projects, Route.ProjectAccess("p"), Route.Tour, Route.UpdateRequired,
    )

    fun testEveryKindHasASample() {
        val kinds = sampleRoutes().map { it.kind }.toSet()
        assertEq(Route.Kind.entries.toSet(), kinds)
    }

    fun testCodecRoundTripsEveryRoute() {
        for (route in sampleRoutes()) {
            assertEq(route, RouteCodec.decode(RouteCodec.encode(route)), route.toString())
            assertEq(route, RouteCodec.fromToken(RouteCodec.token(route)), route.toString())
            assertEq(route, SurfaceLink.route(SurfaceLink.url(route)!!), route.toString())
        }
        assertEq(sampleRoutes(), RouteCodec.decodeList(RouteCodec.encodeList(sampleRoutes())))
        assertEq(null, RouteCodec.decode("{\"k\":\"issue\",\"repo\":\"fm/app\"}"))
        assertEq(null, RouteCodec.decode("not json"))
        assertEq(null, RouteCodec.fromToken("%%%"))
    }

    fun testParsesUniversalRepositoryLinks() {
        val issue = RouteParser.parse("https://app.forkmesh.com/fm/app/issues/12")!!
        assertEq("app.forkmesh.com", issue.project)
        assertEq(false, issue.explicitProject)
        assertEq(listOf(Route.Repo(repo, null), Route.Repo(repo, RepoSection.Issues), Route.Issue(repo, 12)), issue.stack)
        assertEq(
            Route.Pull(repo, 3, PullTab.Files),
            RouteParser.parse("https://forkmesh.com/fm/app/pull/3/files")!!.destination,
        )
        val file = RouteParser.parse("https://app.forkmesh.com/fm/app/blob/main/src/a.kt")!!
        assertEq(
            listOf(Route.Repo(repo, null), Route.Tree(repo, "main", "src"), Route.File(repo, "main", "src/a.kt")),
            file.stack,
        )
        assertEq(Route.Commit(repo, "abcdef1234"), RouteParser.parse("https://app.forkmesh.com/fm/app/commits/abcdef1234")!!.destination)
        assertEq(Route.Commits(repo, "feature/x"), RouteParser.parse("https://app.forkmesh.com/fm/app/commits/feature/x")!!.destination)
        assertEq(Route.Compare(repo, "main", "dev"), RouteParser.parse("https://app.forkmesh.com/fm/app/compare/main...dev")!!.destination)
        assertEq(Route.Run(repo, "77"), RouteParser.parse("https://app.forkmesh.com/fm/app/actions/runs/77")!!.destination)
        assertEq(Route.Release(repo, "v1.2"), RouteParser.parse("https://app.forkmesh.com/fm/app/releases/tag/v1.2")!!.destination)
        assertEq(Route.Repo(repo, RepoSection.Security), RouteParser.parse("https://app.forkmesh.com/fm/app/security")!!.destination)
        assertEq(Route.Profile("bob"), RouteParser.parse("https://app.forkmesh.com/@bob")!!.destination)
        assertEq(Route.Status, RouteParser.parse("https://forkmesh.com/status.html")!!.destination)
        assertEq(listOf(Route.Shortcuts(repo)), RouteParser.parse("https://app.forkmesh.com/fm/app/shortcuts")!!.stack)
        assertEq(listOf(Route.Shortcuts(repo)), RouteParser.parse("forkmesh://fm/app/shortcuts")!!.stack)
        assertEq(null, RouteParser.parse("https://app.forkmesh.com/fm/app/shortcuts/x"))
    }

    fun testRefusesUnsafeOrForeignLinks() {
        assertEq(null, RouteParser.parse("https://github.com/fm/app"))
        assertEq(null, RouteParser.parse("http://app.forkmesh.com/fm/app"))
        assertEq(null, RouteParser.parse("https://app.forkmesh.com/api/repos"))
        assertEq(null, RouteParser.parse("https://app.forkmesh.com/fm/.."))
        assertEq(null, RouteParser.parse("https://app.forkmesh.com/fm/app/unknown"))
        assertEq(null, RouteParser.parse("https://app.forkmesh.com/fm/app/issues/abc"))
        assertEq(null, RouteParser.parse("https://app.forkmesh.com/single"))
        assertEq(null, RouteParser.parse("::not a url"))
    }

    fun testRetiredNextHostsOpenTheProductionProject() {
        val link = RouteParser.parse("https://app.next.forkmesh.com/fm/app")!!
        assertEq("app.forkmesh.com", link.project)
        assertEq("app.forkmesh.com", RouteParser.parse("https://next.forkmesh.com/fm/app")!!.project)
        assertEq("app.forkmesh.com", RouteParser.parse("https://forkmesh.com/fm/app")!!.project)
        assertEq(null, RouteParser.parse("https://www.forkmesh.com/fm/app"))
        val explicit = RouteParser.parse("https://app.forkmesh.com/fm/app?project=self.example.org")!!
        assertEq("self.example.org", explicit.project)
        assertEq(true, explicit.explicitProject)
    }

    fun testParsesDashboardAndCustomSchemeLinks() {
        assertEq(listOf(Route.Notes, Route.Note("n1")), RouteParser.parse("https://app.forkmesh.com/dashboard/notes/n1")!!.stack)
        assertEq(Route.Calendar("2026-09-01", CalendarMode.Week), RouteParser.parse("https://app.forkmesh.com/dashboard/calendar?mode=week&date=2026-09-01")!!.destination)
        assertEq(Route.Profile("me"), RouteParser.parse("https://app.forkmesh.com/dashboard/profile", me = "me")!!.destination)
        assertEq(Route.Settings("profile"), RouteParser.parse("https://app.forkmesh.com/dashboard/profile")!!.destination)
        assertEq(listOf(Route.Agents(AgentFilter.All), Route.Agent("s9")), RouteParser.parse("forkmesh://agents/s9")!!.stack)
        assertEq(Route.Agents(AgentFilter.Running), RouteParser.parse("forkmesh://agents?filter=running")!!.destination)
        assertEq(listOf(Route.Inbox(InboxFilter.Approvals), Route.Approval("s/a")), RouteParser.parse("forkmesh://approvals/s/a")!!.stack)
        assertEq(
            Route.Compose(ComposeTarget(repo = repo, issue = 5, text = "a+b c")),
            RouteParser.parse("forkmesh://compose?repo=fm/app&issue=5&text=a+b%20c")!!.destination,
        )
        assertEq(Route.Search("x", SearchScope.Everything), RouteParser.parse("forkmesh://search?q=x&scope=all")!!.destination)
        assertEq(Route.Inbox(InboxFilter.Reviews), RouteParser.parse("forkmesh://inbox/reviews")!!.destination)
        assertEq(Route.Repo(repo, null), RouteParser.parse("forkmesh://fm/app")!!.destination)
        assertEq(listOf(AppTab.barOrder.first().root), RouteParser.parse("forkmesh://dashboard")!!.stack)
        assertEq(listOf(Route.Docs), RouteParser.parse("https://app.forkmesh.com/dashboard/docs")!!.stack)
        assertEq(listOf(Route.FaceChanger), RouteParser.parse("https://app.forkmesh.com/dashboard/face-changer")!!.stack)
        assertEq(listOf(Route.FaceChanger), RouteParser.parse("forkmesh://face-changer")!!.stack)
        assertEq("Face Changer", RouteCatalog.title(Route.Kind.FaceChanger))
        assertEq(listOf(Route.AppStoreStatus), RouteParser.parse("https://app.forkmesh.com/dashboard/app-store-status")!!.stack)
        assertEq(listOf(Route.AppStoreStatus), RouteParser.parse("forkmesh://app-store-status")!!.stack)
        assertEq(listOf(Route.AppStoreStatus), RouteParser.parse("forkmesh://dashboard/app-store-status")!!.stack)
        assertEq("App Store Status", RouteCatalog.title(Route.Kind.AppStoreStatus))
        expect(Route.AppStoreStatus in HubCatalog.insights.mapNotNull { it.route })
        assertEq(listOf(Route.RssReader), RouteParser.parse("https://app.forkmesh.com/dashboard/rss")!!.stack)
        assertEq(listOf(Route.RssReader), RouteParser.parse("forkmesh://rss-reader")!!.stack)
        assertEq(listOf(Route.RssReader), RouteParser.parse("forkmesh://dashboard/rss")!!.stack)
        assertEq(listOf(Route.Workflows), RouteParser.parse("https://app.forkmesh.com/dashboard/workflows")!!.stack)
        assertEq(listOf(Route.Workflows), RouteParser.parse("forkmesh://workflows")!!.stack)
        assertEq("RSS Reader", RouteCatalog.title(Route.Kind.RssReader))
        expect(Route.RssReader in HubCatalog.personal.mapNotNull { it.route })
        assertEq(listOf(Route.Docs), RouteParser.parse("forkmesh://docs")!!.stack)
        assertEq(listOf(Route.Docs), RouteParser.parse("forkmesh://dashboard/docs")!!.stack)
        assertEq(null, RouteParser.parse("https://forkmesh.com/docs/library"))
        assertEq("Docs", Route.Docs.title)
        assertEq(null, GuestGate.action(Route.Docs, guest = true))
    }

    fun testSharedPluginLinksOpenThePackageOverTheMarket() {
        val stack = listOf(Route.Market(null), Route.Package("face-changer"))
        for (host in listOf("forkmesh.com", "app.forkmesh.com", "app.next.forkmesh.com")) {
            val link = RouteParser.parse("https://$host/market/face-changer")!!
            assertEq(stack, link.stack, host)
            assertEq("app.forkmesh.com", link.project, host)
            assertEq(stack, RouteParser.parse("https://$host/market/face-changer/")!!.stack, host)
        }
        assertEq(Route.Package("acme.crm-2"), RouteParser.parse("https://forkmesh.com/market/acme.crm-2")!!.destination)
        assertEq(RouteParser.stack(Route.Package("notes")), RouteParser.parse("https://forkmesh.com/market/notes")!!.stack)
        for (bad in listOf(
            "previews", "previews/notes.png", "previews/notes", "Notes", "9lives", "no_tes", "notes-", "notes/extra",
            "a%2Fb", "a".repeat(97),
        )) {
            assertEq(null, RouteParser.parse("https://forkmesh.com/market/$bad"), bad)
        }
        assertEq(null, RouteParser.parse("https://www.forkmesh.com/market/notes"))
        assertEq(null, RouteParser.parse("http://forkmesh.com/market/notes"))
        assertEq(listOf(Route.Market(null)), RouteParser.parse("https://forkmesh.com/market.html")!!.stack)
        assertEq(listOf(Route.Market("notes")), RouteParser.parse("forkmesh://market/notes")!!.stack)
        assertEq(listOf(Route.Market("installed")), RouteParser.parse("forkmesh://market/installed")!!.stack)
    }

    fun testInstalledAppsLinksAndConsentStayInTheBrowser() {
        assertEq(listOf(Route.InstalledApps), RouteParser.parse("https://app.forkmesh.com/dashboard/apps")!!.stack)
        assertEq(listOf(Route.InstalledApps), RouteParser.parse("https://forkmesh.com/dashboard/apps/")!!.stack)
        assertEq(listOf(Route.InstalledApps), RouteParser.parse("forkmesh://dashboard/apps")!!.stack)
        assertEq(listOf(Route.InstalledApps), RouteParser.parse("forkmesh://apps")!!.stack)
        assertEq(null, RouteParser.parse("https://app.forkmesh.com/dashboard/apps/build"))
        assertEq(null, RouteParser.parse("https://app.forkmesh.com/oauth/authorize?client_id=fmc_1&scope=chat:read"))
        expect(RouteParser.browserOnly("https://app.forkmesh.com/oauth/authorize?client_id=fmc_1"))
        expect(RouteParser.browserOnly("https://forkmesh.com/OAuth/token"))
        expect(RouteParser.browserOnly("https://app.forkmesh.com/dashboard/apps/build"))
        expect(!RouteParser.browserOnly("https://app.forkmesh.com/dashboard/apps"))
        expect(!RouteParser.browserOnly("https://app.forkmesh.com/fm/app"))
        expect(!RouteParser.browserOnly("https://example.com/oauth/authorize"))
        expect(!RouteParser.browserOnly("forkmesh://oauth/authorize"))
        assertEq("Installed apps", Route.InstalledApps.title)
        assertEq("manage your apps", GuestGate.action(Route.InstalledApps, guest = true))
    }

    fun testStacksPutParentsUnderADestination() {
        assertEq(listOf(Route.Channel("c"), Route.Thread("c", "t")), RouteParser.stack(Route.Thread("c", "t")))
        assertEq(
            listOf(Route.Repo(repo, null), Route.Repo(repo, RepoSection.Pulls), Route.Pull(repo, 2, PullTab.Conversation), Route.Review(repo, 2)),
            RouteParser.stack(Route.Review(repo, 2)),
        )
        assertEq(listOf(Route.Inbox(InboxFilter.Approvals), Route.Approval("x")), RouteParser.stack(Route.Approval("x")))
        assertEq(listOf(Route.Calories), RouteParser.stack(Route.Calories))
    }

    fun testTabsAndModality() {
        assertEq(AppTab.Code, Route.Issue(repo, 1).homeTab)
        assertEq(AppTab.Inbox, Route.Approval("a").homeTab)
        assertEq(AppTab.Chat, Route.Dm("x").homeTab)
        assertEq(null, Route.Notes.homeTab)
        expect(Route.Compose(ComposeTarget()).isModal)
        expect(!Route.Notes.isModal)
        assertEq(listOf(AppTab.Agents, AppTab.Inbox, AppTab.Code, AppTab.Chat), AppTab.barOrder)
        assertEq("fm/app #4", Route.Issue(repo, 4).title)
        assertEq("Pull requests · fm/app", Route.Repo(repo, RepoSection.Pulls).title)
        assertEq("a.kt", Route.File(repo, "", "src/a.kt").title)
    }

    fun testIconsExistInTheBundledFont() {
        val file = File("../app/src/main/assets/fonts/MaterialIcons.codepoints")
        expect(file.isFile, "codepoints file")
        val names = file.readLines().map { it.substringBefore(' ') }.toSet()
        fun known(name: String) = name in names || "${name}_baseline" in names
        val icons = Route.Kind.entries.map(RouteCatalog::icon) + AppTab.entries.map { it.icon } +
            HubCatalog.app(true).map { it.icon } + HubCatalog.app(false).map { it.icon } +
            CommandPalette.suggestions(PaletteContext(target = ComposeTarget(repo), pendingApprovals = 2)).map { it.icon } +
            ShellQuickAction.entries.map { it.icon } + listOf("search", "account_circle", "description", "folder")
        for (icon in icons.toSet()) expect(known(icon), "missing icon $icon")
    }

    fun testHubHidesUnregisteredAndAdminItems() {
        val registered = setOf(Route.Kind.Notes, Route.Kind.Tasks, Route.Kind.Hosts, Route.Kind.Vault, Route.Kind.Calories)
        val groups = HubCatalog.groups(guest = false) { it in registered }
        val ids = groups.flatMap { g -> g.items.map { it.id } }
        expect("notes" in ids)
        expect("calendar" !in ids)
        expect("admin" !in ids)
        expect("signOut" in ids)
        expect("setStatus" in ids)
        expect(groups.none { it.id == "business" })
        val guest = HubCatalog.groups(guest = true) { true }
        val guestIds = guest.flatMap { g -> g.items.map { it.id } }
        expect("signIn" in guestIds && "setStatus" !in guestIds)
        expect("admin" in HubCatalog.groups(false, admin = true) { true }.flatMap { g -> g.items.map { it.id } })
        assertEq("use notes", HubCatalog.guestAction(Route.Notes))
    }

    fun testDesignSystemLinkSitsInTheAppGroupAndHidesWhenRemoved() {
        val item = HubCatalog.app(false).single { it.id == "designSystem" }
        assertEq(HubItem.Action.DesignSystem, item.action)
        assertEq("palette", item.icon)
        val pages = HubCatalog.pages(HubCatalog.groups(guest = false) { true })
        expect("designSystem" in pages.single { it.id == "app" }.items.map { it.id })
        val hidden = HubCatalog.groups(guest = false, hidden = setOf("designSystem")) { true }
        expect("designSystem" !in hidden.flatMap { g -> g.items.map { it.id } })
        expect("world" in hidden.flatMap { g -> g.items.map { it.id } })
    }

    fun testWorkflowsIsANativePageInTheAppGroup() {
        val item = HubCatalog.app(false).single { it.id == "workflows" }
        assertEq(Route.Workflows, item.route)
        assertEq("account_tree", item.icon)
        assertEq("workflows", PackageGate.ROUTE_KINDS[Route.Kind.Workflows])
        assertEq(Route.Workflows, RouteCodec.decode(RouteCodec.encode(Route.Workflows)))
        assertEq("/dashboard/workflows", HubItem.WORKFLOWS_PATH)
        val pages = HubCatalog.pages(HubCatalog.groups(guest = false) { true })
        expect("workflows" in pages.single { it.id == "app" }.items.map { it.id })
        expect("workflows" in HubCatalog.app(true).map { it.id })
    }

    fun testShortcutsIsAHubPageInOperateAfterSpecialAgents() {
        val ids = HubCatalog.operate.map { it.id }
        assertEq(ids.indexOf("teammates") + 1, ids.indexOf("shortcuts"))
        val item = HubCatalog.operate.single { it.id == "shortcuts" }
        assertEq("Shortcuts", item.title)
        assertEq("keyboard_command_key", item.icon)
        assertEq(Route.Shortcuts(null), item.route)
        expect(item.needsRegistration)
        assertEq(null, item.guestAction)
        assertEq("WS17", Route.Kind.Shortcuts.owner)
        assertEq("Shortcuts", Route.Shortcuts(repo).title)
        assertEq(repo, Route.Shortcuts(repo).repoRef)
        assertEq(null, Route.Shortcuts(repo).homeTab)
        assertEq(listOf(Route.Shortcuts(null)), RouteParser.stack(Route.Shortcuts(null)))
        assertEq(mapOf("k" to "shortcuts", "repo" to "fm/app"), RouteCodec.toMap(Route.Shortcuts(repo)))
        assertEq(mapOf("k" to "shortcuts"), RouteCodec.toMap(Route.Shortcuts(null)))
        assertEq(Route.Shortcuts(null), RouteCodec.decode("{\"k\":\"shortcuts\"}"))
        expect("shortcuts" !in HubCatalog.groups(guest = false) { it != Route.Kind.Shortcuts }.flatMap { g -> g.items.map { it.id } })
        val pages = HubCatalog.pages(HubCatalog.groups(guest = false) { true })
        assertEq("operate", pages.single { g -> g.items.any { it.id == "shortcuts" } }.id)
    }

    fun testShellMenuShowsOnlyPagesGroupedLikeTheHub() {
        val groups = HubCatalog.groups(guest = false) { true }
        val pages = HubCatalog.pages(groups)
        assertEq(groups.map { it.id }, pages.map { it.id })
        val ids = pages.flatMap { g -> g.items.map { it.id } }
        expect("notes" in ids && "settings" in ids && "world" in ids)
        expect("signOut" !in ids && "setStatus" !in ids && "reportBug" !in ids)
        expect("signIn" !in HubCatalog.pages(HubCatalog.groups(guest = true) { true }).flatMap { g -> g.items.map { it.id } })
        val onlyActions = listOf(HubGroup("x", "X", listOf(HubItem("signOut", "Sign out", "logout", HubItem.Action.SignOut))))
        expect(HubCatalog.pages(onlyActions).isEmpty())
    }

    fun testAccountOnlyBusinessItemsAskAGuestToSignIn() {
        listOf(Route.Crm(null), Route.Marketing(null), Route.Analytics(null), Route.Competitors, Route.Ads, Route.Coves(null)).forEach {
            expect(HubCatalog.guestAction(it) != null)
        }
        expect(HubCatalog.guestAction(Route.Market(null)) == null)
        expect(HubCatalog.guestAction(Route.Ledger(null)) == null)
    }

    fun testRouterPlansAStackOnTheRightTab() {
        val stack = RouteParser.parse("https://app.forkmesh.com/fm/app/issues/3")!!.stack
        val plan = RouterState.plan(stack, RoutePlacement.Automatic, AppTab.Agents, emptyList())!!
        assertEq(AppTab.Code, plan.tab)
        assertEq(true, plan.replace)
        assertEq(stack, plan.path)
        val inbox = RouterState.plan(listOf(Route.Inbox(InboxFilter.All)), RoutePlacement.Automatic, AppTab.Code, emptyList())!!
        assertEq(AppTab.Inbox, inbox.tab)
        assertEq(emptyList<Route>(), inbox.path)
        val hub = RouterState.plan(listOf(Route.Notes, Route.Note("a")), RoutePlacement.Automatic, AppTab.Chat, listOf(Route.Channel("x")))!!
        assertEq(AppTab.Chat, hub.tab)
        assertEq(listOf(Route.Channel("x"), Route.Notes, Route.Note("a")), hub.path)
        val again = RouterState.plan(listOf(Route.Notes, Route.Note("a")), RoutePlacement.Automatic, AppTab.Chat, hub.path)!!
        assertEq(hub.path, again.path)
        val pinned = RouterState.plan(listOf(Route.Calories), RoutePlacement.tab(AppTab.Agents), AppTab.Chat, emptyList())!!
        assertEq(AppTab.Agents, pinned.tab)
        assertEq(null, RouterState.plan(emptyList(), RoutePlacement.Automatic, AppTab.Chat, emptyList()))
    }

    fun testLinksWaitForSignInAndProjectSwitches() {
        val router = RouterState(project = "app.forkmesh.com", accountReady = false)
        val link = RouteParser.parse("https://app.forkmesh.com/fm/app")!!
        assertEq(LinkOutcome.WaitingForSignIn, router.open(link))
        assertEq(null, router.takePending())
        assertEq(link, router.setAccountReady(true))
        assertEq(null, router.pendingLink)
        val other = link.copy(project = "self.example.org", explicitProject = true)
        assertEq(LinkOutcome.UnknownProject("self.example.org"), router.open(other))
        assertEq(LinkOutcome.SwitchProject("self.example.org"), router.open(other, setOf("self.example.org")))
        assertEq(null, router.takePending())
        assertEq(other, router.activate("self.example.org"))
        val implicit = link.copy(project = "app.other.org", explicitProject = false)
        assertEq(LinkOutcome.Opened, router.open(implicit))
        assertEq(LinkOutcome.Ignored, router.open(ParsedLink(stack = emptyList())))
    }

    fun testRouterArchivesPerProjectAndDropsStaleAgentPaths() {
        var now = 1_000_000L
        val router = RouterState(project = "a", nowMs = { now })
        router.select(AppTab.Code)
        router.setPath(AppTab.Code, listOf(Route.Repo(repo, null), Route.Issue(repo, 1)))
        router.setPath(AppTab.Agents, listOf(Route.Agent("x")))
        repeat(7) { router.remember(Route.Issue(repo, it + 1)) }
        router.remember(Route.Compose(ComposeTarget()))
        assertEq(5, router.recents.size)
        assertEq(Route.Issue(repo, 7), router.recents.first())
        val text = router.archiveText()
        router.activate("b")
        assertEq(AppTab.Agents, router.selectedTab)
        assertEq(emptyList<Route>(), router.path(AppTab.Code))
        router.activate("a")
        assertEq(AppTab.Code, router.selectedTab)
        assertEq(listOf(Route.Repo(repo, null), Route.Issue(repo, 1)), router.path(AppTab.Code))
        val restored = RouterState(project = "a", nowMs = { now })
        restored.load(text)
        assertEq(AppTab.Code, restored.selectedTab)
        assertEq(listOf(Route.Agent("x")), restored.path(AppTab.Agents))
        now += RouterState.RESTORE_WINDOW_MS + 1
        val stale = RouterState(project = "a", nowMs = { now })
        stale.load(text)
        assertEq(AppTab.Agents, stale.selectedTab)
        assertEq(emptyList<Route>(), stale.path(AppTab.Agents))
        assertEq(listOf(Route.Repo(repo, null), Route.Issue(repo, 1)), stale.path(AppTab.Code))
        val broken = RouterState(project = "a")
        broken.load("{bad")
        assertEq(AppTab.Agents, broken.selectedTab)
    }

    fun testComposeContextFollowsTheDeepestRoute() {
        assertEq(ComposeTarget(repo = repo, pull = 3), ComposeContext.target(listOf(Route.Repo(repo, null), Route.Pull(repo, 3, PullTab.Files)), AppTab.Code))
        assertEq(ComposeTarget(channel = "@bob"), ComposeContext.target(listOf(Route.Dm("bob")), AppTab.Chat))
        assertEq(ComposeTarget(channel = "general"), ComposeContext.target(emptyList(), AppTab.Chat, "general"))
        assertEq(ComposeTarget(repo = repo), ComposeContext.target(listOf(Route.Run(repo, "1")), AppTab.Code))
        assertEq(ComposeTarget(), ComposeContext.target(listOf(Route.Notes), AppTab.Agents))
        expect(ComposeContext.startsAgent(ComposeTarget(repo = repo)))
        expect(!ComposeContext.startsAgent(ComposeTarget(channel = "x")))
    }

    fun testVersionGate() {
        assertEq(listOf(1, 2, 4), ClientVersionGate.versionNumbers("v1.2.4"))
        assertEq(listOf(1, 2), ClientVersionGate.versionNumbers("1.2-beta"))
        assertEq(null, ClientVersionGate.versionNumbers("1.2.3.4.5"))
        expect(ClientVersionGate.isTooOld("1.2.4", "1.3"))
        expect(!ClientVersionGate.isTooOld("1.3.0", "1.3"))
        expect(!ClientVersionGate.isTooOld("garbage", "1.3"))
        val body = mapOf("minimumClientVersions" to mapOf("android" to "2.0.0", "ios" to "9"))
        val requirement = ClientVersionGate.requirement(body, "1.9.9")!!
        assertEq("2.0.0", requirement.minimum)
        assertEq(ClientVersionGate.DEFAULT_STORE_URL, requirement.storeUrl)
        assertEq("https://forkmesh.com/mobile", ClientVersionGate.DEFAULT_STORE_URL)
        assertEq(null, ClientVersionGate.requirement(body, "2.0.0"))
        assertEq(null, ClientVersionGate.requirement(mapOf("minimumIosVersion" to "9"), "1.0"))
        assertEq("1.0", ClientVersionGate.requirement(mapOf("minimumAndroidVersion" to "1.1"), "1.0")!!.running)
        assertEq(null, ClientVersionGate.requirement(200, "1"))
        expect(ClientVersionGate.requirement(426, "1") != null)
    }

    fun testSearchMatchRanksAndSourcesBucket() {
        assertEq(0, SearchMatch.score("Notes", "notes"))
        assertEq(1, SearchMatch.score("Notebook", "note"))
        assertEq(2, SearchMatch.score("fm/app-core", "core"))
        assertEq(3, SearchMatch.score("forkmesh", "kme"))
        assertEq(null, SearchMatch.score("abc", " "))
        assertEq(4, SearchMatch.score("my-cool_repo", "my cool repo"))
        assertEq(null, SearchMatch.score("abc", "--"))
        val repos = listOf(Repository("fm", "app", "the app"), Repository("fm", "docs"), Repository("x", "appendix"))
        val repoHits = RepositoriesSearchSource({ repos }).search("app")
        assertEq(listOf("app", "appendix"), repoHits.map { it.title })
        assertEq(listOf("fm · the app", "x"), repoHits.map { it.subtitle })
        assertEq(listOf(RepoRef("fm", "app"), RepoRef("x", "appendix")), RepositoriesSearchSource.named("app", repos, 3))
        assertEq(emptyList<RepoRef>(), RepositoriesSearchSource.named("pend", repos, 3))
        val issues = IssuesSearchSource(false, { listOf(repo) }) {
            listOf(SearchableItem(12, "Crash on start", status = "open", isPull = false), SearchableItem(3, "crash fix", isPull = true))
        }
        val hits = issues.search("#12")
        assertEq(listOf(Route.Issue(repo, 12)), hits.map { it.route })
        assertEq("fm/app #12 · open", hits.first().subtitle)
        val pulls = IssuesSearchSource(true, { listOf(repo) }) { error("offline") }
        assertEq(emptyList<SearchHit>(), pulls.search("crash"))
        assertEq(listOf("@Bob"), PeopleSearchSource({ listOf("Bob") }).search("@bob").map { it.title })
        assertEq(listOf("@Bob", "@bobby"), PeopleSearchSource({ listOf("Bob") }, { listOf("bobby", "bob") }).search("bo").map { it.title })
        assertEq(emptyList<SearchHit>(), PeopleSearchSource({ listOf("Bob") }).search("myrepo"))
        assertEq(listOf(Route.Channel("amygdala"), Route.Dm("amy")), ChatSearchSource { listOf("@amy", "amygdala", "x") }.search("amy").map { it.route })
        assertEq(emptyList<SearchHit>(), CodeSearchSource({ listOf(repo) }) { _, _ -> listOf(CodeMatchItem("a", 1, "b")) }.search("a"))
        val code = CodeSearchSource({ listOf(repo) }) { _, _ -> listOf(CodeMatchItem("src/a.kt", 4, "  val x ")) }.search("val")
        assertEq("fm/app:4 val x", code.single().subtitle)
        expect(DestinationsSearchSource().search("vault").any { it.route == Route.Vault(null) })
    }

    fun testRepositoriesIncludeOnDeviceReposAndSurviveCatalogFailure() {
        val device = { listOf(LocalRepositoryItem("my-garden", "plants"), LocalRepositoryItem("app", publishedAs = "fm/app")) }
        val mixed = RepositoriesSearchSource({ listOf(Repository("fm", "app")) }, device)
        val garden = mixed.search("my garden")
        assertEq(listOf<Route>(Route.LocalRepo("my-garden")), garden.map { it.route })
        assertEq("On this device · plants", garden.single().subtitle)
        assertEq(listOf<Route>(Route.Repo(RepoRef("fm", "app"), null)), mixed.search("app").map { it.route })
        val offline = RepositoriesSearchSource({ error("offline") }, device)
        assertEq(listOf<Route>(Route.LocalRepo("my-garden")), offline.search("garden").map { it.route })
        expect(runCatching { RepositoriesSearchSource({ error("offline") }).search("garden") }.isFailure)
        assertEq(SearchBucketKind.Repositories, SearchBucketKind.entries.first())
        assertEq(GlobalSearchScope.Code, SearchBucketKind.Repositories.scope)
        assertEq(GlobalSearchScope.Code, SearchBucketKind.Commits.scope)
    }

    fun testCommitsMatchSubjectAuthorAndSha() {
        val commits = CommitsSearchSource({ listOf(repo) }) {
            listOf(
                CommitItem("abc1234def", "Fix router crash", "alice"),
                CommitItem("fedcba98", "Add search", "bob"),
                CommitItem("", "router without sha"),
            )
        }
        val byText = commits.search("router")
        assertEq(listOf<Route>(Route.Commit(repo, "abc1234def")), byText.map { it.route })
        assertEq("fm/app abc1234 · alice", byText.single().subtitle)
        assertEq(listOf<Route>(Route.Commit(repo, "fedcba98")), commits.search("fedc").map { it.route })
        assertEq("Add search", commits.search("bob").single().title)
        assertEq(emptyList<SearchHit>(), commits.search("a"))
    }

    fun testGlobalSearchBucketsLimitsAndFailures() {
        val many = object : GlobalSearchSource {
            override val kind = SearchBucketKind.Repositories
            override fun search(query: String) = (1..60).map { SearchHit("r$it", "r$it", icon = "folder", route = Route.Repo(RepoRef("o", "r$it"), null)) }
        }
        val broken = object : GlobalSearchSource {
            override val kind = SearchBucketKind.Chat
            override fun search(query: String): List<SearchHit> = throw IllegalStateException("down")
        }
        val (buckets, failed) = GlobalSearch.runBlocking(listOf(many, broken), "r", GlobalSearchScope.All)
        assertEq(1, buckets.size)
        assertEq(5, buckets[0].hits.size)
        assertEq(60, buckets[0].total)
        expect(buckets[0].hasMore)
        assertEq(listOf(SearchBucketKind.Chat), failed)
        val (expanded, _) = GlobalSearch.runBlocking(listOf(many, broken), "r", GlobalSearchScope.All, SearchBucketKind.Repositories)
        assertEq(50, expanded[0].hits.size)
        val (scoped, scopedFailed) = GlobalSearch.runBlocking(listOf(many, broken), "r", GlobalSearchScope.Chat)
        assertEq(emptyList<SearchBucket>(), scoped)
        assertEq(listOf(SearchBucketKind.Chat), scopedFailed)
        assertEq(emptyList<SearchBucket>(), GlobalSearch.runBlocking(listOf(many), "  ", GlobalSearchScope.All).first)
        assertEq(GlobalSearchScope.Issues, GlobalSearchScope.from(SearchScope.Pulls))
    }

    fun testSavedSearchesPersistAndCap() {
        var now = 10L
        val prefs = MemoryPrefStore()
        val store = SavedSearchStore(prefs) { now }
        store.save("crash", GlobalSearchScope.Issues, 3)
        store.save("", GlobalSearchScope.All, 1)
        expect(store.contains("CRASH", GlobalSearchScope.Issues))
        now = 20
        store.recordRun("crash", GlobalSearchScope.Issues, 9)
        store.recordRun("other", GlobalSearchScope.Issues, 9)
        assertEq(9, store.saved.single().lastCount)
        assertEq(20L, store.saved.single().lastRunAtMs)
        repeat(12) { store.commit("q$it", GlobalSearchScope.All) }
        store.commit("q11", GlobalSearchScope.All)
        store.commit("   ", GlobalSearchScope.All)
        assertEq(10, store.recent.size)
        assertEq("q11", store.recent.first().query)
        val reopened = SavedSearchStore(prefs)
        assertEq(store.saved, reopened.saved)
        assertEq(store.recent, reopened.recent)
        reopened.remove(reopened.saved.single())
        reopened.clearRecent()
        assertEq(0, SavedSearchStore(prefs).saved.size + SavedSearchStore(prefs).recent.size)
        assertEq(Route.Search("crash", SearchScope.Issues), SavedSearch("crash", GlobalSearchScope.Issues).route)
        prefs.putString(SavedSearchStore.SAVED_KEY, "nope")
        assertEq(0, SavedSearchStore(prefs).saved.size)
    }

    fun testPaletteSectionsAndDirectEntries() {
        val context = PaletteContext(target = ComposeTarget(repo = repo), pendingApprovals = 1, recents = listOf(Route.Notes))
        val empty = CommandPalette.sections("", context)
        assertEq(listOf("suggested", "recent", "tabs"), empty.map { it.id })
        assertEq("New agent here", empty[0].entries[0].title)
        expect(empty[0].entries.any { it.title == "Review 1 pending approval" })
        val direct = CommandPalette.sections("fm/app#7", context)
        assertEq(PaletteCommand.Open(Route.Issue(repo, 7)), direct.first().entries[0].command)
        assertEq(PaletteCommand.Open(Route.Pull(repo, 7, PullTab.Conversation)), direct.first().entries[1].command)
        assertEq(PaletteCommand.SearchEverywhere("fm/app#7"), direct.last().entries.single().command)
        val vault = CommandPalette.sections("vau", context)
        expect(vault.any { s -> s.id == "destinations" && s.entries.any { it.command == PaletteCommand.Open(Route.Vault(null)) } })
        assertEq(PaletteCommand.Open(Route.Profile("bob")), CommandPalette.directEntries("@bob").single().command)
        expect(CommandPalette.suggestions(PaletteContext(guest = true)).none { it.id == "newNote" })
        assertEq("Message general", CommandPalette.suggestions(PaletteContext(target = ComposeTarget(channel = "general")))[0].title)
    }

    fun testQuickActions() {
        assertEq(Route.Compose(ComposeTarget()), ShellQuickAction.NewAgent.route())
        assertEq(Route.Repo(repo, null), ShellQuickAction.RecentRepo.route("fm/app"))
        assertEq(null, ShellQuickAction.RecentRepo.route("bad"))
        assertEq(null, ShellQuickAction.RecentAgent.route())
        val items = ShellQuickAction.dynamicItems(listOf(Route.Notes, Route.Agent("s1"), Route.Issue(repo, 2)))
        assertEq(listOf(ShellQuickAction.RecentRepo, ShellQuickAction.RecentAgent), items.map { it.action })
        assertEq("fm/app", items[0].payload)
        assertEq(ShellQuickAction.Inbox, ShellQuickAction.of("inbox"))
    }

    fun testGuestGateCoversEveryAccountRouteWhereverItComesFrom() {
        val gated = listOf(
            Route.Notes, Route.Note(""), Route.Note("n1"), Route.Tasks(null), Route.Task("t"), Route.AllAgents,
            Route.Hosts, Route.Host("mirror35", null), Route.Crm(null), Route.Contact("c"), Route.CrmOrg("o"),
            Route.Marketing(null), Route.Analytics(null), Route.Competitors, Route.Ads, Route.Vault(null),
            Route.Coves(null), Route.Cove("c1"), Route.Calories, Route.Approval("7/a1"), Route.InstalledApps,
        )
        for (route in gated) {
            expect(GuestGate.action(route, guest = true) != null, "guest must sign in before ${route.kind.raw}")
            expect(GuestGate.action(route, guest = false) == null, "a signed-in user opens ${route.kind.raw} directly")
        }
        assertEq(gated.map { it.kind }.toSet(), GuestGate.gatedKinds)
        assertEq("use notes", GuestGate.action(ShellQuickAction.NewNote.route()!!, true))
        val open = listOf(
            Route.Repos(RepoScope.Network), Route.Repo(repo, null), Route.Issue(repo, 1), Route.Chat, Route.Activity,
            Route.Settings(null), Route.Projects, Route.Tour, Route.Inbox(InboxFilter.All), Route.Search("q", SearchScope.Issues),
        )
        for (route in open) expect(GuestGate.action(route, guest = true) == null, "${route.kind.raw} stays open to guests")
        for (item in HubCatalog.workspace + HubCatalog.operate + HubCatalog.business + HubCatalog.personal) {
            val route = item.route ?: continue
            assertEq(item.guestAction, GuestGate.action(route), "hub and router agree on ${route.kind.raw}")
        }
    }

    fun testCustomSchemeLinksAlwaysLandSomewhere() {
        fun dest(url: String) = RouteParser.parse(url, "me")?.stack
        assertEq(listOf(Route.Settings(null)), dest("forkmesh://settings"))
        assertEq(listOf(Route.Settings("appearance")), dest("forkmesh://settings/appearance"))
        assertEq(listOf(Route.Notes, Route.Note("n1")), dest("forkmesh://notes/n1"))
        assertEq(listOf(Route.Chat), dest("forkmesh://chat"))
        assertEq(listOf(Route.Profile("me")), dest("forkmesh://profile"))
        assertEq(listOf(Route.Inbox(InboxFilter.All)), dest("forkmesh://pings"))
        assertEq(listOf(Route.Vault(null)), dest("forkmesh://vault"))
        assertEq(listOf(Route.Projects), dest("forkmesh://projects"))
        assertEq(listOf(Route.Coves(null), Route.Cove("fm/app/c1")), dest("forkmesh://cove/fm/app/c1"))
        assertEq(Route.Issue(repo, 4), dest("forkmesh://fm/app/issues/4")?.last())
        assertEq(listOf(AppTab.barOrder.first().root), dest("forkmesh://nonsense"))
        assertEq(listOf(AppTab.barOrder.first().root), dest("forkmesh://home"))
        expect(RouteParser.parse("https://app.forkmesh.com/nonsense") == null, "unknown web links still go to the browser")
    }
}
