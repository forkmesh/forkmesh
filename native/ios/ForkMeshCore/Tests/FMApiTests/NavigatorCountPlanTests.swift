import Foundation
import XCTest

@testable import FMApi

@MainActor
private final class Gauge {
    var running = 0
    var peak = 0
    var started: [Route.Kind] = []
}

@MainActor
final class NavigatorCountPlanTests: XCTestCase {
    private let now = Date(timeIntervalSince1970: 1_800_000_000)

    func testACoolPhoneRefreshesOnceAMinute() {
        XCTAssertTrue(NavigatorCountPlan.due(loadedAt: nil, now: now, heat: .nominal, lowPower: false))
        XCTAssertFalse(NavigatorCountPlan.due(loadedAt: now.addingTimeInterval(-59), now: now, heat: .fair, lowPower: false))
        XCTAssertTrue(NavigatorCountPlan.due(loadedAt: now.addingTimeInterval(-60), now: now, heat: .nominal, lowPower: false))
    }

    func testAWarmPhoneOrLowPowerModeWaitsFiveMinutes() {
        let twoMinutesAgo = now.addingTimeInterval(-120)
        XCTAssertFalse(NavigatorCountPlan.due(loadedAt: twoMinutesAgo, now: now, heat: .serious, lowPower: false))
        XCTAssertFalse(NavigatorCountPlan.due(loadedAt: twoMinutesAgo, now: now, heat: .nominal, lowPower: true))
        XCTAssertTrue(NavigatorCountPlan.due(loadedAt: now.addingTimeInterval(-300), now: now, heat: .serious, lowPower: true))
    }

    func testACriticallyHotPhoneFetchesNoCounts() {
        XCTAssertNil(NavigatorCountPlan.freshFor(heat: .critical, lowPower: false))
        XCTAssertFalse(NavigatorCountPlan.due(loadedAt: nil, now: now, heat: .critical, lowPower: false))
        XCTAssertFalse(NavigatorCountPlan.due(loadedAt: nil, now: now, heat: .critical, lowPower: false, force: true))
    }

    func testForceSkipsTheWaitButNotTheHeat() {
        let justNow = now.addingTimeInterval(-1)
        XCTAssertTrue(NavigatorCountPlan.due(loadedAt: justNow, now: now, heat: .nominal, lowPower: false, force: true))
        XCTAssertTrue(NavigatorCountPlan.due(loadedAt: justNow, now: now, heat: .serious, lowPower: false, force: true))
    }

    func testOnlyKindsOnTheHomeGridAreFetched() {
        let groups = [
            HubGroup(id: "personal", title: "Personal", items: [
                HubItem(id: "notes", title: "Notes", symbol: "note", action: .route(.notes)),
                HubItem(id: "world", title: "World", symbol: "globe", action: .world),
            ]),
            HubGroup(id: "app", title: "App", items: [
                HubItem(id: "settings", title: "Settings", symbol: "gear", action: .route(.settings(section: nil))),
            ]),
        ]
        let shown = NavigatorCountPlan.shownKinds(groups)
        XCTAssertEqual(shown, [.repos, .notes, .settings])
        XCTAssertEqual(
            NavigatorCountPlan.wanted([.repos, .notes, .calories, .stretch, .sugarBreak, .admin], shown: shown), [.repos, .notes])
    }

    func testCountsLoadAFewAtATime() async {
        let gauge = Gauge()
        let kinds: [Route.Kind] = [.repos, .notes, .tasks, .calendar, .orgs, .calories, .steps, .water, .sleep]
        let counts = await NavigatorCountPlan.run(kinds, width: 3) { kind in
            gauge.running += 1
            gauge.peak = max(gauge.peak, gauge.running)
            gauge.started.append(kind)
            try? await Task.sleep(nanoseconds: 5_000_000)
            gauge.running -= 1
            return kind == .calories ? nil : kind.rawValue.count
        }
        XCTAssertLessThanOrEqual(gauge.peak, 3)
        XCTAssertEqual(Set(gauge.started), Set(kinds))
        XCTAssertNil(counts[.calories], "a failed or uninstalled count shows no pill")
        XCTAssertEqual(counts[.notes], "notes".count)
        XCTAssertEqual(counts.count, kinds.count - 1)
    }
}
