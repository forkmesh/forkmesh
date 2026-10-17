import XCTest
@testable import NexusDNS
@testable import NexusCore

final class NexusDNSTests: XCTestCase {
    func testQueryHasQuestion() throws {
        let query = try DNSWire.query(identifier: 12, name: "example.com", type: .a)
        XCTAssertGreaterThan(query.count, 20)
    }
}
