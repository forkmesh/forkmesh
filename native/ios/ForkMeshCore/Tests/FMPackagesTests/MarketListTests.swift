import FMAccount
import FMBase
import FMNet
import XCTest

@testable import FMApi
@testable import FMPackages

final class MarketListTests: XCTestCase {
    @MainActor func withSettings(_ body: (SettingsService) async throws -> Void) async rethrows {
        let settings = SettingsService.create(prefs: InMemoryKeyValueStore(), secureStore: InMemorySecureStore())
        let previous: SettingsService? = Services.get(SettingsService.self)
        Services.put(settings)
        defer {
            if let previous {
                Services.put(previous)
            } else {
                Services.remove(SettingsService.self)
            }
        }
        try await body(settings)
    }

    func row(
        _ id: String, category: String = "Tools", type: String = "extension.native", defaultOn: Bool = false,
        accountSync: Bool = false, scope: String = "", web: String = "", desktop: Bool = true, world: Bool = false,
        free: Bool = true, publisher: String = "ForkMesh", verified: Bool = true
    ) -> JSONValue {
        .object([
            "id": .string(id), "name": .string(id.capitalized), "category": .string(category), "type": .string(type),
            "bundled": true, "default": .bool(defaultOn), "accountSync": .bool(accountSync), "dataScope": .string(scope),
            "surfaces": .object(["desktop": .bool(desktop), "web": .string(web), "world": .bool(world)]),
            "free": .bool(free), "publisher": .string(publisher), "verified": .bool(verified),
        ])
    }

    @MainActor func testABundledDesktopPackageIsInstalledOnlyWhenTheDesktopInstallsItByDefault() async {
        await withSettings { _ in
            let model = MarketModel(client: PackagesClient(transport: FakePackagesTransport()))
            let winamp = MarketPackage(json: row("winamp").objectValue!)!
            let capture = MarketPackage(json: row("capture", defaultOn: true).objectValue!)!
            XCTAssertTrue(winamp.bundled)
            XCTAssertFalse(model.isInstalled(winamp))
            XCTAssertTrue(model.isInstalled(capture))
            XCTAssertEqual(model.installScope(winamp), .desktop)
            XCTAssertEqual(model.cardAction(winamp, signedIn: true), .desktop)
        }
    }

    @MainActor func testAnAccountPackageHiddenOnThisPhoneReadsNotInstalledUntilInstalledAgain() async {
        await withSettings { settings in
            let transport = FakePackagesTransport()
            transport.on("GET", "/api/market/packages", ["packages": [row("tasks", accountSync: true)]])
            transport.on("GET", "/api/plugins/@account", ["packages": [["id": "tasks", "installed": true]]])
            let store = AccountPackagesStore(
                client: PackagesClient(transport: transport), prefs: InMemoryKeyValueStore(), signedIn: { true })
            let model = MarketModel(client: PackagesClient(transport: transport), account: store)
            await model.refresh(organizations: [])
            let tasks = model.package("tasks")!
            XCTAssertTrue(model.isInstalled(tasks))
            XCTAssertEqual(model.cardAction(tasks, signedIn: true), .open)

            settings.setPackageRemoved("tasks", removed: true)
            XCTAssertFalse(PhonePackages.installed(account: store, settings: settings, organization: []).contains("tasks"))
            XCTAssertFalse(model.isInstalled(tasks))
            XCTAssertEqual(model.cardAction(tasks, signedIn: true), .installPhone)

            let ok = await model.installForAccount(tasks)
            XCTAssertTrue(ok)
            XCTAssertFalse(settings.removedPackages.contains("tasks"))
            XCTAssertTrue(model.isInstalled(tasks))
            XCTAssertFalse(transport.paths.contains("POST /api/plugins/@account/tasks/install"))
        }
    }

    @MainActor func testOneTapInstallForTheAccountReportsItsError() async {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/market/packages", ["packages": [row("notes", accountSync: true)]])
        transport.on("GET", "/api/plugins/@account", ["packages": [["id": "notes", "installed": false]]])
        transport.on("POST", "/api/plugins/@account/notes/install", status: 401, ["error": "sign_in_required"])
        let store = AccountPackagesStore(client: PackagesClient(transport: transport), prefs: InMemoryKeyValueStore(), signedIn: { true })
        let model = MarketModel(client: PackagesClient(transport: transport), account: store)
        await model.refresh(organizations: [])
        let notes = model.package("notes")!
        XCTAssertEqual(model.cardAction(notes, signedIn: false), .signIn)
        XCTAssertEqual(model.cardAction(notes, signedIn: true), .installAccount)
        let failed = await model.installForAccount(notes)
        XCTAssertFalse(failed)
        XCTAssertEqual(model.installErrors["notes"], "Sign in again to continue.")
        transport.routes["POST /api/plugins/@account/notes/install"] = [
            PackagesReply(status: 200, json: ["packages": [["id": "notes", "installed": true]]]),
        ]
        let ok = await model.installForAccount(notes)
        XCTAssertTrue(ok)
        XCTAssertNil(model.installErrors["notes"])
        XCTAssertTrue(model.isInstalled(notes))
    }

