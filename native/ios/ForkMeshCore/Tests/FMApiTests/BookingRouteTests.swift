import Foundation
import XCTest

@testable import FMApi

final class BookingRouteTests: XCTestCase {
    private let bookingId = "0123456789abcdef0123456789abcdef"

    private func web(_ path: String) -> [Route]? {
        RouteParser.parse(URL(string: "https://app.forkmesh.com\(path)")!)?.stack
    }

    func testDashboardLinksOpenBooking() {
        XCTAssertEqual(web("/dashboard/booking"), [.booking(id: nil)])
        XCTAssertEqual(web("/dashboard/booking?booking=\(bookingId)"), [.booking(id: bookingId)])
        XCTAssertEqual(web("/dashboard/booking?booking=NOTHEX"), [.booking(id: nil)])
        XCTAssertNil(web("/dashboard/booking/extra"))
    }

    func testBookingIsAnAccountPackageRoute() throws {
        let route = Route.booking(id: bookingId)
        XCTAssertEqual(route.kind, .booking)
        XCTAssertEqual(route.title, "Booking")
        XCTAssertEqual(RouteCatalog.symbol(.booking), "calendar.badge.clock")
        XCTAssertNil(route.homeTab)
        XCTAssertFalse(route.isModal)
        XCTAssertEqual(PackageGate.packageId(for: route), "booking")
        XCTAssertEqual(PackageGate.packageId(forItem: "booking"), "booking")
        XCTAssertFalse(PackageGate.organizationPackages.contains("booking"))
        XCTAssertTrue(StarterPack.everything.packages.contains("booking"))
        XCTAssertFalse(PackageGate.shows(route, installed: ["crm"]))
        XCTAssertTrue(PackageGate.shows(route, installed: ["booking"]))
        let data = try JSONEncoder().encode([route, Route.booking(id: nil)])
        XCTAssertEqual(try JSONDecoder().decode([Route].self, from: data), [route, .booking(id: nil)])
    }

    func testHubListsBookingNextToCalendar() {
        let index = HubCatalog.workspace.firstIndex { $0.route == .booking(id: nil) }
        XCTAssertEqual(index, HubCatalog.workspace.firstIndex { $0.route == .calendar(date: nil, mode: .month) }.map { $0 + 1 })
        XCTAssertFalse(HubCatalog.personal.contains { $0.route == .booking(id: nil) })
        let item = index.map { HubCatalog.workspace[$0] }
        XCTAssertEqual(item?.id, "booking")
        XCTAssertEqual(item?.title, "Booking")
    }
}
