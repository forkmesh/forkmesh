package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.api.ApiJsonResult
import com.forkmesh.nativeapp.control.ControlCalling
import com.forkmesh.nativeapp.control.ControlEvent
import com.forkmesh.nativeapp.control.ControlFamilyChannel
import com.forkmesh.nativeapp.control.DesktopControlFamilyName
import com.forkmesh.nativeapp.control.LocalControlException
import com.forkmesh.nativeapp.navigation.CommandPalette
import com.forkmesh.nativeapp.navigation.DestinationsSearchSource
import com.forkmesh.nativeapp.navigation.HubCatalog
import com.forkmesh.nativeapp.navigation.PaletteCommand
import com.forkmesh.nativeapp.navigation.PaletteContext
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class FakeTransport : PackagesTransport {
    data class Sent(val method: String, val path: String, val query: List<Pair<String, String>>, val body: Map<String, Any?>?)

    val sent = mutableListOf<Sent>()
    val replies = HashMap<String, ApiJsonResult>()

    fun answer(method: String, path: String, body: Map<String, Any?>, status: Int = 200) {
        replies["$method $path"] = ApiJsonResult(status, body)
    }

    fun last(method: String, path: String): Sent? = sent.lastOrNull { it.method == method && it.path == path }

    override fun send(method: String, path: String, query: List<Pair<String, String>>, body: Map<String, Any?>?): ApiJsonResult {
        sent.add(Sent(method, path, query, body))
        return replies["$method $path"] ?: ApiJsonResult(200, mapOf("ok" to true))
    }
}

class FakeChannel : ControlCalling {
    val requests = mutableListOf<Pair<String, Map<String, Any?>>>()
    val replies = HashMap<String, Map<String, Any?>>()
    val failures = HashMap<String, Exception>()
    val taps = mutableListOf<(ControlEvent) -> Unit>()

    fun params(method: String) = requests.lastOrNull { it.first == method }?.second

    override fun request(method: String, params: Map<String, Any?>, timeoutMs: Long): Map<String, Any?> {
        requests.add(method to params)
        failures[method]?.let { throw it }
        return replies[method] ?: emptyMap()
    }

    override fun addEventTap(tap: (ControlEvent) -> Unit): Any {
        taps.add(tap)
        return tap
    }

    override fun removeEventTap(token: Any) {
        taps.remove(token)
    }
}

class MarketTest {
    private val hexA = "a".repeat(32)
    private val hexB = "b".repeat(32)
    private val solAddress = "7EcDhSYGxXyscszYEp35KHN8vvw3svAuLKTzXwCFLtV"

    fun testTextRulesAndErrors() {
        expect(PackagesText.isPackageId("crm"))
        expect(!PackagesText.isPackageId("Crm"))
        expect(!PackagesText.isPackageId("1crm"))
        expect(PackagesText.isPackageId("my-pkg-"))
        expect(!PackagesText.isOrgName("my-pkg-"))
        expect(PackagesText.isHexId(hexA))
        expect(!PackagesText.isHexId("A".repeat(32)))
        expect(PackagesText.matches(" CR ", "x", "crm"))
        assertEq("Maintainers cannot rate their own package.", PackagesException.sentence("maintainer_cannot_rate"))
        assertEq("The server answered HTTP 502.", PackagesException.sentence("", 502))
        assertEq("Weird thing", PackagesException.sentence("weird_thing"))
        val error = PackagesException.from(ApiJsonResult(422, mapOf("error" to "review_invalid", "fields" to mapOf("problem" to "repetitive"))))
        assertEq("repetitive", error.fields["problem"])
        assertEq("Some review fields need more detail.", PackagesException.describe(error))
        assertEq("Couldn't reach the server.", PackagesException.describe(java.io.IOException()))
    }

    fun testPackageScreenshotsKeepOnlyHttpsImages() {
        val notes = MarketPackage.fromJson(
            mapOf(
                "id" to "notes",
                "screenshots" to listOf(
                    mapOf("url" to "https://forkmesh.com/market/previews/packages/notes/preview/market.webp", "alt" to "Notes list"),
                    mapOf("url" to "http://forkmesh.com/market/previews/packages/notes/preview/plain.png", "alt" to "Plain"),
                    mapOf("url" to "javascript:alert(1)", "alt" to "Script"),
                    mapOf("url" to "", "alt" to "Empty"),
                ),
            ),
        )!!
        assertEq(listOf(PackageScreenshot("https://forkmesh.com/market/previews/packages/notes/preview/market.webp", "Notes list")), notes.screenshots)
        assertEq(emptyList<PackageScreenshot>(), MarketPackage.fromJson(mapOf("id" to "crm"))!!.screenshots)
    }

    fun testInstallAllPlanCoversWhatThePhoneCanInstall() {
        val transport = FakeTransport()
        transport.answer(
            "GET", "/api/market/packages",
            mapOf(
                "packages" to listOf(
                    mapOf("id" to "notes", "name" to "Notes", "accountSync" to true, "dataScope" to "account"),
                    mapOf("id" to "tasks", "name" to "Tasks", "accountSync" to true, "dataScope" to "account", "default" to true),
                    mapOf("id" to "sign-in", "name" to "Sign in", "surfaces" to mapOf("desktop" to true)),
                    mapOf("id" to "crm", "name" to "CRM", "dataScope" to "organization", "surfaces" to mapOf("desktop" to true)),
                    mapOf("id" to "winamp", "name" to "Winamp", "bundled" to true, "surfaces" to mapOf("desktop" to true)),
                    mapOf("id" to "gemma", "name" to "Gemma", "type" to "agent", "accountSync" to true, "dataScope" to "account"),
                    mapOf("id" to "skin", "name" to "Skin", "type" to "theme", "accountSync" to true, "dataScope" to "account"),
                    mapOf("id" to "notes-extra", "name" to "Notes extra", "accountSync" to true, "dataScope" to "account", "dependencies" to listOf("notes")),
                ),
            ),
        )
        val model = MarketModel(PackagesClient(transport), accountSwitch = { if (it == "tasks") true else if (it == "notes" || it == "gemma" || it == "skin" || it == "notes-extra") false else null }, deviceSwitch = { if (it == "sign-in") false else null })
        model.refresh(null)
        val plan = model.installAllPlan()
        assertEq(listOf("notes"), plan.account.map { it.id })
        assertEq(listOf("sign-in"), plan.phone.map { it.id })
        assertEq(2, plan.skipped)
        assertEq(2, plan.count)
    }

