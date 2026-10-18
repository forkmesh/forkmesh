import FMApi
import XCTest

@testable import FMPackages

final class PackageRoutesTests: XCTestCase {
    func testNativePackagesMapToRoutes() {
        XCTAssertEqual(PackageRoutes.route(for: MarketPackage(id: "crm", name: "CRM")), .crm(pipeline: nil))
        XCTAssertEqual(PackageRoutes.route(for: MarketPackage(id: "competitor", name: "C")), .competitors)
        XCTAssertEqual(PackageRoutes.route(for: MarketPackage(id: "vault", name: "V")), .vault(section: nil))
        XCTAssertEqual(PackageRoutes.route(for: MarketPackage(id: "steps", name: "Pedometer")), .steps)
        XCTAssertEqual(PackageRoutes.route(for: MarketPackage(id: "sleep", name: "Sleep")), .sleep)
        XCTAssertEqual(PackageRoutes.route(for: MarketPackage(id: "sugar-break", name: "Sugar Break")), .sugarBreak)
        XCTAssertEqual(PackageRoutes.route(for: MarketPackage(id: "stretch", name: "Stretch Break")), .stretch)
        XCTAssertEqual(PackageRoutes.route(for: MarketPackage(id: "food-guide", name: "Food Guide")), .foodGuide)
        XCTAssertEqual(PackageRoutes.route(for: MarketPackage(id: "water", name: "Water Intake")), .water)
        XCTAssertEqual(PackageRoutes.route(for: MarketPackage(id: "speech", name: "Speech Playground")), .speech)
        XCTAssertEqual(PackageRoutes.route(for: MarketPackage(id: "glasses-alert", name: "Glasses Alert")), .glassesAlert)
        XCTAssertEqual(PackageRoutes.route(for: MarketPackage(id: "face-changer", name: "Face Changer")), .faceChanger)
        XCTAssertEqual(PackageRoutes.route(for: MarketPackage(id: "video-mix-dj-studio", name: "Video Mix DJ Studio")), .videoMixDjStudio)
        let appStore = MarketPackage(id: "app-store-status", name: "App Store Status", webRoute: "/dashboard/app-store-status")
        XCTAssertEqual(appStore.phoneSurface, .native("appStoreStatus"))
        XCTAssertEqual(PackageRoutes.route(for: appStore), .appStoreStatus)
        XCTAssertEqual(PackageRoutes.packageId(forHubItem: "appStoreStatus"), "app-store-status")
        XCTAssertEqual(PackageGate.packageId(for: Route.appStoreStatus), "app-store-status")
        XCTAssertEqual(PackageGate.packageId(forItem: "appStoreStatus"), "app-store-status")
        XCTAssertFalse(PackageGate.shows(Route.appStoreStatus, installed: ["water"]))
        XCTAssertTrue(PackageGate.shows(Route.appStoreStatus, installed: ["app-store-status"]))
        let reader = MarketPackage(id: "rss-reader", name: "RSS Reader")
        XCTAssertEqual(reader.phoneSurface, .native("rssReader"))
        XCTAssertEqual(PackageRoutes.route(for: reader), .rssReader)
        XCTAssertEqual(PackageRoutes.packageId(forHubItem: "rssReader"), "rss-reader")
        XCTAssertEqual(PackageGate.packageId(for: Route.rssReader), "rss-reader")
        XCTAssertEqual(PackageGate.packageId(forItem: "rssReader"), "rss-reader")
        XCTAssertFalse(PackageGate.shows(Route.rssReader, installed: ["water"]))
        XCTAssertTrue(PackageGate.shows(Route.rssReader, installed: ["rss-reader"]))
        XCTAssertTrue(HubCatalog.personal.contains { $0.route == .rssReader })
        XCTAssertEqual(RouteCatalog.title(.rssReader), "RSS Reader")
        let secure = MarketPackage(id: "secure-drive", name: "Secure Drive")
        XCTAssertEqual(secure.phoneSurface, .native("secureDrive"))
        XCTAssertEqual(PackageRoutes.route(for: secure), .secureDrive)
        XCTAssertEqual(PackageRoutes.packageId(forHubItem: "secureDrive"), "secure-drive")
        XCTAssertEqual(PackageGate.packageId(for: Route.secureDrive), "secure-drive")
        XCTAssertEqual(PackageGate.packageId(forItem: "secureDrive"), "secure-drive")
        XCTAssertFalse(PackageGate.shows(Route.secureDrive, installed: ["water"]))
        XCTAssertTrue(PackageGate.shows(Route.secureDrive, installed: ["secure-drive"]))
        XCTAssertTrue(HubCatalog.personal.contains { $0.route == .secureDrive })
        XCTAssertEqual(RouteCatalog.title(.secureDrive), "Secure Drive")
        XCTAssertEqual(PackageRoutes.route(for: MarketPackage(id: "phone-security", name: "Phone Security")), .phoneSecurity)
        XCTAssertEqual(PackageRoutes.packageId(forHubItem: "phoneSecurity"), "phone-security")
        XCTAssertEqual(PackageGate.packageId(for: .phoneSecurity), "phone-security")
        XCTAssertFalse(PackageGate.shows(Route.phoneSecurity, installed: ["glasses-alert"]))
        XCTAssertTrue(PackageGate.shows(Route.phoneSecurity, installed: ["phone-security"]))
        XCTAssertTrue(HubCatalog.personal.contains { $0.route == .phoneSecurity })
        XCTAssertEqual(RouteCatalog.title(.phoneSecurity), "Phone Security")
        XCTAssertEqual(PackageRoutes.route(for: MarketPackage(id: "eye-control", name: "Eye Control")), .eyeControl)
        XCTAssertEqual(PackageRoutes.packageId(forHubItem: "eyeControl"), "eye-control")
        XCTAssertEqual(PackageGate.packageId(for: .eyeControl), "eye-control")
        XCTAssertFalse(PackageGate.shows(Route.eyeControl, installed: ["phone-security"]))
        XCTAssertTrue(PackageGate.shows(Route.eyeControl, installed: ["eye-control"]))
        XCTAssertTrue(HubCatalog.personal.contains { $0.route == .eyeControl })
        XCTAssertEqual(RouteCatalog.title(.eyeControl), "Eye Control")
        XCTAssertEqual(PackageRoutes.route(for: MarketPackage(id: "openwrt", name: "OpenWrt Router")), .openWrtRouter)
        XCTAssertEqual(PackageRoutes.packageId(forHubItem: "openWrtRouter"), "openwrt")
        XCTAssertEqual(PackageGate.packageId(for: .openWrtRouter), "openwrt")
        XCTAssertFalse(PackageGate.shows(Route.openWrtRouter, installed: ["phone-security"]))
        XCTAssertTrue(PackageGate.shows(Route.openWrtRouter, installed: ["openwrt"]))
        XCTAssertTrue(HubCatalog.personal.contains { $0.route == .openWrtRouter })
        XCTAssertEqual(RouteCatalog.title(.openWrtRouter), "Router")
        XCTAssertEqual(PackageRoutes.route(for: MarketPackage(id: "people", name: "People")), .people)
        XCTAssertEqual(PackageRoutes.route(for: MarketPackage(id: "docs", name: "Docs")), .docs)
        XCTAssertEqual(PackageRoutes.route(for: MarketPackage(id: "logs", name: "Logs")), .logs)
        XCTAssertEqual(PackageRoutes.route(for: MarketPackage(id: "diagnostics", name: "Diagnostics")), .diagnostics)
        XCTAssertEqual(PackageRoutes.packageId(forHubItem: "logs"), "logs")
        XCTAssertEqual(PackageRoutes.packageId(forHubItem: "diagnostics"), "diagnostics")
        XCTAssertEqual(PackageGate.packageId(for: .logs), "logs")
        XCTAssertEqual(PackageGate.packageId(for: .diagnostics), "diagnostics")
        XCTAssertNil(PackageRoutes.route(for: MarketPackage(id: "voice-changer", name: "V", desktop: true)))
        XCTAssertNil(PackageRoutes.route(for: MarketPackage(id: "discord", name: "D", webRoute: "/dashboard/discord")))
        XCTAssertEqual(PackageRoutes.route(for: MarketPackage(id: "status-page", name: "S", accountSync: true)), .status)
        let check = MarketPackage(id: "status-check-api", name: "Status: API", accountSync: true)
        XCTAssertNil(PackageRoutes.route(for: check))
        XCTAssertEqual(check.phoneSurface, .everyDevice)
    }

