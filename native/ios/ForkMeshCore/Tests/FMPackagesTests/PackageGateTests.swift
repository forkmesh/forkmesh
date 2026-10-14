import FMAccount
import FMApi
import FMBase
import XCTest

@testable import FMPackages

final class PackageGateTests: XCTestCase {
    static func row(_ id: String, before: String? = "2026-10-11", deps: [String] = [], defaultOn: Bool = false) -> JSONValue {
        var row: JSONObject = [
            "id": .string(id), "name": .string(id), "bundled": true, "default": .bool(defaultOn), "accountSync": true,
            "dependencies": .array(deps.map(JSONValue.string)),
        ]
        if let before { row["installedBefore"] = .string(before) }
        return .object(row)
    }

    static let packages: [MarketPackage] = [
        row("agents"), row("special-agents", deps: ["agents"]), row("branches"), row("pull-requests", deps: ["branches"]),
        row("notes"), row("tour"), row("vault", before: nil), row("status-page"), row("workflows", before: "2026-10-14"),
    ].compactMap { $0.objectValue.flatMap(MarketPackage.init(json:)) }

    @MainActor func store(
        legacy: Bool, prefs: KeyValueStore = InMemoryKeyValueStore(), transport: FakePackagesTransport? = nil,
        namespace: @escaping () -> String = { "" }
    ) -> AccountPackagesStore {
        let store = AccountPackagesStore(
            client: PackagesClient(transport: transport ?? FakePackagesTransport()), prefs: prefs, signedIn: { false },
            legacyProject: { legacy }, projectNamespace: namespace)
        store.adoptCatalog(Self.packages)
        return store
    }

    static func choose(_ prefs: KeyValueStore, _ choices: [String: Bool], namespace: String = "") {
        var saved = JSONObject()
        for (id, on) in choices { saved[id] = .bool(on) }
        prefs.set(JSON.encode(saved), forKey: namespace + AccountPackagesStore.projectInstallsKey)
    }

    func testMarketRowsCarryInstalledBefore() {
        let agents = MarketPackage(json: Self.row("agents").objectValue!)!
        XCTAssertEqual(agents.installedBefore, "2026-10-11")
        XCTAssertFalse(agents.defaultOn)
        XCTAssertEqual(MarketPackage(json: Self.row("vault", before: nil).objectValue!)!.installedBefore, "")
        XCTAssertEqual(MarketPackage(json: ["id": "x", "installedBefore": "soon"])!.installedBefore, "")
        let info = AccountPackageInfo(package: agents)
        XCTAssertEqual(AccountPackageInfo(json: info.json)?.installedBefore, "2026-10-11")
    }

    @MainActor func testANewProjectHidesEveryGatedPackage() async {
        let store = store(legacy: false)
        for id in ["agents", "special-agents", "branches", "pull-requests", "notes", "tour", "vault", "status-page"] {
            XCTAssertFalse(store.isInstalled(id), id)
        }
        XCTAssertFalse(store.isInstalled("chat"))
        XCTAssertTrue(store.isInstalled("some-unknown-plugin"))
        XCTAssertTrue(store.installedGatedPackages.isEmpty)
    }

    @MainActor func testALegacyProjectKeepsWhatWasInstalledBefore() async {
        let store = store(legacy: true)
        for id in ["agents", "special-agents", "branches", "pull-requests", "notes", "tour", "status-page"] {
            XCTAssertTrue(store.isInstalled(id), id)
        }
        XCTAssertFalse(store.isInstalled("vault"))
        XCTAssertTrue(store.isInstalled("chat"))
        XCTAssertFalse(store.installedGatedPackages.contains("vault"))
        XCTAssertFalse(store.installedGatedPackages.contains("crm"))
    }

    @MainActor func testTheAccountAnswerWins() async {
        let prefs = InMemoryKeyValueStore()
        let state = AccountPackagesState(packages: [
            AccountPackageEntry(id: "notes", installed: false), AccountPackageEntry(id: "vault", installed: true),
        ])
        prefs.set(JSON.encode(state.json), forKey: AccountPackagesStore.stateKey)
        let legacy = store(legacy: true, prefs: prefs)
        XCTAssertFalse(legacy.isInstalled("notes"))
        XCTAssertTrue(legacy.isInstalled("vault"))
        let fresh = store(legacy: false, prefs: prefs)
        XCTAssertFalse(fresh.isInstalled("vault"))
        XCTAssertFalse(fresh.isInstalled("agents"))
        Self.choose(prefs, ["vault": true, "notes": true])
        let picked = store(legacy: false, prefs: prefs)
        XCTAssertTrue(picked.isInstalled("vault"))
        XCTAssertFalse(picked.isInstalled("notes"))
    }

