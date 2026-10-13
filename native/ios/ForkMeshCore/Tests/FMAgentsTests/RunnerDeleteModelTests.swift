import FMApi
import FMBase
import XCTest
@testable import FMAgents

@MainActor
final class RunnerDeleteModelTests: XCTestCase {
    private final class FakeRunnerApi: RunnerAgentsApi {
        var calls: [String] = []
        var discardError: Error?
        var runningDiscards = 0

        func runnerAgentDiff(
            _ owner: String, _ repo: String, _ sessionId: String, page: Int, pageSize: Int, orgScope: Bool
        ) async throws -> RunnerAgentDiffPage {
            throw AgentLaunchException("unused")
        }

        func mergeRunnerAgent(
            _ owner: String, _ repo: String, _ sessionId: String, cleanup: Bool, orgScope: Bool
        ) async throws -> RunnerAgentMergeResult {
            throw AgentLaunchException("unused")
        }

        func cleanupRunnerAgent(
            _ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool
        ) async throws -> RunnerAgentMergeResult {
            throw AgentLaunchException("unused")
        }

        func discardRunnerAgentBranch(
            _ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool
        ) async throws -> RunnerAgentMergeResult {
            calls.append("discard \(sessionId)")
            if runningDiscards > 0 {
                runningDiscards -= 1
                throw AgentLaunchException("The agent is still running.", code: "session_running", status: 409)
            }
            if let discardError { throw discardError }
            return RunnerAgentMergeResult(json: ["status": "cleaned", "branchDeleted": true])
        }

        func stopAgentSession(_ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool) async throws
            -> AgentStopResult
        {
            calls.append("stop \(sessionId)")
            return AgentStopResult(json: ["stopped": false])
        }

        func cancelAgentSession(_ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool) async throws {
            calls.append("delete \(sessionId)")
        }

        func linkRunnerAgentPull(
            _ owner: String, _ repo: String, _ sessionId: String, number: Int, orgScope: Bool
        ) async throws -> RunnerAgentPullLink {
            throw AgentLaunchException("unused")
        }

        func agentLaunchSession(_ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool) async throws
            -> AgentLaunchSession
        {
            throw AgentLaunchException("unused")
        }

        func compare(_ owner: String, _ name: String, base: String, head: String) async throws -> RepoComparison {
            throw AgentLaunchException("unused")
        }

        func runnerDefaultBranch(_ owner: String, _ repo: String) async -> String { "main" }

        func updateRunnerAgent(
            _ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool
        ) async throws -> RunnerAgentMergeResult {
            throw AgentLaunchException("unused")
        }

        func completeTask(_ taskId: String, note: String) async throws {}
    }

    private let ref = RunnerAgentRef(owner: "alice", repo: "site", sessionId: "s1", branch: "agent/12-fix-login")

    func testDeleteRemovesTheBranchBeforeTheRun() async {
        let api = FakeRunnerApi()
        let model = RunnerDeleteModel(api: api, ref: ref)
        let ok = await model.run()
        XCTAssertTrue(ok)
        XCTAssertEqual(api.calls, ["discard s1", "delete s1"])
        XCTAssertTrue(model.note.contains("is deleted on ForkMesh"))
    }

    func testTheDeleteSaysWhatWentAndWhatStayed() async {
        let api = FakeRunnerApi()
        var withPull = ref
        withPull.pull = 31
        let model = RunnerDeleteModel(api: api, ref: withPull)
        _ = await model.run()
        XCTAssertEqual(
            model.report.summary(place: "on ForkMesh"),
            "Deleted the agent on ForkMesh with branch agent/12-fix-login. "
                + "Kept pull request #31: the phone cannot close it.")
    }

    func testABranchAlreadyGoneIsReportedAsGone() async {
        let api = FakeRunnerApi()
        api.discardError = AgentLaunchException("This run has no agent branch.", code: "no_branch", status: 409)
        let model = RunnerDeleteModel(api: api, ref: ref)
        _ = await model.run()
        XCTAssertEqual(
            model.report.summary(),
            "Deleted the agent. Already gone: branch agent/12-fix-login. No pull request was attached.")
    }

    func testARunWithoutABranchOnlyDeletesTheRun() async {
        let api = FakeRunnerApi()
        let model = RunnerDeleteModel(api: api, ref: RunnerAgentRef(owner: "alice", repo: "site", sessionId: "s1"))
        let ok = await model.run()
        XCTAssertTrue(ok)
        XCTAssertEqual(api.calls, ["delete s1"])
    }

    func testABranchTheServerDoesNotKnowStillDeletesTheRun() async {
        let api = FakeRunnerApi()
        api.discardError = AgentLaunchException("This run has no agent branch.", code: "no_branch", status: 409)
        let ok = await RunnerDeleteModel(api: api, ref: ref).run()
        XCTAssertTrue(ok)
        XCTAssertEqual(api.calls, ["discard s1", "delete s1"])
    }

    func testAMemberWhoMayNotDeleteBranchesStillDeletesTheirRun() async {
        let api = FakeRunnerApi()
        api.discardError = AgentLaunchException("Only an owner.", code: "forbidden", status: 403)
        let ok = await RunnerDeleteModel(api: api, ref: ref).run()
        XCTAssertTrue(ok)
        XCTAssertEqual(api.calls, ["discard s1", "delete s1"])
    }

    func testABranchThatCouldNotBeDeletedKeepsTheRun() async {
        let api = FakeRunnerApi()
        api.discardError = AgentLaunchException("Try again.", code: "branch_delete_pending", status: 503)
        let model = RunnerDeleteModel(api: api, ref: ref)
        let ok = await model.run()
        XCTAssertFalse(ok)
        XCTAssertFalse(model.deleted)
        XCTAssertEqual(api.calls, ["discard s1"])
        XCTAssertFalse(model.errorText.isEmpty)
    }

    func testARunningRunIsStoppedBeforeItsBranchAndRunGo() async {
        let api = FakeRunnerApi()
        let ok = await RunnerDeleteModel(api: api, ref: ref, active: true, pause: { _ in }).run()
        XCTAssertTrue(ok)
        XCTAssertEqual(api.calls, ["stop s1", "discard s1", "delete s1"])
    }

    func testARunStillStoppingIsWaitedForBeforeTheBranchGoes() async {
        let api = FakeRunnerApi()
        api.runningDiscards = 2
        var pauses: [Int] = []
        let ok = await RunnerDeleteModel(api: api, ref: ref, pause: { pauses.append($0) }).run()
        XCTAssertTrue(ok)
        XCTAssertEqual(api.calls, ["discard s1", "stop s1", "discard s1", "discard s1", "delete s1"])
        XCTAssertEqual(pauses, [RunnerDeleteModel.discardPauseMs, RunnerDeleteModel.discardPauseMs])
    }

    func testARunThatNeverStopsIsStillDeleted() async {
        let api = FakeRunnerApi()
        api.runningDiscards = RunnerDeleteModel.discardTries
        let ok = await RunnerDeleteModel(api: api, ref: ref, active: true, pause: { _ in }).run()
        XCTAssertTrue(ok)
        XCTAssertEqual(api.calls.filter { $0 == "discard s1" }.count, RunnerDeleteModel.discardTries)
        XCTAssertEqual(api.calls.last, "delete s1")
    }
}
