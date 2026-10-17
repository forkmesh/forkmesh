import XCTest
@testable import NexusScan
@testable import NexusCore

final class NexusScanTests: XCTestCase {
    func testSSHBannerIsConfirmed() {
        let identity = ServiceInspector().identify(data: Data("SSH-2.0-OpenSSH_9.0\r\n".utf8), port: 22)
        XCTAssertEqual(identity?.evidence, .confirmed)
        XCTAssertEqual(identity?.name, "ssh")
    }
}