    @MainActor func testAnOldAccountsFallbackFollowsTheProject() async {
        let prefs = InMemoryKeyValueStore()
        let state = AccountPackagesState(json: [
            "packages": [
                ["id": "agents", "installed": true, "legacy": true],
                ["id": "notes", "installed": true],
            ],
        ])
        XCTAssertEqual(AccountPackagesState(json: state.json), state)
        prefs.set(JSON.encode(state.json), forKey: AccountPackagesStore.stateKey)
        let fresh = store(legacy: false, prefs: prefs)
        XCTAssertFalse(fresh.isInstalled("agents"))
        XCTAssertFalse(fresh.isInstalled("notes"))
        XCTAssertEqual(fresh.installedGatedPackages, [])
        let legacy = store(legacy: true, prefs: prefs)
        XCTAssertTrue(legacy.isInstalled("agents"))
        XCTAssertTrue(legacy.isInstalled("notes"))
    }

    @MainActor func testALegacyHookInstallFollowsTheProject() async {
        let prefs = InMemoryKeyValueStore()
        let state = AccountPackagesState(json: [
            "packages": [
                ["id": "vault", "installed": true, "legacy": true],
                ["id": "notes", "installed": false, "legacy": true],
            ],
        ])
        prefs.set(JSON.encode(state.json), forKey: AccountPackagesStore.stateKey)
        let fresh = store(legacy: false, prefs: prefs)
        XCTAssertEqual(fresh.info("vault")?.installedBefore, "")
        XCTAssertFalse(fresh.isInstalled("vault"))
        XCTAssertFalse(fresh.installedGatedPackages.contains("vault"))
        let legacy = store(legacy: true, prefs: prefs)
        XCTAssertTrue(legacy.isInstalled("vault"))
        XCTAssertTrue(legacy.installedGatedPackages.contains("vault"))
        XCTAssertFalse(legacy.isInstalled("notes"))
    }

    @MainActor func testANewProjectShowsOnlyWhatItInstalled() async {
        let prefs = InMemoryKeyValueStore()
        let state = AccountPackagesState(json: [
            "packages": [
                ["id": "agents", "installed": true, "legacy": true],
                ["id": "vault", "installed": true, "legacy": true],
                ["id": "notes", "installed": true],
                ["id": "workflows", "installed": true],
                ["id": "tour", "installed": false],
            ],
        ])
        prefs.set(JSON.encode(state.json), forKey: AccountPackagesStore.stateKey)
        XCTAssertTrue(store(legacy: false, prefs: prefs).installedGatedPackages.isEmpty)
        Self.choose(prefs, ["vault": true, "notes": true, "tour": true, "workflows": false])
        let fresh = store(legacy: false, prefs: prefs)
        XCTAssertTrue(fresh.ownsInstalls)
        XCTAssertEqual(fresh.installedGatedPackages, ["vault", "notes"])
        XCTAssertFalse(fresh.isInstalled("tour"))
        let legacy = store(legacy: true, prefs: prefs)
        XCTAssertFalse(legacy.ownsInstalls)
        XCTAssertTrue(legacy.installedGatedPackages.isSuperset(of: ["agents", "vault", "notes", "workflows"]))
    }

