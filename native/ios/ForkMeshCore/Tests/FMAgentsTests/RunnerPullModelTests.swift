import FMApi
import FMBase
import XCTest
@testable import FMAgents

private let pullBaseOid = String(repeating: "a", count: 40)
private let pullHeadOid = String(repeating: "b", count: 40)

@MainActor
final class RunnerPullModelTests: XCTestCase {
    private final class FakeApi: RunnerAgentsApi {
        var calls: [String] = []
        var session = AgentLaunchSession(sessionId: "s1")
        var comparison = RepoComparison(
            baseOid: pullBaseOid, headOid: pullHeadOid, patch: "diff --git a/x b/x\n",
            commits: "From abc")
        var compareError: Error?
        var linkError: Error?

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
            calls.append("link \(sessionId) #\(number)")
            if let linkError { throw linkError }
            return RunnerAgentPullLink(json: [
                "ok": true, "status": "linked", "prNumber": .int(Int64(number)),
                "message": .string("Opened pull request #\(number) for agent/12-fix-login."),
            ])
        }

        func agentLaunchSession(_ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool) async throws
            -> AgentLaunchSession
        {
            calls.append("session \(sessionId)")
            return session
        }

        func compare(_ owner: String, _ name: String, base: String, head: String) async throws -> RepoComparison {
            calls.append("compare \(base)...\(head)")
            if let compareError { throw compareError }
            return comparison
        }

        func runnerDefaultBranch(_ owner: String, _ repo: String) async -> String {
            calls.append("default")
            return "trunk"
        }

        func updateRunnerAgent(
            _ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool
        ) async throws -> RunnerAgentMergeResult {
            throw AgentLaunchException("unused")
        }

