package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.code.RepoHubSection
import com.forkmesh.nativeapp.code.RepoOverviewModel
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.navigation.AppTab
import com.forkmesh.nativeapp.navigation.CommandPalette
import com.forkmesh.nativeapp.navigation.ComposeTarget
import com.forkmesh.nativeapp.navigation.DestinationsSearchSource
import com.forkmesh.nativeapp.navigation.HubCatalog
import com.forkmesh.nativeapp.navigation.PaletteCommand
import com.forkmesh.nativeapp.navigation.PaletteContext
import com.forkmesh.nativeapp.navigation.RepoRef
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.navigation.ShellQuickAction
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class PackageGateTest {
    private fun row(id: String, before: String? = "2026-10-11", deps: List<String> = emptyList(), default: Boolean = false): Map<String, Any?> =
        buildMap {
            put("id", id)
            put("name", id.replaceFirstChar { it.uppercase() })
            put("default", default)
            put("accountSync", true)
            put("dependencies", deps)
            if (before != null) put("installedBefore", before)
        }

    private fun catalog(): List<MarketPackage> = listOf(
        row("agents"), row("inbox"), row("chat"), row("code"), row("notes"), row("status-page"),
        row("special-agents", deps = listOf("agents")), row("branches"), row("pull-requests", deps = listOf("branches")),
        row("timers", before = null), row("vault", before = null), row("workflows", before = "2026-10-14"),
        row("status-check-api", before = null, deps = listOf("status-page"), default = true),
    ).mapNotNull(MarketPackage::fromJson)

    private fun store(legacy: Boolean, prefs: MemoryPrefStore = MemoryPrefStore()): AccountPackagesStore =
        AccountPackagesStore(null, prefs) { legacy }.also { it.rememberCatalog(catalog()) }

    private fun state(vararg rows: Pair<String, Boolean>) = AccountPackagesState(rows.map { AccountPackage(it.first, it.second) })

    private fun chosen(vararg choices: Pair<String, Boolean>): MemoryPrefStore =
        MemoryPrefStore(mapOf(PackageGate.PROJECT_INSTALLS_KEY to Json.stringify(choices.toMap())))

    private fun packed(pack: StarterPack, vararg choices: Pair<String, Boolean>): MemoryPrefStore =
        chosen(*choices).also { PackageGate.chooseStarterPack(pack, it) }

    fun testStarterPacksHoldTheirPackages() {
        assertEq(listOf(StarterPack.MINIMAL, StarterPack.STANDARD, StarterPack.EVERYTHING), StarterPack.values().toList())
        assertEq(listOf("minimal", "standard", "everything"), StarterPack.values().map { it.id })
        assertEq(StarterPack.STANDARD, StarterPack.FALLBACK)
        assertEq(setOf("code"), StarterPack.MINIMAL.packages)
        assertEq(setOf("agents", "chat", "inbox", "code", "tour"), StarterPack.STANDARD.packages)
        assertEq(PackageGate.STARTER, StarterPack.STANDARD.packages)
        assertEq(PackageGate.GATED - PackageGate.ORGANIZATION_PACKAGES, StarterPack.EVERYTHING.packages)
        expect(StarterPack.EVERYTHING.packages.containsAll(listOf("agents", "notes", "vault", "pull-requests", "workflows")))
        expect(StarterPack.EVERYTHING.packages.none { it in PackageGate.ORGANIZATION_PACKAGES })
        assertEq(StarterPack.STANDARD, StarterPack.fromId(null))
        assertEq(StarterPack.STANDARD, StarterPack.fromId("bogus"))
        assertEq(StarterPack.MINIMAL, StarterPack.fromId("minimal"))
        assertEq(StarterPack.EVERYTHING, StarterPack.fromId("everything"))
        StarterPack.values().forEach { expect(it.label.isNotEmpty() && it.summary.isNotEmpty(), it.id) }
    }

    fun testANewProjectChoosesItsStarterPackOnce() {
        val prefs = MemoryPrefStore()
        expect(!PackageGate.legacyProject(prefs))
        expect(PackageGate.needsStarterPack(prefs, legacy = false))
        assertEq(StarterPack.STANDARD, PackageGate.starterPack(prefs))
        PackageGate.chooseStarterPack(StarterPack.MINIMAL, prefs)
        assertEq("minimal", prefs.getString(PackageGate.STARTER_PACK_KEY))
        assertEq(StarterPack.MINIMAL, PackageGate.starterPack(prefs))
        expect(!PackageGate.needsStarterPack(prefs, legacy = false))
        expect(!PackageGate.needsStarterPack(MemoryPrefStore(), legacy = true))
        expect(!PackageGate.needsStarterPack(null, legacy = false))
        assertEq(true, PhonePackages.isOn(PhonePackages.SIGN_IN, prefs, legacy = false))
    }

    fun testTheMinimalPackStartsANewProjectWithCodeOnly() {
        val minimal = store(legacy = false, packed(StarterPack.MINIMAL))
        assertEq(StarterPack.MINIMAL, minimal.starterPack)
        expect(minimal.isInstalled("code"))
        for (id in listOf("agents", "chat", "inbox", "tour", "notes", "special-agents")) expect(!minimal.isInstalled(id), id)
        val surfaces = PackageSurfaces.of(minimal::isInstalled)
        assertEq(setOf(AppTab.Agents, AppTab.Inbox, AppTab.Chat), surfaces.tabs)
        expect(surfaces.code && !surfaces.agents)
        val picked = store(legacy = false, packed(StarterPack.MINIMAL, "agents" to true, "code" to false))
        expect(picked.isInstalled("agents") && !picked.isInstalled("code"))
        expect(store(legacy = true, packed(StarterPack.MINIMAL)).isInstalled("agents"))
        expect(!PackageGate.fallback("agents", null, legacy = false, pack = StarterPack.MINIMAL))
        expect(PackageGate.fallback("code", null, legacy = false, pack = StarterPack.MINIMAL))
    }

    fun testTheEverythingPackStartsANewProjectWithEveryPackage() {
        val everything = store(legacy = false, packed(StarterPack.EVERYTHING))
        for (id in listOf("agents", "special-agents", "vault", "timers", "workflows", "notes", "pull-requests", "status-page")) {
            expect(everything.isInstalled(id), id)
        }
        val picked = store(legacy = false, packed(StarterPack.EVERYTHING, "notes" to false, "agents" to false))
        expect(!picked.isInstalled("notes") && !picked.isInstalled("special-agents"))
        expect(picked.isInstalled("vault"))
        expect(!store(legacy = true, packed(StarterPack.EVERYTHING)).isInstalled("vault"))
        expect(PackageGate.fallback("vault", null, legacy = false, pack = StarterPack.EVERYTHING))
        expect(!PackageGate.fallback("crm", null, legacy = false, pack = StarterPack.EVERYTHING))
    }

    fun testTheStandardPackIsTheDefault() {
        val unset = store(legacy = false)
        assertEq(StarterPack.STANDARD, unset.starterPack)
        val standard = store(legacy = false, packed(StarterPack.STANDARD))
        for (id in PackageGate.STARTER) expect(unset.isInstalled(id) && standard.isInstalled(id), id)
        for (id in listOf("notes", "vault", "special-agents")) expect(!unset.isInstalled(id) && !standard.isInstalled(id), id)
    }

    fun testMarketRowsCarryInstalledBeforeIntoTheCachedCatalog() {
        val pkg = MarketPackage.fromJson(row("agents"))!!
        assertEq("2026-10-11", pkg.installedBefore)
        assertEq("", MarketPackage.fromJson(row("timers", before = null))!!.installedBefore)
        assertEq("2026-10-11", MarketPackage.fromJson(mapOf("id" to "notes", "install" to mapOf("installedBefore" to "2026-10-11")))!!.installedBefore)
        assertEq("", MarketPackage.fromJson(mapOf("id" to "notes", "installedBefore" to "soon"))!!.installedBefore)
        val prefs = MemoryPrefStore()
        store(legacy = true, prefs)
        val reopened = AccountPackagesStore(null, prefs) { true }
        assertEq("2026-10-11", reopened.catalog.getValue("agents").installedBefore)
        assertEq("", reopened.catalog.getValue("timers").installedBefore)
    }

    fun testLegacyMarkerIsWrittenOnceFromEarlierUse() {
        val fresh = MemoryPrefStore()
        expect(!PackageGate.legacyProject(fresh))
        assertEq(false, fresh.getBool(PackageGate.LEGACY_KEY))
        fresh.putString(AccountPackagesStore.STATE_KEY, "{}")
        expect(!PackageGate.legacyProject(fresh) { true })
        val used = MemoryPrefStore(mapOf("nav/tour.v1" to true))
        expect(PackageGate.legacyProject(used))
        used.remove("nav/tour.v1")
        expect(PackageGate.legacyProject(used))
        val signedIn = MemoryPrefStore()
        expect(PackageGate.legacyProject(signedIn) { true })
        expect(PackageGate.legacyProject(null))
    }

    fun testNewProjectStartsWithOnlyTheStarterPackages() {
        val store = store(legacy = false)
        for (id in listOf("notes", "special-agents", "pull-requests", "timers", "vault", "workflows")) {
            expect(!store.isInstalled(id), id)
        }
        for (id in PackageGate.STARTER) expect(store.isInstalled(id), id)
        expect(!store.isInstalled("never-heard-of"))
        val surfaces = PackageSurfaces.of(store::isInstalled)
        assertEq(emptySet<AppTab>(), surfaces.tabs)
        expect(surfaces.agents && surfaces.code && !surfaces.signIn)
        expect("allAgents" !in surfaces.hubItems && "tour" !in surfaces.hubItems)
        expect("teammates" in surfaces.hubItems && "notes" in surfaces.hubItems)
        expect(RepoHubSection.Pulls in surfaces.repoSections && RepoHubSection.Branches in surfaces.repoSections)
        expect(Route.Kind.Compose !in surfaces.routeKinds)
    }

    fun testANewProjectCanRemoveAStarterPackage() {
        val fresh = store(legacy = false, chosen("agents" to false, "tour" to false))
        fresh.adopt(state("chat" to false))
        expect(!fresh.isInstalled("agents") && !fresh.isInstalled("tour"))
        expect(fresh.isInstalled("chat") && fresh.isInstalled("inbox") && fresh.isInstalled("code"))
    }

    fun testLegacyProjectKeepsWhatWasInstalledBefore() {
        val store = store(legacy = true)
        for (id in listOf("agents", "inbox", "chat", "code", "notes", "special-agents", "pull-requests")) expect(store.isInstalled(id), id)
        expect(!store.isInstalled("timers"))
        expect(!store.isInstalled("vault"))
        expect(store.isInstalled("never-heard-of"))
        expect(store.isInstalled("status-check-api"))
        val unloaded = AccountPackagesStore(null, MemoryPrefStore()) { true }
        expect(unloaded.isInstalled("agents"))
        expect(!AccountPackagesStore(null, MemoryPrefStore()) { false }.isInstalled("notes"))
        expect(AccountPackagesStore(null, MemoryPrefStore()) { false }.isInstalled("agents"))
    }

    fun testAccountAnswerWinsAndDependenciesMustBeInstalled() {
        val legacy = store(legacy = true)
        legacy.adopt(state("agents" to false, "chat" to true))
        expect(!legacy.isInstalled("agents"))
        expect(!legacy.isInstalled("special-agents"))
        expect(legacy.isInstalled("inbox"))
        val fresh = store(legacy = false, chosen("timers" to true, "special-agents" to true, "pull-requests" to true, "branches" to true))
        fresh.adopt(state("timers" to true, "special-agents" to true, "pull-requests" to true, "branches" to false))
        expect(fresh.isInstalled("timers"))
        expect(fresh.isInstalled("special-agents"))
        expect(!fresh.isInstalled("pull-requests"))
        fresh.adopt(state("timers" to true, "special-agents" to true, "agents" to true, "pull-requests" to true, "branches" to true))
        expect(fresh.isInstalled("special-agents"))
        expect(fresh.isInstalled("pull-requests"))
        expect(!fresh.isInstalled("notes"))
    }

    fun testAnOldAccountsFallbackFollowsTheProject() {
        val reply = AccountPackagesState.fromJson(
            mapOf(
                "packages" to listOf(
                    mapOf("id" to "agents", "installed" to true, "legacy" to true),
                    mapOf("id" to "notes", "installed" to true),
                ),
            ),
        )
        assertEq(AccountPackagesState.fromJson(reply.toJson()), reply)
        val fresh = store(legacy = false, chosen("notes" to true))
        fresh.adopt(reply)
        expect(fresh.isInstalled("agents"))
        expect(fresh.isInstalled("notes"))
        val picked = store(legacy = false, chosen("agents" to true))
        picked.adopt(reply)
        expect(picked.isInstalled("agents"))
        expect(!picked.isInstalled("notes"))
        val legacy = store(legacy = true)
        legacy.adopt(reply)
        expect(legacy.isInstalled("agents"))
        expect(legacy.isInstalled("notes"))
    }

    fun testPlacesAreParsedAndAnOlderServerMeansEverywhere() {
        val entry = AccountPackage.fromJson(
            mapOf("id" to "notes", "installed" to true, "places" to listOf("web", "mobile", "watch"), "availablePlaces" to listOf("desktop", "mobile", "web")),
        )!!
        assertEq(listOf("mobile", "web"), entry.places)
        assertEq(listOf("mobile", "desktop", "web"), entry.availablePlaces)
        expect(entry.placedHere)
        val old = AccountPackage.fromJson(mapOf("id" to "notes", "installed" to true))!!
        assertEq(PackagePlaces.ALL, old.places)
        assertEq(emptyList<String>(), old.availablePlaces)
        expect(old.placedHere)
        val desktopOnly = AccountPackage.fromJson(mapOf("id" to "notes", "installed" to true, "places" to listOf("desktop")))!!
        expect(!desktopOnly.placedHere)
        val reply = AccountPackagesState(listOf(entry, desktopOnly.copy(id = "tasks"), old.copy(id = "timers")))
        assertEq(reply, AccountPackagesState.fromJson(reply.toJson()))
        assertEq(listOf("mobile"), MarketPackage.fromJson(mapOf("id" to "food-guide", "places" to listOf("mobile")))!!.places)
        assertEq(PackagePlaces.ALL, MarketPackage.fromJson(row("notes"))!!.places)
        assertEq(listOf("Phone", "Desktop", "Web"), PackagePlaces.ALL.map(PackagePlaces::label))
    }

    fun testAPackageShowsHereOnlyWhenItsPlacesHaveMobile() {
        val store = store(legacy = true)
        store.adopt(
            AccountPackagesState(
                listOf(
                    AccountPackage("notes", true, places = listOf("desktop", "web")),
                    AccountPackage("tasks", true, places = listOf("mobile")),
                    AccountPackage("agents", true, places = listOf("web")),
                    AccountPackage("special-agents", true, places = listOf("mobile", "desktop")),
                    AccountPackage("status-page", true, places = listOf("desktop")),
                ),
            ),
        )
        expect(store.isInstalled("notes"))
        expect(!store.shows("notes"))
        expect(store.shows("tasks"))
        expect(!store.shows("agents"))
        expect(store.shows("special-agents"))
        expect(!store.shows("status-page"))
        expect(store.shows("status-check-api"))
        val surfaces = PackageSurfaces.of(store::shows)
        expect("notes" in surfaces.hubItems && "tasks" !in surfaces.hubItems)
        expect(!surfaces.agents && "teammates" !in surfaces.hubItems)
        expect(Route.Kind.Notes in surfaces.routeKinds && Route.Kind.Tasks !in surfaces.routeKinds)
        store.adopt(AccountPackagesState(listOf(AccountPackage("notes", false, places = listOf("mobile")))))
        expect(!store.shows("notes"))
    }

    fun testALegacyEntryIsInstalledOnlyInALegacyProject() {
        val reply = AccountPackagesState.fromJson(
            mapOf(
                "packages" to listOf(
                    mapOf("id" to "vault", "installed" to true, "legacy" to true),
                    mapOf("id" to "pull-requests", "installed" to true, "legacy" to true),
                    mapOf("id" to "branches", "installed" to false),
                    mapOf("id" to "timers", "installed" to true),
                ),
            ),
        )
        val fresh = store(legacy = false)
        fresh.adopt(reply)
        expect(!fresh.isInstalled("vault"))
        expect(!fresh.shows("vault"))
        expect(!fresh.isInstalled("timers"))
        val legacy = store(legacy = true)
        legacy.adopt(reply)
        expect(legacy.isInstalled("vault"))
        expect(legacy.shows("vault"))
        expect(!legacy.isInstalled("pull-requests"))
        expect("vault" !in PackageSurfaces.of(legacy::shows).hubItems)
        expect("vault" in PackageSurfaces.of(fresh::shows).hubItems)
    }

    fun testWorkflowsIsAGatedPackage() {
        assertEq("workflows", PackageGate.HUB_ITEMS["workflows"])
        expect("workflows" in PackageGate.GATED)
        val fresh = store(legacy = false)
        expect(!fresh.isInstalled("workflows"))
        val hidden = PackageSurfaces.of(fresh::shows).hubItems
        expect("workflows" in hidden)
        val ids = HubCatalog.groups(guest = false, hidden = hidden) { true }.flatMap { it.items }.map { it.id }
        expect("workflows" !in ids && "market" in ids && "settings" in ids)
        expect("workflows" !in HubCatalog.pages(HubCatalog.groups(guest = false, hidden = hidden) { true }).flatMap { it.items }.map { it.id })
        expect(store(legacy = true).isInstalled("workflows"))
        expect(AccountPackagesStore(null, MemoryPrefStore()) { true }.isInstalled("workflows"))
        expect(!AccountPackagesStore(null, MemoryPrefStore()) { false }.isInstalled("workflows"))
        fresh.adopt(AccountPackagesState(listOf(AccountPackage("workflows", true))))
        expect(!fresh.isInstalled("workflows"))
        val picked = store(legacy = false, chosen("workflows" to true))
        picked.adopt(AccountPackagesState(listOf(AccountPackage("workflows", true, places = listOf("desktop", "web")))))
        expect(picked.isInstalled("workflows") && !picked.shows("workflows"))
        picked.adopt(AccountPackagesState(listOf(AccountPackage("workflows", true))))
        expect("workflows" !in PackageSurfaces.of(picked::shows).hubItems)
    }

    fun testDocsIsADefaultPackageInTheAppGroup() {
        assertEq("docs", PackageGate.HUB_ITEMS["docs"])
        assertEq("docs", PackageGate.ROUTE_KINDS[Route.Kind.Docs])
        expect("docs" in PackageGate.GATED)
        assertEq(PackagePhoneSurface.Native("docs"), MarketPackage(id = "docs", name = "Docs").phoneSurface)
        val removed = PackageSurfaces(setOf("docs"))
        expect("docs" in removed.hubItems && Route.Kind.Docs in removed.routeKinds && !removed.showsRoute(Route.Docs))
        expect("docs" !in HubCatalog.groups(guest = true, hidden = removed.hubItems) { true }.flatMap { it.items }.map { it.id })
        val shown = HubCatalog.groups(guest = true, hidden = PackageSurfaces.ALL.hubItems) { true }
        assertEq("app", shown.single { group -> group.items.any { it.id == "docs" } }.id)
        val docs = MarketPackage.fromJson(row("docs", before = null, default = true))!!
        val fresh = AccountPackagesStore(null, MemoryPrefStore()) { false }.also { it.rememberCatalog(catalog() + docs) }
        expect(fresh.isInstalled("docs"))
        expect(!fresh.isInstalled("notes"))
        val placed = AccountPackagesStore(null, MemoryPrefStore()) { false }.also { it.rememberCatalog(catalog() + docs) }
        placed.adopt(AccountPackagesState(listOf(AccountPackage("docs", true, places = listOf("mobile", "desktop")))))
        expect(placed.shows("docs"))
        placed.adopt(AccountPackagesState(listOf(AccountPackage("docs", false))))
        expect(!placed.shows("docs"))
    }

    fun testTheMarkerForACreatedProjectIsNewEvenWithAStoredSession() {
        val created = MemoryPrefStore()
        PackageGate.markNewProject(created)
        expect(!PackageGate.legacyProject(created) { true })
        created.putString(AccountPackagesStore.STATE_KEY, "{}")
        created.putBool("nav/tour.v1", true)
        expect(!PackageGate.legacyProject(created) { true })
        assertEq(false, created.getBool(PackageGate.LEGACY_KEY))
        val existing = MemoryPrefStore()
        expect(PackageGate.legacyProject(existing) { true })
    }

    fun testANewProjectShowsOnlyWhatItInstalled() {
        val fresh = store(legacy = false, chosen("timers" to true, "vault" to true, "notes" to false))
        fresh.adopt(
            AccountPackagesState.fromJson(
                mapOf(
                    "packages" to listOf(
                        mapOf("id" to "agents", "installed" to true, "legacy" to true),
                        mapOf("id" to "vault", "installed" to true, "legacy" to true),
                        mapOf("id" to "notes", "installed" to true),
                        mapOf("id" to "inbox", "installed" to true),
                        mapOf("id" to "timers", "installed" to true),
                    ),
                ),
            ),
        )
        expect(fresh.isInstalled("timers"))
        expect(fresh.isInstalled("vault"))
        for (id in listOf("notes", "workflows", "status-page", "status-check-api")) expect(!fresh.isInstalled(id), id)
        for (id in listOf("inbox", "agents", "chat")) expect(fresh.isInstalled(id), id)
        val hidden = PackageSurfaces.of(fresh::shows).hubItems
        expect("timers" !in hidden && "vault" !in hidden && "allAgents" !in hidden)
        expect("notes" in hidden && "shortcuts" in hidden && "workflows" in hidden)
        val withPage = store(legacy = false, chosen("status-page" to true))
        expect(withPage.isInstalled("status-page"))
        expect(withPage.isInstalled("status-check-api"))
        withPage.adopt(state("status-check-api" to false))
        expect(!withPage.isInstalled("status-check-api"))
    }

    fun testAnAccountInstallFromAnotherProjectDoesNotShowHere() {
        val fresh = store(legacy = false)
        fresh.adopt(state("timers" to true, "notes" to true, "agents" to true))
        expect(!fresh.isInstalled("timers") && !fresh.isInstalled("notes"))
        val chosenHere = store(legacy = false, chosen("timers" to true))
        chosenHere.adopt(state("timers" to false))
        expect(!chosenHere.isInstalled("timers"))
    }

    fun testALegacyProjectStillFollowsTheAccount() {
        val legacy = store(legacy = true, chosen("notes" to false, "timers" to true))
        legacy.adopt(state("notes" to true, "timers" to false, "inbox" to true))
        expect(legacy.isInstalled("notes"))
        expect(!legacy.isInstalled("timers"))
        expect(legacy.isInstalled("inbox"))
        expect(legacy.isInstalled("agents"))
    }

    fun testOrganizationPackagesComeFromOrganizationInstalls() {
        val states = listOf(
            OrgPackageState("acme", "owner", true, listOf(PluginInstall("crm", true), PluginInstall("ledger", true))),
            OrgPackageState("beta", "member", false, listOf(PluginInstall("marketing", false), PluginInstall("analytics", true))),
        )
        assertEq(setOf("crm", "analytics"), PackageGate.organizationInstalled(states))
        val surfaces = PackageSurfaces.of { it !in PackageGate.ORGANIZATION_PACKAGES || it in setOf("crm") }
        expect("crm" !in surfaces.hubItems)
        expect("marketing" in surfaces.hubItems && "competitors" in surfaces.hubItems)
    }

    fun testHubKeepsCoreItemsWhenEverythingIsRemoved() {
        val surfaces = PackageSurfaces(PackageGate.GATED)
        val ids = HubCatalog.groups(guest = false, admin = true, hidden = surfaces.hubItems) { true }.flatMap { it.items }.map { it.id }
        for (core in listOf("market", "settings", "projects", "setStatus", "ads", "admin")) expect(core in ids, core)
        for (gone in listOf("notes", "allAgents", "teammates", "status", "tour", "designSystem", "world", "orgs", "reportBug", "signOut", "crm", "workflows", "docs")) {
            expect(gone !in ids, gone)
        }
        val guestIds = HubCatalog.groups(guest = true, hidden = surfaces.hubItems) { true }.flatMap { it.items }.map { it.id }
        expect("signIn" !in guestIds)
        val shown = HubCatalog.groups(guest = true, hidden = PackageSurfaces.ALL.hubItems) { true }.flatMap { it.items }.map { it.id }
        expect("signIn" in shown && "notes" in shown)
    }

    fun testSearchPaletteAndQuickActionsFollowTheGate() {
        val surfaces = PackageSurfaces(setOf("agents", "inbox", "chat", "notes", "timers", "issues"))
        val hidden = surfaces.routeKinds
        val destinations = DestinationsSearchSource.routes(hidden)
        expect(destinations.none { it.kind == Route.Kind.Notes || it.kind == Route.Kind.Agents || it.kind == Route.Kind.Inbox })
        expect(destinations.any { it.kind == Route.Kind.Repos })
        val context = PaletteContext(target = ComposeTarget(repo = RepoRef("a", "b")), pendingApprovals = 2, hidden = hidden)
        val commands = CommandPalette.sections("", context).flatMap { it.entries }.map { it.command }
        assertEq(listOf<PaletteCommand>(PaletteCommand.SwitchProject, PaletteCommand.SelectTab(AppTab.Code)), commands)
        expect(CommandPalette.sections("notes", context).flatMap { it.entries }.none { it.command == PaletteCommand.Open(Route.Notes) })
        val open = CommandPalette.sections("", PaletteContext()).flatMap { it.entries }.map { it.id }
        expect("compose" in open && "tab:agents" in open && "timer" in open)
        val quick = ShellQuickAction.visible(listOf(Route.Agent("run-1"), Route.Repo(RepoRef("a", "b"), null)), hidden).map { it.action }
        assertEq(listOf(ShellQuickAction.Search, ShellQuickAction.RecentRepo), quick)
        assertEq(6, ShellQuickAction.visible(listOf(Route.Agent("run-1"), Route.Repo(RepoRef("a", "b"), null)), emptySet()).size)
    }

    fun testRepoTilesDropRemovedSections() {
        val model = RepoOverviewModel(Repository(owner = "a", name = "b"), null)
        val surfaces = PackageSurfaces(setOf("pull-requests", "issues", "agents"))
        val sections = model.tiles(surfaces.repoSections).map { it.section }
        expect(RepoHubSection.Code in sections && RepoHubSection.Commits in sections && RepoHubSection.Settings in sections)
        expect(RepoHubSection.Pulls !in sections && RepoHubSection.Issues !in sections && RepoHubSection.Board !in sections)
        expect(RepoHubSection.Agents !in sections && RepoHubSection.MergeQueue !in sections)
        assertEq(RepoHubSection.entries.size, model.tiles().size)
    }
}
