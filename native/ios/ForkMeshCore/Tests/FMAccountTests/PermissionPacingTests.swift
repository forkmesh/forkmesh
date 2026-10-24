import FMBase
import XCTest
@testable import FMAccount

final class PermissionPacingTests: XCTestCase {
    func testAFreshInstallShowsTheIntroAndHoldsNearbyUntilMeshIsOpened() {
        let prefs = InMemoryKeyValueStore()
        PermissionPacing.settle(prefs, freshInstall: true)

        XCTAssertTrue(PermissionPacing.needsIntro(prefs))
        XCTAssertFalse(PermissionPacing.nearbyAllowed(prefs))

        PermissionPacing.finishIntro(prefs)
        PermissionPacing.markNearbyOpened(prefs)
        XCTAssertFalse(PermissionPacing.needsIntro(prefs))
        XCTAssertTrue(PermissionPacing.nearbyAllowed(prefs))
    }

    func testAnUpdatedInstallSkipsTheIntroAndKeepsTheMeshAsItWas() {
        let prefs = InMemoryKeyValueStore()
        PermissionPacing.settle(prefs, freshInstall: false)

        XCTAssertFalse(PermissionPacing.needsIntro(prefs))
        XCTAssertTrue(PermissionPacing.nearbyAllowed(prefs))
    }

    func testTheFirstDecisionSticksOnLaterLaunches() {
        let prefs = InMemoryKeyValueStore()
        PermissionPacing.settle(prefs, freshInstall: true)
        PermissionPacing.settle(prefs, freshInstall: false)
        XCTAssertTrue(PermissionPacing.needsIntro(prefs))
        XCTAssertFalse(PermissionPacing.nearbyAllowed(prefs))

        PermissionPacing.finishIntro(prefs)
        PermissionPacing.settle(prefs, freshInstall: true)
        XCTAssertFalse(PermissionPacing.needsIntro(prefs))
    }

    func testTheIntroStaysShort() {
        XCTAssertLessThanOrEqual(PermissionPacing.reasons.count, 5)
        XCTAssertEqual(Set(PermissionPacing.reasons.map(\.id)).count, PermissionPacing.reasons.count)
        for reason in PermissionPacing.reasons {
            XCTAssertLessThanOrEqual(reason.reason.count, 90, reason.id)
        }
    }
}