    func testHubShowsOnlyInstalledOrgPackages() {
        let ids = PackageRoutes.hubRoutes(installed: ["crm", "marketing"]).map(\.id)
        XCTAssertEqual(ids, ["market", "crm", "marketing", "ledger"])
    }

    func testHubShowsCrmOnlyWhenAnOrganizationInstalledIt() {
        XCTAssertEqual(PackageRoutes.hubRoutes(installed: []).map(\.id), ["market", "ledger"])
        XCTAssertEqual(PackageRoutes.hubRoutes(installed: ["crm"]).map(\.id), ["market", "crm", "ledger"])
    }

    func testHubShowsAdReviewOnlyToReviewers() {
        XCTAssertEqual(PackageRoutes.hubRoutes(installed: ["crm"], adReviewer: true).map(\.id), ["market", "crm", "ads", "ledger"])
        XCTAssertEqual(PackageRoutes.hubRoutes(installed: ["crm"], adReviewer: true).map(\.title), ["Market", "CRM", "Ad review", "Ledger"])
    }

    func testBarItemsNameTheirPackage() {
        XCTAssertEqual(PackageRoutes.packageId(forHubItem: "crm"), "crm")
        XCTAssertEqual(PackageRoutes.packageId(forHubItem: "competitors"), "competitor")
        XCTAssertEqual(PackageRoutes.packageId(forHubItem: "competitor"), "competitor")
        XCTAssertEqual(PackageRoutes.packageId(forHubItem: "notes"), "notes")
        XCTAssertEqual(PackageRoutes.packageId(forHubItem: "tour"), "tour")
        XCTAssertEqual(PackageRoutes.packageId(forHubItem: "status"), "status-page")
        XCTAssertEqual(PackageRoutes.packageId(forHubItem: "sugarBreak"), "sugar-break")
        XCTAssertEqual(PackageRoutes.packageId(forHubItem: "stretch"), "stretch")
        XCTAssertEqual(PackageRoutes.packageId(forHubItem: "foodGuide"), "food-guide")
        XCTAssertEqual(PackageRoutes.packageId(forHubItem: "Agents"), "agents")
        XCTAssertEqual(PackageRoutes.packageId(forHubItem: "repo-pulls"), "pull-requests")
        XCTAssertNil(PackageRoutes.packageId(forHubItem: "Repos"))
        XCTAssertNil(PackageRoutes.packageId(forHubItem: "settings"))
    }

