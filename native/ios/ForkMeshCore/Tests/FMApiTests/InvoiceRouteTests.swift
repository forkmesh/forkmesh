import Foundation
import XCTest

@testable import FMApi

final class InvoiceRouteTests: XCTestCase {
    private let invoiceId = "0123456789abcdef0123456789abcdef"

    private func web(_ path: String) -> [Route]? {
        RouteParser.parse(URL(string: "https://app.forkmesh.com\(path)")!)?.stack
    }

    func testDashboardLinksOpenInvoices() {
        XCTAssertEqual(web("/dashboard/invoices"), [.invoices(organization: nil, invoice: nil)])
        XCTAssertEqual(web("/dashboard/invoices?org=acme"), [.invoices(organization: "acme", invoice: nil)])
        XCTAssertEqual(
            web("/dashboard/invoices?invoice=\(invoiceId)"), [.invoices(organization: nil, invoice: invoiceId)])
        XCTAssertEqual(
            web("/dashboard/invoices?org=Acme&invoice=\(invoiceId)"), [.invoices(organization: "acme", invoice: invoiceId)])
        XCTAssertEqual(web("/dashboard/invoices?invoice=nothex"), [.invoices(organization: nil, invoice: nil)])
        XCTAssertEqual(web("/dashboard/invoices?org=../x"), [.invoices(organization: nil, invoice: nil)])
        XCTAssertNil(web("/dashboard/invoices/extra"))
    }

    func testInvoicesIsABusinessPackageRoute() throws {
        let route = Route.invoices(organization: "acme", invoice: invoiceId)
        XCTAssertEqual(route.kind, .invoices)
        XCTAssertEqual(route.kind.owner, "WS19")
        XCTAssertEqual(route.title, "Invoices")
        XCTAssertEqual(RouteCatalog.symbol(.invoices), "receipt")
        XCTAssertNil(route.homeTab)
        XCTAssertFalse(route.isModal)
        XCTAssertEqual(PackageGate.packageId(for: route), "invoices")
        XCTAssertEqual(PackageGate.packageId(forItem: "invoices"), "invoices")
        XCTAssertTrue(PackageGate.organizationPackages.contains("invoices"))
        XCTAssertFalse(StarterPack.everything.packages.contains("invoices"))
        XCTAssertFalse(PackageGate.shows(route, installed: ["crm"]))
        XCTAssertTrue(PackageGate.shows(route, installed: ["invoices"]))
        let data = try JSONEncoder().encode([route, Route.invoices(organization: nil, invoice: nil)])
        XCTAssertEqual(try JSONDecoder().decode([Route].self, from: data), [route, .invoices(organization: nil, invoice: nil)])
    }

    func testHubListsInvoicesOnlyWhenRegistered() {
        let hidden = HubCatalog.groups(guest: false) { _ in false }.flatMap(\.items)
        XCTAssertFalse(hidden.contains { $0.route?.kind == .invoices })
        let business = HubCatalog.groups(guest: false) { $0 == .invoices }.first { $0.id == "business" }
        XCTAssertEqual(business?.items.map(\.id), ["market", "invoices"])
        XCTAssertEqual(business?.items.last?.title, "Invoices")
    }
}
