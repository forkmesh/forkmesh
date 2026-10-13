import FMApi
import FMBase
import XCTest
@testable import FMAgents

@MainActor
final class RunnerMergeModelTests: XCTestCase {
    private final class FakeApi: RunnerAgentsApi {
        var calls: [String] = []
        var mergeAnswer: JSONObject = [:]
        var updateAnswer: JSONObject = [:]
        var updateError: Error?
        var session = AgentLaunchSession(sessionId: "s1")
        var sessionError: Error?
        var completeError: Error?
        var notes: [String] = []

        func runnerAgentDiff(
            _ owner: String, _ repo: String, _ sessionId: String, page: Int, pageSize: Int, orgScope: Bool
        ) async throws -> RunnerAgentDiffPage {
            throw AgentLaunchException("unused")
        }

        func mergeRunnerAgent(
            _ owner: String, _ repo: String, _ sessionId: String, cleanup: Bool, orgScope: Bool
        ) async throws -> RunnerAgentMergeResult {
            calls.append("merge \(sessionId)")
            return RunnerAgentMergeResult(json: mergeAnswer)
        }

        func cleanupRunnerAgent(
            _ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool
        ) async throws -> RunnerAgentMergeResult {
            throw AgentLaunchException("unused")
        }

        func discardRunnerAgentBranch(
            _ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool
        ) async throws -> RunnerAgentMergeResult {
            throw AgentLaunchException("unused")
        }

        func stopAgentSession(_ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool) async throws
            -> AgentStopResult
        {
            AgentStopResult(json: ["stopped": false])
        }

        func cancelAgentSession(_ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool) async throws {}

        func linkRunnerAgentPull(
            _ owner: String, _ repo: String, _ sessionId: String, number: Int, orgScope: Bool
        ) async throws -> RunnerAgentPullLink {
            throw AgentLaunchException("unused")
        }

        func agentLaunchSession(_ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool) async throws
            -> AgentLaunchSession
        {
            calls.append("session \(sessionId)")
            if let sessionError { throw sessionError }
            return session
        }

        func compare(_ owner: String, _ name: String, base: String, head: String) async throws -> RepoComparison {
            throw AgentLaunchException("unused")
        }

        func runnerDefaultBranch(_ owner: String, _ repo: String) async -> String { "main" }

        func updateRunnerAgent(
            _ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool
        ) async throws -> RunnerAgentMergeResult {
            calls.append("update \(sessionId)")
            if let updateError { throw updateError }
            return RunnerAgentMergeResult(json: updateAnswer)
        }

        func completeTask(_ taskId: String, note: String) async throws {
            calls.append("complete \(taskId)")
            notes.append(note)
            if let completeError { throw completeError }
        }
    }

    private let taskId = "0123456789abcdef0123456789abcdef"
    private let ref = RunnerAgentRef(owner: "alice", repo: "site", sessionId: "s1", branch: "agent/12-fix-login", base: "main")
    private let merged: JSONObject = [
        "ok": true, "status": "merged", "branch": "agent/12-fix-login", "base": "main",
        "mergeOid": "abcdef0123456789abcdef0123456789abcdef01", "branchDeleted": true, "cleanupPending": [],
        "message": "Merged agent/12-fix-login into main and cleaned up.",
    ]

    func testAMergeCompletesTheLinkedTaskWithANote() async {
        let api = FakeApi()
        api.mergeAnswer = merged
        api.session = AgentLaunchSession(sessionId: "s1", title: "Fix login", taskKey: "task:\(taskId)")
        let model = RunnerMergeModel(api: api, ref: ref)
        let ok = await model.merge()
        XCTAssertTrue(ok)
        XCTAssertTrue(model.done)
        XCTAssertEqual(api.calls, ["merge s1", "session s1", "complete \(taskId)"])
        XCTAssertEqual(
            api.notes, ["Merged agent/12-fix-login into main (abcdef0). Merged agent/12-fix-login into main and cleaned up."])
        XCTAssertEqual(model.taskNote, RunnerAgentText.taskCompleted)
        XCTAssertEqual(model.mergeOid, "abcdef0123456789abcdef0123456789abcdef01")
        XCTAssertEqual(model.repo, RepoRef(owner: "alice", name: "site"))
        XCTAssertEqual(model.landedNotice, RunnerAgentText.taskCompleted)
    }

    func testAFailedTaskCompletionKeepsTheMerge() async {
        let api = FakeApi()
        api.mergeAnswer = merged
        api.session = AgentLaunchSession(sessionId: "s1", title: "[task:\(taskId)] Fix login")
        api.completeError = AgentLaunchException("Not a member.", code: "org_member_required", status: 403)
        let model = RunnerMergeModel(api: api, ref: ref)
        let ok = await model.merge()
        XCTAssertTrue(ok)
        XCTAssertTrue(model.done)
        XCTAssertTrue(model.errorText.isEmpty)
        XCTAssertEqual(model.taskNote, "The merge landed, but the linked task could not be marked done (Not a member.).")
    }

    func testARunWithoutATaskCompletesNothing() async {
        let api = FakeApi()
        api.mergeAnswer = merged
        api.session = AgentLaunchSession(sessionId: "s1", title: "Fix login")
        let model = RunnerMergeModel(api: api, ref: ref)
        _ = await model.merge()
        XCTAssertEqual(api.calls, ["merge s1", "session s1"])
        XCTAssertEqual(model.taskNote, "")

        let lost = FakeApi()
        lost.mergeAnswer = merged
        lost.sessionError = AgentLaunchException("gone")
        let quiet = RunnerMergeModel(api: lost, ref: ref)
        let ok = await quiet.merge()
        XCTAssertTrue(ok)
        XCTAssertEqual(quiet.taskNote, "")
    }

    func testUpdateFromBaseReportsTheOutcome() async {
        let api = FakeApi()
        api.updateAnswer = ["ok": true, "status": "updated", "branch": "agent/12-fix-login", "base": "main", "message": "Updated agent/12-fix-login from main."]
        let model = RunnerMergeModel(api: api, ref: ref)
        XCTAssertEqual(model.updateTitle, "Update from main")
        let ok = await model.update()
        XCTAssertTrue(ok)
        XCTAssertTrue(model.updated)
        XCTAssertFalse(model.done)
        XCTAssertEqual(model.successLine, "Updated agent/12-fix-login from main.")
        XCTAssertEqual(api.calls, ["update s1"])

        api.updateAnswer = ["ok": true, "status": "already"]
        let again = RunnerMergeModel(api: api, ref: ref)
        _ = await again.update()
        XCTAssertEqual(again.successLine, "agent/12-fix-login is already up to date with main.")
    }

    func testAnUpdateConflictOffersTheFix() async {
        let api = FakeApi()
        api.updateError = AgentLaunchException(
            "agent/12-fix-login conflicts with main. Conflicts: a.txt.", code: "merge_conflict", status: 409)
        let model = RunnerMergeModel(api: api, ref: ref)
        let ok = await model.update()
        XCTAssertFalse(ok)
        XCTAssertTrue(model.conflicted)
        XCTAssertEqual(model.errorText, "agent/12-fix-login conflicts with main. Conflicts: a.txt.")
        XCTAssertFalse(model.updated)
    }
}
