import FMApi
import FMBase
import FMChat
import FMControl
import Foundation
import XCTest

@testable import FMAgents

final class RemoteViewModelsTests: XCTestCase {
    @MainActor func testMergeIsOneClickAndSendsTheBranch() async {
        let channel = FakeAgentOpsChannel()
        channel.reply("agentops.prepare", confirmationJSON("merge", typeToConfirm: "agent/fix"))
        channel.reply("agentops.merge", ["branch": "agent/fix", "merged": true, "stats": ["files": 2, "commit": "abc"]])
        let model = MergeCleanupModel(client: AgentOpsClient(channel: channel), sessionId: 7)
        XCTAssertFalse(model.canRun)
        await model.prepare()
        XCTAssertTrue(model.canRun)
        let ok = await model.run()
        XCTAssertTrue(ok)
        XCTAssertEqual(channel.params("agentops.merge")?.str("typed"), "agent/fix")
        XCTAssertEqual(model.mergedBranch, "agent/fix")
        XCTAssertEqual(model.mergeStats?.files, 2)
        XCTAssertFalse(model.canRun, "a merged session cannot be merged again")
    }

    @MainActor func testMergeNowSendsTheDesktopsBranchName() async {
        let channel = FakeAgentOpsChannel()
        channel.reply("agentops.prepare", confirmationJSON("merge", typeToConfirm: "agent/fix"))
        channel.reply("agentops.merge", ["branch": "agent/fix", "merged": true])
        let model = MergeCleanupModel(client: AgentOpsClient(channel: channel), sessionId: 7)
        let ok = await model.mergeNow()
        XCTAssertTrue(ok)
        XCTAssertEqual(channel.methods(), ["agentops.prepare", "agentops.merge"])
        XCTAssertEqual(model.mergedBranch, "agent/fix")
        let again = await model.mergeNow()
        XCTAssertFalse(again, "a merged session cannot be merged again")
    }

    @MainActor func testMergeNowStopsWhenPrepareFails() async {
        let channel = FakeAgentOpsChannel()
        channel.failures["agentops.prepare"] = FakeControlError(code: "agent_running")
        let model = MergeCleanupModel(client: AgentOpsClient(channel: channel), sessionId: 1)
        let ok = await model.mergeNow()
        XCTAssertFalse(ok)
        XCTAssertFalse(channel.methods().contains("agentops.merge"))
        XCTAssertEqual(model.errorText, AgentOpsClient.describe(code: "agent_running"))
    }

    @MainActor func testExpiredMergeTokenIsNotSent() async {
        let channel = FakeAgentOpsChannel()
        channel.reply("agentops.prepare", confirmationJSON("merge", typeToConfirm: "b"))
        let clock = FixedClock(epochMillis: 0)
        let model = MergeCleanupModel(client: AgentOpsClient(channel: channel, clock: clock), sessionId: 1)
        await model.prepare()
        clock.advance(by: 61)
        let ok = await model.run()
        XCTAssertFalse(ok)
        XCTAssertFalse(channel.methods().contains("agentops.merge"))
        XCTAssertEqual(model.errorText, ControlErrorText.describe(code: "confirmation_expired"))
    }

    @MainActor func testMergeConflictIsFlaggedForAnAgentFix() async {
        let channel = FakeAgentOpsChannel()
        channel.reply("agentops.prepare", confirmationJSON("merge", typeToConfirm: "agent/fix"))
        channel.failures["agentops.merge"] = FakeControlError(code: "conflicts")
        let model = MergeCleanupModel(client: AgentOpsClient(channel: channel), sessionId: 7)
        await model.prepare()
        XCTAssertFalse(model.conflicted)
        let ok = await model.run()
        XCTAssertFalse(ok)
        XCTAssertTrue(model.conflicted)
        XCTAssertEqual(model.errorText, AgentOpsClient.describe(code: "conflicts"))
        let refused = FakeAgentOpsChannel()
        refused.reply("agentops.prepare", confirmationJSON("merge", typeToConfirm: "agent/fix"))
        refused.failures["agentops.merge"] = FakeControlError(code: "merge_failed")
        let other = MergeCleanupModel(client: AgentOpsClient(channel: refused), sessionId: 7)
        await other.prepare()
        _ = await other.run()
        XCTAssertFalse(other.conflicted)
        XCTAssertEqual(other.errorText, AgentOpsClient.describe(code: "merge_failed"))
    }

