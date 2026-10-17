import FMBase
import XCTest

@testable import FMPackages

final class AccountPackagesTests: XCTestCase {
    static let statusPageRow: JSONValue = [
        "id": "status-page", "name": "Status Page", "bundled": true, "default": true, "accountSync": true,
        "dependencies": [], "surfaces": ["desktop": true, "web": "/dashboard/status-monitors"],
        "settings": [
            ["key": "onlyProblems", "type": "boolean", "label": "Show only systems that need attention", "default": false],
            [
                "key": "refreshMinutes", "type": "choice", "label": "Recheck healthy systems every", "default": "15",
                "options": [
                    ["value": "2", "label": "2 minutes"], ["value": "5", "label": "5 minutes"],
                    ["value": "15", "label": "15 minutes"], ["value": "30", "label": "30 minutes"],
                ],
            ],
            ["key": "colour", "type": "text", "label": "Unsupported", "default": "red"],
        ],
        "monitors": ["systems": [], "prefixes": []],
    ]

    static func checkRow(_ id: String, systems: [String] = [], prefixes: [String] = [], defaultOn: Bool = true) -> JSONValue {
        [
            "id": .string(id), "name": .string("Status: " + id), "bundled": true, "default": .bool(defaultOn),
            "accountSync": true, "dependencies": ["status-page"], "settings": [],
            "monitors": ["systems": .array(systems.map(JSONValue.string)), "prefixes": .array(prefixes.map(JSONValue.string))],
        ]
    }

    static let catalog: JSONObject = [
        "packages": [
            statusPageRow, checkRow("status-check-api", systems: ["api"]),
            checkRow("status-check-website", systems: ["website", "desktop_website"]),
            checkRow("status-check-cores", prefixes: ["core:"]), checkRow("status-check-extra", systems: ["extra"], defaultOn: false),
            ["id": "crm", "name": "CRM", "dataScope": "organization"],
        ],
    ]

    static func entry(_ id: String, _ installed: Bool, _ settings: JSONObject = [:]) -> JSONValue {
        ["id": .string(id), "installed": .bool(installed), "settings": .object(settings)]
    }

    static let accountState: JSONObject = [
        "ok": true,
        "packages": [
            entry("status-page", true, ["onlyProblems": true, "refreshMinutes": "5"]), entry("status-check-api", true),
            entry("status-check-website", false), entry("status-check-cores", true), entry("status-check-extra", false),
        ],
    ]

    static let monitorList: JSONObject = [
        "ok": true, "installed": true, "max": 3,
        "monitors": [["id": .string(hexA), "name": "Blog", "url": "https://blog.example.com/"]],
    ]

    func testMarketRowsCarryAccountFields() {
        let page = MarketPackage(json: Self.statusPageRow.objectValue!)!
        XCTAssertTrue(page.accountSync)
        XCTAssertTrue(page.defaultOn)
        XCTAssertEqual(page.settings.map(\.key), ["onlyProblems", "refreshMinutes"])
        XCTAssertEqual(page.settings[0].type, .boolean)
        XCTAssertEqual(page.settings[0].defaultValue, .bool(false))
        XCTAssertEqual(page.settings[1].type, .choice)
        XCTAssertEqual(page.settings[1].defaultValue, .string("15"))
        XCTAssertEqual(page.settings[1].options.map(\.value), ["2", "5", "15", "30"])
        XCTAssertTrue(page.settings[1].accepts(.string("30")))
        XCTAssertFalse(page.settings[1].accepts(.string("7")))
        XCTAssertFalse(page.settings[0].accepts(.string("yes")))
        XCTAssertTrue(page.monitors.isEmpty)
        let check = MarketPackage(json: Self.checkRow("status-check-cores", prefixes: ["core:"]).objectValue!)!
        XCTAssertEqual(check.dependencies, ["status-page"])
        XCTAssertEqual(check.monitors, PackageMonitors(systems: [], prefixes: ["core:"]))
        let objectDeps = MarketPackage(json: ["id": "a", "dependencies": [["id": "status-page"], "Bad Id", "a"]])!
        XCTAssertEqual(objectDeps.dependencies, ["status-page"])
        let plain = MarketPackage(json: ["id": "crm"])!
        XCTAssertFalse(plain.accountSync)
        XCTAssertFalse(plain.defaultOn)
        XCTAssertEqual(plain.settings, [])
        let badChoice = PackageSetting(json: ["key": "k", "type": "choice", "default": "z", "options": [["value": "a"]]])
        XCTAssertEqual(badChoice?.defaultValue, .string("a"))
        XCTAssertNil(PackageSetting(json: ["key": "k", "type": "choice", "options": []]))
    }