    @MainActor func testAnOrganizationInstallOnOneScreenShowsOnTheOther() async {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/market/packages", ["packages": [row("crm", scope: "organization", web: "/dashboard/crm")]])
        transport.on("GET", "/api/plugins/acme", [
            "organization": "acme", "role": "owner", "canManage": true, "plugins": [["id": "crm", "installed": false]],
        ])
        transport.on("POST", "/api/plugins/acme/crm/install", [
            "organization": "acme", "role": "owner", "canManage": true, "plugins": [["id": "crm", "installed": true]],
        ])
        transport.on("POST", "/api/market/reviews/uses", ["ok": true])
        transport.on("POST", "/api/plugins/acme/crm/uninstall", ["ok": true, "uninstalled": true, "hadData": false])
        let ledger = OrganizationPackagesLedger()
        let list = MarketModel(client: PackagesClient(transport: transport), ledger: ledger)
        let detail = MarketModel(client: PackagesClient(transport: transport), ledger: ledger)
        await list.refresh(organizations: ["acme"])
        await detail.refresh(organizations: ["acme"])
        let crm = list.package("crm")!
        XCTAssertFalse(list.isInstalled(crm))
        XCTAssertEqual(list.cardAction(crm, signedIn: true), .installOrganization("acme"))
        let ok = await detail.install(crm, organization: "acme")
        XCTAssertTrue(ok)
        XCTAssertTrue(list.isInstalled(crm))
        XCTAssertEqual(list.cardAction(crm, signedIn: true), .open)
        _ = await detail.uninstall(crm, organization: "acme")
        XCTAssertFalse(list.isInstalled(crm))
    }

    @MainActor func testAnOrganizationThatFailsToLoadKeepsItsLastStateAndSaysSo() async {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/market/packages", ["packages": [row("crm", scope: "organization")]])
        transport.on("GET", "/api/plugins/acme", [
            "organization": "acme", "role": "member", "canManage": false, "plugins": [["id": "crm", "installed": true]],
        ])
        let ledger = OrganizationPackagesLedger()
        let model = MarketModel(client: PackagesClient(transport: transport), ledger: ledger)
        await model.refresh(organizations: ["acme"])
        XCTAssertTrue(model.isInstalled(model.package("crm")!))
        transport.routes["GET /api/plugins/acme"] = [PackagesReply(status: 503, json: ["error": ""])]
        await model.refresh(organizations: ["acme"])
        XCTAssertTrue(model.isInstalled(model.package("crm")!))
        XCTAssertTrue(model.partialError.hasSuffix("your organizations' packages."))
    }

    @MainActor func testOrganizationCardActions() async {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/market/packages", ["packages": [row("crm", scope: "organization")]])
        transport.on("GET", "/api/plugins/acme", [
            "organization": "acme", "role": "owner", "canManage": true, "plugins": [],
        ])
        transport.on("GET", "/api/plugins/beta", [
            "organization": "beta", "role": "admin", "canManage": true, "plugins": [],
        ])
        transport.on("GET", "/api/plugins/gamma", [
            "organization": "gamma", "role": "member", "canManage": false, "plugins": [],
        ])
        let model = MarketModel(client: PackagesClient(transport: transport))
        await model.refresh(organizations: [])
        let crm = model.package("crm")!
        XCTAssertEqual(model.cardAction(crm, signedIn: true), .createOrganization)
        await model.refresh(organizations: ["acme", "beta"])
        XCTAssertEqual(model.cardAction(crm, signedIn: true), .chooseOrganization(["acme", "beta"]))
        await model.refresh(organizations: ["gamma"])
        XCTAssertEqual(model.cardAction(crm, signedIn: true), .askAdmin)
    }

    @MainActor func testEverySortOrdersTheList() async {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/market/packages", ["packages": [row("zeta"), row("alpha"), row("mid")]])
        transport.on("GET", "/api/market/installs", ["packages": [
            "zeta": ["installs": 1, "downloads": 50, "accounts": 1, "organizations": 0],
            "alpha": ["installs": 2, "downloads": 1, "accounts": 3, "organizations": 1],
            "mid": ["installs": 0, "downloads": 9],
        ]])
        transport.on("GET", "/api/market/reviews/summary", [
            "packages": ["mid": ["average": 4.5, "count": 2], "zeta": ["average": 4.5, "count": 9]],
            "usage": ["alpha": ["views": 3, "opens": 30, "people": 1], "mid": ["views": 1, "opens": 2, "people": 4]],
        ])
        let model = MarketModel(client: PackagesClient(transport: transport))
        await model.refresh(organizations: [])
        XCTAssertEqual(model.counts["alpha"], PackageCounts(installs: 2, downloads: 1, accounts: 3, organizations: 1))
        XCTAssertEqual(model.counts["alpha"]?.total, 6)
        XCTAssertEqual(model.usage["mid"], PackageUsage(views: 1, opens: 2, people: 4))
        let expected: [MarketSort: [String]] = [
            .featured: ["zeta", "alpha", "mid"],
            .name: ["alpha", "mid", "zeta"],
            .rating: ["zeta", "mid", "alpha"],
            .installs: ["alpha", "zeta", "mid"],
            .downloads: ["zeta", "mid", "alpha"],
            .used: ["mid", "alpha", "zeta"],
            .opened: ["alpha", "mid", "zeta"],
            .installedFirst: ["zeta", "alpha", "mid"],
        ]
        XCTAssertEqual(Set(expected.keys), Set(MarketSort.allCases))
        for (sort, ids) in expected {
            model.sort = sort
            XCTAssertEqual(model.visible.map(\.id), ids, sort.rawValue)
        }
    }

