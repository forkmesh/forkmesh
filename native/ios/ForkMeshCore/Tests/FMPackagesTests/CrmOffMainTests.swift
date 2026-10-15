import FMBase
import Foundation
import XCTest

@testable import FMPackages

@MainActor
final class CrmOffMainTests: XCTestCase {
    private func contacts(_ count: Int) -> JSONValue {
        .array((0..<count).map { (index: Int) -> JSONValue in
            .object([
                "id": .string(String(format: "%032x", index)), "organization": "acme",
                "name": .string("Contact \(index)"), "email": .string("c\(index)@x.test"),
            ])
        })
    }

    func testALargeReplyParsesTheSameAsOnTheMainThread() async {
        let body: Data = JSON.encodeData(.object(["organization": "acme", "contacts": contacts(400)]))
        XCTAssertGreaterThanOrEqual(body.count, ApiPackagesTransport.detachedParseBytes)
        let parsed: JSONObject = await ApiPackagesTransport.parse(body)
        XCTAssertEqual(parsed, JSON.parseObject(body))
        XCTAssertEqual(parsed.maps("contacts").count, 400)
    }

    func testASmallReplyAndAnEmptyBodyParse() async {
        let body: Data = JSON.encodeData(.object(["ok": true]))
        XCTAssertLessThan(body.count, ApiPackagesTransport.detachedParseBytes)
        let small: JSONObject = await ApiPackagesTransport.parse(body)
        XCTAssertEqual(small, ["ok": true])
        let empty: JSONObject = await ApiPackagesTransport.parse(Data())
        XCTAssertEqual(empty, JSONObject())
    }

    func testTheOrganizationIsBuiltOffTheMainThreadWithTheSameResult() async throws {
        let transport = FakePackagesTransport()
        let reply: JSONObject = ["organization": "acme", "role": "owner", "contacts": contacts(50)]
        transport.on("GET", "/api/crm/acme", reply)
        let data: CrmOrganizationData? = try await PackagesClient(transport: transport).crm("acme")
        XCTAssertEqual(data, CrmOrganizationData(json: reply))
        XCTAssertEqual(data?.contacts.count, 50)
    }

    func testAnUnchangedReplyStillReturnsNil() async throws {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/crm/acme", ["unchanged": true])
        let data: CrmOrganizationData? = try await PackagesClient(transport: transport).crm("acme", since: "r1")
        XCTAssertNil(data)
    }
}