    @MainActor func testAFailedMergeSaysWhyWhenTheDesktopDoes() async {
        let channel = FakeAgentOpsChannel()
        channel.reply("agentops.prepare", confirmationJSON("merge", typeToConfirm: "agent/fix"))
        channel.failures["agentops.merge"] = LocalControlException(
            "merge_failed", message: " Couldn't merge agent/fix: main has uncommitted changes. ")
        let model = MergeCleanupModel(client: AgentOpsClient(channel: channel), sessionId: 7)
        await model.prepare()
        let ok = await model.run()
        XCTAssertFalse(ok)
        XCTAssertEqual(
            model.errorText, "The merge did not finish. Couldn't merge agent/fix: main has uncommitted changes.")
        XCTAssertEqual(
            AgentOpsClient.describe(LocalControlException("merge_failed", message: "  ")),
            AgentOpsClient.describe(code: "merge_failed"))
    }

    @MainActor func testADirtyCheckoutOffersToSetAsideAndMerge() async {
        let channel = FakeAgentOpsChannel()
        channel.reply("agentops.prepare", confirmationJSON("merge", typeToConfirm: "agent/fix"))
        channel.failures["agentops.merge"] = LocalControlException(
            "checkout_dirty", message: "Can't merge agent/fix yet: notes.txt has uncommitted changes.")
        let model = MergeCleanupModel(client: AgentOpsClient(channel: channel), sessionId: 7)
        let refused = await model.mergeNow()
        XCTAssertFalse(refused)
        XCTAssertTrue(model.canSetAside)
        XCTAssertFalse(model.conflicted)
        XCTAssertEqual(
            model.errorText, "The merge did not finish. Can't merge agent/fix yet: notes.txt has uncommitted changes.")
        XCTAssertEqual(channel.params("agentops.merge")?["offerSetAside"], .bool(true))
        XCTAssertNil(channel.params("agentops.merge")?["setAside"], "a plain merge never sets anything aside")

        channel.failures["agentops.merge"] = nil
        channel.reply(
            "agentops.merge",
            ["branch": "agent/fix", "merged": true, "setAside": "A copy of all uncommitted work is on branch wip/x."])
        let ok = await model.mergeNow(setAside: true)
        XCTAssertTrue(ok)
        XCTAssertEqual(channel.params("agentops.merge")?["setAside"], .bool(true))
        XCTAssertFalse(model.canSetAside)
        XCTAssertEqual(model.setAsideNote, "A copy of all uncommitted work is on branch wip/x.")
        XCTAssertEqual(
            model.outcome.message(),
            "Merged agent/fix and cleaned up. A copy of all uncommitted work is on branch wip/x.")
    }

    @MainActor func testOtherFailuresDoNotOfferToSetAside() async {
        let channel = FakeAgentOpsChannel()
        channel.reply("agentops.prepare", confirmationJSON("merge", typeToConfirm: "agent/fix"))
        channel.failures["agentops.merge"] = LocalControlException("merge_failed", message: "main is on another branch")
        let model = MergeCleanupModel(client: AgentOpsClient(channel: channel), sessionId: 7)
        _ = await model.mergeNow()
        XCTAssertFalse(model.canSetAside)
    }