    @MainActor func testFirstRunIsInstalledWithDefaults() async {
        let store = AccountPackagesStore(
            client: PackagesClient(transport: FakePackagesTransport()), prefs: InMemoryKeyValueStore(), signedIn: { false })
        XCTAssertTrue(store.statusPageOn)
        XCTAssertFalse(store.onlyProblems)
        XCTAssertEqual(store.refreshMinutes, 15)
        XCTAssertNil(store.monitorRules)
        XCTAssertTrue(store.isInstalled("status-check-api"))
        XCTAssertEqual(store.monitors, [])
    }

    @MainActor func testCatalogDefaultsApplyBeforeTheAccountAnswers() async {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/market/packages", Self.catalog)
        let store = AccountPackagesStore(client: PackagesClient(transport: transport), prefs: InMemoryKeyValueStore(), signedIn: { false })
        await store.refresh()
        XCTAssertEqual(store.catalog.map(\.id), [
            "status-page", "status-check-api", "status-check-website", "status-check-cores", "status-check-extra",
        ])
        XCTAssertFalse(transport.paths.contains("GET /api/plugins/@account"))
        XCTAssertTrue(store.statusPageOn)
        XCTAssertFalse(store.isInstalled("status-check-extra"))
        XCTAssertEqual(store.settings("status-page"), ["onlyProblems": false, "refreshMinutes": "15"])
        XCTAssertEqual(store.monitorRules?.systems, ["api", "website", "desktop_website"])
        XCTAssertEqual(store.monitorRules?.prefixes, ["core:"])
    }

    @MainActor func testRefreshAdoptsTheAccountAndCachesIt() async {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/market/packages", Self.catalog)
        transport.on("GET", "/api/plugins/@account", Self.accountState)
        transport.on("GET", "/api/status-monitors", Self.monitorList)
        let prefs = InMemoryKeyValueStore()
        let store = AccountPackagesStore(client: PackagesClient(transport: transport), prefs: prefs, signedIn: { true })
        await store.refresh()
        XCTAssertTrue(store.statusPageOn)
        XCTAssertTrue(store.onlyProblems)
        XCTAssertEqual(store.refreshMinutes, 5)
        XCTAssertEqual(store.monitorRules?.systems, ["api"])
        XCTAssertEqual(store.monitorRules?.prefixes, ["core:"])
        XCTAssertEqual(store.monitors.map(\.name), ["Blog"])
        XCTAssertEqual(store.monitorLimit, 3)

        let again = AccountPackagesStore(
            client: PackagesClient(transport: FakePackagesTransport()), prefs: prefs, signedIn: { false })
        XCTAssertEqual(again.state, store.state)
        XCTAssertEqual(again.catalog, store.catalog)
        XCTAssertEqual(again.monitors, store.monitors)
        XCTAssertTrue(again.onlyProblems)
        XCTAssertEqual(again.monitorRules, store.monitorRules)
    }

