package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.navigation.CommandPalette
import com.forkmesh.nativeapp.navigation.DestinationsSearchSource
import com.forkmesh.nativeapp.navigation.PaletteCommand
import com.forkmesh.nativeapp.navigation.PaletteContext
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class AccountPackagesTest {
    private val hexA = "a".repeat(32)
    private val hexB = "b".repeat(32)

    private fun marketRows(): List<Map<String, Any?>> = listOf(
        mapOf(
            "id" to "status-page", "name" to "Status Page", "default" to true, "accountSync" to true, "dependencies" to emptyList<String>(),
            "settings" to listOf(
                mapOf("key" to "onlyProblems", "type" to "boolean", "label" to "Show only systems that need attention", "default" to false),
                mapOf(
                    "key" to "refreshMinutes", "type" to "choice", "label" to "Recheck healthy systems every", "default" to "15",
                    "options" to listOf(mapOf("value" to "2", "label" to "2 minutes"), mapOf("value" to "15", "label" to "15 minutes")),
                ),
                mapOf("key" to "bad key", "type" to "boolean"),
                mapOf("key" to "mystery", "type" to "slider"),
            ),
        ),
        mapOf(
            "id" to "status-check-api", "name" to "Status: API", "default" to true, "accountSync" to true,
            "dependencies" to listOf("status-page"), "monitors" to mapOf("systems" to listOf("api"), "prefixes" to emptyList<String>()),
        ),
        mapOf(
            "id" to "status-check-cores", "name" to "Status: Core Nodes", "default" to true, "accountSync" to true,
            "dependencies" to listOf(mapOf("id" to "status-page", "version" to "^2.0.0")), "monitors" to mapOf("prefixes" to listOf("core:")),
        ),
        mapOf("id" to "crm", "name" to "CRM", "dataScope" to "organization"),
    )

    private fun accountReply(page: Boolean, api: Boolean = page, changed: List<String> = emptyList()): Map<String, Any?> = mapOf(
        "ok" to true,
        "packages" to listOf(
            mapOf("id" to "status-page", "installed" to page, "settings" to mapOf("onlyProblems" to false, "refreshMinutes" to "15")),
            mapOf("id" to "status-check-api", "installed" to api, "settings" to emptyMap<String, Any?>()),
            mapOf("id" to "status-check-cores", "installed" to page, "settings" to emptyMap<String, Any?>()),
        ),
        "changed" to changed,
    )

    fun testMarketRowsDecodeTheAccountFields() {
        val packages = marketRows().mapNotNull(MarketPackage::fromJson)
        val page = packages.first { it.id == "status-page" }
        expect(page.accountSync)
        expect(page.isDefault)
        assertEq(listOf("onlyProblems", "refreshMinutes"), page.settings.map { it.key })
        assertEq(PackageSettingType.BOOLEAN, page.settings[0].type)
        assertEq(false, page.settings[0].default)
        assertEq("15", page.settings[1].default)
        assertEq("2 minutes", page.settings[1].optionLabel("2"))
        expect(!page.settings[1].accepts("7"))
        assertEq(PackagePhoneSurface.Native("status"), page.phoneSurface)
        val api = packages.first { it.id == "status-check-api" }
        assertEq(listOf("status-page"), api.dependencies)
        expect(api.monitors.covers("api"))
        expect(!api.monitors.covers("website"))
        val cores = packages.first { it.id == "status-check-cores" }
        assertEq(listOf("status-page"), cores.dependencies)
        expect(cores.monitors.covers("core:n1"))
        val crm = packages.first { it.id == "crm" }
        expect(!crm.accountSync && !crm.isDefault && crm.dependencies.isEmpty() && crm.settings.isEmpty() && crm.monitors.isEmpty)
    }

    fun testFirstRunIsInstalledWithDefaults() {
        val store = AccountPackagesStore(null, MemoryPrefStore())
        expect(store.state == null)
        expect(store.isInstalled("status-page"))
        expect(store.isInstalled("status-check-api"))
        assertEq(emptyMap<String, Any?>(), store.settings("status-page"))
        assertFails<PackagesException> { store.refresh() }
        expect(store.isInstalled("status-page"))
    }

    fun testRefreshCachesTheAccountStateAndSurvivesFailures() {
        val prefs = MemoryPrefStore()
        val transport = FakeTransport()
        transport.answer("GET", "/api/plugins/@account", accountReply(page = false))
        val store = AccountPackagesStore(PackagesClient(transport), prefs)
        var notified = 0
        store.addListener { notified++ }
        store.refresh()
        expect(!store.isInstalled("status-page"))
        expect(!store.isInstalled("status-check-api"))
        assertEq(1, notified)
        transport.answer("GET", "/api/plugins/@account", mapOf("error" to "invalid_session"), status = 401)
        val thrown = assertFails<PackagesException> { store.refresh() }
        assertEq("invalid_session", thrown.code)
        expect(!store.isInstalled("status-page"))
        val reopened = AccountPackagesStore(null, prefs)
        expect(!reopened.isInstalled("status-page"))
        assertEq("15", reopened.settings("status-page")["refreshMinutes"])
    }

    fun testInstallUninstallAndSettingsPostToTheAccountRoutes() {
        val transport = FakeTransport()
        val store = AccountPackagesStore(PackagesClient(transport), MemoryPrefStore())
        transport.answer("POST", "/api/plugins/@account/status-page/uninstall", accountReply(page = false, changed = listOf("status-page", "status-check-api", "status-check-cores")))
        val removed = store.uninstall("status-page")
        assertEq(listOf("status-page", "status-check-api", "status-check-cores"), removed.changed)
        assertEq(emptyMap<String, Any?>(), transport.last("POST", "/api/plugins/@account/status-page/uninstall")?.body)
        expect(!store.isInstalled("status-check-api"))
        transport.answer("POST", "/api/plugins/@account/status-check-api/install", accountReply(page = true, changed = listOf("status-check-api", "status-page")))
        store.install("status-check-api")
        expect(store.isInstalled("status-page"))
        transport.answer(
            "POST", "/api/plugins/@account/status-page/settings",
            mapOf("ok" to true, "packages" to listOf(mapOf("id" to "status-page", "installed" to true, "settings" to mapOf("onlyProblems" to true, "refreshMinutes" to "15")))),
        )
        store.saveSettings("status-page", mapOf("onlyProblems" to true))
        assertEq(mapOf("settings" to mapOf("onlyProblems" to true)), transport.last("POST", "/api/plugins/@account/status-page/settings")?.body)
        assertEq(true, store.settings("status-page")["onlyProblems"])
        transport.answer("POST", "/api/plugins/@account/status-page/settings", mapOf("error" to "invalid_setting", "key" to "refreshMinutes"), status = 400)
        assertEq("invalid_setting", assertFails<PackagesException> { store.saveSettings("status-page", mapOf("refreshMinutes" to "7")) }.code)
        assertEq(true, store.settings("status-page")["onlyProblems"])
        assertFails<PackagesException> { store.install("../crm") }
    }

    fun testSetPlacesPostsToThePlacesRoute() {
        val transport = FakeTransport()
        val prefs = MemoryPrefStore()
        val store = AccountPackagesStore(PackagesClient(transport), prefs)
        transport.answer(
            "POST", "/api/plugins/@account/status-page/places",
            mapOf(
                "ok" to true,
                "packages" to listOf(
                    mapOf("id" to "status-page", "installed" to true, "places" to listOf("desktop", "web"), "availablePlaces" to listOf("mobile", "desktop", "web")),
                ),
            ),
        )
        store.setPlaces("status-page", listOf("web", "desktop", "fridge"))
        assertEq(mapOf("places" to listOf("desktop", "web")), transport.last("POST", "/api/plugins/@account/status-page/places")?.body)
        expect(store.isInstalled("status-page"))
        expect(!store.shows("status-page"))
        assertEq(listOf("mobile", "desktop", "web"), store.state?.find("status-page")?.availablePlaces)
        assertEq("invalid_places", assertFails<PackagesException> { store.setPlaces("status-page", emptyList()) }.code)
        assertEq("invalid_places", assertFails<PackagesException> { store.setPlaces("status-page", listOf("fridge")) }.code)
        transport.answer("POST", "/api/plugins/@account/status-page/places", mapOf("error" to "plugin_not_installed"), status = 409)
        val refused = assertFails<PackagesException> { store.setPlaces("status-page", listOf("mobile")) }
        assertEq("install_first", refused.code)
        assertEq("Install this package first.", refused.message)
        expect(!store.shows("status-page"))
        val reopened = AccountPackagesStore(null, prefs)
        assertEq(listOf("desktop", "web"), reopened.state?.find("status-page")?.places)
        expect(reopened.isInstalled("status-page") && !reopened.shows("status-page"))
    }

    fun testCatalogFillsDefaultsAndDependencyNotes() {
        val prefs = MemoryPrefStore()
        val store = AccountPackagesStore(null, prefs)
        store.rememberCatalog(marketRows().mapNotNull(MarketPackage::fromJson))
        assertEq(setOf("status-page", "status-check-api", "status-check-cores"), store.catalog.keys)
        assertEq(mapOf<String, Any?>("onlyProblems" to false, "refreshMinutes" to "15"), store.settings("status-page"))
        assertEq(store.catalog.keys, AccountPackagesStore(null, prefs).catalog.keys)
        val catalog = store.catalog.values
        val page = store.catalog.getValue("status-page")
        val api = store.catalog.getValue("status-check-api")
        assertEq(listOf("status-check-api", "status-check-cores"), AccountPackageGraph.dependents("status-page", catalog).map { it.id })
        assertEq(listOf("status-page"), AccountPackageGraph.dependencies("status-check-api", catalog).map { it.id })
        assertEq(
            "Removing Status Page also removes the 2 packages that need it from all your devices: Status: API and Status: Core Nodes.",
            AccountPackageGraph.removeNote(page, catalog) { true },
        )
        assertEq("Removing Status: API takes it off all your devices.", AccountPackageGraph.removeNote(api, catalog) { true })
        assertEq("Installing Status: API also brings back Status Page on all your devices.", AccountPackageGraph.installNote(api, catalog) { it != "status-page" })
        assertEq("Installing Status: API adds it to all your devices.", AccountPackageGraph.installNote(api, catalog) { true })
        assertEq("Installed Status: API and Status Page on all your devices.", AccountPackageGraph.changedNote(true, api, listOf("status-check-api", "status-page"), catalog))
        assertEq("Removed Status Page on all your devices.", AccountPackageGraph.changedNote(false, page, listOf("status-page"), catalog))
    }

    fun testInstallInANewProjectRecordsThePackageAndItsMissingDependencies() {
        val transport = FakeTransport()
        val prefs = MemoryPrefStore()
        val store = AccountPackagesStore(PackagesClient(transport), prefs) { false }
        store.rememberCatalog(marketRows().mapNotNull(MarketPackage::fromJson))
        store.adopt(AccountPackagesState.fromJson(accountReply(page = false)))
        expect(store.perProject)
        expect(!store.isInstalled("status-page") && !store.isInstalled("status-check-api"))
        transport.answer("POST", "/api/plugins/@account/status-check-api/install", accountReply(page = true, changed = listOf("status-check-api", "status-page")))
        val answer = store.install("status-check-api")
        expect(transport.last("POST", "/api/plugins/@account/status-check-api/install") != null)
        assertEq(listOf("status-check-api", "status-page"), answer.changed)
        assertEq(mapOf("status-check-api" to true, "status-page" to true), store.projectChoices)
        expect(store.isInstalled("status-page") && store.isInstalled("status-check-api") && store.isInstalled("status-check-cores"))
        assertEq(store.projectChoices, AccountPackagesStore(null, prefs) { false }.projectChoices)
        transport.answer("POST", "/api/plugins/@account/status-page/install", mapOf("error" to "offline"), status = 503)
        store.uninstall("status-page")
        assertFails<PackagesException> { store.install("status-page") }
        assertEq(false, store.projectChoices["status-page"])
    }

    fun testRemovalInANewProjectMakesNoServerCallAndLeavesTheAccountInstalled() {
        val transport = FakeTransport()
        val prefs = MemoryPrefStore()
        val store = AccountPackagesStore(PackagesClient(transport), prefs) { false }
        store.rememberCatalog(marketRows().mapNotNull(MarketPackage::fromJson))
        store.adopt(AccountPackagesState.fromJson(accountReply(page = true)))
        expect(store.isInstalled("status-page") && store.isInstalled("status-check-api"))
        var notified = 0
        store.addListener { notified++ }
        val removed = store.uninstall("status-page")
        expect(transport.sent.isEmpty())
        assertEq(listOf("status-page", "status-check-api", "status-check-cores"), removed.changed)
        assertEq(true, store.state?.find("status-page")?.installed)
        expect(!store.isInstalled("status-page") && !store.isInstalled("status-check-api") && !store.isInstalled("status-check-cores"))
        assertEq(1, notified)
        val reopened = AccountPackagesStore(null, prefs) { false }
        expect(!reopened.isInstalled("status-page"))
        expect(AccountPackagesStore(null, prefs) { true }.isInstalled("status-page"))
        val legacy = AccountPackagesStore(PackagesClient(transport), MemoryPrefStore()) { true }
        transport.answer("POST", "/api/plugins/@account/status-page/uninstall", accountReply(page = false, changed = listOf("status-page")))
        legacy.uninstall("status-page")
        expect(transport.last("POST", "/api/plugins/@account/status-page/uninstall") != null)
        expect(legacy.projectChoices.isEmpty())
        expect(!legacy.isInstalled("status-page"))
    }

    fun testProjectNotesNameThisProject() {
        val store = AccountPackagesStore(null, MemoryPrefStore())
        store.rememberCatalog(marketRows().mapNotNull(MarketPackage::fromJson))
        val catalog = store.catalog.values
        val page = store.catalog.getValue("status-page")
        val api = store.catalog.getValue("status-check-api")
        assertEq("Your other projects keep it.", AccountPackageGraph.removeNote(api, catalog, project = true) { true })
        assertEq(
            "Removing Status Page also removes the 2 packages that need it from this project: Status: API and Status: Core Nodes. Your other projects keep it.",
            AccountPackageGraph.removeNote(page, catalog, project = true) { true },
        )
        assertEq("Installing Status: API adds it to this project.", AccountPackageGraph.installNote(api, catalog, project = true) { true })
        assertEq("Installing Status: API also brings back Status Page in this project.", AccountPackageGraph.installNote(api, catalog, project = true) { it != "status-page" })
        assertEq("Installed Status: API and Status Page in this project.", AccountPackageGraph.changedNote(true, api, listOf("status-check-api", "status-page"), catalog, project = true))
        assertEq("Removed Status Page from this project.", AccountPackageGraph.changedNote(false, page, listOf("status-page"), catalog, project = true))
    }

    fun testMarketModelReadsAccountInstallState() {
        val transport = FakeTransport()
        transport.answer("GET", "/api/market/packages", mapOf("packages" to marketRows()))
        val account = mapOf("status-page" to false)
        val model = MarketModel(PackagesClient(transport), accountSwitch = { account[it] })
        model.refresh(emptyList())
        expect(!model.isInstalled(model.pkg("status-page")!!))
        expect(model.isInstalled(model.pkg("status-check-api")!!))
    }

    fun testStatusMonitorsListAddRemoveAndValidate() {
        val prefs = MemoryPrefStore()
        val transport = FakeTransport()
        val store = StatusMonitorsStore(PackagesClient(transport), prefs)
        assertEq(StatusMonitorList(), store.list)
        transport.answer(
            "GET", "/api/status-monitors",
            mapOf(
                "ok" to true, "installed" to true, "max" to 20,
                "monitors" to listOf(
                    mapOf("id" to hexA, "name" to "Blog", "url" to "https://blog.example.com/"),
                    mapOf("id" to "nope", "name" to "Bad", "url" to "https://x.example.com/"),
                    mapOf("id" to hexB, "name" to "Creds", "url" to "https://u:p@x.example.com/"),
                ),
            ),
        )
        store.refresh()
        assertEq(listOf(StatusMonitor(hexA, "Blog", "https://blog.example.com/")), store.monitors)
        assertEq(store.monitors, StatusMonitorsStore(null, prefs).monitors)
        transport.answer(
            "POST", "/api/status-monitors/monitors",
            mapOf(
                "ok" to true, "monitor" to mapOf("id" to hexB, "name" to "Shop", "url" to "http://shop.example.com"),
                "monitors" to listOf(
                    mapOf("id" to hexA, "name" to "Blog", "url" to "https://blog.example.com/"),
                    mapOf("id" to hexB, "name" to "Shop", "url" to "http://shop.example.com"),
                ),
            ),
        )
        store.add(" Shop ", " http://shop.example.com ")
        assertEq(mapOf("name" to "Shop", "url" to "http://shop.example.com"), transport.last("POST", "/api/status-monitors/monitors")?.body)
        assertEq(2, store.monitors.size)
        assertEq("invalid_name", assertFails<PackagesException> { store.add("", "https://a.example.com") }.code)
        assertEq("invalid_name", assertFails<PackagesException> { store.add("x".repeat(41), "https://a.example.com") }.code)
        assertEq("invalid_url", assertFails<PackagesException> { store.add("A", "ftp://a.example.com") }.code)
        assertEq("invalid_url", assertFails<PackagesException> { store.add("A", "https://") }.code)
        assertEq("invalid_url", assertFails<PackagesException> { store.add("A", "https://a.example.com/" + "x".repeat(300)) }.code)
        transport.answer("DELETE", "/api/status-monitors/monitors/$hexA", mapOf("ok" to true, "monitors" to listOf(mapOf("id" to hexB, "name" to "Shop", "url" to "http://shop.example.com"))))
        store.remove(hexA)
        assertEq(listOf(hexB), store.monitors.map { it.id })
        assertEq("monitor_not_found", assertFails<PackagesException> { store.remove("../x") }.code)
        transport.answer("POST", "/api/status-monitors/monitors", mapOf("error" to "too_many_monitors"), status = 409)
        val full = assertFails<PackagesException> { store.add("More", "https://more.example.com") }
        assertEq("too_many_monitors", full.code)
        expect(full.message!!.contains("most monitors"))
        store.clear()
        expect(store.monitors.isEmpty())
        expect(!store.list.installed)
    }

    fun testRemovedStatusPageLeavesHubSearchAndPalette() {
        val hidden = PackageSurfaces(setOf("status-page")).routeKinds
        assertEq(setOf(Route.Kind.Status), hidden)
        assertEq(emptySet<Route.Kind>(), PackageSurfaces.ALL.routeKinds)
        assertEq(setOf("status"), PackageSurfaces(setOf("status-page")).hubItems)
        expect(DestinationsSearchSource().search("status").any { it.route == Route.Status })
        expect(DestinationsSearchSource(DestinationsSearchSource.routes(hidden)).search("status").none { it.route == Route.Status })
        val open = PaletteCommand.Open(Route.Status)
        expect(CommandPalette.sections("status", PaletteContext()).flatMap { it.entries }.any { it.command == open })
        val context = PaletteContext(recents = listOf(Route.Status, Route.Notes), hidden = hidden)
        expect(CommandPalette.sections("status", context).flatMap { it.entries }.none { it.command == open })
        assertEq(listOf<PaletteCommand>(PaletteCommand.Open(Route.Notes)), CommandPalette.sections("", context).first { it.id == "recent" }.entries.map { it.command })
    }

    fun testUpdateDotsFollowSeenAndLatestVersions() {
        val prefs = MemoryPrefStore()
        val store = AccountPackagesStore(null, prefs)
        expect(store.updated.isEmpty(), "no catalog, no dots")
        store.rememberCatalog(listOf(MarketPackage(id = "crm", name = "CRM", version = "1.38.0"), MarketPackage(id = "notes", name = "Notes", version = "2.0.0")))
        assertEq(setOf("crm", "notes"), store.updated)
        expect(store.isUpdated("crm"), "never opened counts as updated")
        store.markViewed("crm")
        assertEq(setOf("notes"), store.updated)
        store.rememberCatalog(listOf(MarketPackage(id = "crm", name = "CRM", version = "1.39.0")))
        assertEq(setOf("crm", "notes"), store.updated)
        store.markAllViewed()
        expect(store.updated.isEmpty(), "all viewed")
        expect(store.canShowUpdates, "dots can be shown again")
        store.markAllUnviewed()
        assertEq(setOf("crm", "notes"), store.updated)
        val again = AccountPackagesStore(null, prefs)
        assertEq(setOf("crm", "notes"), again.updated)
        expect(prefs.getString(AccountPackagesStore.VERSIONS_KEY).orEmpty().contains("\"latest\""), "stored under packages/versions")
    }

    fun testUpdateDotsMapHubItemsToPackages() {
        val store = AccountPackagesStore(null, MemoryPrefStore())
        val crmKind = PackageGate.ROUTE_KINDS.entries.first { it.value == "crm" }.key
        store.rememberCatalog(listOf(MarketPackage(id = "crm", name = "CRM", version = "1.0.0")))
        expect(store.isUpdated("crm-hub-item", crmKind), "a hub item maps to its package by route")
        store.markViewed("crm-hub-item", crmKind)
        expect(!store.isUpdated("crm"), "viewing the hub item marks the package")
        expect(store.canMarkUpdated("crm-hub-item", crmKind), "a viewed package can show its dot again")
    }

    fun testVersionMapIgnoresBadEntries() {
        assertEq(mapOf("crm" to "1.0.0"), AccountPackagesStore.versionMap(mapOf("crm" to "1.0.0", "bad id" to "1.0.0", "notes" to "", "x" to 3)))
        assertEq(setOf("b"), AccountPackagesStore.changed(mapOf("a" to "1"), mapOf("a" to "1", "b" to "2")))
    }
}