    @MainActor func testInstallingAndRemovingInANewProjectStaysInTheProject() async {
        let prefs = InMemoryKeyValueStore()
        let transport = FakePackagesTransport()
        transport.on("POST", "/api/plugins/@account/special-agents/install", [
            "ok": true, "changed": ["special-agents", "agents"],
            "packages": [["id": "agents", "installed": true], ["id": "special-agents", "installed": true]],
        ])
        let fresh = store(legacy: false, prefs: prefs, transport: transport)
        let installed = await fresh.install("special-agents")
        XCTAssertEqual(installed, ["special-agents", "agents"])
        XCTAssertEqual(fresh.projectChoices, ["special-agents": true, "agents": true])
        XCTAssertEqual(fresh.installedGatedPackages, ["agents", "special-agents"])
        let removed = await fresh.uninstall("agents")
        XCTAssertEqual(removed, ["agents", "special-agents"])
        XCTAssertNil(transport.last("POST", "/api/plugins/@account/agents/uninstall"))
        XCTAssertEqual(fresh.state?.entry("agents")?.installed, true)
        XCTAssertTrue(fresh.installedGatedPackages.isEmpty)
        XCTAssertTrue(store(legacy: true, prefs: prefs).isInstalled("special-agents"))
        transport.on("POST", "/api/plugins/@account/notes/uninstall", [
            "ok": true, "changed": ["notes"], "packages": [["id": "notes", "installed": false]],
        ])
        _ = await fresh.uninstall("notes", everywhere: true)
        XCTAssertNotNil(transport.last("POST", "/api/plugins/@account/notes/uninstall"))
        XCTAssertEqual(fresh.projectChoices["notes"], false)
    }

    @MainActor func testEachProjectKeepsItsOwnChoices() async {
        let prefs = InMemoryKeyValueStore()
        let state = AccountPackagesState(packages: [AccountPackageEntry(id: "notes", installed: true)])
        prefs.set(JSON.encode(state.json), forKey: AccountPackagesStore.stateKey)
        Self.choose(prefs, ["notes": true], namespace: "p/one/")
        var namespace = "p/one/"
        let fresh = store(legacy: false, prefs: prefs, namespace: { namespace })
        XCTAssertTrue(fresh.isInstalled("notes"))
        namespace = "p/two/"
        XCTAssertFalse(fresh.isInstalled("notes"))
        let removed = AccountPackagesState(packages: [AccountPackageEntry(id: "notes", installed: false)])
        prefs.set(JSON.encode(removed.json), forKey: AccountPackagesStore.stateKey)
        namespace = "p/one/"
        XCTAssertFalse(store(legacy: false, prefs: prefs, namespace: { namespace }).isInstalled("notes"))
    }

    func testPlacesParseFromTheAccountAndTheMarket() {
        let entry = AccountPackageEntry(json: [
            "id": "notes", "installed": true, "places": ["web", "mobile", "watch"], "availablePlaces": ["mobile", "web"],
        ])!
        XCTAssertEqual(entry.places, [.mobile, .web])
        XCTAssertEqual(entry.availablePlaces, [.mobile, .web])
        XCTAssertTrue(entry.shows(on: .mobile))
        XCTAssertFalse(entry.shows(on: .desktop))
        XCTAssertEqual(AccountPackageEntry(json: entry.json), entry)
        let older = AccountPackageEntry(json: ["id": "notes", "installed": true])!
        XCTAssertEqual(older.places, [.mobile, .desktop, .web])
        XCTAssertEqual(older.availablePlaces, [.mobile, .desktop, .web])
        XCTAssertEqual(AccountPackageEntry(json: ["id": "notes", "installed": true, "places": []])!.places, PackagePlace.allCases)
        XCTAssertEqual(MarketPackage(json: ["id": "food-guide", "places": ["mobile"]])!.places, [.mobile])
        XCTAssertEqual(MarketPackage(json: ["id": "notes"])!.places, [.mobile, .desktop, .web])
        XCTAssertEqual(PackagePlace.allCases.map(\.title), ["Phone", "Desktop", "Web"])
        XCTAssertEqual(PackagePlace.here, .mobile)
    }