    @MainActor func testConflictFixSendsOnceAndExplainsAFailure() async {
        let fix = ConflictFixModel()
        let failed = await fix.send { throw FakeControlError(code: "not_connected") }
        XCTAssertFalse(failed)
        XCTAssertEqual(fix.errorText, AgentOpsClient.describe(code: "not_connected"))
        var calls = 0
        let ok = await fix.send { calls += 1 }
        XCTAssertTrue(ok)
        XCTAssertTrue(fix.sent)
        XCTAssertEqual(fix.errorText, "")
        let again = await fix.send { calls += 1 }
        XCTAssertFalse(again)
        XCTAssertEqual(calls, 1)
    }

    @MainActor func testMergeFailureExplains() async {
        let channel = FakeAgentOpsChannel()
        channel.failures["agentops.prepare"] = FakeControlError(code: "agent_running")
        let model = MergeCleanupModel(client: AgentOpsClient(channel: channel), sessionId: 1)
        await model.prepare()
        XCTAssertNil(model.confirmation)
        XCTAssertEqual(model.errorText, AgentOpsClient.describe(code: "agent_running"))
    }

    @MainActor func testBulkUsesSelectionOrEveryEligibleRow() async {
        let channel = FakeAgentOpsChannel()
        channel.reply("agentops.prepare", confirmationJSON("stop"))
        channel.reply("agentops.bulk", ["done": [1], "skipped": []])
        let model = BulkActionsModel(client: AgentOpsClient(channel: channel))
        let rows = [
            BulkCandidate(id: 1, isActive: true, stoppable: true),
            BulkCandidate(id: 2, isActive: false, stoppable: false),
            BulkCandidate(id: 3, isActive: true, stoppable: false),
            BulkCandidate(id: 4, isActive: false, stoppable: false, merged: true),
        ]
        XCTAssertEqual(model.eligible(.stop, in: rows), [1])
        XCTAssertEqual(model.eligible(.continueRun, in: rows), [2])
        model.toggle(2)
        XCTAssertEqual(model.eligible(.stop, in: rows), [])
        model.toggle(2)
        await model.prepare(.stop, in: rows)
        XCTAssertEqual(model.pendingAction, .stop)
        let outcome = await model.run()
        XCTAssertEqual(outcome?.done, [1])
        XCTAssertEqual(channel.params("agentops.bulk")?["ids"], [1])
        XCTAssertNil(model.confirmation)
    }

    @MainActor func testBulkWithNothingEligibleSendsNothing() async {
        let channel = FakeAgentOpsChannel()
        let model = BulkActionsModel(client: AgentOpsClient(channel: channel))
        await model.prepare(.stop, in: [BulkCandidate(id: 1, isActive: false, stoppable: false)])
        XCTAssertTrue(channel.requests.isEmpty)
        XCTAssertEqual(model.errorText, "Nothing to stop.")
    }

    @MainActor func testUsageAndWorktreesModels() async {
        let channel = FakeAgentOpsChannel()
        channel.reply("agentops.usage", ["providers": [["id": "codex", "label": "Codex", "windows": []]]])
        channel.reply(
            "agentops.worktrees",
            ["worktrees": [["name": "app", "branch": "main", "isMain": true], ["name": "wt", "branch": "agent/x"]]])
        let client = AgentOpsClient(channel: channel)
        let usage = AgentUsageModel(client: client)
        await usage.refresh()
        XCTAssertEqual(usage.summary?.providers.first?.id, "codex")
        let worktrees = WorktreesModel(client: client, owner: "me", name: "app")
        await worktrees.refresh()
        XCTAssertTrue(worktrees.loaded)
        XCTAssertEqual(worktrees.agentWorktrees.map(\.branch), ["agent/x"])
    }

    @MainActor func testGrantsRevokeRemovesTheRow() async {
        let channel = FakeAgentOpsChannel()
        channel.reply("agentops.grants", ["grants": [["id": "g1", "capability": "process.execute"]]])
        channel.reply("agentops.prepare", confirmationJSON("revoke"))
        let model = CapabilityGrantsModel(client: AgentOpsClient(channel: channel))
        await model.refresh()
        let grant = model.grants[0]
        let confirmation = await model.prepareRevoke(grant)
        XCTAssertNotNil(confirmation)
        let ok = await model.revoke(grant, confirmation: confirmation!)
        XCTAssertTrue(ok)
        XCTAssertTrue(model.grants.isEmpty)
        XCTAssertEqual(channel.params("agentops.revoke")?.str("grantId"), "g1")
    }