    @MainActor func testSignedOutOrAFailedRequestKeepsTheLastState() async {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/market/packages", Self.catalog)
        transport.on("GET", "/api/plugins/@account", Self.accountState)
        transport.on("GET", "/api/status-monitors", Self.monitorList)
        var signedIn = true
        let store = AccountPackagesStore(client: PackagesClient(transport: transport), prefs: InMemoryKeyValueStore(), signedIn: { signedIn })
        await store.refresh()
        let known = store.state
        transport.routes["GET /api/plugins/@account"] = [PackagesReply(status: 503, json: ["error": ""])]
        await store.refresh()
        XCTAssertEqual(store.state, known)
        XCTAssertEqual(store.monitors.count, 1)
        signedIn = false
        let calls = transport.sent.count
        await store.refresh()
        XCTAssertEqual(transport.sent.count, calls)
        XCTAssertEqual(store.state, known)
    }

    @MainActor func testRemovingStatusPageTakesItsChecksAndMonitors() async {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/market/packages", Self.catalog)
        transport.on("GET", "/api/plugins/@account", Self.accountState)
        transport.on("GET", "/api/status-monitors", Self.monitorList)
        let store = AccountPackagesStore(client: PackagesClient(transport: transport), prefs: InMemoryKeyValueStore(), signedIn: { true })
        await store.refresh()
        XCTAssertEqual(store.dependents(of: "status-page").map(\.id), ["status-check-api", "status-check-cores"])
        XCTAssertEqual(store.missingDependencies(of: "status-check-api"), [])
        transport.on("POST", "/api/plugins/@account/status-page/uninstall", [
            "ok": true, "changed": ["status-page", "status-check-api", "status-check-cores"],
            "packages": [
                Self.entry("status-page", false), Self.entry("status-check-api", false), Self.entry("status-check-website", false),
                Self.entry("status-check-cores", false), Self.entry("status-check-extra", false),
            ],
        ])
        let monitorReads = transport.paths.filter { $0 == "GET /api/status-monitors" }.count
        let changed = await store.uninstall("status-page")
        XCTAssertEqual(changed, ["status-page", "status-check-api", "status-check-cores"])
        XCTAssertNotNil(transport.last("POST", "/api/plugins/@account/status-page/uninstall")?.body)
        XCTAssertFalse(store.statusPageOn)
        XCTAssertFalse(store.isInstalled("status-check-api"))
        XCTAssertEqual(store.monitors, [])
        XCTAssertEqual(store.monitorRules?.systems, [])
        XCTAssertEqual(transport.paths.filter { $0 == "GET /api/status-monitors" }.count, monitorReads)
        XCTAssertEqual(store.missingDependencies(of: "status-check-api").map(\.name), ["Status Page"])
        XCTAssertEqual(store.dependents(of: "status-page"), [])

        transport.on("POST", "/api/plugins/@account/status-check-api/install", [
            "ok": true, "changed": ["status-check-api", "status-page"],
            "packages": [Self.entry("status-page", true), Self.entry("status-check-api", true)],
        ])
        transport.routes["GET /api/status-monitors"] = [PackagesReply(status: 200, json: ["ok": true, "monitors": []])]
        let installed = await store.install("status-check-api")
        XCTAssertEqual(installed, ["status-check-api", "status-page"])
        XCTAssertTrue(store.statusPageOn)
        XCTAssertEqual(transport.paths.last, "GET /api/status-monitors")
    }

    @MainActor func testAFailedChangeKeepsTheStateAndSaysWhy() async {
        let transport = FakePackagesTransport()
        transport.on("POST", "/api/plugins/@account/status-page/uninstall", status: 401, ["error": "invalid_session"])
        let store = AccountPackagesStore(client: PackagesClient(transport: transport), prefs: InMemoryKeyValueStore(), signedIn: { true })
        let changed = await store.uninstall("status-page")
        XCTAssertNil(changed)
        XCTAssertTrue(store.statusPageOn)
        XCTAssertEqual(store.errorText, "Sign in again to continue.")
        XCTAssertFalse(store.isBusy("status-page"))
    }