    @MainActor func testEveryFilterNarrowsTheList() async {
        await withSettings { _ in
            let transport = FakePackagesTransport()
            transport.on("GET", "/api/market/packages", ["packages": [
                row("notes", category: "Productivity", defaultOn: true, accountSync: true),
                row("crm", category: "Business", scope: "organization", web: "/dashboard/crm"),
                row("aquarium", category: "Fun", type: "virtual.object", world: true),
                row("winamp", category: "Fun", type: "theme", free: false, publisher: "Nullsoft", verified: false),
            ]])
            transport.on("GET", "/api/market/reviews/summary", ["packages": ["winamp": ["average": 3, "count": 1]]])
            let model = MarketModel(client: PackagesClient(transport: transport))
            await model.refresh(organizations: [])
            XCTAssertEqual(model.categories, ["Business", "Fun", "Productivity"])
            XCTAssertEqual(model.kinds, ["extension.native", "theme", "virtual.object"])
            XCTAssertEqual(model.publishers, ["ForkMesh", "Nullsoft"])
            XCTAssertTrue(model.hasPaid)
            @MainActor func ids(_ change: (inout MarketFilters) -> Void) -> [String] {
                var filters = MarketFilters()
                change(&filters)
                model.filters = filters
                defer { model.filters = MarketFilters() }
                return model.visible.map(\.id)
            }
            XCTAssertEqual(ids { $0.category = "Fun" }, ["aquarium", "winamp"])
            XCTAssertEqual(ids { $0.kind = "theme" }, ["winamp"])
            XCTAssertEqual(ids { $0.runsOn = .phone }, ["notes", "crm", "aquarium"])
            XCTAssertEqual(ids { $0.runsOn = .web }, ["crm"])
            XCTAssertEqual(ids { $0.runsOn = .world }, ["aquarium"])
            XCTAssertEqual(ids { $0.price = .paid }, ["winamp"])
            XCTAssertEqual(ids { $0.price = .free }, ["notes", "crm", "aquarium"])
            XCTAssertEqual(ids { $0.scope = .account }, ["notes"])
            XCTAssertEqual(ids { $0.scope = .organization }, ["crm"])
            XCTAssertEqual(ids { $0.scope = .device }, ["aquarium"])
            XCTAssertEqual(ids { $0.scope = .desktop }, ["winamp"])
            XCTAssertEqual(ids { $0.publisher = "Nullsoft" }, ["winamp"])
            XCTAssertEqual(ids { $0.verifiedOnly = true }, ["notes", "crm", "aquarium"])
            XCTAssertEqual(ids { $0.ratedOnly = true }, ["winamp"])
            model.filter = .installed
            XCTAssertEqual(model.visible.map(\.id), ["notes"])
            model.filter = .notInstalled
            XCTAssertEqual(model.visible.map(\.id), ["crm", "aquarium", "winamp"])
            model.filter = .all
            model.query = "nullsoft"
            XCTAssertEqual(model.visible.map(\.id), ["winamp"])
            XCTAssertEqual(MarketFilters(category: "Fun", verifiedOnly: true).activeCount, 2)
        }
    }

    func testDottedPackageIdsFromTheServerAreKept() {
        XCTAssertTrue(PackagesText.isPackageId("nemotron-3.5-lightning"))
        XCTAssertTrue(PackagesText.isPackageId("muse-spark-1.3-contributor"))
        XCTAssertFalse(PackagesText.isPackageId("a..b"))
        XCTAssertFalse(PackagesText.isPackageId("a.-b"))
        XCTAssertFalse(PackagesText.isPackageId("a-.b"))
        XCTAssertFalse(PackagesText.isPackageId("a."))
        XCTAssertFalse(PackagesText.isPackageId(".a"))
        XCTAssertFalse(PackagesText.isPackageId("../a"))
        XCTAssertFalse(PackagesText.isOrgName("acme.io"))
        XCTAssertNotNil(MarketPackage(json: row("nemotron-3.5-lightning").objectValue!))
    }

    func testRowsCarryPriceAndPublisher() {
        let paid = MarketPackage(json: row("winamp", free: false, publisher: "Nullsoft", verified: false).objectValue!)!
        XCTAssertFalse(paid.free)
        XCTAssertEqual(paid.publisher, "Nullsoft")
        XCTAssertFalse(paid.verified)
        let old = MarketPackage(json: ["id": "old"])!
        XCTAssertTrue(old.free)
        XCTAssertEqual(old.publisher, "")
    }
}