    func testSortOrdersCoverTheDesktopMenu() {
        XCTAssertEqual(AgentListSort.allCases.count, 12)
        let a = AgentListItem(
            session: AgentSession(id: 1, issueTitle: "beta", status: "running", createdAtMs: 100, costUsd: 1, totalTokens: 50,
                                  filesChanged: 3),
            priority: 10)
        let b = AgentListItem(
            session: AgentSession(id: 2, issueNumber: 4, issueTitle: "Alpha", status: "success", createdAtMs: 200,
                                  finishedAtMs: 900, costUsd: 5, totalTokens: 10, filesChanged: 9),
            priority: 90)
        let c = AgentListItem(
            session: AgentSession(id: 3, issueTitle: "gamma", status: "waiting", createdAtMs: 50, durationMs: 5_000),
            needsYou: true)
        let items = [a, b, c]
        func ids(_ sort: AgentListSort) -> [Int] { AgentListOrdering.sorted(items, by: sort, nowMs: 1_000).map(\.session.id) }
        XCTAssertEqual(ids(.priority), [1, 3, 2])
        XCTAssertEqual(AgentListItem(session: AgentSession(id: 4, priority: 7)).priority, 7)
        XCTAssertEqual(AgentSession(json: ["id": .int(5), "priority": .int(250)]).priority, 99)
        XCTAssertEqual(AgentSession(json: ["id": .int(6)]).priority, 50)
        XCTAssertEqual(ids(.newestAdded), [2, 1, 3])
        XCTAssertEqual(ids(.oldestAdded), [3, 1, 2])
        XCTAssertEqual(ids(.recentActivity), [2, 1, 3])
        XCTAssertEqual(ids(.status), [3, 1, 2])
        XCTAssertEqual(ids(.title), [2, 1, 3])
        XCTAssertEqual(ids(.issueNumber).first, 2)
        XCTAssertEqual(ids(.tokenUsage).first, 1)
        XCTAssertEqual(ids(.cost).first, 2)
        XCTAssertEqual(ids(.runtime).first, 3)
        XCTAssertEqual(ids(.filesChanged).first, 2)
        let wrapped = AgentListOrdering.sorted(["x", "y", "z"], by: .newestAdded) { name -> AgentListItem in
            name == "x" ? a : name == "y" ? b : c
        }
        XCTAssertEqual(wrapped, ["y", "x", "z"])
    }

    func testFiltersMatchTheAgentsRoutes() {
        let running = AgentListItem(session: AgentSession(id: 1, status: "running", createdAtMs: 100))
        let queued = AgentListItem(session: AgentSession(id: 2, status: "queued", createdAtMs: 100))
        let waiting = AgentListItem(session: AgentSession(id: 3, status: "waiting", createdAtMs: 100))
        let old = AgentListItem(session: AgentSession(id: 4, status: "success", createdAtMs: 100, finishedAtMs: 200))
        let items = [running, queued, waiting, old]
        func ids(_ filter: AgentFilter, now: Int = 0) -> [Int] {
            AgentListOrdering.apply(items, filter: filter, sort: .oldestAdded, nowMs: now).map(\.session.id)
        }
        XCTAssertEqual(ids(.all), [1, 2, 3, 4])
        XCTAssertEqual(ids(.needsYou), [3])
        XCTAssertEqual(ids(.running), [1, 3])
        XCTAssertEqual(ids(.queued), [2])
        XCTAssertEqual(ids(.recent), [4])
        XCTAssertEqual(ids(.recent, now: 200 + 2 * 86_400_000), [])
    }
}
