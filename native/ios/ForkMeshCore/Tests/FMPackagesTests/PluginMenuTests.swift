import FMApi
import XCTest

@testable import FMPackages

final class PluginMenuTests: XCTestCase {
    func testAPluginHomeScreenNamesItsPackage() {
        XCTAssertEqual(PluginMenu.packageId(for: .crm(pipeline: nil)), "crm")
        XCTAssertEqual(PluginMenu.packageId(for: .crmOrg(id: "acme")), "crm")
        XCTAssertEqual(PluginMenu.packageId(for: .rssReader), "rss-reader")
        XCTAssertEqual(PluginMenu.packageId(for: .faceChanger), "face-changer")
        XCTAssertEqual(PluginMenu.packageId(for: .appStoreStatus), "app-store-status")
        XCTAssertEqual(PluginMenu.packageId(for: .marketing(section: "email")), "marketing")
        XCTAssertEqual(PluginMenu.packageId(for: .ledger(section: nil)), "ledger")
        XCTAssertEqual(PluginMenu.packageId(for: .calendar(date: nil, mode: .month)), "calendar")
        XCTAssertEqual(PluginMenu.packageId(for: .status), "status-page")
        XCTAssertEqual(PluginMenu.packageId(for: .localAI), "on-device-ai")
        XCTAssertEqual(PluginMenu.packageId(for: .logs), "logs")
        XCTAssertEqual(PluginMenu.packageId(for: .diagnostics), "diagnostics")
    }

    func testEveryHomeKindBelongsToAMarketplacePackage() {
        for kind in PluginMenu.homeKinds {
            let package = PackageGate.kinds[kind]
            XCTAssertNotNil(package, "\(kind)")
            XCTAssertTrue(PackageGate.isGated(package ?? ""), "\(kind)")
        }
    }

    func testEveryPhoneScreenAPackageOpensGetsTheMenuExceptTheTabsAndTheTour() {
        let tabs: Set<Route.Kind> = [.agents, .inbox, .chat, .tour]
        for id in MarketPackage.nativeDestinations.keys {
            guard let route = PackageRoutes.route(for: MarketPackage(id: id, name: id)) else { continue }
            if tabs.contains(route.kind) {
                XCTAssertNil(PluginMenu.packageId(for: route), id)
            } else {
                XCTAssertNotNil(PluginMenu.packageId(for: route), id)
            }
        }
    }

    func testDetailScreensTabsAndTheMarketGetNoPluginMenu() {
        XCTAssertNil(PluginMenu.packageId(for: .contact(id: "1")))
        XCTAssertNil(PluginMenu.packageId(for: .note(id: "1")))
        XCTAssertNil(PluginMenu.packageId(for: .cove(id: "x")))
        XCTAssertNil(PluginMenu.packageId(for: .chat))
        XCTAssertNil(PluginMenu.packageId(for: .inbox(.all)))
        XCTAssertNil(PluginMenu.packageId(for: .allAgents))
        XCTAssertNil(PluginMenu.packageId(for: .market(filter: nil)))
        XCTAssertNil(PluginMenu.packageId(for: .package(id: "crm")))
        XCTAssertNil(PluginMenu.packageId(for: .settings(section: nil)))
        XCTAssertNil(PluginMenu.packageId(for: .tour))
        XCTAssertFalse(PluginMenu.addsButton(for: .package(id: "crm")))
    }

    func testScreensWithTheirOwnMoreMenuGetNoSecondButton() {
        let repo = RepoRef(owner: "jett", name: "site")
        XCTAssertTrue(PluginMenu.screenOwnsMenu(.crm(pipeline: nil)))
        XCTAssertTrue(PluginMenu.screenOwnsMenu(.crmOrg(id: "acme")))
        XCTAssertTrue(PluginMenu.screenOwnsMenu(.teammates))
        XCTAssertTrue(PluginMenu.screenOwnsMenu(.coves(repo)))
        XCTAssertFalse(PluginMenu.addsButton(for: .crm(pipeline: nil)))
        XCTAssertFalse(PluginMenu.addsButton(for: .teammates))
        XCTAssertFalse(PluginMenu.addsButton(for: .coves(repo)))
        XCTAssertTrue(PluginMenu.addsButton(for: .coves(nil)))
        XCTAssertTrue(PluginMenu.addsButton(for: .rssReader))
        XCTAssertTrue(PluginMenu.addsButton(for: .calendar(date: nil, mode: .month)))
        let owners: [Route] = [.crm(pipeline: nil), .crmOrg(id: "acme"), .teammates, .coves(repo)]
        for route in owners {
            XCTAssertNotNil(PluginMenu.packageId(for: route), "\(route)")
        }
    }

    func testTheMenuOffersRateAndFeedbackAndUninstallOnlyWhenPossible() {
        XCTAssertEqual(PluginMenu.items(canUninstall: true), [.rate, .feedback, .uninstall])
        XCTAssertEqual(PluginMenu.items(canUninstall: false), [.rate, .feedback])
        XCTAssertEqual(PluginMenu.buttonIdentifier, "plugin-more")
        XCTAssertEqual(PluginMenu.feedbackLabel, "Send feedback or report a bug")
    }

    func testLabelsUseTheCatalogNameAndFallBackToTheRouteTitle() {
        XCTAssertEqual(PluginMenu.name(catalog: "RSS Reader", route: .rssReader), "RSS Reader")
        XCTAssertEqual(PluginMenu.name(catalog: "  ", route: .crm(pipeline: nil)), "CRM")
        XCTAssertEqual(PluginMenu.name(catalog: nil, route: .crmOrg(id: "acme")), "CRM")
        XCTAssertEqual(PluginMenu.rateLabel("CRM", rated: false), "Rate CRM")
        XCTAssertEqual(PluginMenu.rateLabel("CRM", rated: true), "Edit your rating of CRM")
    }
}