    fun testMarketCatalogFiltersAndInstall() {
        val transport = FakeTransport()
        transport.answer(
            "GET", "/api/market/packages",
            mapOf(
                "packages" to listOf(
                    mapOf("id" to "crm", "name" to "CRM", "category" to "Business", "dataScope" to "organization", "surfaces" to mapOf("desktop" to true)),
                    mapOf("id" to "notes", "name" to "", "bundled" to true, "default" to true, "accountSync" to true, "dataScope" to "account"),
                    mapOf("id" to "Bad", "name" to "x"),
                    mapOf("id" to "skins", "name" to "Skins", "category" to "Look", "surfaces" to mapOf("web" to "/skins"), "permissions" to listOf(mapOf("id" to "p", "label" to "l"))),
                    mapOf("id" to "orb", "name" to "Orb", "surfaces" to mapOf("world" to true)),
                ),
            ),
        )
        transport.answer("GET", "/api/market/installs", mapOf("packages" to mapOf("crm" to mapOf("installs" to 3, "downloads" to 9))))
        transport.answer("GET", "/api/market/reviews/summary", mapOf("packages" to mapOf("crm" to mapOf("average" to 7.0, "count" to 2, "histogram" to mapOf("5" to 2)))), 500)
        transport.answer("GET", "/api/plugins/acme", mapOf("organization" to "acme", "role" to "owner", "canManage" to true, "plugins" to emptyList<Any>()))
        val model = MarketModel(PackagesClient(transport))
        model.refresh(listOf("acme", "Bad Org"))
        assertEq(4, model.packages.size)
        assertEq("notes", model.pkg("notes")?.name)
        assertEq(3, model.counts["crm"]?.installs)
        assertEq("Couldn't load ratings.", model.partialError)
        assertEq(1, model.organizations.size)
        assertEq(PackagePhoneSurface.Native("crm"), model.pkg("crm")?.phoneSurface)
        assertEq(PackagePhoneSurface.Web("/skins"), model.pkg("skins")?.phoneSurface)
        assertEq(PackagePhoneSurface.WorldOnly, model.pkg("orb")?.phoneSurface)
        expect(model.pkg("skins")!!.hasSensitivePermissions)
        assertEq(listOf("Business", "Look", "Other"), model.shelves.map { it.category })
        model.filter = MarketFilter.INSTALLED
        assertEq(listOf("notes"), model.visible.map { it.id })
        model.filter = MarketFilter.NOT_INSTALLED
        model.query = "cr"
        assertEq(listOf("crm"), model.visible.map { it.id })
        assertEq(listOf("acme"), model.installTargets(model.pkg("crm")!!).map { it.organization })
        transport.answer(
            "POST", "/api/plugins/acme/crm/install",
            mapOf("organization" to "acme", "role" to "owner", "canManage" to true, "plugins" to listOf(mapOf("id" to "crm", "installed" to true))),
        )
        expect(model.install(model.pkg("crm")!!, "acme"))
        expect(model.isInstalled(model.pkg("crm")!!))
        assertEq(mapOf("market" to "forkmesh", "package" to "crm", "source" to "mobile"), transport.last("POST", "/api/market/reviews/uses")?.body)
        expect(!model.install(model.pkg("crm")!!, "Not An Org"))
        assertEq("That organization was not found, or you are not a member.", model.errorText)
        assertEq(MarketFilter.NOT_INSTALLED, MarketFilter.of("not-installed"))
        assertEq("5.0 (2)", RatingSummary.fromJson(mapOf("average" to 7.0, "count" to 2)).label)
    }

    fun testInstallStateFollowsWhatThePhoneCanKnow() {
        val transport = FakeTransport()
        transport.answer(
            "GET", "/api/market/packages",
            mapOf(
                "packages" to listOf(
                    mapOf("id" to "winamp", "name" to "Winamp Classic", "category" to "Themes", "type" to "theme", "bundled" to true, "surfaces" to mapOf("desktop" to true)),
                    mapOf("id" to "capture", "name" to "Capture", "category" to "Utilities", "type" to "extension.native", "bundled" to true, "default" to true, "surfaces" to mapOf("desktop" to true)),
                    mapOf("id" to "crm", "name" to "CRM", "category" to "Business", "dataScope" to "organization", "bundled" to true, "surfaces" to mapOf("desktop" to true, "web" to "/crm")),
                    mapOf("id" to "agents", "name" to "Agents", "category" to "Agents", "bundled" to true, "accountSync" to true, "installedBefore" to "2026-10-11"),
                    mapOf("id" to "sign-in", "name" to "Sign in", "category" to "Account", "bundled" to true),
                    mapOf("id" to "orb", "name" to "Orb", "category" to "3D", "type" to "virtual.object", "free" to false, "surfaces" to mapOf("world" to true)),
                ),
            ),
        )
        transport.answer("GET", "/api/market/installs", mapOf("packages" to mapOf("winamp" to mapOf("installs" to 1500, "downloads" to 4000), "crm" to mapOf("installs" to 12, "downloads" to 30))))
        transport.answer(
            "GET", "/api/market/reviews/summary",
            mapOf(
                "packages" to mapOf("orb" to mapOf("average" to 4.5, "count" to 2), "crm" to mapOf("average" to 3.0, "count" to 9)),
                "usage" to mapOf("agents" to mapOf("people" to 40, "opens" to 300), "crm" to mapOf("people" to 5, "opens" to 9)),
            ),
        )
        transport.answer("GET", "/api/plugins/acme", mapOf("organization" to "acme", "role" to "owner", "canManage" to true, "plugins" to listOf(mapOf("id" to "crm", "installed" to true))))
        var legacy = true
        val model = MarketModel(PackagesClient(transport)) { if (it == "sign-in") false else if (it == "crm") true else null }
        model.legacy = { legacy }
        model.refresh(listOf("acme"))
        val pkg = { id: String -> model.pkg(id)!! }

        expect(!model.isInstalled(pkg("winamp")), "a bundled desktop package is not installed just because it ships with the desktop")
        expect(model.isInstalled(pkg("capture")), "a default desktop package is on")
        assertEq(MarketCardAction.DESKTOP, model.cardAction(pkg("winamp"), opens = false))
        assertEq(PackageInstallMode.DESKTOP, model.installMode(pkg("winamp")))
        assertEq(PackageInstallMode.PHONE, model.installMode(pkg("sign-in")))
        assertEq(MarketCardAction.INSTALL_ON_PHONE, model.cardAction(pkg("sign-in"), opens = true))

        expect(model.isInstalled(pkg("agents")), "an account package the store does not know yet follows the legacy fallback")
        legacy = false
        expect(model.isInstalled(pkg("agents")), "a starter package is on in a new project")
        assertEq(MarketCardAction.OPEN, model.cardAction(pkg("agents"), opens = true))
        model.starterPack = { StarterPack.MINIMAL }
        expect(!model.isInstalled(pkg("agents")), "the Minimal pack leaves Agents out of a new project")
        assertEq(MarketCardAction.INSTALL, model.cardAction(pkg("agents"), opens = true))
        model.starterPack = { StarterPack.EVERYTHING }
        expect(model.isInstalled(pkg("agents")))
        model.starterPack = { StarterPack.STANDARD }

        expect(model.isInstalled(pkg("crm")))
        assertEq(MarketCardAction.OPEN, model.cardAction(pkg("crm"), opens = true))
        transport.answer("GET", "/api/plugins/acme", mapOf("error" to "unavailable"), 503)
        model.refresh(listOf("acme"))
        expect(model.isInstalled(pkg("crm")), "a failed organization fetch keeps the last known install")
        expect(!model.organizationsComplete)
        val fresh = MarketModel(PackagesClient(transport)) { null }
        fresh.organizationCache = { it == "crm" }
        fresh.refresh(null)
        expect(fresh.isInstalled(fresh.pkg("crm")!!), "unknown organizations fall back to the Home gate's cached answer")
        fresh.organizationCache = { null }
        expect(!fresh.isInstalled(fresh.pkg("crm")!!))
        assertEq(MarketCardAction.NEEDS_ORGANIZATION, fresh.cardAction(fresh.pkg("crm")!!, opens = true))

        assertEq(PackageUsage(40, 300), model.usage["agents"])
        assertEq(listOf("★ 3.0 (9)", "12 installs", "5 users"), MarketText.stats(model.counts["crm"], model.usage["crm"], model.ratings["crm"]))
        assertEq(listOf("1.5k installs"), MarketText.stats(model.counts["winamp"], null, null))
        assertEq("12k", MarketText.compact(12_345))
        assertEq("World object", MarketText.typeLabel("virtual.object"))

        model.options = MarketOptions(sort = MarketSort.INSTALLS)
        assertEq(listOf("winamp", "crm"), model.shelves.single().packages.take(2).map { it.id })
        model.options = MarketOptions(sort = MarketSort.USED)
        assertEq(listOf("agents", "crm"), model.shelves.single().packages.take(2).map { it.id })
        model.options = MarketOptions(sort = MarketSort.RATING)
        assertEq(listOf("orb", "crm"), model.shelves.single().packages.take(2).map { it.id })
        model.options = MarketOptions(sort = MarketSort.NAME, price = MarketPrice.PAID)
        assertEq(listOf("orb"), model.visible.map { it.id })
        model.options = MarketOptions(platform = MarketPlatform.WEB)
        assertEq(listOf("crm"), model.visible.map { it.id })
        model.options = MarketOptions(platform = MarketPlatform.PHONE)
        assertEq(listOf("agents", "crm", "sign-in"), model.visible.map { it.id }.sorted())
        model.options = MarketOptions(mode = PackageInstallMode.DESKTOP, type = "theme")
        assertEq(listOf("winamp"), model.visible.map { it.id })
        assertEq(2, model.options.activeFilters)
        model.options = MarketOptions(category = "Business")
        assertEq(listOf("crm"), model.visible.map { it.id })
        assertEq(MarketOptions(), model.options.cleared())
        assertEq(listOf("3D", "Account", "Agents", "Business", "Themes", "Utilities"), model.categories)
        assertEq(MarketSort.USED, MarketSort.of("used"))
    }

