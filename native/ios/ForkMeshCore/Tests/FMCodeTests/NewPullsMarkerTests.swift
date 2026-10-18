import FMApi
import FMBase
import XCTest

@testable import FMCode

final class NewPullsMarkerTests: XCTestCase {
    private func pull(_ number: Int, _ status: String = "open") -> PublishedPull {
        PublishedPull(number: number, title: "PR \(number)", status: status)
    }

    func testFirstSightRecordsABaselineInsteadOfFlaggingEveryOpenPull() {
        let marker = NewPullsMarker(store: InMemoryKeyValueStore())
        XCTAssertEqual(marker.newCount("Acme/Repo", pulls: [pull(1), pull(2), pull(3, "merged")]), 0)
        XCTAssertEqual(marker.seen("acme/repo"), 3)
    }

    func testOnlyOpenPullsAboveTheMarkCountAsNew() {
        let marker = NewPullsMarker(store: InMemoryKeyValueStore())
        _ = marker.newCount("acme/repo", pulls: [pull(1)])
        let later = [pull(1), pull(2), pull(3, "closed"), pull(4), pull(5, "merged")]
        XCTAssertEqual(marker.newCount("acme/repo", pulls: later), 2)
    }

    func testAnEmptyRepositoryCountsItsFirstPull() {
        let marker = NewPullsMarker(store: InMemoryKeyValueStore())
        XCTAssertEqual(marker.newCount("acme/empty", pulls: []), 0)
        XCTAssertEqual(marker.newCount("acme/empty", pulls: [pull(1)]), 1)
    }

    func testOpeningThePullRequestsPageClearsTheBadge() {
        let marker = NewPullsMarker(store: InMemoryKeyValueStore())
        _ = marker.newCount("acme/repo", pulls: [pull(1)])
        let pulls = [pull(1), pull(2), pull(3)]
        XCTAssertEqual(marker.newCount("acme/repo", pulls: pulls), 2)
        marker.markSeen("ACME/repo", pulls: pulls)
        XCTAssertEqual(marker.newCount("acme/repo", pulls: pulls), 0)
    }

    func testMarkingSeenNeverMovesTheMarkBackwards() {
        let marker = NewPullsMarker(store: InMemoryKeyValueStore())
        marker.markSeen("acme/repo", pulls: [pull(9)])
        marker.markSeen("acme/repo", pulls: [pull(2)])
        XCTAssertEqual(marker.seen("acme/repo"), 9)
    }

    func testMarksPersistPerRepositoryAndStayBounded() {
        let store = InMemoryKeyValueStore()
        let marker = NewPullsMarker(store: store)
        for index in 0..<(NewPullsMarker.limit + 5) {
            marker.markSeen("acme/repo\(index)", pulls: [pull(index + 1)])
        }
        let reread = NewPullsMarker(store: store)
        XCTAssertEqual(reread.seen("acme/repo\(NewPullsMarker.limit + 4)"), NewPullsMarker.limit + 5)
        XCTAssertNil(reread.seen("acme/repo0"))
        XCTAssertEqual(store.stringList(forKey: NewPullsMarker.key)?.count, NewPullsMarker.limit)
    }

    func testPollingSlowsWhenThePhoneIsWarmAndStopsWhenCritical() {
        XCTAssertEqual(NewPullsMarker.pollEvery(heat: .nominal, lowPower: false), NewPullsMarker.calmPollEvery)
        XCTAssertEqual(NewPullsMarker.pollEvery(heat: .fair, lowPower: true), NewPullsMarker.warmPollEvery)
        XCTAssertEqual(NewPullsMarker.pollEvery(heat: .serious, lowPower: false), NewPullsMarker.warmPollEvery)
        XCTAssertNil(NewPullsMarker.pollEvery(heat: .critical, lowPower: false))
    }
}
