import XCTest
@testable import NexusConnectivity

final class NexusConnectivityTests: XCTestCase {
    func testTracerouteGuidance() { XCTAssertTrue(ConnectivityToolkit().tracerouteAvailability().contains("iOS")) }
}
