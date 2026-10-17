import XCTest
@testable import NexusKit

final class NexusKitTests: XCTestCase {
    func testConfigurationDefaultsToNativeSafety() { XCTAssertFalse(NexusConfiguration().authorization.allowPublicTargets) }
}