    @MainActor func testPlacesChangeThroughTheAccount() async {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/market/packages", Self.catalog)
        transport.on("GET", "/api/plugins/@account", Self.accountState)
        transport.on("GET", "/api/status-monitors", Self.monitorList)
        let store = AccountPackagesStore(client: PackagesClient(transport: transport), prefs: InMemoryKeyValueStore(), signedIn: { true })
        await store.refresh()
        XCTAssertEqual(store.places("status-page"), [.mobile, .desktop, .web])
        XCTAssertTrue(store.statusPageShown)
        transport.on("POST", "/api/plugins/@account/status-page/places", [
            "ok": true,
            "packages": [
                ["id": "status-page", "installed": true, "settings": [:], "places": ["desktop", "web"],
                 "availablePlaces": ["mobile", "desktop", "web"]],
                Self.entry("status-check-api", true),
            ],
        ])
        let moved = await store.setPlace("status-page", .mobile, on: false)
        XCTAssertTrue(moved)
        XCTAssertEqual(transport.last("POST", "/api/plugins/@account/status-page/places")?.body, ["places": ["desktop", "web"]])
        XCTAssertEqual(store.places("status-page"), [.desktop, .web])
        XCTAssertTrue(store.statusPageOn)
        XCTAssertFalse(store.statusPageShown)
        XCTAssertFalse(store.isBusy("status-page"))

        let calls = transport.sent.count
        let same = await store.setPlaces("status-page", [.web, .desktop])
        XCTAssertTrue(same)
        let none = await store.setPlaces("status-page", [])
        XCTAssertFalse(none)
        XCTAssertEqual(store.errorText, "Keep it on at least one place. Remove it to take it off everywhere.")
        XCTAssertEqual(transport.sent.count, calls)

        transport.routes["POST /api/plugins/@account/status-page/places"] = [
            PackagesReply(status: 409, json: ["error": "plugin_not_installed"]),
        ]
        let refused = await store.setPlace("status-page", .mobile, on: true)
        XCTAssertFalse(refused)
        XCTAssertEqual(transport.last("POST", "/api/plugins/@account/status-page/places")?.body, ["places": ["mobile", "desktop", "web"]])
        XCTAssertEqual(store.errorText, "Install this package first.")
        XCTAssertEqual(store.places("status-page"), [.desktop, .web])

        transport.on("POST", "/api/plugins/@account/status-check-extra/install", Self.accountState)
        await store.install("status-check-extra")
        XCTAssertEqual(transport.last("POST", "/api/plugins/@account/status-check-extra/install")?.body, [:])
        _ = try? await store.client.installForAccount("status-check-extra", places: [.web, .mobile])
        XCTAssertEqual(
            transport.last("POST", "/api/plugins/@account/status-check-extra/install")?.body, ["places": ["mobile", "web"]])
    }

    @MainActor func testSettingsSaveThroughTheAccountAndRevertOnError() async {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/market/packages", Self.catalog)
        transport.on("GET", "/api/plugins/@account", Self.accountState)
        transport.on("GET", "/api/status-monitors", Self.monitorList)
        let store = AccountPackagesStore(client: PackagesClient(transport: transport), prefs: InMemoryKeyValueStore(), signedIn: { true })
        await store.refresh()
        transport.on("POST", "/api/plugins/@account/status-page/settings", [
            "ok": true, "packages": [Self.entry("status-page", true, ["onlyProblems": false, "refreshMinutes": "30"])],
        ])
        let saved = await store.setSetting("status-page", key: "refreshMinutes", value: "30")
        XCTAssertTrue(saved)
        XCTAssertEqual(transport.last("POST", "/api/plugins/@account/status-page/settings")?.body, ["settings": ["refreshMinutes": "30"]])
        XCTAssertEqual(store.refreshMinutes, 30)
        XCTAssertFalse(store.onlyProblems)

        let refused = await store.setSetting("status-page", key: "refreshMinutes", value: "7")
        XCTAssertFalse(refused)
        XCTAssertEqual(store.refreshMinutes, 30)

        transport.routes["POST /api/plugins/@account/status-page/settings"] = [
            PackagesReply(status: 400, json: ["error": "invalid_setting", "key": "onlyProblems"]),
        ]
        let failed = await store.setSetting("status-page", key: "onlyProblems", value: true)
        XCTAssertFalse(failed)
        XCTAssertFalse(store.onlyProblems)
        XCTAssertEqual(store.errorText, "That setting value is not allowed.")
    }

