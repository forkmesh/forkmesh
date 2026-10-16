import FMBase
import Foundation
import XCTest

@testable import FMPackages

@MainActor
final class HeldPackagesTransport: PackagesTransport {
    var inFlight = 0
    var most = 0
    var held: [CheckedContinuation<Void, Never>] = []

    func send(_ method: String, _ path: String, query: [(String, String)], body: JSONObject?) async throws
        -> PackagesReply
    {
        inFlight += 1
        most = max(most, inFlight)
        await withCheckedContinuation { held.append($0) }
        inFlight -= 1
        let organization = String(path.split(separator: "/").last ?? "")
        if organization == "broken" { return PackagesReply(status: 500, json: ["error": "boom"]) }
        return PackagesReply(status: 200, json: ["organization": .string(organization), "plugins": []])
    }

    func release() {
        let gates = held
        held = []
        for gate in gates { gate.resume() }
    }
}

@MainActor
final class OrgPackagesReadTests: XCTestCase {
    func testEveryOrganizationIsAskedAtOnceAndAnswersInOrder() async throws {
        let transport = HeldPackagesTransport()
        let client = PackagesClient(transport: transport)
        let reading = Task { await client.organizationPackages(["acme", "broken", "beta"]) }
        for _ in 0..<200 where transport.held.count < 3 { await Task.yield() }
        XCTAssertEqual(transport.held.count, 3)
        XCTAssertEqual(transport.most, 3)

        transport.release()
        let states = await reading.value
        XCTAssertEqual(states.map { $0?.organization }, ["acme", nil, "beta"])
    }

    func testNoOrganizationsAsksNothing() async {
        let transport = HeldPackagesTransport()
        let states = await PackagesClient(transport: transport).organizationPackages([])
        XCTAssertTrue(states.isEmpty)
        XCTAssertEqual(transport.most, 0)
    }
}