    fun testDependentPackagesAreNestedUnderTheirParent() {
        val transport = FakeTransport()
        transport.answer(
            "GET", "/api/market/packages",
            mapOf(
                "packages" to listOf(
                    mapOf("id" to "status-page", "name" to "Status Page", "category" to "Utilities", "bundled" to true, "default" to true, "accountSync" to true),
                    mapOf("id" to "status-check-api", "name" to "Status: API", "category" to "Status checks", "bundled" to true, "default" to true, "accountSync" to true, "dependencies" to listOf("status-page")),
                    mapOf("id" to "status-check-cores", "name" to "Status: Core Nodes", "category" to "Status checks", "bundled" to true, "default" to true, "accountSync" to true, "dependencies" to listOf("status-page")),
                    mapOf("id" to "orphan", "name" to "Orphan", "category" to "Tools", "dependencies" to listOf("missing")),
                ),
            ),
        )
        val model = MarketModel(PackagesClient(transport))
        model.refresh(emptyList())
        assertEq("status-page", model.marketParent(model.pkg("status-check-api")!!))
        assertEq(null, model.marketParent(model.pkg("orphan")!!))
        assertEq(listOf("status-check-api", "status-check-cores"), model.nested(model.pkg("status-page")!!).map { it.id })
        assertEq(listOf("status-page", "orphan"), model.visible.map { it.id })
        assertEq(listOf("Tools", "Utilities"), model.shelves.map { it.category })
        model.query = "core nodes"
        assertEq(listOf("status-page"), model.visible.map { it.id })
    }

    fun testSignInIsSwitchedPerPhoneAndStartsOn() {
        val prefs = com.forkmesh.nativeapp.api.MemoryPrefStore()
        assertEq(true, PhonePackages.isOn(PhonePackages.SIGN_IN, prefs, legacy = true))
        assertEq(true, PhonePackages.isOn(PhonePackages.SIGN_IN, prefs, legacy = false))
        assertEq(true, PhonePackages.isOn("crm", prefs, legacy = true))
        assertEq(null, PhonePackages.isOn("workflows", prefs, legacy = true))
        PhonePackages.set(PhonePackages.SIGN_IN, false, prefs)
        assertEq(false, PhonePackages.isOn("sign-in", prefs, legacy = true))
        val transport = FakeTransport()
        transport.answer(
            "GET", "/api/market/packages",
            mapOf("packages" to listOf(mapOf("id" to "sign-in", "name" to "Sign in", "bundled" to true, "installedBefore" to "2026-10-11"))),
        )
        val model = MarketModel(PackagesClient(transport)) { PhonePackages.isOn(it, prefs, legacy = true) }
        model.refresh(emptyList())
        val pkg = model.pkg("sign-in")!!
        assertEq("2026-10-11", pkg.installedBefore)
        assertEq(PackagePhoneSurface.Native("signIn"), pkg.phoneSurface)
        expect(!model.isInstalled(pkg))
        PhonePackages.set(PhonePackages.SIGN_IN, true, prefs)
        expect(model.isInstalled(pkg))
        val tour = MarketPackage(id = "tour", name = "Tour", desktop = true, bundled = true)
        assertEq(PackagePhoneSurface.Native("tour"), tour.phoneSurface)
    }

    fun testUninstalledFromThePhoneLeavesTheHomeMenuUntilReinstalled() {
        val prefs = com.forkmesh.nativeapp.api.MemoryPrefStore()
        val transport = FakeTransport()
        transport.answer(
            "GET", "/api/market/packages",
            mapOf("packages" to listOf(mapOf("id" to "crm", "name" to "CRM", "dataScope" to "organization"))),
        )
        transport.answer("GET", "/api/plugins/acme", mapOf("organization" to "acme", "canManage" to true, "plugins" to listOf(mapOf("id" to "crm", "installed" to true))))
        val model = MarketModel(PackagesClient(transport)) { PhonePackages.isOn(it, prefs, legacy = true) }
        model.refresh(listOf("acme"))
        val crm = model.pkg("crm")!!
        expect(model.isInstalled(crm))
        assertEq(true, PhonePackages.isOn("crm", prefs, legacy = true))
        val installed = { id: String -> id == "crm" && PhonePackages.isOn(id, prefs, legacy = true) != false }
        expect(PackageSurfaces.of(installed).showsRoute(Route.Crm(null)))

        PhonePackages.set("crm", false, prefs)
        assertEq(setOf("crm"), PhonePackages.removed(prefs))
        expect(!model.isInstalled(crm))
        expect(model.organizations.first().isInstalled("crm"), "the device uninstall keeps the organization's install")
        model.filter = MarketFilter.INSTALLED
        expect(model.visible.isEmpty())
        val surfaces = PackageSurfaces.of(installed)
        expect(!surfaces.showsRoute(Route.Crm(null)))
        expect(!surfaces.showsRoute(Route.Contact("c1")))
        expect("crm" in surfaces.hubItems)

        PhonePackages.set("crm", true, prefs)
        expect(PhonePackages.removed(prefs).isEmpty())
        expect(model.isInstalled(crm))
        expect(PackageSurfaces.of(installed).showsRoute(Route.Crm(null)))
    }

