import FMBase
import Foundation
import Observation
import XCTest

@MainActor
@Observable
private final class Source {
    var count = 0
    var noise = 0
}

private final class Raised: @unchecked Sendable {
    private let lock = NSLock()
    private var times = 0
    var count: Int { lock.lock(); defer { lock.unlock() }; return times }
    func raise() { lock.lock(); times += 1; lock.unlock() }
}

@MainActor
final class ThrottledSnapshotTests: XCTestCase {
    private func settle(_ seconds: TimeInterval = 0.05) async {
        try? await Task.sleep(nanoseconds: UInt64(seconds * 1_000_000_000))
    }

    private func watch(_ snapshot: ThrottledSnapshot<Int>, _ raised: Raised) {
        withObservationTracking { _ = snapshot.value } onChange: { raised.raise() }
    }

    func testABurstOfChangesIsReadOnce() async {
        let source = Source()
        let snapshot = ThrottledSnapshot<Int>(interval: 0.2) { source.count }
        let before = snapshot.evaluations
        await settle(0.25)
        for _ in 0..<50 { source.count += 1 }
        await settle(0.05)
        XCTAssertEqual(snapshot.value, 50)
        XCTAssertEqual(snapshot.evaluations, before + 1)
    }

    func testChangesInsideTheIntervalWaitForItsEnd() async {
        let source = Source()
        let snapshot = ThrottledSnapshot<Int>(interval: 0.3) { source.count }
        await settle(0.35)
        source.count = 1
        await settle(0.05)
        XCTAssertEqual(snapshot.value, 1, "the first change after a quiet spell shows at once")
        source.count = 2
        await settle(0.05)
        XCTAssertEqual(snapshot.value, 1, "a second change inside the interval waits")
        await settle(0.4)
        XCTAssertEqual(snapshot.value, 2)
    }

    func testAnUnchangedResultDoesNotNotifyReaders() async {
        let source = Source()
        let snapshot = ThrottledSnapshot<Int>(interval: 0.01) { source.count }
        let raised = Raised()
        watch(snapshot, raised)
        let before = snapshot.evaluations
        source.noise += 1
        source.count = 5
        source.count = 0
        await settle(0.1)
        XCTAssertGreaterThan(snapshot.evaluations, before)
        XCTAssertEqual(raised.count, 0)
        source.count = 3
        await settle(0.1)
        XCTAssertEqual(snapshot.value, 3)
        XCTAssertEqual(raised.count, 1)
    }

    func testRefreshNowReadsAtOnce() async {
        let source = Source()
        let snapshot = ThrottledSnapshot<Int>(interval: 10) { source.count }
        source.count = 1
        await settle(0.05)
        source.count = 2
        snapshot.refreshNow()
        XCTAssertEqual(snapshot.value, 2)
    }
}
