import FMBase
import Foundation
import XCTest

@testable import FMPackages

@MainActor
final class CrmPageGateTransport: PackagesTransport {
    let inner = FakePackagesTransport()
    var held: String?
    private var waiting: CheckedContinuation<Void, Never>?

    func send(_ method: String, _ path: String, query: [(String, String)], body: JSONObject?) async throws -> PackagesReply {
        if path == held {
            held = nil
            await withCheckedContinuation { waiting = $0 }
        }
        return try await inner.send(method, path, query: query, body: body)
    }

    var isWaiting: Bool { waiting != nil }

    func release() {
        waiting?.resume()
        waiting = nil
    }
}

@MainActor
final class CrmPagingTests: XCTestCase {
    private func hex(_ index: Int) -> String { String(format: "%032x", index) }

    private func contact(_ index: Int, _ stage: String = "lead") -> JSONValue {
        .object([
            "id": .string(hex(index)), "organization": "acme", "name": .string("Contact \(index)"),
            "flow": "customers", "stage": .string(stage),
        ])
    }

    private func firstPage(_ org: String = "acme", next: String = "c1", total: Int = 5) -> JSONObject {
        [
            "organization": .string(org), "role": "owner",
            "contacts": [contact(1, "customer"), contact(2)],
            "summary": ["contacts": .int(Int64(total)), "sent": 3],
            "contactsNext": .string(next), "contactsTotal": .int(Int64(total)), "revision": "r1",
        ]
    }

    private func page(_ contacts: [JSONValue], next: String) -> JSONObject {
        ["ok": true, "contacts": .array(contacts), "next": .string(next)]
    }

    private func queries(_ transport: FakePackagesTransport, _ path: String) -> [[String: String]] {
        transport.sent.filter { $0.path == path }.map { Dictionary($0.query, uniquingKeysWith: { _, last in last }) }
    }

    func testTheSummaryAsksForTheFirst250Contacts() async throws {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/crm/acme", firstPage(next: ""))
        let model = CrmModel(client: PackagesClient(transport: transport), organization: "acme")
        await model.load()
        XCTAssertEqual(queries(transport, "/api/crm/acme").last?["contacts"], "250")
        _ = try await PackagesClient(transport: transport).crm("acme")
        XCTAssertNil(queries(transport, "/api/crm/acme").last?["contacts"])
    }

    func testParsesTheCursorAndTotalAndCountsCustomers() {
        let data = CrmOrganizationData(json: firstPage())
        XCTAssertEqual(data.contactsNext, "c1")
        XCTAssertEqual(data.contactsTotal, 5)
        XCTAssertEqual(data.counts.contacts, 5)
        XCTAssertEqual(data.counts.customers, 1)
        XCTAssertFalse(data.contactsComplete)
        let legacy = CrmOrganizationData(json: ["contacts": [contact(1)], "summary": ["customers": 7]])
        XCTAssertEqual(legacy.contactsNext, "")
        XCTAssertEqual(legacy.contactsTotal, 1)
        XCTAssertEqual(legacy.counts.customers, 7)
        XCTAssertTrue(legacy.contactsComplete)
    }

    func testPagingFollowsCursorsAndSkipsKnownContacts() async {
        let transport = CrmPageGateTransport()
        transport.inner.on("GET", "/api/crm/acme", firstPage())
        transport.inner.on("GET", "/api/crm/acme/contacts", page([contact(2), contact(3, "customer")], next: "c2"))
        transport.inner.on("GET", "/api/crm/acme/contacts", page([contact(4), contact(5, "customer")], next: ""))
        transport.held = "/api/crm/acme/contacts"
        let model = CrmModel(client: PackagesClient(transport: transport), organization: "acme")
        await model.load()
        XCTAssertEqual(model.data?.contacts.count, 2)
        XCTAssertTrue(model.contactsLoading)
        XCTAssertEqual(model.contactsStatus, "Loading contacts, 2 of 5")
        while !transport.isWaiting { await Task.yield() }
        transport.release()
        await model.contactsTask?.value
        XCTAssertEqual(model.data?.contacts.map(\.id), (1...5).map(hex))
        XCTAssertEqual(model.data?.counts.customers, 3)
        XCTAssertFalse(model.contactsLoading)
        XCTAssertEqual(model.contactsStatus, "")
        XCTAssertEqual(
            queries(transport.inner, "/api/crm/acme/contacts"),
            [["limit": "1000", "cursor": "c1"], ["limit": "1000", "cursor": "c2"]])
        transport.inner.routes["GET /api/crm/acme"] = [PackagesReply(status: 200, json: ["unchanged": true, "revision": "r1"])]
        await model.load()
        XCTAssertEqual(queries(transport.inner, "/api/crm/acme").last?["since"], "r1")
        XCTAssertEqual(model.data?.contacts.count, 5)
    }

    func testANewerLoadDropsAnOlderBackgroundLoad() async {
        let transport = CrmPageGateTransport()
        transport.inner.on("GET", "/api/crm/acme", firstPage())
        transport.inner.on("GET", "/api/crm/acme/contacts", page([contact(3)], next: ""))
        transport.inner.on("GET", "/api/crm/beta", firstPage("beta", next: "", total: 2))
        transport.held = "/api/crm/acme/contacts"
        let model = CrmModel(client: PackagesClient(transport: transport), organization: "acme")
        await model.load()
        let stale = model.contactsTask
        while !transport.isWaiting { await Task.yield() }
        model.organization = "beta"
        await model.load()
        transport.release()
        await stale?.value
        XCTAssertEqual(model.data?.organization, "beta")
        XCTAssertEqual(model.data?.contacts.count, 2)
        XCTAssertFalse(model.contactsLoading)
        XCTAssertEqual(model.errorText, "")
    }

    func testAFailedPageClearsTheRevisionSoTheNextLoadReadsEverything() async {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/crm/acme", firstPage())
        transport.on("GET", "/api/crm/acme/contacts", status: 500, ["error": "server_error"])
        let model = CrmModel(client: PackagesClient(transport: transport), organization: "acme")
        await model.load()
        await model.contactsTask?.value
        XCTAssertFalse(model.contactsLoading)
        XCTAssertEqual(model.data?.revision, "")
        XCTAssertTrue(model.errorText.hasPrefix("Some contacts did not load"))
        XCTAssertEqual(model.data?.contacts.count, 2)
        await model.load()
        XCTAssertNil(queries(transport, "/api/crm/acme").last?["since"])
    }
}
