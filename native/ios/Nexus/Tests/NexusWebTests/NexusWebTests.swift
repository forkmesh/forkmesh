import XCTest
@testable import NexusWeb
@testable import NexusCore

final class NexusWebTests: XCTestCase {
    func testSecurityHeadersObservation() {
        let results = SecurityInsights.http(url: URL(string: "https://example.com")!, status: 200, headers: [])
        XCTAssertFalse(results.isEmpty)
    }
}
