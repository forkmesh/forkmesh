import FMBase
import FMChat
import FMControl
import Foundation
import XCTest

@testable import FMInsights

final class DesktopLoadLightTests: XCTestCase {
    func testTheHealthLevelBecomesALoad() {
        XCTAssertEqual(DesktopStrain(level: "ok"), .calm)
        XCTAssertEqual(DesktopStrain(level: "busy"), .busy)
        XCTAssertEqual(DesktopStrain(level: "strained"), .strained)
        XCTAssertEqual(DesktopStrain(level: ""), .calm)
        XCTAssertEqual(DesktopStrain.strained.label, "under pressure")
    }

    @MainActor func testReadsOnlyOverAnOpenLinkAndWaitsLongerWhileCalm() async {
        let channel = FakeDesktopChannel()
        channel.replies[DesktopStatsMethod.health] = object(#"{"level":"ok"}"#)
        var open = false
        var clock = Date(timeIntervalSince1970: 0)
        let model = DesktopLoadLightModel(channel: channel, linkOpen: { open }, now: { clock })
        await model.refresh()
        XCTAssertTrue(channel.requests.isEmpty)
        XCTAssertNil(model.load)
        open = true
        await model.refresh()
        XCTAssertEqual(model.load, .calm)
        clock = clock.addingTimeInterval(DesktopLoadLightModel.busyFloor + 1)
        await model.refresh()
        XCTAssertEqual(channel.requests.count, 1)
        clock = clock.addingTimeInterval(DesktopLoadLightModel.calmFloor)
        channel.replies[DesktopStatsMethod.health] = object(#"{"level":"busy"}"#)
        await model.refresh()
        XCTAssertEqual(model.load, .busy)
        clock = clock.addingTimeInterval(DesktopLoadLightModel.busyFloor + 1)
        await model.refresh()
        XCTAssertEqual(channel.requests.count, 3)
    }

    @MainActor func testAnOlderDesktopClearsTheLoadAndAFailureKeepsIt() async {
        let channel = FakeDesktopChannel()
        channel.replies[DesktopStatsMethod.health] = object(#"{"level":"strained"}"#)
        let model = DesktopLoadLightModel(channel: channel, linkOpen: { true })
        await model.read()
        XCTAssertEqual(model.load, .strained)
        channel.failure = LocalControlException("timeout")
        await model.read()
        XCTAssertEqual(model.load, .strained)
        channel.failure = LocalControlException("unsupported")
        await model.read()
        XCTAssertNil(model.load)
    }
}