    @MainActor func testMonitorsAreCheckedAddedAndRemoved() async {
        let transport = FakePackagesTransport()
        let store = AccountPackagesStore(client: PackagesClient(transport: transport), prefs: InMemoryKeyValueStore(), signedIn: { true })
        let empty = await store.addMonitor(name: "  ", url: "https://a.example/")
        XCTAssertFalse(empty)
        XCTAssertEqual(store.monitorError, "Give the monitor a name of 1 to 40 characters.")
        for bad in ["ftp://a.example/", "https://user:pw@a.example/", "not a url", "https://" + String(repeating: "a", count: 300)] {
            XCTAssertEqual(StatusMonitor.problem(name: "A", url: bad), "invalid_url", bad)
        }
        XCTAssertNil(StatusMonitor.problem(name: "Blog", url: "http://blog.example.com:8080/health"))
        XCTAssertTrue(transport.sent.isEmpty)

        transport.on("POST", "/api/status-monitors/monitors", [
            "ok": true, "monitor": ["id": .string(hexB), "name": "Shop", "url": "https://shop.example/"],
            "monitors": [
                ["id": .string(hexA), "name": "Blog", "url": "https://blog.example.com/"],
                ["id": .string(hexB), "name": "Shop", "url": "https://shop.example/"],
            ],
        ])
        let added = await store.addMonitor(name: " Shop ", url: "https://shop.example/ ")
        XCTAssertTrue(added)
        XCTAssertEqual(transport.last("POST", "/api/status-monitors/monitors")?.body, ["name": "Shop", "url": "https://shop.example/"])
        XCTAssertEqual(store.monitors.map(\.id), [hexA, hexB])
        XCTAssertEqual(store.monitorError, "")

        transport.on("DELETE", "/api/status-monitors/monitors/\(hexA)", [
            "ok": true, "monitors": [["id": .string(hexB), "name": "Shop", "url": "https://shop.example/"]],
        ])
        let removed = await store.removeMonitor(hexA)
        XCTAssertTrue(removed)
        XCTAssertEqual(store.monitors.map(\.name), ["Shop"])

        transport.routes["POST /api/status-monitors/monitors"] = [PackagesReply(status: 409, json: ["error": "too_many_monitors"])]
        let full = await store.addMonitor(name: "More", url: "https://more.example/")
        XCTAssertFalse(full)
        XCTAssertEqual(store.monitorError, "You have the most monitors allowed. Remove one first.")
        let badId = await store.removeMonitor("../x")
        XCTAssertFalse(badId)

        transport.on("GET", "/api/status-monitors", status: 409, ["error": "status_page_not_installed"])
        await store.refreshMonitors()
        XCTAssertEqual(store.monitors, [])
    }