    fun testActivityIsReportedAndThrottled() {
        val transport = FakeTransport()
        transport.answer("POST", "/api/market/reviews/activity", mapOf("ok" to true, "kind" to "open", "views" to 2, "opens" to 7))
        val client = PackagesClient(transport)
        assertEq(2 to 7, client.reportActivity("calories", ActivityKind.OPEN))
        assertEq(
            mapOf("market" to "forkmesh", "package" to "calories", "kind" to "open", "source" to "mobile"),
            transport.last("POST", "/api/market/reviews/activity")?.body,
        )
        assertEq("unknown_package", assertFails<PackagesException> { client.reportActivity("../x", ActivityKind.VIEW) }.code)

        var now = 1_000L
        val throttle = ActivityThrottle(clock = { now })
        expect(throttle.admit("crm", ActivityKind.VIEW))
        expect(!throttle.admit("crm", ActivityKind.VIEW))
        expect(throttle.admit("crm", ActivityKind.OPEN))
        expect(throttle.admit("notes", ActivityKind.VIEW))
        now += ActivityThrottle.WINDOW_MS - 1
        expect(!throttle.admit("crm", ActivityKind.VIEW))
        now += 1
        expect(throttle.admit("crm", ActivityKind.VIEW))

        transport.sent.clear()
        val reporter = PackageActivityReporter({ client }, ActivityThrottle(clock = { now }))
        expect(reporter.opened(Route.Kind.Contact))
        expect(!reporter.opened(Route.Kind.Crm))
        expect(!reporter.opened(Route.Kind.Repos))
        assertEq(listOf("crm"), transport.sent.map { it.body?.get("package") })
        transport.answer("POST", "/api/market/reviews/activity", mapOf("error" to "sign_in_required"), 401)
        expect(!reporter.report("notes", ActivityKind.VIEW))
        expect(!PackageActivityReporter({ null }).report("notes", ActivityKind.VIEW))

        transport.answer(
            "GET", "/api/market/reviews/forkmesh/crm/activity",
            mapOf(
                "ok" to true, "totals" to mapOf("views" to 12, "opens" to 30, "people" to 4), "page" to 1, "limit" to 50, "hasMore" to true,
                "people" to listOf(
                    mapOf("name" to "bob", "views" to 9, "opens" to 1, "firstAt" to 5, "lastAt" to 9),
                    mapOf("name" to "ann", "views" to 1, "opens" to 20, "firstAt" to 1, "lastAt" to 2),
                    mapOf("name" to "", "views" to 1),
                ),
            ),
        )
        val page = client.packageActivity("crm", page = 2)
        assertEq(listOf("page" to "2", "limit" to "50"), transport.last("GET", "/api/market/reviews/forkmesh/crm/activity")?.query)
        assertEq(listOf("ann", "bob"), page.people.map { it.name })
        assertEq("20 opens · 1 view", page.people.first().label)
        assertEq("12 · 4 people", page.totals.viewsLabel)
        expect(page.hasMore)
    }

    fun testReviewPageCarriesActivityAndQuickRating() {
        val transport = FakeTransport()
        val good = "It crashes when I open the settings page after the latest update today"
        transport.answer(
            "GET", "/api/market/reviews/forkmesh/crm",
            mapOf(
                "summary" to mapOf("average" to 4.0, "count" to 1),
                "activity" to mapOf("views" to 12, "opens" to 30, "people" to 1),
                "viewer" to mapOf(
                    "signedIn" to true, "canRate" to true, "isMaintainer" to true, "views" to 3, "opens" to 1,
                    "review" to mapOf("id" to hexB, "stars" to 3, "problem" to good, "comment" to "keep me"),
                ),
            ),
        )
        transport.answer("GET", "/api/market/reviews/forkmesh/crm/activity", mapOf("ok" to true, "people" to listOf(mapOf("name" to "ann", "opens" to 1))))
        val model = PackageReviewsModel(PackagesClient(transport), "crm")
        model.refresh()
        val page = model.page!!
        assertEq(PackageActivityTotals(12, 30, 1), page.activity)
        assertEq("12 · 1 person", page.activity.viewsLabel)
        assertEq("Opened 1 time · viewed 3 times", PackageActivityTotals.youLabel(page.viewer))
        expect(model.canSeeActivity)
        model.refreshActivity()
        assertEq(listOf("ann"), model.activity?.people?.map { it.name })

        assertEq(QuickRate.NeedsReview, model.quickRate(4))
        assertEq(4, model.draft.stars)
        assertEq(good, model.draft.problem)
        expect(transport.last("POST", "/api/market/reviews/forkmesh/crm") == null)

        transport.answer("POST", "/api/market/reviews/forkmesh/crm", mapOf("ok" to true, "review" to mapOf("id" to hexB, "stars" to 5)))
        assertEq(QuickRate.Submitted, model.quickRate(5))
        val sent = transport.last("POST", "/api/market/reviews/forkmesh/crm")?.body
        assertEq(5L, sent?.get("stars"))
        assertEq("keep me", sent?.get("comment"))

        transport.answer("POST", "/api/market/reviews/forkmesh/crm", mapOf("error" to "rating_rate_limited"), 429)
        assertEq(QuickRate.Failed("Too many ratings for now. Try again later."), model.quickRate(5))

        transport.answer("GET", "/api/market/reviews/forkmesh/crm", mapOf("viewer" to mapOf("canRate" to false, "reason" to "package_use_required")))
        model.refresh()
        assertEq(QuickRate.Failed("Install or use the package before rating it."), model.quickRate(5))
        expect(!model.canSeeActivity)
        model.refreshActivity()
        assertEq(null, model.activity)
        assertEq(QuickRate.Failed("Sign in again to continue."), PackageReviewsModel(PackagesClient(transport), "crm").quickRate(5))
    }

    private fun uninstallModel(device: MutableMap<String, Boolean?>, account: MutableMap<String, Boolean?>): MarketModel {
        val transport = FakeTransport()
        transport.answer(
            "GET", "/api/market/packages",
            mapOf(
                "packages" to listOf(
                    mapOf("id" to "notes", "name" to "Notes", "accountSync" to true, "dataScope" to "account"),
                    mapOf("id" to "agents", "name" to "Agents", "accountSync" to true, "installedBefore" to "2026-10-11"),
                    mapOf("id" to "sign-in", "name" to "Sign in", "bundled" to true),
                    mapOf("id" to "crm", "name" to "CRM", "dataScope" to "organization"),
                    mapOf("id" to "wiki", "name" to "Wiki", "dataScope" to "organization"),
                    mapOf("id" to "board", "name" to "Board", "dataScope" to "organization"),
                    mapOf("id" to "capture", "name" to "Capture", "default" to true, "surfaces" to mapOf("desktop" to true)),
                    mapOf("id" to "orb", "name" to "Orb", "surfaces" to mapOf("world" to true)),
                ),
            ),
        )
        transport.answer(
            "GET", "/api/market/reviews/summary",
            mapOf("usage" to mapOf("orb" to mapOf("people" to 1, "opens" to 90), "crm" to mapOf("people" to 9, "opens" to 20), "wiki" to mapOf("people" to 2, "opens" to 20))),
        )
        val wikiAndCrm = listOf(mapOf("id" to "wiki", "installed" to true), mapOf("id" to "crm", "installed" to true))
        transport.answer("GET", "/api/plugins/acme", mapOf("organization" to "acme", "canManage" to true, "plugins" to wikiAndCrm))
        transport.answer("GET", "/api/plugins/beta", mapOf("organization" to "beta", "canManage" to true, "plugins" to wikiAndCrm))
        transport.answer(
            "GET", "/api/plugins/gamma",
            mapOf("organization" to "gamma", "canManage" to false, "plugins" to listOf(mapOf("id" to "board", "installed" to true), mapOf("id" to "wiki", "installed" to true))),
        )
        val model = MarketModel(PackagesClient(transport), accountSwitch = { account[it] }, deviceSwitch = { device[it] })
        model.refresh(listOf("acme", "beta", "gamma"))
        return model
    }

    fun testUninstallActionFollowsHowThePackageIsInstalled() {
        val device = mutableMapOf<String, Boolean?>("sign-in" to true, "crm" to true)
        val account = mutableMapOf<String, Boolean?>("notes" to true)
        val model = uninstallModel(device, account)
        val pkg = { id: String -> model.pkg(id)!! }

        assertEq(MarketUninstall.Account, model.uninstallAction(pkg("notes"), signedIn = true))
        assertEq(null, model.uninstallAction(pkg("notes"), signedIn = false), "a guest cannot change the account's packages")
        expect(model.isInstalled(pkg("agents")))
        assertEq(null, model.uninstallAction(pkg("agents"), signedIn = true), "an install the account store has not confirmed has no uninstall")
        account["notes"] = false
        assertEq(null, model.uninstallAction(pkg("notes"), signedIn = true))

        assertEq(MarketUninstall.Phone, model.uninstallAction(pkg("sign-in"), signedIn = false))
        device["sign-in"] = false
        assertEq(null, model.uninstallAction(pkg("sign-in"), signedIn = true))

        assertEq(MarketUninstall.Phone, model.uninstallAction(pkg("crm"), signedIn = true), "a per-phone organization package hides on this phone")
        device["crm"] = false
        assertEq(null, model.uninstallAction(pkg("crm"), signedIn = true))

        assertEq(MarketUninstall.Organization(listOf("acme", "beta")), model.uninstallAction(pkg("wiki"), signedIn = true))
        assertEq(null, model.uninstallAction(pkg("wiki"), signedIn = false))
        expect(model.isInstalled(pkg("board")))
        assertEq(null, model.uninstallAction(pkg("board"), signedIn = true), "only an owner or admin can remove it from an organization")

        expect(model.isInstalled(pkg("capture")))
        assertEq(null, model.uninstallAction(pkg("capture"), signedIn = true), "desktop packages are removed on the desktop")
        assertEq(null, model.uninstallAction(pkg("orb"), signedIn = true))
    }