    func testPipelineCarriesOrganization() {
        XCTAssertEqual(PackageRoutes.organization(fromPipeline: "acme/investors").organization, "acme")
        XCTAssertEqual(PackageRoutes.organization(fromPipeline: "acme/investors").flow, "investors")
        XCTAssertNil(PackageRoutes.organization(fromPipeline: "investors").organization)
        XCTAssertEqual(PackageRoutes.organization(fromPipeline: "investors").flow, "investors")
        XCTAssertNil(PackageRoutes.organization(fromPipeline: nil).flow)
    }

    func testSections() {
        XCTAssertEqual(LedgerSection(route: "REWARDS"), .rewards)
        XCTAssertEqual(LedgerSection(route: "bogus"), .records)
        XCTAssertEqual(LedgerSection(route: "tips"), .records)
        XCTAssertEqual(LedgerSection(route: "treasury"), .records)
        XCTAssertEqual(LedgerSection.allCases, [.records, .rewards, .mine])
        XCTAssertEqual(MarketingSection(route: nil), .overview)
        XCTAssertEqual(MarketingSection(route: "plan"), .plan)
        XCTAssertEqual(MarketingSection(route: "content"), .plan)
        XCTAssertEqual(MarketingSection(route: "Ads-Manager"), .ads)
        XCTAssertEqual(MarketingSection(route: "reddit-desk"), .reddit)
        XCTAssertEqual(MarketingSection(route: "bogus"), .overview)
        XCTAssertEqual(MarketFilter(route: "not-installed"), .notInstalled)
    }
}