    @MainActor func testAPackageShowsOnlyWhereItIsPlaced() async {
        let prefs = InMemoryKeyValueStore()
        let state = AccountPackagesState(json: [
            "packages": [
                ["id": "notes", "installed": true, "places": ["desktop", "web"]],
                ["id": "agents", "installed": true, "places": ["web"]],
                ["id": "special-agents", "installed": true, "places": ["mobile"]],
                ["id": "tour", "installed": true, "places": ["mobile", "desktop"]],
                ["id": "workflows", "installed": true],
            ],
        ])
        prefs.set(JSON.encode(state.json), forKey: AccountPackagesStore.stateKey)
        Self.choose(prefs, ["notes": true, "agents": true, "special-agents": true, "tour": true, "workflows": true])
        let fresh = store(legacy: false, prefs: prefs)
        XCTAssertTrue(fresh.isInstalled("notes"))
        XCTAssertFalse(fresh.isPlaced("notes"))
        XCTAssertTrue(fresh.isPlaced("notes", on: .web))
        XCTAssertFalse(fresh.isShownHere("notes"))
        XCTAssertFalse(fresh.isShownHere("agents"))
        XCTAssertTrue(fresh.isShownHere("special-agents"))
        XCTAssertTrue(fresh.isShownHere("tour"))
        XCTAssertTrue(fresh.isShownHere("workflows"))
        XCTAssertEqual(fresh.installedGatedPackages, ["special-agents", "tour", "workflows"])
        XCTAssertEqual(fresh.places("notes"), [.desktop, .web])
        XCTAssertEqual(fresh.places("branches"), PackagePlace.allCases)
        let legacy = store(legacy: true, prefs: prefs)
        XCTAssertTrue(legacy.isShownHere("branches"))
        XCTAssertTrue(legacy.isShownHere("pull-requests"))
        XCTAssertFalse(legacy.installedGatedPackages.contains("notes"))
        let settings = SettingsService.create(prefs: InMemoryKeyValueStore(), secureStore: InMemorySecureStore())
        let installed = PhonePackages.installed(account: fresh, settings: settings, organization: [])
        XCTAssertFalse(installed.contains("notes"))
        XCTAssertTrue(installed.contains("special-agents"))
    }

    @MainActor func testWorkflowsIsGatedLikeTheOtherPackages() async {
        XCTAssertTrue(PackageGate.isGated("workflows"))
        XCTAssertFalse(store(legacy: false).isInstalled("workflows"))
        XCTAssertTrue(store(legacy: true).isInstalled("workflows"))
        let empty = AccountPackagesStore(
            client: PackagesClient(transport: FakePackagesTransport()), prefs: InMemoryKeyValueStore(), signedIn: { false },
            legacyProject: { false })
        XCTAssertFalse(empty.isInstalled("workflows"))
        let groups = HubCatalog.groups(guest: false) { _ in true }
        XCTAssertFalse(PackageGate.filter(groups, installed: []).flatMap(\.items).contains { $0.id == "workflows" })
        XCTAssertTrue(PackageGate.filter(groups, installed: ["workflows"]).flatMap(\.items).contains { $0.id == "workflows" })
        XCTAssertEqual(MarketPackage(json: Self.row("workflows").objectValue!)!.phoneSurface, .native("workflows"))
        XCTAssertEqual(PackageRoutes.packageId(forHubItem: "workflows"), "workflows")
    }

    @MainActor func testAChildNeedsItsDependency() async {
        let prefs = InMemoryKeyValueStore()
        let state = AccountPackagesState(packages: [
            AccountPackageEntry(id: "pull-requests", installed: true), AccountPackageEntry(id: "branches", installed: false),
            AccountPackageEntry(id: "special-agents", installed: true),
        ])
        prefs.set(JSON.encode(state.json), forKey: AccountPackagesStore.stateKey)
        let fresh = store(legacy: false, prefs: prefs)
        XCTAssertFalse(fresh.isInstalled("pull-requests"))
        XCTAssertFalse(fresh.isInstalled("special-agents"))
        let legacy = store(legacy: true, prefs: prefs)
        XCTAssertFalse(legacy.isInstalled("pull-requests"))
        XCTAssertTrue(legacy.isInstalled("special-agents"))
        XCTAssertEqual(legacy.missingDependencies(of: "pull-requests").map(\.id), ["branches"])
        XCTAssertEqual(fresh.dependents(of: "agents").map(\.id), [])
        XCTAssertEqual(legacy.dependents(of: "agents").map(\.id), ["special-agents"])
    }

    @MainActor func testUnknownGatedPackagesFollowTheProjectBeforeTheCatalogLoads() async {
        let empty = AccountPackagesStore(
            client: PackagesClient(transport: FakePackagesTransport()), prefs: InMemoryKeyValueStore(), signedIn: { false },
            legacyProject: { false })
        XCTAssertFalse(empty.isInstalled("agents"))
        XCTAssertFalse(empty.statusPageOn)
        XCTAssertTrue(empty.isInstalled("status-check-api"))
        let old = AccountPackagesStore(
            client: PackagesClient(transport: FakePackagesTransport()), prefs: InMemoryKeyValueStore(), signedIn: { false },
            legacyProject: { true })
        XCTAssertTrue(old.isInstalled("agents"))
        XCTAssertTrue(old.statusPageOn)
    }

