import Foundation
import XCTest

@testable import FMApi

final class PokerRouteTests: XCTestCase {
    private let tableId = "0123456789abcdef0123456789abcdef"

    private func web(_ path: String) -> [Route]? {
        RouteParser.parse(URL(string: "https://app.forkmesh.com\(path)")!)?.stack
    }

    func testDashboardLinksOpenPoker() {
        XCTAssertEqual(web("/dashboard/poker"), [.poker(table: nil)])
        XCTAssertEqual(web("/dashboard/poker?table=\(tableId)"), [.poker(table: tableId)])
        XCTAssertEqual(web("/dashboard/poker?table=NOTHEX"), [.poker(table: nil)])
        XCTAssertEqual(web("/dashboard/poker?table=0123456789ABCDEF0123456789ABCDEF"), [.poker(table: nil)])
        XCTAssertNil(web("/dashboard/poker/extra"))
        XCTAssertEqual(RouteParser.parse(URL(string: "forkmesh://poker?table=\(tableId)")!)?.stack, [.poker(table: tableId)])
        XCTAssertEqual(RouteParser.parse(URL(string: "forkmesh://dashboard/poker")!)?.stack, [.poker(table: nil)])
    }

    func testPokerIsAnAccountPackageRoute() throws {
        let route = Route.poker(table: tableId)
        XCTAssertEqual(route.kind, .poker)
        XCTAssertEqual(route.kind.owner, "existing")
        XCTAssertEqual(route.title, "Poker")
        XCTAssertEqual(RouteCatalog.symbol(.poker), "suit.spade.fill")
        XCTAssertNil(route.homeTab)
        XCTAssertFalse(route.isModal)
        XCTAssertEqual(PackageGate.packageId(for: route), "poker")
        XCTAssertEqual(PackageGate.packageId(forItem: "poker"), "poker")
        XCTAssertFalse(PackageGate.organizationPackages.contains("poker"))
        XCTAssertFalse(PackageGate.shows(route, installed: ["water"]))
        XCTAssertTrue(PackageGate.shows(route, installed: ["poker"]))
        XCTAssertTrue(HubCatalog.personal.contains { $0.route == .poker(table: nil) })
        let data = try JSONEncoder().encode([route, Route.poker(table: nil)])
        XCTAssertEqual(try JSONDecoder().decode([Route].self, from: data), [route, .poker(table: nil)])
    }
}
