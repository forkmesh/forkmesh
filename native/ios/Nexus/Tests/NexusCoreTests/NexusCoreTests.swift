import XCTest
@testable import NexusCore

final class NexusCoreTests: XCTestCase {
    func testIPv6AndNetworkFormatting() throws {
        XCTAssertEqual(IPv6Addr("2001:db8::1")?.description, "2001:db8::1")
        XCTAssertTrue(IPv4Network("192.168.1.12/24")!.contains(IPv4Addr("192.168.1.99")!))
    }

    func testTargetAndPortParsing() throws {
        XCTAssertEqual(try TargetSpecification.parse("192.168.1.2-4").hosts().count, 3)
        XCTAssertEqual(try PortList(parsing: "22,80-82").description, "22,80-82")
    }

    func testPublicTargetsRequireAttestation() {
        let decision = ScanAuthorizationPolicy(allowPublicTargets: true).evaluate(addresses: [.v4(IPv4Addr("8.8.8.8")!)], portCount: 1, isRange: false, attestation: nil)
        XCTAssertEqual(decision, .requiresAttestation("Confirm you own or are authorized to test the internet hosts in this scan."))
    }
}
