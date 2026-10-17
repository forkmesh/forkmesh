import XCTest
@testable import NexusRemote

final class NexusRemoteTests: XCTestCase {
    func testConfigurationCarriesApprovedProfiles() { XCTAssertEqual(RemoteScannerConfiguration(baseURL: URL(string: "https://scanner.example")!, token: "x", agentName: "lab", allowedProfiles: ["safe"]).allowedProfiles, ["safe"]) }
}