    fun testMostOpenedAndInstalledFirstSorts() {
        val model = uninstallModel(mutableMapOf("sign-in" to true, "crm" to true), mutableMapOf("notes" to true))
        assertEq(MarketSort.OPENED, MarketSort.of("opened"))
        assertEq(MarketSort.INSTALLED_FIRST, MarketSort.of("Installed-First"))
        assertEq("Most opened", MarketSort.OPENED.title)
        assertEq("Installed first", MarketSort.INSTALLED_FIRST.title)
        model.options = MarketOptions(sort = MarketSort.OPENED)
        assertEq(listOf("orb", "crm", "wiki"), model.shelves.single().packages.take(3).map { it.id })
        model.options = MarketOptions(sort = MarketSort.INSTALLED_FIRST)
        val ids = model.shelves.single().packages.map { it.id }
        assertEq(listOf("agents", "board", "capture", "crm", "notes", "sign-in", "wiki", "orb"), ids)
    }

    fun testMarketUninstallKeepsTheExport() {
        val transport = FakeTransport()
        transport.answer("GET", "/api/market/packages", mapOf("packages" to listOf(mapOf("id" to "crm", "name" to "CRM", "dataScope" to "organization"))))
        transport.answer("GET", "/api/plugins/acme", mapOf("organization" to "acme", "role" to "admin", "canManage" to true, "plugins" to listOf(mapOf("id" to "crm", "installed" to true))))
        transport.answer(
            "POST", "/api/plugins/acme/crm/uninstall",
            mapOf("ok" to true, "hadData" to true, "filename" to "../evil.json", "export" to mapOf("contacts" to listOf(mapOf("name" to "Ada")))),
        )
        val model = MarketModel(PackagesClient(transport))
        model.refresh(listOf("acme"))
        val crm = model.pkg("crm")!!
        expect(model.isInstalled(crm))
        expect(model.uninstall(crm, "acme"))
        expect(!model.isInstalled(crm))
        assertEq("forkmesh-crm-acme.json", model.removed?.filename)
        assertEq("""{"contacts":[{"name":"Ada"}]}""", model.removed?.export)
        model.dismissRemoval()
        transport.answer("POST", "/api/plugins/acme/crm/uninstall", mapOf("error" to "plugin_not_installed"), 409)
        expect(model.uninstall(crm, "acme"))
        assertEq(null, model.removed)
        transport.answer("POST", "/api/plugins/acme/crm/uninstall", mapOf("error" to "organization_admin_required"), 403)
        expect(!model.uninstall(crm, "acme"))
        assertEq("Only an owner or admin of the organization can do that.", model.errorText)
    }

    fun testReviewRulesAndModel() {
        assertEq(true, ReviewDraft(stars = 5).issues(ReviewRules()).isEmpty())
        var draft = ReviewDraft(stars = 3)
        assertEq("required", draft.issues(ReviewRules())["problem"])
        draft = draft.copy(problem = "too short")
        assertEq("too_short", draft.issues(ReviewRules())["problem"])
        draft = draft.copy(problem = "a".repeat(20) + " " + "b".repeat(30))
        assertEq("too_few_words", draft.issues(ReviewRules())["problem"])
        val good = "It crashes when I open the settings page after the latest update today"
        draft = draft.copy(problem = good, improvement = "Please check the settings screen for a null pointer when loading it")
        assertEq(true, draft.issues(ReviewRules()).isEmpty())
        draft = draft.copy(improvement = "  " + good.uppercase() + " ")
        assertEq("same_as_problem", draft.issues(ReviewRules())["improvement"])
        assertEq("too_long", ReviewDraft(comment = "x".repeat(4001)).issues(ReviewRules())["comment"])
        val body = ReviewDraft(stars = 9, problem = " p ", category = "bug").body
        assertEq(5L, body["stars"])
        assertEq("p", body["problem"])
        expect("comment" !in body)
        val rules = ReviewRules.fromJson(mapOf("reviewRequiredBelow" to 4, "categories" to listOf(mapOf("id" to "ux", "label" to "UX"))))
        assertEq(listOf("ux"), rules.categories.map { it.id })
        expect(ReviewDraft(stars = 4).issues(rules).isEmpty())
        assertEq("a b\nc", ReviewDraft.clean("  a \t b \n c "))

        val transport = FakeTransport()
        transport.answer(
            "GET", "/api/market/reviews/forkmesh/crm",
            mapOf(
                "summary" to mapOf("average" to 4.5, "count" to 2),
                "reviews" to listOf(mapOf("id" to hexA, "stars" to 4, "reviewer" to mapOf("name" to "alice", "displayName" to "Alice"), "reply" to mapOf("text" to "Fixed"))),
                "viewer" to mapOf("canRate" to true, "review" to mapOf("id" to hexB, "stars" to 4, "problem" to "p")),
            ),
        )
        val model = PackageReviewsModel(PackagesClient(transport), "crm")
        model.refresh()
        assertEq("Alice", model.page?.reviews?.first()?.reviewerLabel)
        assertEq("Fixed", model.page?.reviews?.first()?.reply?.text)
        assertEq(4, model.draft.stars)
        assertEq("p", model.draft.problem)
        assertEq(listOf("sort" to "newest", "page" to "1", "limit" to "20"), transport.last("GET", "/api/market/reviews/forkmesh/crm")?.query)
        model.draft = ReviewDraft(stars = 5)
        transport.answer("POST", "/api/market/reviews/forkmesh/crm", mapOf("error" to "review_invalid", "fields" to mapOf("problem" to "repetitive")), 422)
        expect(model.canSubmit)
        expect(!model.submit())
        assertEq("repetitive", model.fieldIssues["problem"])
        assertEq("Some review fields need more detail.", model.errorText)
        transport.answer("POST", "/api/market/reviews/items/$hexA/helpful", mapOf("helpful" to 3, "viewerFoundHelpful" to true))
        model.toggleHelpful(model.page!!.reviews.first())
        assertEq(3, model.page?.reviews?.first()?.helpful)
        assertEq(
            "Install or use the package before rating it.",
            ReviewViewer.fromJson(mapOf("canRate" to false, "reason" to "package_use_required")).cannotRateReason,
        )
    }