        func completeTask(_ taskId: String, note: String) async throws {}
    }

    private final class FakeInbox: RunnerPullInbox {
        var opened: [(title: String, base: String, head: String, patch: String, commits: String, description: String)] = []
        var number = 31
        var error: Error?

        func openNewPull(
            _ owner: String, _ name: String, title: String, base: String, head: String, patch: String, commits: String,
            description: String, authorName: String
        ) async throws -> Int {
            if let error { throw error }
            opened.append((title, base, head, patch, commits, description))
            return number
        }
    }

    private let ref = RunnerAgentRef(owner: "alice", repo: "site", sessionId: "s1", branch: "agent/12-fix-login")

    private func runnerSession() -> AgentLaunchSession {
        var session = AgentLaunchSession(
            sessionId: "s1", owner: "alice", repo: "site", status: "completed", targetNode: "fmnext-n1",
            title: "Fix the login page", branchName: "agent/12-fix-login", baseBranch: "main")
        session.runner = "fmnext-r1"
        session.history = [
            AgentLaunchMessage(role: "user", text: "Fix the login page"),
            AgentLaunchMessage(role: "assistant", text: "  Fixed the redirect.  "),
            AgentLaunchMessage(role: "assistant", text: "   "),
        ]
        return session
    }

    func testCreatePullOpensItThenLinksTheRun() async {
        let api = FakeApi()
        api.session = runnerSession()
        let inbox = FakeInbox()
        let model = RunnerPullModel(api: api, inbox: inbox, ref: ref)
        let ok = await model.open()
        XCTAssertTrue(ok)
        XCTAssertEqual(model.number, 31)
        XCTAssertEqual(model.errorText, "")
        XCTAssertEqual(model.notice, "Opened pull request #31 for agent/12-fix-login.")
        XCTAssertEqual(api.calls, ["session s1", "compare main...agent/12-fix-login", "link s1 #31"])
        let pull = inbox.opened.first
        XCTAssertEqual(pull?.title, "Fix the login page")
        XCTAssertEqual(pull?.base, "main")
        XCTAssertEqual(pull?.head, "agent/12-fix-login")
        XCTAssertEqual(pull?.patch, "diff --git a/x b/x\n")
        XCTAssertEqual(pull?.commits, "From abc")
        XCTAssertEqual(
            pull?.description,
            "Opened from the agent run on the fmnext-r1 runner (agent/12-fix-login).\n\nFixed the redirect.")
    }

    func testAMissingBaseFallsBackToTheRepositoryDefault() async {
        let api = FakeApi()
        var session = runnerSession()
        session.baseBranch = ""
        session.title = ""
        api.session = session
        let inbox = FakeInbox()
        _ = await RunnerPullModel(api: api, inbox: inbox, ref: ref).open()
        XCTAssertEqual(api.calls.prefix(3), ["session s1", "default", "compare trunk...agent/12-fix-login"])
        XCTAssertEqual(inbox.opened.first?.title, "agent/12-fix-login")
    }

    func testABranchWithNoChangesOpensNothing() async {
        let api = FakeApi()
        api.session = runnerSession()
        api.comparison.patch = "  \n"
        let inbox = FakeInbox()
        let model = RunnerPullModel(api: api, inbox: inbox, ref: ref)
        let ok = await model.open()
        XCTAssertFalse(ok)
        XCTAssertTrue(inbox.opened.isEmpty)
        XCTAssertEqual(
            model.errorText,
            "agent/12-fix-login has no changes against main, so there is nothing to open a pull request for.")
    }

    func testABranchTheCompareCannotFindOpensNothing() async {
        let api = FakeApi()
        api.session = runnerSession()
        api.compareError = ApiException("HTTP 404 for /api/repo/alice/site/compare")
        let inbox = FakeInbox()
        let model = RunnerPullModel(api: api, inbox: inbox, ref: ref)
        let ok = await model.open()
        XCTAssertFalse(ok)
        XCTAssertTrue(inbox.opened.isEmpty)
        XCTAssertEqual(model.errorText, "agent/12-fix-login is not on ForkMesh yet, so no pull request was opened.")
    }

    func testALinkFailureAfterThePullOpenedStillNamesThePull() async {
        let api = FakeApi()
        api.session = runnerSession()
        api.linkError = AgentLaunchException(
            "Pull request #9 is already open for agent/12-fix-login.", code: "pull_exists", status: 409)
        let model = RunnerPullModel(api: api, inbox: FakeInbox(), ref: ref)
        let ok = await model.open()
        XCTAssertTrue(ok)
        XCTAssertEqual(model.number, 31)
        XCTAssertEqual(
            model.errorText,
            "Opened pull request #31, but the run could not be linked to it: "
                + "Pull request #9 is already open for agent/12-fix-login.")
    }

    func testAnInboxRefusalIsReported() async {
        let api = FakeApi()
        api.session = runnerSession()
        let inbox = FakeInbox()
        inbox.error = ApiException("inbox_full")
        let model = RunnerPullModel(api: api, inbox: inbox, ref: ref)
        let ok = await model.open()
        XCTAssertFalse(ok)
        XCTAssertEqual(model.errorText, "The pull request could not be opened (inbox_full).")
        XCTAssertFalse(api.calls.contains { $0.hasPrefix("link") })
    }

    func testARunThatAlreadyHasAPullOpensNoOther() async {
        let api = FakeApi()
        var session = runnerSession()
        session.prNumber = 9
        api.session = session
        let inbox = FakeInbox()
        let model = RunnerPullModel(api: api, inbox: inbox, ref: ref)
        let ok = await model.open()
        XCTAssertTrue(ok)
        XCTAssertEqual(model.number, 9)
        XCTAssertTrue(inbox.opened.isEmpty)
        XCTAssertEqual(api.calls, ["session s1"])
    }

    func testWithoutAnInboxNothingIsSent() async {
        let api = FakeApi()
        let model = RunnerPullModel(api: api, inbox: nil, ref: ref)
        let ok = await model.open()
        XCTAssertFalse(ok)
        XCTAssertTrue(api.calls.isEmpty)
        XCTAssertFalse(model.errorText.isEmpty)
    }
}
