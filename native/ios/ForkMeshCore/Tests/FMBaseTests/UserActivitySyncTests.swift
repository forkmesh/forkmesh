import XCTest
@testable import FMBase

@MainActor
final class UserActivitySyncTests: XCTestCase {
    private var calendar: Calendar {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = TimeZone(identifier: "UTC")!
        return calendar
    }

    private func start() -> (UserActivityLog, FixedClock) {
        let clock = FixedClock(calendar.date(from: DateComponents(year: 2026, month: 10, day: 6, hour: 9))!)
        return (UserActivityLog(store: nil, clock: clock, calendar: calendar), clock)
    }

    private func slots(_ pairs: [(Int, Int)] = []) -> JSONValue {
        var counts = [JSONValue](repeating: .int(0), count: 288)
        for (slot, value) in pairs { counts[slot] = .int(Int64(value)) }
        return .array(counts)
    }

    private func answer(_ others: JSONValue, node: String = "alice", sources: [JSONValue] = []) -> (status: Int, body: JSONObject) {
        (200, ["ok": .bool(true), "nodeName": .string(node), "others": others, "sources": .array(sources)])
    }

    func testFirstSyncSendsTheDayAndAddsTheOtherDevices() async {
        let (log, clock) = start()
        log.record()
        var sent: [JSONObject] = []
        let sources: [JSONValue] = [
            .object(["kind": .string("desktop"), "total": .int(3), "self": .bool(false)]),
            .object(["kind": .string("ios"), "total": .int(1), "self": .bool(true)]),
        ]
        let sync = UserActivitySync(log: log, kind: "ios", clock: clock) { (payload: JSONObject) in
            sent.append(payload)
            return self.answer(self.slots([(108, 3)]), sources: sources)
        }
        let ok = await sync.sync(account: "alice")
        XCTAssertTrue(ok)
        XCTAssertEqual(sent.count, 1)
        XCTAssertEqual(sent[0].str("day"), "2026-10-06")
        XCTAssertEqual(sent[0].str("device"), log.deviceKey)
        XCTAssertEqual(sent[0].str("kind"), "ios")
        XCTAssertEqual(sent[0]["counts"]?.arrayValue?[108].intValue, 1)
        XCTAssertEqual(log.counts()[108], 4)
        XCTAssertEqual(log.otherKinds(), ["desktop"])
    }

    func testUnchangedCountsAreOnlyReadAfterTheReadInterval() async {
        let (log, clock) = start()
        var sent: [JSONObject] = []
        let sync = UserActivitySync(log: log, kind: "ios", clock: clock) { (payload: JSONObject) in
            sent.append(payload)
            return self.answer(self.slots())
        }
        _ = await sync.sync(account: "alice")
        XCTAssertTrue(sent[0].contains("counts"))
        clock.advance(by: UserActivitySync.interval)
        let idle = await sync.sync(account: "alice")
        XCTAssertFalse(idle)
        let forced = await sync.sync(account: "alice", force: true)
        XCTAssertTrue(forced)
        XCTAssertFalse(sent[1].contains("counts"))
        clock.advance(by: 5)
        let tooSoon = await sync.sync(account: "alice", force: true)
        XCTAssertFalse(tooSoon)
        log.record()
        let fresh = await sync.sync(account: "alice")
        XCTAssertTrue(fresh)
        XCTAssertTrue(sent[2].contains("counts"))
    }

    func testAnotherAccountsAnswerOrAFailureIsDropped() async {
        let (log, clock) = start()
        log.record()
        var reply = answer(slots([(5, 9)]), node: "bob")
        let sync = UserActivitySync(log: log, kind: "ios", clock: clock) { (_: JSONObject) in reply }
        let other = await sync.sync(account: "alice")
        XCTAssertFalse(other)
        XCTAssertEqual(log.counts().reduce(0, +), 1)
        reply = (500, [:])
        let failed = await sync.sync(account: "alice")
        XCTAssertFalse(failed)
        reply = (200, ["nodeName": .string("alice"), "others": .array([.int(1)])])
        let short = await sync.sync(account: "alice")
        XCTAssertFalse(short)
        let offline = UserActivitySync(log: log, kind: "ios", clock: clock) { (_: JSONObject) in throw URLError(.notConnectedToInternet) }
        let thrown = await offline.sync(account: "alice")
        XCTAssertFalse(thrown)
    }

    func testSigningOutOrSwitchingAccountsDropsTheOtherDevices() async {
        let (log, clock) = start()
        var node = "alice"
        var sent: [JSONObject] = []
        let sync = UserActivitySync(log: log, kind: "ios", clock: clock) { (payload: JSONObject) in
            sent.append(payload)
            return self.answer(self.slots([(1, 2)]), node: node)
        }
        _ = await sync.sync(account: "alice")
        XCTAssertEqual(log.counts().reduce(0, +), 2)
        let signedOut = await sync.sync(account: "")
        XCTAssertFalse(signedOut)
        XCTAssertEqual(log.counts().reduce(0, +), 0)
        node = "bob"
        let bob = await sync.sync(account: "bob")
        XCTAssertTrue(bob)
        XCTAssertTrue(sent.last?.contains("counts") == true)
    }
}