    fun testCrmModel() {
        val transport = FakeTransport()
        transport.answer("GET", "/api/crm", mapOf("organizations" to listOf(mapOf("name" to "acme", "installed" to true, "role" to "admin"), mapOf("name" to "BAD"))))
        val contact = mapOf("id" to hexA, "name" to "Ann", "company" to "Co", "title" to "CTO", "flow" to "sales", "stage" to "lead", "value" to 500, "tags" to listOf("vip"))
        transport.answer(
            "GET", "/api/crm/acme",
            mapOf(
                "organization" to "acme", "revision" to "r1",
                "contacts" to listOf(contact, mapOf("id" to hexB, "email" to "b@x.io", "flow" to "sales", "stage" to "lost"), mapOf("id" to "short")),
                "settings" to mapOf("flows" to listOf(mapOf("id" to "sales", "name" to "Sales", "stages" to listOf("lead", "won")))),
                "emails" to listOf(mapOf("id" to "e1", "contactId" to hexA, "createdAt" to 1), mapOf("id" to "e2", "contactId" to hexA, "sentAt" to 5, "status" to "bounce")),
                "members" to listOf("ann", mapOf("name" to "bob"), ""),
            ),
        )
        val model = CrmModel(PackagesClient(transport))
        model.refresh()
        assertEq("acme", model.organization)
        val data = model.data!!
        assertEq(2, data.contacts.size)
        assertEq("Sales", model.flow.name)
        assertEq(listOf("lead", "won", "other"), data.stageColumns(model.flow).map { it.stage })
        assertEq(listOf("e2", "e1"), data.activityFor(hexA).map { it.id })
        expect(data.activityFor(hexA).first().failed)
        assertEq(listOf("ann", "bob"), data.members)
        assertEq(500L, data.stageValue(model.flow, "lead"))
        assertEq("CTO · Co", data.contacts[0].subtitle)
        assertEq("b@x.io", data.contacts[1].displayName)
        assertEq(1, data.contactsIn(model.flow, "vip").size)
        transport.answer("GET", "/api/crm/acme", mapOf("unchanged" to true))
        model.load()
        assertEq(listOf("since", "tz"), transport.last("GET", "/api/crm/acme")?.query?.map { it.first })
        assertEq("r1", transport.last("GET", "/api/crm/acme")?.query?.first()?.second)
        assertEq(2, model.data?.contacts?.size)
        val saved = contact + ("stage" to "won")
        transport.answer("PATCH", "/api/crm/acme/contacts/$hexA", mapOf("contact" to saved))
        expect(model.move(model.contact(hexA)!!, "won"))
        assertEq(mapOf("stage" to "won"), transport.last("PATCH", "/api/crm/acme/contacts/$hexA")?.body)
        assertEq("won", model.contact(hexA)?.stage)
        val edit = CrmContactEdit(name = " New ", tags = "a, A, b,")
        assertEq(listOf("a", "b"), edit.tagList)
        assertEq(listOf("a", "b"), edit.changes(null)["tags"])
        assertEq(null, model.save(CrmContactEdit(), null))
        assertEq("A contact needs a name or an email address.", model.errorText)
        assertEq(CrmContactRef("acme", hexA), CrmContactRef.of("acme/$hexA"))
        assertEq(null, CrmContactRef.of("acme/x"))
        assertEq("Soft Bounce", CrmStageLabel.title("soft-bounce"))
    }

    fun testCrmDeleteAndInstall() {
        val transport = FakeTransport()
        val summary = mapOf(
            "organization" to "acme", "revision" to "r1",
            "contacts" to listOf(mapOf("id" to hexA, "name" to "Ann", "stage" to "lead"), mapOf("id" to hexB, "name" to "Bo", "stage" to "lead")),
        )
        transport.answer("GET", "/api/crm", mapOf("organizations" to listOf(mapOf("name" to "acme", "installed" to true, "role" to "member"))))
        transport.answer("GET", "/api/crm/acme", summary)
        val model = CrmModel(PackagesClient(transport))
        model.refresh()
        expect(!model.needsInstall)
        expect(!model.needsOrganization)
        expect(model.delete(model.contact(hexA)!!))
        expect(transport.last("DELETE", "/api/crm/acme/contacts/$hexA") != null)
        assertEq(null, model.contact(hexA))
        assertEq(1, model.data?.contacts?.size)
        transport.answer("DELETE", "/api/crm/acme/contacts/$hexB", mapOf("error" to "contact_not_found"), status = 404)
        expect(!model.delete(model.contact(hexB)!!))
        expect(model.contact(hexB) != null)
        expect(model.errorText.isNotEmpty())

        val member = FakeTransport()
        member.answer("GET", "/api/crm", mapOf("organizations" to listOf(mapOf("name" to "acme", "installed" to false, "role" to "member"))))
        member.answer("GET", "/api/crm/acme", mapOf("error" to "plugin_not_installed"), status = 409)
        val memberModel = CrmModel(PackagesClient(member))
        memberModel.refresh()
        expect(memberModel.needsInstall)
        expect(!memberModel.canInstall)
        expect(!memberModel.install())
        assertEq(null, member.last("POST", "/api/plugins/acme/crm/install"))

        val owner = FakeTransport()
        owner.answer("GET", "/api/crm", mapOf("organizations" to listOf(mapOf("name" to "acme", "installed" to false, "role" to "owner"))))
        owner.answer("GET", "/api/crm/acme", mapOf("error" to "plugin_not_installed"), status = 409)
        val ownerModel = CrmModel(PackagesClient(owner))
        ownerModel.refresh()
        expect(ownerModel.canInstall)
        owner.answer("GET", "/api/crm/acme", summary)
        expect(ownerModel.install())
        expect(owner.last("POST", "/api/plugins/acme/crm/install") != null)
        expect(!ownerModel.needsInstall)
        assertEq(2, ownerModel.data?.contacts?.size)
        assertEq("", ownerModel.errorText)

        val newcomer = FakeTransport()
        val newcomerModel = CrmModel(PackagesClient(newcomer))
        expect(!newcomerModel.needsOrganization)
        newcomer.answer("GET", "/api/crm", mapOf("error" to "unavailable"), status = 503)
        newcomerModel.refresh()
        expect(!newcomerModel.needsOrganization)
        newcomer.answer("GET", "/api/crm", mapOf("organizations" to emptyList<Any>()))
        newcomerModel.refresh()
        expect(newcomerModel.needsOrganization)
        expect(!newcomerModel.needsInstall && !newcomerModel.canInstall)
        newcomer.answer("GET", "/api/crm", mapOf("organizations" to listOf(mapOf("name" to "fresh", "installed" to false, "role" to "owner"))))
        newcomer.answer("GET", "/api/crm/fresh", mapOf("error" to "plugin_not_installed"), status = 409)
        newcomerModel.refresh()
        expect(!newcomerModel.needsOrganization)
        expect(newcomerModel.canInstall)
    }

