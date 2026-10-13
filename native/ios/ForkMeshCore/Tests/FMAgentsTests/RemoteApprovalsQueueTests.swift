import FMBase
import FMControl
import Foundation
import XCTest

@testable import FMAgents

final class RemoteApprovalsQueueTests: XCTestCase {
    @MainActor func testRefreshSortsNewestFirst() async {
        let channel = FakeAgentOpsChannel()
        channel.reply(
            "agentops.approvals",
            ["approvals": [approvalJSON(approvalA, session: 1, askedAt: 10), approvalJSON(approvalB, session: 2, askedAt: 20)]])
        let queue = ApprovalsQueue(client: AgentOpsClient(channel: channel))
        await queue.refresh()
        XCTAssertEqual(queue.approvals.map(\.id), [approvalB, approvalA])
        XCTAssertEqual(queue.count, 2)
        XCTAssertEqual(queue.sessionIdsNeedingYou, [1, 2])
        XCTAssertEqual(queue.approvals(forSession: 1).map(\.id), [approvalA])
        XCTAssertEqual(queue.approval(id: approvalB)?.sessionId, 2)
    }

    @MainActor func testApproveRunsPrepareThenAnswer() async {
        let channel = FakeAgentOpsChannel()
        channel.reply("agentops.approvals", ["approvals": [approvalJSON(approvalA, session: 1)]])
        channel.reply("agentops.prepare", confirmationJSON("approve", token: "t1"))
        let queue = ApprovalsQueue(client: AgentOpsClient(channel: channel))
        await queue.refresh()
        let ok = await queue.approve(approvalA, lifetime: .worktree)
        XCTAssertTrue(ok)
        XCTAssertEqual(channel.methods(), ["agentops.approvals", "agentops.prepare", "agentops.answer"])
        XCTAssertEqual(channel.params("agentops.answer")?.str("token"), "t1")
        XCTAssertEqual(channel.params("agentops.answer")?.str("lifetime"), "worktree")
        XCTAssertTrue(queue.isEmpty)
        XCTAssertEqual(queue.lastAnswered, approvalA)
    }

    @MainActor func testDenyAlreadyAnsweredDropsTheRow() async {
        let channel = FakeAgentOpsChannel()
        channel.reply("agentops.approvals", ["approvals": [approvalJSON(approvalA, session: 1)]])
        channel.failures["agentops.prepare"] = FakeControlError(code: "unknown_approval")
        let queue = ApprovalsQueue(client: AgentOpsClient(channel: channel))
        await queue.refresh()
        let ok = await queue.deny(approvalA)
        XCTAssertFalse(ok)
        XCTAssertTrue(queue.isEmpty)
        XCTAssertEqual(queue.errorText, AgentOpsClient.describe(code: "unknown_approval"))
    }

    @MainActor func testBadIdIsRefusedLocally() async {
        let channel = FakeAgentOpsChannel()
        let queue = ApprovalsQueue(client: AgentOpsClient(channel: channel))
        let ok = await queue.approve("nope")
        XCTAssertFalse(ok)
        XCTAssertTrue(channel.requests.isEmpty)
    }

    @MainActor func testOlderDesktopIsUnsupportedNotAnError() async {
        let channel = FakeAgentOpsChannel()
        channel.failures["agentops.approvals"] = FakeControlError(code: "unsupported")
        let queue = ApprovalsQueue(client: AgentOpsClient(channel: channel))
        await queue.refresh()
        XCTAssertTrue(queue.unsupported)
        XCTAssertEqual(queue.errorText, "")
    }

    @MainActor func testBellRefreshesOnceAndStopRemovesTheTap() async {
        let channel = FakeAgentOpsChannel()
        channel.reply("agentops.approvals", ["approvals": [approvalJSON(approvalA, session: 1)]])
        let queue = ApprovalsQueue(client: AgentOpsClient(channel: channel))
        queue.start()
        queue.start()
        XCTAssertEqual(channel.tapCount, 1)
        channel.emit("agentops.changed", ["what": "approvals", "count": 1])
        channel.emit("agentops.changed", ["what": "approvals", "count": 1])
        for _ in 0..<20 where queue.isEmpty { await Task.yield() }
        XCTAssertEqual(queue.count, 1)
        XCTAssertLessThanOrEqual(channel.methods().filter { $0 == "agentops.approvals" }.count, 2)
        queue.stop()
        XCTAssertEqual(channel.tapCount, 0)
    }
}
