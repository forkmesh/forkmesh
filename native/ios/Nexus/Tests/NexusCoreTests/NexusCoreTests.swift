import XCTest
@testable import NexusCore

final class NexusCoreTests: XCTestCase {
    func testIPv6AndNetworkFormatting() throws {
        XCTAssertEqual(IPv6Addr("2001:db8::1")?.description, "2001:db8::1")
        XCTAssertTrue(IPv4Network("192.168.1.12/24")!.contains(IPv4Addr("192.168.1.99")!))
    }

    func testLocalNetworkSuggestion() {
        func entry(_ name: String, _ address: String, _ netmask: String) -> InterfaceAddress {
            InterfaceAddress(name: name, address: IPv4Addr(address)!, netmask: IPv4Addr(netmask)!)
        }
        XCTAssertEqual(LocalNetwork.suggested(from: [entry("pdp_ip0", "10.40.2.9", "255.255.255.255"), entry("en0", "192.168.7.33", "255.255.0.0")])?.description, "192.168.7.0/24")
        XCTAssertEqual(LocalNetwork.suggested(from: [entry("eth0", "10.1.2.200", "255.255.255.192")])?.description, "10.1.2.192/26")
        XCTAssertNil(LocalNetwork.suggested(from: [entry("en0", "8.8.8.8", "255.255.255.0"), entry("en1", "169.254.3.4", "255.255.0.0")]))
        XCTAssertNil(LocalNetwork.suggested(from: [entry("utun3", "100.64.0.7", "255.192.0.0")]))
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