    @MainActor func testTheGateCombinesAccountDeviceAndOrganization() async {
        let settings = SettingsService.create(prefs: InMemoryKeyValueStore(), secureStore: InMemorySecureStore())
        let fresh = store(legacy: false)
        var installed = PhonePackages.installed(account: fresh, settings: settings, organization: ["crm", "ledger"])
        XCTAssertEqual(installed, ["crm"])
        settings.setSignInOn(true)
        installed = PhonePackages.installed(account: fresh, settings: settings, organization: [])
        XCTAssertEqual(installed, ["sign-in"])
        XCTAssertTrue(PhonePackages.isInstalled("sign-in", account: fresh, settings: settings))
        XCTAssertFalse(PhonePackages.isInstalled("notes", account: fresh, settings: settings))
        let legacy = PhonePackages.installed(account: store(legacy: true), settings: settings, organization: [])
        XCTAssertTrue(legacy.isSuperset(of: ["agents", "notes", "special-agents", "pull-requests", "sign-in"]))
        XCTAssertFalse(legacy.contains("vault"))
        XCTAssertFalse(legacy.contains("crm"))
    }

    @MainActor func testHubFilteringFollowsTheGate() async {
        let groups = HubCatalog.groups(guest: false) { _ in true }
        let fresh = PackageGate.filter(groups, installed: [])
        let ids = Set(fresh.flatMap(\.items).map(\.id))
        XCTAssertTrue(ids.isSuperset(of: ["market", "settings"]))
        XCTAssertTrue(ids.isDisjoint(with: ["workflows", "notes", "teammates", "desktops", "status", "tour", "designSystem", "world", "people", "docs", "orgs", "reportBug", "signOut", "crm", "vault"]))
        let some = PackageGate.filter(groups, installed: ["notes", "sign-in", "crm", "people", "docs"])
        let shown = Set(some.flatMap(\.items).map(\.id))
        XCTAssertTrue(shown.isSuperset(of: ["notes", "signOut", "crm", "people", "docs"]))
        XCTAssertFalse(shown.contains("tasks"))
        XCTAssertEqual(PackageGate.filter(groups, installed: nil), groups)
        let business = PackageRoutes.hubRoutes(installed: []).map(\.id)
        XCTAssertEqual(business, ["market", "ledger"])
        XCTAssertEqual(PackageRoutes.hubRoutes(installed: ["crm"]).map(\.id), ["market", "crm", "ledger"])
    }

    func testBarAndRepoItemsMapToTheirPackages() {
        XCTAssertEqual(PackageGate.packageId(forItem: "Agents"), "agents")
        XCTAssertEqual(PackageGate.packageId(forItem: "currentRepo"), "code")
        XCTAssertEqual(PackageGate.packageId(forItem: "repo-pulls"), "pull-requests")
        XCTAssertEqual(PackageGate.packageId(forItem: "teammates"), "special-agents")
        XCTAssertNil(PackageGate.packageId(forItem: "Repos"))
        XCTAssertNil(PackageGate.packageId(forItem: "Search"))
        XCTAssertNil(PackageGate.packageId(forItem: "market"))
        XCTAssertEqual(PackageGate.packageId(forItem: "workflows"), "workflows")
        XCTAssertNil(PackageGate.packageId(forItem: "settings"))
        XCTAssertFalse(PackageGate.shows(item: "repo-issues", installed: ["pull-requests"]))
        XCTAssertTrue(PackageGate.shows(item: "Repos", installed: []))
        let repo = RepoRef(owner: "a", name: "b")
        XCTAssertEqual(PackageGate.packageId(for: Route.repo(repo, section: .branches)), "branches")
        XCTAssertNil(PackageGate.packageId(for: Route.repo(repo, section: nil)))
        XCTAssertEqual(PackageGate.packageId(for: Route.pull(repo, number: 1, tab: .conversation)), "pull-requests")
        for id in PackageGate.packages.subtracting(PackageGate.organizationPackages).subtracting(PackageGate.devicePackages) {
            XCTAssertNotNil(MarketPackage.nativeDestinations[id], id)
        }
    }
}
