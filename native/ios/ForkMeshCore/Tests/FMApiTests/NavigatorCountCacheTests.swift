import Foundation
import XCTest

@testable import FMApi

final class NavigatorCountCacheTests: XCTestCase {
    func testCountsSurviveARoundTrip() {
        let cache = NavigatorCountCache(
            day: "2026-10-19", counts: [.notes: 12, .repos: 4, .calories: 1800], assignedTasks: 3, bugReports: 2)
        let back = NavigatorCountCache.decode(cache.encoded)
        XCTAssertEqual(back, cache)
        XCTAssertEqual(back?.kinds, [.notes: 12, .repos: 4, .calories: 1800])
        XCTAssertEqual(back?.assignedTasks, 3)
        XCTAssertEqual(back?.bugReports, 2)
    }

    func testUnreadableOrUnknownEntriesAreDropped() {
        XCTAssertNil(NavigatorCountCache.decode(nil))
        XCTAssertNil(NavigatorCountCache.decode("not json"))
        let old = NavigatorCountCache.decode(#"{"day":"2026-10-19","counts":{"notes":5,"retired":9},"assignedTasks":0}"#)
        XCTAssertEqual(old?.kinds, [.notes: 5])
        XCTAssertNil(old?.bugReports)
    }

    func testANewDayForgetsOnlyTheDailyCounts() {
        let cache = NavigatorCountCache(
            day: "2026-10-18", counts: [.notes: 12, .calendar: 2, .steps: 9000, .water: 1500, .repos: 4])
        XCTAssertEqual(cache.on("2026-10-18"), cache)
        let today = cache.on("2026-10-19")
        XCTAssertEqual(today.day, "2026-10-19")
        XCTAssertEqual(today.kinds, [.notes: 12, .repos: 4])
    }

    func testOnlyCountsNotYetKnownAreFetched() {
        XCTAssertEqual(
            NavigatorCountPlan.missing([.repos, .notes, .tasks, .calendar], known: [.notes, .calendar]), [.repos, .tasks])
        XCTAssertEqual(NavigatorCountPlan.missing([.repos], known: [.repos]), [])
    }

    func testTheKeyNamesTheProjectAndTheUser() {
        XCTAssertEqual(NavigatorCountCache.key(project: "prod", user: "jett"), "shell.navigatorCounts.prod.jett")
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = TimeZone(identifier: "UTC")!
        XCTAssertEqual(NavigatorCountCache.day(Date(timeIntervalSince1970: 1_800_000_000), calendar: calendar), "2027-01-15")
    }
}
