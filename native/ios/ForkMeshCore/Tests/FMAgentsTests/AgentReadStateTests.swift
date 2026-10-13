import FMApi
import FMBase
import FMChat
import FMControl
import XCTest
@testable import FMAgents

final class AgentReadStateTests: XCTestCase {
    private let repo = Repository(owner: "forkmesh", name: "forkmesh")

    private func desktopRow(_ id: Int, status: String, finished: Int, unread: Bool? = nil) -> UnifiedAgentRow {
        var json: JSONObject = [
            "id": .int(Int64(id)), "repositoryOwner": "forkmesh", "repositoryName": "Forkmesh",
            "status": .string(status), "prompt": "Fix it", "finishedAtMs": .int(Int64(finished)),
        ]
        if let unread { json["unread"] = .bool(unread) }
        return UnifiedAgentRow.fromDesktop(LocalAgentRow.fromJSON(json), transport: .lan, desktop: "studio")
    }

    private func cloudLine(_ id: String, status: String, completed: Int) -> AgentLine {
        AgentLines.line(
            .launched(
                repo, AgentLaunchSession(sessionId: id, status: status, updatedAtMs: completed, completedAtMs: completed),
                [], cloud: true))
    }

    func testLinesCarryAStableReadKeyAndFinishTime() {
        let row = desktopRow(7, status: "done", finished: 900, unread: true)
        let control = AgentLines.line(row)
        XCTAssertEqual(control.readKey, "desktop:forkmesh#7")
        XCTAssertEqual(control.finishedAtMs, 900)
        XCTAssertEqual(control.desktopSessionId, 7)
        XCTAssertEqual(control.desktopUnread, true)

        let pushed = AgentLines.merge(entries: [.desktop(repo, AgentSession(id: 7, status: "done", finishedAtMs: 500))], rows: [row])
        XCTAssertEqual(pushed.count, 1)
        XCTAssertEqual(pushed[0].readKey, "desktop:forkmesh#7")
        XCTAssertEqual(pushed[0].finishedAtMs, 900)
        XCTAssertEqual(pushed[0].desktopSessionId, 7)
        XCTAssertEqual(pushed[0].desktopUnread, true)

        let unreached = AgentLines.line(.desktop(repo, AgentSession(id: 7, status: "done", finishedAtMs: 500)))
        XCTAssertEqual(unreached.readKey, "desktop:forkmesh#7")
        XCTAssertEqual(unreached.desktopSessionId, 0, "a pushed session no desktop in reach lists is not marked there")
        XCTAssertNil(unreached.desktopUnread)

        let run = cloudLine("s1", status: "completed", completed: 800)
        XCTAssertEqual(run.readKey, "run:s1")
        XCTAssertEqual(run.finishedAtMs, 800)
        XCTAssertEqual(run.desktopSessionId, 0)
        var node = UnifiedAgentRow.fromCloud(AgentLaunchSession(sessionId: "s1", status: "completed", completedAtMs: 800))
        XCTAssertEqual(node.finishedAtMs, 800)
        XCTAssertEqual(AgentLines.line(node).readKey, "run:s1")
        node.cloudSessionId = ""
        XCTAssertEqual(AgentLines.line(node).readKey, "control:node:s1")
    }

    func testAFinishAfterTheBaselineIsUnreadUntilOpened() {
        var state = AgentReadState(baselineMs: 1_000)
        let old = cloudLine("old", status: "completed", completed: 900)
        let fresh = cloudLine("new", status: "completed", completed: 1_500)
        let running = cloudLine("run", status: "running", completed: 0)
        let queued = cloudLine("wait", status: "queued", completed: 0)
        XCTAssertFalse(state.isUnread(old), "a finish before the feature started counts as read")
        XCTAssertTrue(state.isUnread(fresh))
        XCTAssertFalse(state.isUnread(running))
        XCTAssertFalse(state.isUnread(queued))

        XCTAssertTrue(state.markRead(fresh))
        XCTAssertFalse(state.isUnread(fresh))
        XCTAssertEqual(state.seen["run:new"], 1_500)
        XCTAssertFalse(state.markRead(fresh), "opening again changes nothing")

        let again = cloudLine("new", status: "completed", completed: 2_000)
        XCTAssertTrue(state.isUnread(again), "a later finish is unread again")
        XCTAssertFalse(state.markRead(running), "a run that never finished records nothing")

        var merged = fresh
        merged.status = .merged
        merged.finishedAtMs = 3_000
        XCTAssertFalse(state.isUnread(merged))
    }

