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
        XCTAssertEqual(PackageRoutes.route(for: MarketPackage(id: "people", name: "People")), .people)
        XCTAssertEqual(PackageRoutes.route(for: MarketPackage(id: "docs", name: "Docs")), .docs)
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
