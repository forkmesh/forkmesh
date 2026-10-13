import FMAccount
import FMBase
import FMNet
import XCTest

@testable import FMApi
@testable import FMPackages

final class MarketTests: XCTestCase {
    func packageJSON(
        _ id: String, scope: String = "", web: String = "", category: String = "Productivity", defaultOn: Bool = false
    ) -> JSONValue {
        .object([
            "id": .string(id), "name": .string(id.uppercased()), "version": "1.0.0", "summary": "Does things",
            "category": .string(category), "dataScope": .string(scope), "bundled": .bool(scope != "organization"),
            "default": .bool(defaultOn),
            "surfaces": .object(["desktop": true, "web": .string(web), "worker": true]),
            "permissions": [["id": "organization.crm.read", "label": "Read", "risk": "sensitive"]],
        ])
    }

    func testPackageParsingAndPhoneSurface() {
        let crm = MarketPackage(json: packageJSON("crm", scope: "organization", web: "/dashboard/crm").objectValue!)!
        XCTAssertEqual(crm.dataScope, .organization)
        XCTAssertEqual(crm.phoneSurface, .native("crm"))
        XCTAssertTrue(crm.installsPerOrganization)
        XCTAssertTrue(crm.hasSensitivePermissions)
        let web = MarketPackage(json: packageJSON("discord", web: "/dashboard/discord").objectValue!)!
        XCTAssertEqual(web.phoneSurface, .web("/dashboard/discord"))
        let desk = MarketPackage(json: packageJSON("voice-changer").objectValue!)!
        XCTAssertEqual(desk.phoneSurface, .desktopOnly)
        XCTAssertNil(MarketPackage(json: ["id": "Bad Id"]))
        XCTAssertNil(MarketPackage(json: ["id": "../etc"]))
    }

    func testPackageScreenshotsKeepOnlyHttpsImages() {
        var json = packageJSON("notes").objectValue!
        json["screenshots"] = [
            ["url": "https://forkmesh.com/market/previews/packages/notes/preview/market.webp", "alt": "Notes list"],
            ["url": "http://forkmesh.com/market/previews/packages/notes/preview/plain.png", "alt": "Plain"],
            ["url": "javascript:alert(1)", "alt": "Script"],
            ["url": "", "alt": "Empty"],
        ]
        let notes = MarketPackage(json: json)!
        XCTAssertEqual(notes.screenshots.map(\.url.absoluteString), [
            "https://forkmesh.com/market/previews/packages/notes/preview/market.webp",
        ])
        XCTAssertEqual(notes.screenshots.first?.alt, "Notes list")
        XCTAssertEqual(MarketPackage(json: packageJSON("crm").objectValue!)!.screenshots, [])
    }

    func testTextValidators() {
        XCTAssertTrue(PackagesText.isOrgName("forkmesh"))
        XCTAssertTrue(PackagesText.isOrgName("acme-2"))
        XCTAssertFalse(PackagesText.isOrgName("acme-"))
        XCTAssertFalse(PackagesText.isOrgName("Acme"))
        XCTAssertFalse(PackagesText.isOrgName("a/b"))
        XCTAssertFalse(PackagesText.isOrgName(""))
        XCTAssertTrue(PackagesText.isHexId(hexA))
        XCTAssertFalse(PackagesText.isHexId("g" + String(repeating: "a", count: 31)))
        XCTAssertFalse(PackagesText.isHexId("abc"))
    }

    func testRatingSummary() {
        let summary = RatingSummary(json: ["average": 4.3, "count": 12, "histogram": ["1": 0, "2": 1, "5": 7]])
        XCTAssertEqual(summary.histogram, [0, 1, 0, 0, 7])
        XCTAssertEqual(summary.label, "4.3 (12)")
        XCTAssertEqual(RatingSummary().label, "No ratings yet")
    }