    func testTheDesktopFlagWinsUntilThePhoneOpensThatFinish() {
        var state = AgentReadState(baselineMs: 10_000)
        let flagged = AgentLines.line(desktopRow(7, status: "done", finished: 900, unread: true))
        XCTAssertTrue(state.isUnread(flagged), "the desktop's flag ignores the phone's baseline")
        let cleared = AgentLines.line(desktopRow(7, status: "done", finished: 20_000, unread: false))
        XCTAssertFalse(state.isUnread(cleared), "the desktop read it")
        let older = AgentLines.line(desktopRow(8, status: "done", finished: 20_000))
        XCTAssertTrue(state.isUnread(older), "an older desktop sends no flag: the phone's rule decides")

        state.markRead(flagged)
        XCTAssertFalse(state.isUnread(flagged), "the dot goes before the desktop's next list")
        let finishedAgain = AgentLines.line(desktopRow(7, status: "done", finished: 1_200, unread: true))
        XCTAssertTrue(state.isUnread(finishedAgain))
    }

    func testAppliedMarksEachLineAndSkipsTheOpenOne() {
        let state = AgentReadState(baselineMs: 0)
        let a = cloudLine("a", status: "completed", completed: 10)
        let b = cloudLine("b", status: "failed", completed: 20)
        let shown = state.applied([a, b], open: "run:b")
        XCTAssertEqual(shown.map(\.unread), [true, false])
        XCTAssertTrue(shown[0].accessibilitySummary.hasPrefix("Unread, "))
        XCTAssertFalse(shown[1].accessibilitySummary.contains("Unread"))
    }

    func testTheStoreKeepsABaselineAndABoundedMap() {
        let store = InMemoryKeyValueStore()
        let first = AgentReadState.load(store, nowMs: 5_000)
        XCTAssertEqual(first.baselineMs, 5_000)
        XCTAssertEqual(store.int(forKey: "agents/readBaselineMs"), 5_000)
        XCTAssertEqual(AgentReadState.load(store, nowMs: 9_000).baselineMs, 5_000, "the baseline is set once")

        var state = first
        state.markRead(cloudLine("a", status: "completed", completed: 6_000))
        state.save(store)
        XCTAssertEqual(store.string(forKey: "agents/seenFinishedAt"), #"{"run:a":6000}"#)
        XCTAssertEqual(AgentReadState.load(store, nowMs: 9_000).seen, ["run:a": 6_000])

        store.set("not json", forKey: AgentReadState.seenKey)
        XCTAssertEqual(AgentReadState.load(store, nowMs: 9_000).seen, [:])

        var full = AgentReadState(baselineMs: 0, seen: Dictionary(uniqueKeysWithValues: (1...5).map { ("k\($0)", $0) }))
        full.prune(limit: 3)
        XCTAssertEqual(full.seen, ["k3": 3, "k4": 4, "k5": 5], "the oldest finishes go first")
        var many = AgentReadState(baselineMs: 0)
        for n in 1...(AgentReadState.limit + 10) {
            many.markRead(cloudLine("s\(n)", status: "completed", completed: n))
        }
        XCTAssertEqual(many.seen.count, AgentReadState.limit)
        XCTAssertNil(many.seen["run:s1"])
    }

    @MainActor func testOpeningRecordsTheFinishAndAsksTheDesktop() async {
        let store = InMemoryKeyValueStore()
        var marked: [Int] = []
        let tracker = AgentReadTracker(store: store, nowMs: 0) { marked.append($0) }
        let line = AgentLines.line(desktopRow(7, status: "done", finished: 900, unread: true))
        let run = cloudLine("s1", status: "completed", completed: 50)
        XCTAssertEqual(tracker.lines([line, run]).map(\.unread), [true, true])

        tracker.open(line)
        XCTAssertEqual(marked, [7])
        XCTAssertEqual(tracker.openKey, "desktop:forkmesh#7")
        XCTAssertEqual(tracker.lines([line, run]).map(\.unread), [false, true])
        XCTAssertEqual(AgentReadState.load(store, nowMs: 0).seen["desktop:forkmesh#7"], 900)

        let finishedWhileOpen = AgentLines.line(desktopRow(7, status: "done", finished: 1_400, unread: true))
        XCTAssertFalse(tracker.lines([finishedWhileOpen])[0].unread, "the open agent shows no dot")
        tracker.close(finishedWhileOpen)
        XCTAssertEqual(tracker.openKey, "")
        XCTAssertEqual(marked, [7, 7])
        XCTAssertFalse(tracker.lines([finishedWhileOpen])[0].unread)

        tracker.open(run)
        XCTAssertEqual(marked, [7, 7], "a cloud run has no desktop to tell")
        tracker.close(run)
        XCTAssertFalse(tracker.lines([run])[0].unread)
    }
}
