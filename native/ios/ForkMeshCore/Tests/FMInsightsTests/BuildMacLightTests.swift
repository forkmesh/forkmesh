import FMBase
import FMChat
import FMControl
import Foundation
import XCTest

@testable import FMInsights

final class BuildMacLightTests: XCTestCase {
    func testTheDesktopAnswerBecomesALight() {
        XCTAssertNil(BuildMacLight(json: object(#"{"configured":false}"#)))
        XCTAssertNil(BuildMacLight(json: object(#"{"configured":true,"state":"asleep"}"#)))
        let idle = BuildMacLight(json: object(#"{"configured":true,"state":"idle","job":"stale"}"#))
        XCTAssertEqual(idle, BuildMacLight(state: .idle))
        XCTAssertEqual(idle?.label, "Office MacBook, idle")
        XCTAssertEqual(idle?.healthy, true)
        let offline = BuildMacLight(json: object(#"{"configured":true,"state":"offline"}"#))
        XCTAssertEqual(offline?.healthy, false)
        XCTAssertEqual(offline?.label, "Office MacBook, not reachable from the desktop")
        let building = BuildMacLight(
            json: object(#"{"configured":true,"state":"building","job":"desktop-release v1.2.5","sinceMs":42}"#))
        XCTAssertEqual(building?.sinceMs, 42)
        XCTAssertEqual(building?.label, "Office MacBook, building the macOS release v1.2.5")
        XCTAssertEqual(building?.summary, "building the macOS release v1.2.5")
    }

    func testJobNames() {
        XCTAssertEqual(BuildMacLight(state: .building, job: "ci/abc123-1790000000").jobLabel, "an iOS ad hoc build")
        XCTAssertEqual(BuildMacLight(state: .building, job: "ios-release v1.3.0").jobLabel, "the iOS release v1.3.0")
        XCTAssertEqual(BuildMacLight(state: .building, job: "custom").jobLabel, "custom")
        XCTAssertEqual(BuildMacLight(state: .building).jobLabel, "a job")
    }

    @MainActor func testLoadsOnlyOverAnOpenLinkAndWaitsLongerWhileIdle() async {
        let channel = FakeDesktopChannel()
        channel.replies[BuildMacMethod.status] = object(#"{"configured":true,"state":"idle"}"#)
        var open = false
        var clock = Date(timeIntervalSince1970: 0)
        let model = BuildMacLightModel(channel: channel, linkOpen: { open }, now: { clock })
        await model.refresh()
        XCTAssertTrue(channel.requests.isEmpty)
        XCTAssertNil(model.light)
        open = true
        await model.refresh()
        XCTAssertEqual(model.light?.state, .idle)
        clock = clock.addingTimeInterval(BuildMacLightModel.busyFloor + 1)
        await model.refresh()
        XCTAssertEqual(channel.requests.count, 1)
        clock = clock.addingTimeInterval(BuildMacLightModel.idleFloor)
        channel.replies[BuildMacMethod.status] = object(#"{"configured":true,"state":"building","job":"ci/x"}"#)
        await model.refresh()
        XCTAssertEqual(model.light?.state, .building)
        clock = clock.addingTimeInterval(BuildMacLightModel.busyFloor + 1)
        await model.refresh()
        XCTAssertEqual(channel.requests.count, 3)
    }

    @MainActor func testAnOlderDesktopHidesTheLightAndAFailureKeepsIt() async {
        let channel = FakeDesktopChannel()
        channel.replies[BuildMacMethod.status] = object(#"{"configured":true,"state":"offline"}"#)
        let model = BuildMacLightModel(channel: channel, linkOpen: { true })
        await model.load()
        XCTAssertEqual(model.light?.state, .offline)
        channel.failure = LocalControlException("timeout")
        await model.load()
        XCTAssertEqual(model.light?.state, .offline)
        channel.failure = LocalControlException("unsupported")
        await model.load()
        XCTAssertNil(model.light)
    }
}
