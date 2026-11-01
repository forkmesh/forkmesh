import Foundation
import XCTest

@testable import FMApi

final class SurveyRouteTests: XCTestCase {
    private let surveyId = "0123456789abcdef0123456789abcdef"

    private func web(_ path: String) -> [Route]? {
        RouteParser.parse(URL(string: "https://app.forkmesh.com\(path)")!)?.stack
    }

    func testDashboardLinksOpenSurveys() {
        XCTAssertEqual(web("/dashboard/surveys"), [.surveys(organization: nil, survey: nil)])
        XCTAssertEqual(web("/dashboard/surveys?org=acme"), [.surveys(organization: "acme", survey: nil)])
        XCTAssertEqual(web("/dashboard/surveys?survey=\(surveyId)"), [.surveys(organization: nil, survey: surveyId)])
        XCTAssertEqual(
            web("/dashboard/surveys?org=Acme&survey=\(surveyId)"), [.surveys(organization: "acme", survey: surveyId)])
        XCTAssertEqual(web("/dashboard/surveys?survey=nothex"), [.surveys(organization: nil, survey: nil)])
        XCTAssertEqual(
            web("/dashboard/surveys?survey=\(surveyId.uppercased())"), [.surveys(organization: nil, survey: nil)])
        XCTAssertEqual(web("/dashboard/surveys?org=../x"), [.surveys(organization: nil, survey: nil)])
        XCTAssertNil(web("/dashboard/surveys/extra"))
    }

    func testSurveysIsABusinessPackageRoute() throws {
        let route = Route.surveys(organization: "acme", survey: surveyId)
        XCTAssertEqual(route.kind, .surveys)
        XCTAssertEqual(route.kind.owner, "WS19")
        XCTAssertEqual(route.title, "Surveys")
        XCTAssertEqual(RouteCatalog.symbol(.surveys), "list.clipboard")
        XCTAssertNil(route.homeTab)
        XCTAssertFalse(route.isModal)
        XCTAssertEqual(PackageGate.packageId(for: route), "surveys")
        XCTAssertEqual(PackageGate.packageId(forItem: "surveys"), "surveys")
        XCTAssertTrue(PackageGate.organizationPackages.contains("surveys"))
        XCTAssertFalse(StarterPack.everything.packages.contains("surveys"))
        XCTAssertFalse(PackageGate.shows(route, installed: ["invoices"]))
        XCTAssertTrue(PackageGate.shows(route, installed: ["surveys"]))
        XCTAssertTrue(ShellPanes.listKinds.contains(.surveys))
        let data = try JSONEncoder().encode([route, Route.surveys(organization: nil, survey: nil)])
        XCTAssertEqual(try JSONDecoder().decode([Route].self, from: data), [route, .surveys(organization: nil, survey: nil)])
    }

    func testHubListsSurveysOnlyWhenRegistered() {
        let hidden = HubCatalog.groups(guest: false) { _ in false }.flatMap(\.items)
        XCTAssertFalse(hidden.contains { $0.route?.kind == .surveys })
        let business = HubCatalog.groups(guest: false) { $0 == .surveys }.first { $0.id == "business" }
        XCTAssertEqual(business?.items.map(\.id), ["market", "surveys"])
        XCTAssertEqual(business?.items.last?.title, "Surveys")
        let both = HubCatalog.groups(guest: false) { $0 == .surveys || $0 == .invoices }.first { $0.id == "business" }
        XCTAssertEqual(both?.items.map(\.id), ["market", "invoices", "surveys"])
    }
}