    fun testMarketingAnalyticsCompetitorsAds() {
        val overview = MarketingOverview.fromJson(
            mapOf(
                "content" to listOf(mapOf("id" to hexA, "status" to "queued", "channel" to "x"), mapOf("id" to hexB, "status" to "archived"), mapOf("id" to "nope")),
                "ads" to listOf(mapOf("id" to hexA, "status" to "paused", "budgetCents" to 10_000, "spendCents" to 1_205, "currency" to "USD", "impressions" to 200, "clicks" to 5)),
                "channelCatalog" to listOf(mapOf("id" to "x", "name" to "X")),
                "summary" to mapOf("published" to 2, "channels" to listOf(mapOf("channel" to "x", "reach" to 10))),
            ),
        )
        assertEq(listOf("queued"), overview.plan.map { it.status })
        assertEq("X", overview.channelName("x"))
        assertEq("12.05 USD of 100.00 USD", overview.ads[0].spendLabel)
        assertEq(0.025, overview.ads[0].clickRate)
        assertEq(1, overview.activeAds.size)
        assertEq("-1.05", MoneyText.cents(-105, ""))
        val health = MarketingOverview.fromJson(
            mapOf(
                "settings" to mapOf("channels" to listOf(mapOf("id" to "x", "profileUrl" to "https://x.com/acme"), mapOf("id" to "reddit", "profileUrl" to ""))),
                "summary" to mapOf("channels" to listOf(
                    mapOf("channel" to "x", "followers" to 450, "rating" to 4.5, "health" to mapOf(
                        "state" to "attention", "followersChange" to -50, "engagementRate" to 2.5, "postsLast30d" to 3,
                        "checks" to listOf(mapOf("id" to "link", "label" to "Channel link added", "ok" to true), mapOf("id" to "growing", "label" to "Audience growing over 30 days", "ok" to false)),
                    )),
                    mapOf("channel" to "reddit", "health" to mapOf("state" to "setup")),
                    mapOf("channel" to "google-search", "visits" to 6, "indexedPages" to 1230, "health" to mapOf("state" to "good")),
                    mapOf("channel" to "email", "outbound" to 42, "closedDeals" to 3, "closedValue" to 2000, "health" to mapOf("state" to "good")),
                    mapOf("channel" to "github", "followers" to 7, "stars" to 130, "forks" to 9, "health" to mapOf("state" to "good", "starsChange" to 30)),
                )),
            ),
        ).channels
        assertEq("Needs attention", health[0].healthTitle)
        assertEq(-50L, health[0].followersChange)
        assertEq(listOf("Audience growing over 30 days"), health[0].missingChecks)
        assertEq(true, health[0].canCapture)
        assertEq("https://app.forkmesh.com/dashboard/marketing?org=acme&page=channels&capture=x", health[0].captureUrl("acme"))
        assertEq(false, health[1].canCapture)
        assertEq(null, health[1].followersChange)
        assertEq(true, health[2].isGoogle)
        assertEq(false, health[0].isGoogle)
        assertEq(6L, health[2].visits)
        assertEq(1230L, health[2].indexedPages)
        assertEq(true, health[3].isEmail)
        assertEq(42L, health[3].outbound)
        assertEq(3L, health[3].closedDeals)
        assertEq(2000L, health[3].closedValue)
        assertEq(false, health[3].canCapture)
        assertEq(true, health[4].isGitHub)
        assertEq(false, health[0].isGitHub)
        assertEq(130L, health[4].stars)
        assertEq(9L, health[4].forks)
        assertEq(30L, health[4].starsChange)
        assertEq(null, health[0].starsChange)
        val transport = FakeTransport()
        val client = PackagesClient(transport)
        assertFails<PackagesException> { client.setContentStatus("acme", hexA, "bogus") }
        transport.answer("PATCH", "/api/marketing/acme/content/$hexA", mapOf("content" to mapOf("id" to hexA, "status" to "published")))
        client.setContentStatus("acme", hexA, "published", nowMs = 77)
        assertEq(77L, transport.last("PATCH", "/api/marketing/acme/content/$hexA")?.body?.get("publishedAt"))

        assertEq(mapOf("kind" to "overview", "from" to 100L - 86_400_000, "to" to 100L), AnalyticsRange.DAY.query(100))
        val analytics = AnalyticsOverview.fromJson(
            mapOf(
                "result" to mapOf(
                    "totals" to mapOf("visitors" to 10), "previous" to mapOf("visitors" to 8),
                    "timeseries" to mapOf("labels" to listOf(1, 2), "series" to listOf(mapOf("name" to "pv", "data" to listOf(1, 2.5), "total" to 3.5))),
                    "breakdowns" to mapOf("pages" to listOf(mapOf("value" to "/", "count" to 4))),
                ),
            ),
        )
        assertEq(listOf(1.0, 2.5), analytics.series[0].data)
        assertEq(0.25, AnalyticsTotals.change(analytics.totals.visitors, analytics.previous.visitors))
        assertEq(null, AnalyticsTotals.change(1, 0))
        assertEq("Website", AnalyticsProject.fromJson(mapOf("siteDefault" to true)).name)

        val board = CompetitorBoard.fromJson(
            mapOf("competitors" to listOf(mapOf("id" to hexA, "name" to "Rival", "status" to "primary"), mapOf("id" to hexB, "name" to "Old", "status" to "archived", "lastReviewedAt" to 0))),
        )
        assertEq(listOf("primary", "archived"), board.grouped().map { it.status })
        expect(board.competitors[0].needsReview(nowMs = 1))
        expect(!board.competitors[1].needsReview(nowMs = 1))
        expect(!Competitor(website = "http://x").websiteIsValid)
        expect(Competitor(website = " https://x.io ").websiteIsValid)
        assertEq("watch", Competitor(status = "weird").body["status"])
        transport.answer("POST", "/api/competitors/acme/items", mapOf("competitor" to mapOf("id" to hexA, "name" to "N")))
        assertEq(hexA, client.saveCompetitor("acme", Competitor(name = "N")).id)

        val ad = OwnedAd.fromJson(mapOf("id" to hexA, "status" to "approved", "paused" to true, "delivery" to "over-allowance", "contentHash" to "h"))
        assertEq(listOf("resume", "withdraw"), ad.ownerActions)
        assertEq(listOf("suspend"), ad.reviewActions)
        assertEq("Over your live-ad allowance", ad.deliveryText)
        assertEq("Some New State", OwnedAd(id = "x", delivery = "some-new-state").deliveryText)
        val reject = AdReviewDecision("reject")
        expect(!reject.isComplete)
        assertFails<PackagesException> { client.reviewAd(ad, reject) }
        assertEq(mapOf("action" to "approve", "codes" to listOf("a", "b"), "contentHash" to "h"), AdReviewDecision("approve", codes = listOf("b", "a", "b")).body("h"))
        transport.answer("GET", "/api/ads/mine", mapOf("isReviewer" to true, "tier" to mapOf("label" to "Pro", "liveAds" to 3), "ads" to listOf(mapOf("id" to hexA, "status" to "pending"))))
        transport.answer("GET", "/api/ads/review", mapOf("ads" to listOf(mapOf("id" to hexA, "status" to "pending")), "counts" to mapOf("pending" to 1)))
        val ads = AdsModel(client)
        ads.refresh()
        assertEq(3, ads.account?.liveAds)
        assertEq(1, ads.queue?.counts?.get("pending"))
        expect(ads.review(ads.queue!!.ads[0], AdReviewDecision("approve")))
        assertEq(0, ads.queue?.ads?.size)
        val competitors = OrgPackageModel("", { PackageOrganization.list(mapOf("organizations" to listOf(mapOf("name" to "acme", "installed" to true)))) }) { board }
        competitors.refresh()
        assertEq("acme", competitors.organization)
        competitors.update { it.copy(competitors = emptyList()) }
        assertEq(0, competitors.payload?.competitors?.size)
    }