    @MainActor func testMarketModelLoadsFiltersAndInstallsPerOrganization() async {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/market/packages", [
            "packages": [
                packageJSON("crm", scope: "organization"), packageJSON("vault", scope: "account", category: "Security", defaultOn: true),
                packageJSON("marketing", scope: "organization"),
            ],
        ])
        transport.on("GET", "/api/market/installs", ["packages": ["crm": ["installs": 4, "downloads": 9]]])
        transport.on("GET", "/api/market/reviews/summary", ["packages": ["crm": ["average": 5, "count": 1]]])
        transport.on("GET", "/api/plugins/acme", [
            "organization": "acme", "role": "owner", "canManage": true,
            "plugins": [["id": "crm", "installed": true], ["id": "marketing", "installed": false]],
        ])
        let model = MarketModel(client: PackagesClient(transport: transport))
        await model.refresh(organizations: ["acme", "Bad Org"])
        XCTAssertEqual(model.packages.map(\.id), ["crm", "vault", "marketing"])
        XCTAssertEqual(model.counts["crm"]?.installs, 4)
        XCTAssertEqual(model.ratings["crm"]?.count, 1)
        XCTAssertFalse(transport.paths.contains("GET /api/plugins/Bad Org"))
        XCTAssertEqual(model.shelves.map(\.category), ["Productivity", "Security"])

        model.filter = .installed
        XCTAssertEqual(Set(model.visible.map(\.id)), ["crm", "vault"])
        model.filter = .notInstalled
        XCTAssertEqual(model.visible.map(\.id), ["marketing"])
        model.filter = .all
        model.query = "vault"
        XCTAssertEqual(model.visible.map(\.id), ["vault"])

        let marketing = model.package("marketing")!
        XCTAssertEqual(model.installTargets(marketing).map(\.organization), ["acme"])
        XCTAssertTrue(model.installTargets(model.package("vault")!).isEmpty)
        transport.on("POST", "/api/plugins/acme/marketing/install", [
            "organization": "acme", "role": "owner", "canManage": true,
            "plugins": [["id": "crm", "installed": true], ["id": "marketing", "installed": true]],
        ])
        transport.on("POST", "/api/market/reviews/uses", ["ok": true])
        let installed = await model.install(marketing, organization: "acme")
        XCTAssertTrue(installed)
        XCTAssertTrue(model.isInstalled(marketing))
        XCTAssertEqual(transport.last("POST", "/api/market/reviews/uses")?.body?.str("source"), "mobile")
    }

    @MainActor func testStatusPageIsNoLongerSwitchedPerDevice() async {
        let settings = SettingsService.create(prefs: InMemoryKeyValueStore(), secureStore: InMemorySecureStore())
        XCTAssertNil(PhonePackages.isOn("status-page", settings: settings))
        XCTAssertNil(PhonePackages.isOn("crm", settings: settings))
        let page = MarketPackage(id: "status-page", name: "Status Page", bundled: true, accountSync: true)
        XCTAssertEqual(PackageRoutes.route(for: page), .status)
        XCTAssertEqual(page.phoneSurface, .native("status"))
    }

    @MainActor func testDesignSystemAndTourFollowTheAccountNotTheDevice() {
        let settings = SettingsService.create(prefs: InMemoryKeyValueStore(), secureStore: InMemorySecureStore())
        XCTAssertNil(PhonePackages.isOn(PhonePackages.designSystem, settings: settings))
        XCTAssertNil(PhonePackages.isOn(PhonePackages.tour, settings: settings))
        let design = MarketPackage(id: "design-system", name: "Design", bundled: true, accountSync: true)
        XCTAssertEqual(design.phoneSurface, .native("designSystem"))
    }

    @MainActor func testSignInIsSwitchedPerDeviceAndStartsFromTheProjectMarker() {
        let fresh = SettingsService.create(prefs: InMemoryKeyValueStore(), secureStore: InMemorySecureStore())
        XCTAssertEqual(PhonePackages.isOn(PhonePackages.signIn, settings: fresh), false)
        PhonePackages.set(PhonePackages.signIn, on: true, settings: fresh)
        XCTAssertEqual(PhonePackages.isOn("sign-in", settings: fresh), true)
        let used = InMemoryKeyValueStore()
        used.set("{}", forKey: "shell/bar-uses")
        let legacy = SettingsService.create(prefs: used, secureStore: InMemorySecureStore())
        XCTAssertEqual(PhonePackages.isOn(PhonePackages.signIn, settings: legacy), true)
        let signIn = MarketPackage(id: "sign-in", name: "Sign in", bundled: true)
        XCTAssertEqual(signIn.phoneSurface, .native("signIn"))
    }

    @MainActor func testAquariumIsSwitchedPerDeviceAndStartsOff() {
        let settings = SettingsService.create(prefs: InMemoryKeyValueStore(), secureStore: InMemorySecureStore())
        XCTAssertEqual(PhonePackages.isOn(PhonePackages.aquarium, settings: settings), false)
        XCTAssertFalse(settings.aquariumOn)
        PhonePackages.set("aquarium", on: true, settings: settings)
        XCTAssertEqual(PhonePackages.isOn("aquarium", settings: settings), true)
        XCTAssertTrue(settings.aquariumOn)
        XCTAssertNil(PhonePackages.isOn("tour", settings: settings))
        PhonePackages.set(PhonePackages.aquarium, on: false, settings: settings)
        XCTAssertFalse(settings.aquariumOn)
        let aquarium = MarketPackage(id: "aquarium", name: "Aquarium", desktop: true, bundled: true)
        XCTAssertEqual(aquarium.phoneSurface, .native("aquarium"))
        XCTAssertNil(PackageRoutes.route(for: aquarium))
    }

    @MainActor func testAquariumReadsAsNotInstalledUntilTurnedOn() {
        let prefs = InMemoryKeyValueStore()
        let settings = SettingsService.create(prefs: prefs, secureStore: InMemorySecureStore())
        let previous: SettingsService? = Services.get(SettingsService.self)
        Services.put(settings)
        defer {
            if let previous {
                Services.put(previous)
            } else {
                Services.remove(SettingsService.self)
            }
        }
        let model = MarketModel(client: PackagesClient(transport: FakePackagesTransport()))
        let aquarium = MarketPackage(id: "aquarium", name: "Aquarium", desktop: true, bundled: true)
        XCTAssertFalse(model.isInstalled(aquarium))
        settings.setAquariumOn(true)
        XCTAssertTrue(model.isInstalled(aquarium))
    }

    @MainActor func testUninstallFromThePhoneHidesAnOrganizationPackageUntilReinstalled() {
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
        let crm = MarketPackage(json: packageJSON("crm", scope: "organization").objectValue!)!
        let sleep = MarketPackage(id: "sleep", name: "Sleep", bundled: true, accountSync: true)
        let aquarium = MarketPackage(id: "aquarium", name: "Aquarium", desktop: true, bundled: true)
        XCTAssertTrue(PhonePackages.canRemove(crm))
        XCTAssertFalse(PhonePackages.canRemove(sleep))
        XCTAssertFalse(PhonePackages.canRemove(aquarium))
        XCTAssertTrue(PhonePackages.installed(account: nil, settings: settings, organization: ["crm"]).contains("crm"))

        PhonePackages.setRemoved("crm", removed: true, settings: settings)
        XCTAssertTrue(PhonePackages.isRemoved("crm", settings: settings))
        XCTAssertFalse(PhonePackages.installed(account: nil, settings: settings, organization: ["crm"]).contains("crm"))
        XCTAssertFalse(PhonePackages.isInstalled("crm", account: nil, settings: settings))
        XCTAssertFalse(PackageGate.shows(.crm(pipeline: nil), installed: PhonePackages.installed(
            account: nil, settings: settings, organization: ["crm"])))
        let reloaded = SettingsService.create(prefs: settings.prefs, secureStore: InMemorySecureStore())
        XCTAssertEqual(reloaded.removedPackages, ["crm"])

        PhonePackages.setRemoved("crm", removed: false, settings: settings)
        XCTAssertTrue(PhonePackages.installed(account: nil, settings: settings, organization: ["crm"]).contains("crm"))
    }

    @MainActor func testTourOpensOnThePhone() {
        let tour = MarketPackage(id: "tour", name: "Tour", desktop: true, bundled: true, accountSync: true)
        XCTAssertEqual(tour.phoneSurface, .native("tour"))
        XCTAssertEqual(PackageRoutes.route(for: tour), .tour)
    }

    @MainActor func testMarketModelReportsServerErrors() async {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/market/packages", status: 503, ["error": ""])
        let model = MarketModel(client: PackagesClient(transport: transport))
        await model.refresh(organizations: [])
        XCTAssertEqual(model.errorText, "The server answered HTTP 503.")
    }

    @MainActor func testInstallRefusedForMember() async {
        let transport = FakePackagesTransport()
        transport.on("POST", "/api/plugins/acme/crm/install", status: 403, ["error": "organization_admin_required"])
        let model = MarketModel(client: PackagesClient(transport: transport))
        let ok = await model.install(MarketPackage(id: "crm", name: "CRM", dataScope: .organization), organization: "acme")
        XCTAssertFalse(ok)
        XCTAssertEqual(model.errorText, "Only an owner or admin of the organization can do that.")
    }

    @MainActor func testUninstallRemovesThePackageAndKeepsTheExport() async {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/market/packages", ["packages": [packageJSON("crm", scope: "organization")]])
        transport.on("GET", "/api/plugins/acme", [
            "organization": "acme", "role": "admin", "canManage": true, "plugins": [["id": "crm", "installed": true]],
        ])
        transport.on("POST", "/api/plugins/acme/crm/uninstall", [
            "ok": true, "uninstalled": true, "plugin": "crm", "organization": "acme", "hadData": true,
            "filename": "../../evil.json", "export": ["contacts": [["name": "Ada"]]],
        ])
        let model = MarketModel(client: PackagesClient(transport: transport))
        await model.refresh(organizations: ["acme"])
        let crm = model.package("crm")!
        XCTAssertTrue(model.isInstalled(crm))
        let ok = await model.uninstall(crm, organization: "acme")
        XCTAssertTrue(ok)
        XCTAssertFalse(model.isInstalled(crm))
        XCTAssertEqual(model.removed?.filename, "forkmesh-crm-acme.json")
        let export = JSON.parseObject(model.removed?.export ?? Data())
        XCTAssertEqual(export.maps("contacts").first?.str("name"), "Ada")
        model.filter = .notInstalled
        XCTAssertEqual(model.visible.map(\.id), ["crm"])
    }

    @MainActor func testUninstallOfAnAlreadyRemovedPackageClearsIt() async {
        let transport = FakePackagesTransport()
        transport.on("POST", "/api/plugins/acme/crm/uninstall", status: 409, ["error": "plugin_not_installed"])
        let model = MarketModel(client: PackagesClient(transport: transport))
        let ok = await model.uninstall(MarketPackage(id: "crm", name: "CRM", dataScope: .organization), organization: "acme")
        XCTAssertTrue(ok)
        XCTAssertNil(model.removed)
        XCTAssertEqual(model.errorText, "")
    }

    @MainActor func testClientRefusesUnsafePathSegments() async {
        let transport = FakePackagesTransport()
        let client = PackagesClient(transport: transport)
        do {
            _ = try await client.organizationPackages("../admin")
            XCTFail("expected a refusal")
        } catch let error as PackagesError {
            XCTAssertEqual(error.code, "organization_not_found")
        } catch {
            XCTFail("\(error)")
        }
        do {
            _ = try await client.crmContact(CrmContactRef(organization: "acme", contactId: "../../x"))
            XCTFail("expected a refusal")
        } catch {}
        XCTAssertTrue(transport.sent.isEmpty)
    }

    @MainActor func testApiTransportSendsBearerAndOriginOnWrites() async throws {
        let http = FakeHTTPClient()
        http.enqueue(status: 200, json: ["ok": true, "organization": "acme", "plugins": []])
        let api = ApiService(
            settings: StaticApiSettings(serverUrl: "wss://app.example.test/api/relay"), http: http, sleep: { _ in })
        api.sessionTokenProvider = { "tok" }
        let client = PackagesClient(transport: ApiPackagesTransport(api: api))
        http.enqueue(status: 200, json: ["ok": true])
        _ = try await client.install("crm", organization: "acme")
        let request = try XCTUnwrap(http.requests.first)
        XCTAssertEqual(request.method, "POST")
        XCTAssertEqual(request.url.path, "/api/plugins/acme/crm/install")
        XCTAssertEqual(request.headers["Authorization"], "Bearer tok")
        XCTAssertEqual(request.headers["Origin"], "https://app.example.test")
    }

    @MainActor func testApiTransportLeavesOriginOffReads() async throws {
        let http = FakeHTTPClient()
        http.enqueue(status: 200, json: ["packages": []])
        let api = ApiService(settings: StaticApiSettings(serverUrl: "wss://app.example.test/api/relay"), http: http, sleep: { _ in })
        _ = try await PackagesClient(transport: ApiPackagesTransport(api: api)).packages()
        XCTAssertNil(http.requests.first?.headers["Origin"])
    }
}