    @MainActor func testMarketShowsAccountPackagesFromTheAccount() async {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/market/packages", Self.catalog)
        transport.on("GET", "/api/plugins/@account", Self.accountState)
        transport.on("GET", "/api/status-monitors", Self.monitorList)
        let store = AccountPackagesStore(client: PackagesClient(transport: transport), prefs: InMemoryKeyValueStore(), signedIn: { true })
        let model = MarketModel(client: PackagesClient(transport: transport), account: store)
        await model.refresh(organizations: [])
        XCTAssertTrue(transport.paths.contains("GET /api/plugins/@account"))
        XCTAssertEqual(transport.paths.filter { $0 == "GET /api/market/packages" }.count, 1)
        XCTAssertTrue(model.isInstalled(model.package("status-page")!))
        XCTAssertTrue(model.isInstalled(model.package("status-check-api")!))
        XCTAssertFalse(model.isInstalled(model.package("status-check-website")!))
        XCTAssertEqual(model.visible.map(\.id), ["status-page", "crm"])
        XCTAssertEqual(model.nested(in: model.package("status-page")!).map(\.id),
                       ["status-check-api", "status-check-website", "status-check-cores", "status-check-extra"])
        XCTAssertEqual(model.marketParent(of: model.package("status-check-api")!), "status-page")
        XCTAssertNil(model.marketParent(of: model.package("status-page")!))
        model.query = "cores"
        XCTAssertEqual(model.visible.map(\.id), ["status-page"])
        model.query = ""
        let alone = MarketModel(client: PackagesClient(transport: transport))
        XCTAssertTrue(alone.isInstalled(MarketPackage(id: "status-check-api", name: "A", defaultOn: true, accountSync: true)))
        XCTAssertFalse(alone.isInstalled(MarketPackage(id: "status-check-extra", name: "E", defaultOn: false, accountSync: true)))
    }

    @MainActor func testAnUpdatedPackageIsMarkedUntilItIsViewed() {
        let prefs = InMemoryKeyValueStore()
        let store = AccountPackagesStore(
            client: PackagesClient(transport: FakePackagesTransport()), prefs: prefs, signedIn: { false })
        store.adoptCatalog([
            MarketPackage(id: "calories", name: "Calories", version: "2.5.1"),
            MarketPackage(id: "notes", name: "Notes", version: "1.1.0"),
            MarketPackage(id: "tasks", name: "Tasks"),
        ])
        XCTAssertTrue(store.updated.isEmpty)

        store.adoptCatalog([
            MarketPackage(id: "calories", name: "Calories", version: "2.6.0"),
            MarketPackage(id: "notes", name: "Notes", version: "1.1.0"),
            MarketPackage(id: "tasks", name: "Tasks", version: "1.0.0"),
        ])
        XCTAssertEqual(store.updated, ["calories"])
        XCTAssertTrue(store.isUpdated(hubItem: "calories"))
        XCTAssertFalse(store.isUpdated(hubItem: "notes"))
        XCTAssertFalse(store.isUpdated(hubItem: "tasks"))

        let again = AccountPackagesStore(
            client: PackagesClient(transport: FakePackagesTransport()), prefs: prefs, signedIn: { false })
        XCTAssertEqual(again.updated, ["calories"])

        store.markViewed(hubItem: "calories")
        XCTAssertTrue(store.updated.isEmpty)
        let viewed = AccountPackagesStore(
            client: PackagesClient(transport: FakePackagesTransport()), prefs: prefs, signedIn: { false })
        XCTAssertTrue(viewed.updated.isEmpty)
    }

    @MainActor func testHubItemsMapToTheirPackage() {
        let store = AccountPackagesStore(
            client: PackagesClient(transport: FakePackagesTransport()), prefs: InMemoryKeyValueStore(), signedIn: { false })
        store.adoptCatalog([
            MarketPackage(id: "sugar-break", name: "Sugar break", version: "1.0.0"),
            MarketPackage(id: "agents", name: "Agents", version: "1.0.0"),
        ])
        store.adoptCatalog([
            MarketPackage(id: "sugar-break", name: "Sugar break", version: "1.1.0"),
            MarketPackage(id: "agents", name: "Agents", version: "1.2.0"),
        ])
        XCTAssertTrue(store.isUpdated(hubItem: "sugarBreak"))
        XCTAssertTrue(store.isUpdated(hubItem: "Agents"))
        store.markViewed("agents")
        XCTAssertFalse(store.isUpdated(hubItem: "Agents"))
        XCTAssertTrue(store.isUpdated(hubItem: "sugarBreak"))
    }
}
