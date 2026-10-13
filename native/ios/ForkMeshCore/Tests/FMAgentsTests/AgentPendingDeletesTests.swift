import FMAgents
import XCTest

@MainActor
final class AgentPendingDeletesTests: XCTestCase {
    private func settle(_ pending: AgentPendingDeletes, until done: @MainActor () -> Bool) async {
        for _ in 0..<200 where !done() {
            try? await Task.sleep(for: .milliseconds(10))
        }
    }

    func testTheAgentHidesAtOnceAndIsDeletedWhenTheWindowCloses() async {
        let pending = AgentPendingDeletes(window: .milliseconds(50))
        var ran = 0
        XCTAssertTrue(pending.schedule("desktop:7", message: "Agent deleted.") { ran += 1; return .deleted("Deleted the agent.") })

        XCTAssertTrue(pending.isHidden("desktop:7"))
        XCTAssertTrue(pending.isWaiting("desktop:7"))
        XCTAssertEqual(pending.offer?.key, "desktop:7")
        XCTAssertEqual(pending.offer?.message, "Agent deleted.")
        XCTAssertEqual(ran, 0)

        await settle(pending) { ran == 1 }
        XCTAssertEqual(ran, 1)
        XCTAssertNil(pending.offer)
        XCTAssertFalse(pending.isWaiting("desktop:7"))
        XCTAssertTrue(pending.isHidden("desktop:7"))
        XCTAssertFalse(pending.undo("desktop:7"))
        XCTAssertEqual(pending.reports, 1)
        XCTAssertEqual(pending.report, "Deleted the agent.")
    }

    func testUndoInsideTheWindowKeepsTheAgent() async {
        let pending = AgentPendingDeletes(window: .milliseconds(80))
        var ran = 0
        pending.schedule("runner:s1", message: "Agent deleted.") { ran += 1; return .deleted("Deleted the agent.") }

        XCTAssertTrue(pending.undo("runner:s1"))
        XCTAssertFalse(pending.isHidden("runner:s1"))
        XCTAssertNil(pending.offer)

        try? await Task.sleep(for: .milliseconds(200))
        XCTAssertEqual(ran, 0)
    }

    func testAFailedDeleteBringsTheAgentBackAndSaysWhy() async {
        let pending = AgentPendingDeletes(window: .milliseconds(20))
        pending.schedule("desktop:3", message: "Agent deleted.") { .failed("The desktop is not in reach.") }

        await settle(pending) { pending.failures == 1 }
        XCTAssertEqual(pending.failures, 1)
        XCTAssertEqual(pending.failure, "The desktop is not in reach.")
        XCTAssertFalse(pending.isHidden("desktop:3"))
    }

    func testTheSameAgentIsNotScheduledTwice() {
        let pending = AgentPendingDeletes(window: .seconds(60))
        XCTAssertTrue(pending.schedule("desktop:9", message: "a") { .deleted("a") })
        XCTAssertFalse(pending.schedule("desktop:9", message: "b") { .deleted("b") })
        XCTAssertEqual(pending.offer?.message, "a")
        pending.undo("desktop:9")
    }

    func testTheOfferFollowsTheLatestDelete() {
        let pending = AgentPendingDeletes(window: .seconds(60))
        pending.schedule("desktop:1", message: "first") { .deleted("first") }
        pending.schedule("desktop:2", message: "second") { .deleted("second") }
        XCTAssertEqual(pending.offer?.key, "desktop:2")
        pending.undo("desktop:1")
        XCTAssertEqual(pending.offer?.key, "desktop:2")
        XCTAssertTrue(pending.isHidden("desktop:2"))
        pending.undo("desktop:2")
        XCTAssertNil(pending.offer)
    }

    func testTheUndoWindowIsFiveSeconds() {
        XCTAssertEqual(AgentPendingDeletes.undoWindow, .seconds(5))
    }

    func testKeysMatchTheRunWhicheverListShowsIt() {
        let ref = RunnerAgentRef(owner: "o", repo: "r", sessionId: "s1", orgScope: false, branch: "b", base: "main")
        XCTAssertEqual(AgentLineDeletion.desktop(sessionId: 7).key, "desktop:7")
        XCTAssertEqual(AgentLineDeletion.outside(id: -4).key, "desktop:-4")
        XCTAssertEqual(AgentLineDeletion.runner(ref, merged: false, active: true).key, "runner:s1")
        XCTAssertEqual(AgentLineDeletion.runner(ref, merged: true, active: false).key, "runner:s1")
    }
}