    fun testLamportsAndLedger() {
        assertEq("0 SOL", Lamports.sol(0))
        assertEq("1 SOL", Lamports.sol(1_000_000_000))
        assertEq("0.05 SOL", Lamports.sol(50_000_000))
        assertEq("1.000000001 SOL", Lamports.sol(1_000_000_001))
        assertEq("-0.5 SOL", Lamports.sol(-500_000_000))
        assertEq(50_000_000L, Lamports.parse("0.05"))
        assertEq(1_000_000_000L, Lamports.parse("1"))
        assertEq(1_000_000_001L, Lamports.parse("1.000000001"))
        assertEq(500_000_000L, Lamports.parse(".5"))
        for (bad in listOf("-1", "1.0000000001", "1e3", ".", "", "1.2.3", "9999999999", "+1")) assertEq(null, Lamports.parse(bad), bad)
        expect(SolanaAddress.isValid(solAddress))
        expect(!SolanaAddress.isValid("0OIl" + solAddress.drop(4)))
        expect(!SolanaAddress.isValid("short"))

        val transport = FakeTransport()
        transport.answer(
            "GET", "/api/ledger",
            mapOf(
                "entries" to listOf(
                    mapOf("id" to "a", "amountLamports" to 5_000_000, "transactionId" to "tx"),
                    mapOf("id" to "b", "currency" to "USD", "amountUsd" to "5.00"),
                    mapOf("title" to "no id"),
                ),
                "page" to mapOf("hasMore" to true, "nextCursor" to mapOf("beforeAt" to 10, "beforeId" to "b")),
                "wallets" to listOf(mapOf("key" to "ops", "address" to "x"), mapOf("key" to "treasury", "address" to solAddress)),
                "solUsd" to mapOf("available" to true, "price" to "150.000000"),
            ),
        )
        transport.answer(
            "GET", "/api/rewards/estimates",
            mapOf("rewards" to listOf(mapOf("rewardId" to "r1", "repository" to "a/b", "sourceKind" to "pull", "subjectNumber" to 7), mapOf("rewardId" to "r2", "status" to "paid"), mapOf("rewardId" to ""))),
        )
        transport.answer("GET", "/api/world/wallet", mapOf("ok" to true, "lamports" to 42))
        val secure = MemorySecretStore()
        val model = LedgerModel(PackagesClient(transport), PersonalLedgerStore(secure))
        model.refresh(solAddress)
        assertEq("0.005 SOL", model.entries[0].amountText)
        expect(model.entries[0].verified)
        assertEq("$5.00", model.entries[1].amountText)
        assertEq("solana:$solAddress", model.treasury?.donationUri)
        assertEq("150.000000", model.solUsd)
        assertEq(listOf("r1"), model.pendingRewards.map { it.rewardId })
        assertEq(listOf("r2"), model.paidRewards.map { it.rewardId })
        assertEq("a/b PR #7", model.pendingRewards[0].subjectLabel)
        assertEq(42L, model.ownBalance?.lamports)
        transport.answer("GET", "/api/ledger", mapOf("entries" to listOf(mapOf("id" to "b"), mapOf("id" to "c"))))
        model.loadMore()
        assertEq(listOf("a", "b", "c"), model.entries.map { it.id })
        assertEq(null, model.next)
        assertEq(listOf("limit", "beforeAt", "beforeId"), transport.last("GET", "/api/ledger")?.query?.map { it.first })
        model.add(PersonalRecord.make(PersonalRecordKind.EXPENSE, "Hosting", 1200, occurredAt = 1, id = "h"))
        model.add(PersonalRecord.make(PersonalRecordKind.INCOME, "Sponsor", 5000, occurredAt = 2, id = "s"))
        assertEq(listOf("Sponsor", "Hosting"), model.records.map { it.title })
        assertEq(PersonalTotal("USD", 5000, 1200), model.totals.single())
        model.toggleSettled("h")
        expect(model.records[1].settled)
        assertEq(2, LedgerModel(PackagesClient(transport), PersonalLedgerStore(secure)).records.size)
        model.remove("s")
        assertEq(listOf("Hosting"), PersonalLedgerStore(secure).load().map { it.title })
        val store = PersonalLedgerStore(MemorySecretStore())
        val many = (0 until 600).map { PersonalRecord.make(PersonalRecordKind.EXPENSE, "r$it", 1, occurredAt = it.toLong(), id = "id$it") }
        assertEq(PersonalLedgerStore.MAX_RECORDS, store.save(many).size)
        assertEq("r599", store.load().first().title)
        assertEq(0L, PersonalRecord.make(PersonalRecordKind.EXPENSE, "x", -5, occurredAt = 0).amountMinor)
    }

    fun testWalletTipFlow() {
        var now = 1_000L
        val channel = FakeChannel()
        val wallet = WalletClient(ControlFamilyChannel(DesktopControlFamilyName.Wallet, channel)) { now }
        channel.replies["wallet.status"] = mapOf("linked" to true, "minLamports" to 1_000_000, "maxLamports" to 2_000_000_000)
        channel.replies["wallet.rewards"] = mapOf("rewards" to listOf(mapOf("rewardId" to "rw", "lamports" to 5)))
        channel.replies["wallet.prepare"] = mapOf("token" to "t1", "action" to "tip", "summary" to "Tip bob", "expiresInMs" to 60_000)
        channel.replies["wallet.request"] = mapOf("id" to "q1", "status" to "awaiting_owner")
        val transport = FakeTransport()
        transport.answer("GET", "/api/accounts/bob", mapOf("solana" to solAddress))
        val model = WalletRequestModel(wallet, PackagesClient(transport))
        var changes = 0
        model.start { changes++ }
        assertEq(1, model.rewards.size)
        model.recipient = " Bob "
        model.lookUpAddress()
        assertEq(solAddress, model.address)
        model.amount = "0.0001"
        assertEq("The smallest amount is 0.001 SOL.", model.amountProblem)
        expect(!model.canPrepareTip)
        model.amount = "0.5"
        expect(model.canPrepareTip)
        model.prepareTip()
        assertEq("t1", model.confirmation?.token)
        assertEq(mapOf("recipient" to "bob", "address" to solAddress, "lamports" to 500_000_000L, "action" to "tip"), channel.params("wallet.prepare"))
        expect(model.send())
        assertEq("t1", channel.params("wallet.request")?.get("token"))
        assertEq("awaiting_owner", model.lastRequest?.status)
        assertEq("", model.amount)
        assertEq(null, model.confirmation)
        channel.taps.single()(ControlEvent("wallet.changed", emptyMap()))
        channel.taps.single()(ControlEvent("wallet.other", emptyMap()))
        assertEq(1, changes)
        model.amount = "0.5"
        model.prepareTip()
        now += 61_000
        val before = channel.requests.size
        expect(!model.send())
        assertEq("That confirmation expired. Try again.", model.errorText)
        assertEq(before, channel.requests.size)
        model.stop()
        assertEq(0, channel.taps.size)
        channel.failures["wallet.status"] = LocalControlException("unknown_method")
        model.refresh()
        expect(model.unavailable)
        assertEq("Your desktop needs an update before it can take payment requests from the phone.", model.errorText)
        assertEq("No tip wallet is linked on your desktop. Link one there first.", WalletClient.describe(LocalControlException("no_wallet")))
        assertFails<PackagesException> { wallet.prepareTip(TipRequest("bob", "bad", 5)) }
        transport.answer("GET", "/api/accounts/carol", mapOf("solana" to "nope"))
        model.recipient = "carol"
        model.lookUpAddress()
        assertEq("@carol has not set a payout address.", model.errorText)
        val unavailable = WalletRequestModel(null, PackagesClient(transport))
        expect(unavailable.unavailable)
        expect(!unavailable.send())
    }

    fun testRoutes() {
        assertEq(listOf("market", "crm", "ads", "ledger"), PackageRoutes.hubRows(setOf("crm", "ledger")).map { it.id })
        assertEq(listOf("market", "crm", "competitor", "ads", "ledger"), PackageRoutes.hubRows(setOf("competitor")).map { it.id })
        assertEq(PipelineTarget("acme", "sales"), PackageRoutes.organizationFromPipeline("acme/sales"))
        assertEq(PipelineTarget(null, "sales"), PackageRoutes.organizationFromPipeline("sales"))
        assertEq(PipelineTarget("acme", null), PackageRoutes.organizationFromPipeline("acme/"))
        assertEq(PipelineTarget(null, null), PackageRoutes.organizationFromPipeline(null))
        assertEq("competitors", PackageRoutes.destination(MarketPackage(id = "competitor", name = "C")))
        assertEq(null, PackageRoutes.destination(MarketPackage(id = "orb", name = "Orb", world = true)))
        assertEq(LedgerSection.TIPS, LedgerSection.of("TIPS"))
        assertEq(MarketingSection.OVERVIEW, MarketingSection.of("nope"))
        assertEq(MarketingSection.PLAN, MarketingSection.of("content"))
        assertEq(MarketingSection.ADS, MarketingSection.of("Ads-Manager"))
        assertEq(MarketingSection.REDDIT, MarketingSection.of("reddit-desk"))
    }
}
