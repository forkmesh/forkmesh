import FMAccount
import FMApi
import FMBase
import FMNet
import Foundation
import XCTest

@testable import FMCode

final class ActionsRunnerTests: XCTestCase {
    private let repo = RepoRef(owner: "alice", name: "tool")
    private static let head = String(repeating: "c", count: 40)

    @MainActor private func api(_ http: FakeHTTPClient) -> ApiService {
        let api = ApiService(settings: StaticApiSettings(serverUrl: "wss://ci.test/ws"), http: http, sleep: { _ in })
        api.sessionTokenProvider = { "session-token" }
        return api
    }

    private func body(_ request: HTTPRequest?) -> JSONObject {
        guard let data = request?.body, let parsed = try? JSON.parse(data) else { return [:] }
        return parsed.objectValue ?? [:]
    }

    private static func runnerSummary(status: String = "running", controlAvailable: Bool = true) -> JSONValue {
        .object([
            "ok": true, "node": "runner", "local": true, "updatedAt": 10, "source": "runner",
            "controlAvailable": .bool(controlAvailable), "access": "member",
            "workflows": .array([
                .object(["path": ".forkmesh/ci.yml", "name": "CI", "events": .array(["push", "pull_request"]),
                         "schedules": .array([]), "inputs": .array([])]),
                .object(["path": ".forkmesh/deploy.yml", "name": "Deploy", "events": .array(["workflow_dispatch"]),
                         "schedules": .array([]), "inputs": .array([])]),
            ]),
            "runs": .array([
                .object(["id": 501, "workflow": "CI", "workflowPath": ".forkmesh/ci.yml", "commit": .string(head),
                         "ref": "refs/heads/pr/bob/fix", "status": .string(status), "createdAt": 5_000, "startedAt": 5_000,
                         "logTail": "tail only", "pullNumber": 9]),
            ]),
        ])
    }

    private static func checksPayload(
        required: Bool, reason: String = "author", author: String = "bob", trust: String = "", canApprove: Bool = true,
        approvedBy: String = ""
    ) -> JSONValue {
        .object([
            "ok": true, "number": 9, "configured": true, "head": .string(head),
            "runs": .array([
                .object(["id": 501, "workflow": "CI", "workflowPath": ".forkmesh/ci.yml",
                         "status": .string(required ? "awaiting-approval" : "success"), "commit": .string(head),
                         "createdAt": 5_000, "startedAt": 0, "finishedAt": 0]),
            ]),
            "approval": .object(["required": .bool(required), "reason": .string(required ? reason : ""), "author": .string(author),
                                 "authorTrust": .string(trust), "approvedBy": .string(approvedBy)]),
            "canApprove": .bool(canApprove), "viewer": "alice", "previousHeads": 1,
        ])
    }

    func testRunnerSummaryParsesSourceWorkflowsAndControl() {
        guard case let .object(json) = ActionsRunnerTests.runnerSummary() else { return XCTFail() }
        let summary = ActionsSummary(json: json)
        XCTAssertEqual(summary.source, "runner")
        XCTAssertTrue(summary.isRunner)
        XCTAssertTrue(summary.controlAvailable)
        XCTAssertNil(summary.actionsEnabled)
        XCTAssertEqual(summary.workflows.map(\.path), [".forkmesh/ci.yml", ".forkmesh/deploy.yml"])
        XCTAssertEqual(summary.workflows.filter(\.canDispatch).map(\.path), [".forkmesh/deploy.yml"])
        let other = ActionsSummary(json: ["ok": true, "schemaVersion": 2, "node": "n", "controlAvailable": true, "source": "mirror"])
        XCTAssertFalse(other.isRunner)
        XCTAssertTrue(other.controlAvailable)
        XCTAssertEqual(other.actionsEnabled, false)
        let stamped = ActionsSummary(json: ["ok": true, "schemaVersion": 2, "source": "runner", "controlAvailable": true])
        XCTAssertNil(stamped.actionsEnabled)
        let publicView = ActionsSummary(json: ["ok": true, "access": "public", "source": "runner", "controlAvailable": false])
        XCTAssertFalse(publicView.controlAvailable)
    }

    @MainActor func testRunnerSummaryHidesTheSwitchesTheServerRefuses() async {
        let http = FakeHTTPClient()
        http.on("GET", path: "/api/repo/alice/tool/actions/runs", json: ActionsRunnerTests.runnerSummary())
        let model = RepoActionsModel(client: api(http), repo: repo, makeRequestId: { "req_0123456789abcdef" })
        await model.load()
        XCTAssertTrue(model.canControl)
        XCTAssertFalse(model.canSwitchWorkflows)
        XCTAssertFalse(model.canSwitchRepository)
        XCTAssertEqual(model.switchesUnavailableMessage, RepoActionsModel.runnerSwitchMessage)
        XCTAssertEqual(model.emptyMessage, "No workflow runs yet.")
        XCTAssertEqual(model.dispatchableWorkflows.map(\.path), [".forkmesh/deploy.yml"])
        let switched = await model.setEnabled(model.workflows[0], false)
        XCTAssertFalse(switched)
        XCTAssertFalse(http.requests.contains { $0.method == "POST" })

        let mirror = FakeHTTPClient()
        mirror.on("GET", path: "/api/repo/alice/tool/actions/runs", json: [
            "ok": true, "schemaVersion": 2, "node": "alice-mac", "controlAvailable": true, "actionsEnabled": true,
            "source": "mirror", "workflows": .array([]), "runs": .array([]),
        ])
        let other = RepoActionsModel(client: api(mirror), repo: repo)
        await other.load()
        XCTAssertTrue(other.canSwitchWorkflows)
        XCTAssertTrue(other.canSwitchRepository)
    }

    @MainActor func testAnAppliedRunnerControlSettlesAtOnceAndReloads() async {
        let http = FakeHTTPClient()
        http.on("GET", path: "/api/repo/alice/tool/actions/runs", json: ActionsRunnerTests.runnerSummary())
        http.on("POST", path: "/api/repo/alice/tool/actions/control", status: 202,
                json: ["ok": true, "status": "applied", "action": "cancel", "runId": 501])
        let model = RepoActionsModel(client: api(http), repo: repo, makeRequestId: { "req_0123456789abcdef" })
        await model.load()
        XCTAssertEqual(model.actions(for: model.run(501)!), [.cancel])
        let sent = await model.send(.cancel, runId: 501)
        XCTAssertTrue(sent)
        XCTAssertEqual(body(http.requests.first { $0.method == "POST" })["node"], "runner")
        XCTAssertFalse(model.isPending(runId: 501))
        XCTAssertEqual(model.notice, ActionsNotice(text: "Cancel applied.", good: true))
        XCTAssertEqual(http.requests.filter { $0.method == "GET" }.count, 2)
    }

    @MainActor func testRunnerRunLogsLoadFollowAndStopOnceFinished() async {
        let http = FakeHTTPClient()
        http.on("GET", path: "/api/repo/alice/tool/actions/runs", json: ActionsRunnerTests.runnerSummary())
        http.on("GET", path: "/api/repo/alice/tool/actions/runs/501/log",
                json: ["ok": true, "runId": 501, "log": "step 1\nstep 2\n", "updatedAt": 6_000])
        let model = RepoActionsModel(client: api(http), repo: repo)
        await model.load()
        XCTAssertFalse(model.hasFullLog(501))
        XCTAssertEqual(model.logLines(501), ["tail only"])
        await model.follow(runId: 501)
        XCTAssertTrue(model.hasFullLog(501))
        XCTAssertEqual(model.run(501)?.logTail, "step 1\nstep 2\n")
        XCTAssertEqual(model.logLines(501), ["step 1", "step 2"])
        XCTAssertEqual(model.fullLog(501), "step 1\nstep 2\n")
        XCTAssertEqual(http.lastRequest?.headers["Authorization"], "Bearer session-token")

        http.on("GET", path: "/api/repo/alice/tool/actions/runs", json: ActionsRunnerTests.runnerSummary(status: "failed"))
        http.on("GET", path: "/api/repo/alice/tool/actions/runs/501/log",
                json: ["ok": true, "runId": 501, "log": "step 1\nstep 2\nerror: boom\n", "updatedAt": 7_000])
        await model.load()
        await model.loadLog(runId: 501)
        XCTAssertEqual(model.logLines(501).last, "error: boom")
        let fetched = http.requests.filter { $0.url.path.hasSuffix("/log") }.count
        await model.loadLog(runId: 501)
        XCTAssertEqual(http.requests.filter { $0.url.path.hasSuffix("/log") }.count, fetched)
    }

    @MainActor func testRunnerLogErrorsKeepTheTailAndSayWhy() async {
        let http = FakeHTTPClient()
        http.on("GET", path: "/api/repo/alice/tool/actions/runs", json: ActionsRunnerTests.runnerSummary())
        http.on("GET", path: "/api/repo/alice/tool/actions/runs/501/log", status: 404, json: ["error": "run_not_found"])
        let model = RepoActionsModel(client: api(http), repo: repo)
        await model.load()
        await model.loadLog(runId: 501)
        XCTAssertEqual(model.logProblem, "This run was not found.")
        XCTAssertEqual(model.run(501)?.logTail, "tail only")
        XCTAssertFalse(model.hasFullLog(501))

        let other = FakeHTTPClient()
        other.on("GET", path: "/api/repo/alice/tool/actions/runs", json: [
            "ok": true, "schemaVersion": 2, "node": "alice-mac", "controlAvailable": true, "runs": .array([
                .object(["id": 7, "workflow": "CI", "status": "running", "createdAt": 1]),
            ]),
        ])
        let mirror = RepoActionsModel(client: api(other), repo: repo)
        await mirror.load()
        await mirror.loadLog(runId: 7)
        XCTAssertFalse(other.requests.contains { $0.url.path.hasSuffix("/log") })
    }

    @MainActor func testPullChecksButtonsFollowTheApprovalState() async {
        let http = FakeHTTPClient()
        http.on("GET", path: "/api/repo/alice/tool/pulls/9/checks", json: ActionsRunnerTests.checksPayload(required: true))
        let model = PullChecksModel(client: api(http), repo: repo, number: 9)
        await model.load()
        XCTAssertTrue(model.isVisible)
        XCTAssertEqual(model.actions, [.approve, .trust])
        XCTAssertEqual(model.checks?.approval.reasonLabel, "@bob is not trusted to run code here.")
        XCTAssertEqual(model.checks?.shortHead, "ccccccc")
        XCTAssertEqual(model.checks?.runs.first?.status, .awaitingApproval)
        XCTAssertEqual(PullChecksAction.trust.label(author: "bob"), "Always run for @bob")
        XCTAssertTrue(PullChecksAction.approve.needsAuthentication)
        XCTAssertFalse(PullChecksAction.untrust.needsAuthentication)

        http.on("GET", path: "/api/repo/alice/tool/pulls/9/checks",
                json: ActionsRunnerTests.checksPayload(required: true, reason: "workflows", trust: "writer"))
        await model.load()
        XCTAssertEqual(model.actions, [.approve])
        XCTAssertEqual(model.checks?.approval.reasonLabel, "This pull request changes files under .forkmesh/.")

        http.on("GET", path: "/api/repo/alice/tool/pulls/9/checks",
                json: ActionsRunnerTests.checksPayload(required: false, trust: "allowlist", approvedBy: "alice"))
        await model.load()
        XCTAssertEqual(model.actions, [.untrust])
        XCTAssertEqual(model.checks?.approval.approvedBy, "alice")

        http.on("GET", path: "/api/repo/alice/tool/pulls/9/checks", json: ActionsRunnerTests.checksPayload(required: false, author: ""))
        await model.load()
        XCTAssertEqual(model.actions, [])

        http.on("GET", path: "/api/repo/alice/tool/pulls/9/checks",
                json: ActionsRunnerTests.checksPayload(required: true, canApprove: false))
        await model.load()
        XCTAssertEqual(model.actions, [])
        XCTAssertTrue(model.isVisible)
    }

    @MainActor func testNoRunnerOnThisInstanceShowsNothing() async {
        let http = FakeHTTPClient()
        http.on("GET", path: "/api/repo/alice/tool/pulls/9/checks", status: 404, json: ["error": "not_available"])
        let model = PullChecksModel(client: api(http), repo: repo, number: 9)
        await model.load()
        XCTAssertTrue(model.unavailable)
        XCTAssertFalse(model.isVisible)
        XCTAssertNil(model.error)
        XCTAssertEqual(model.actions, [])
    }

    @MainActor func testPullChecksActionsSendTheShownHeadAndRefresh() async {
        let http = FakeHTTPClient()
        http.on("GET", path: "/api/repo/alice/tool/pulls/9/checks", json: ActionsRunnerTests.checksPayload(required: true))
        http.on("POST", path: "/api/repo/alice/tool/pulls/9/checks", json: ["ok": true, "approved": 2])
        let model = PullChecksModel(client: api(http), repo: repo, number: 9)
        await model.load()
        let approved = await model.perform(.approve)
        XCTAssertTrue(approved)
        let post = http.requests.last { $0.method == "POST" }
        XCTAssertEqual(post?.url.path, "/api/repo/alice/tool/pulls/9/checks")
        XCTAssertEqual(body(post), ["action": "approve", "head": .string(ActionsRunnerTests.head)])
        XCTAssertEqual(model.notice, ActionsNotice(text: "Approved: 2 checks started.", good: true))
        XCTAssertEqual(http.lastRequest?.method, "GET")

        http.on("POST", path: "/api/repo/alice/tool/pulls/9/checks", json: ["ok": true, "trusted": "bob", "approved": 1])
        let trusted = await model.perform(.trust)
        XCTAssertTrue(trusted)
        XCTAssertEqual(body(http.requests.last { $0.method == "POST" }), ["action": "trust", "account": "bob"])
        XCTAssertEqual(model.notice?.text, "Checks now run automatically for @bob. Started 1 check.")

        http.on("GET", path: "/api/repo/alice/tool/pulls/9/checks",
                json: ActionsRunnerTests.checksPayload(required: false, trust: "allowlist"))
        await model.load()
        http.on("POST", path: "/api/repo/alice/tool/pulls/9/checks", json: ["ok": true, "untrusted": "bob"])
        let untrusted = await model.perform(.untrust)
        XCTAssertTrue(untrusted)
        XCTAssertEqual(body(http.requests.last { $0.method == "POST" }), ["action": "untrust", "account": "bob"])
        XCTAssertEqual(model.notice?.text, "Checks for @bob wait for approval again.")
    }

    @MainActor func testPullChecksErrorsAreExplainedAndReloaded() async {
        let http = FakeHTTPClient()
        http.on("GET", path: "/api/repo/alice/tool/pulls/9/checks", json: ActionsRunnerTests.checksPayload(required: true))
        http.on("POST", path: "/api/repo/alice/tool/pulls/9/checks", status: 409, json: ["error": "stale_head", "message": "newer"])
        let model = PullChecksModel(client: api(http), repo: repo, number: 9)
        await model.load()
        let stale = await model.perform(.approve)
        XCTAssertFalse(stale)
        XCTAssertEqual(model.notice, ActionsNotice(
            text: "Approve and run: The pull request has a newer commit. Review it before approving.", good: false))
        XCTAssertEqual(http.lastRequest?.method, "GET")
        XCTAssertEqual(CIError(status: 409, code: "not_awaiting_approval").description, "These checks are no longer waiting for approval.")
        XCTAssertEqual(CIError(status: 409, code: "author_unknown").description,
                       "Only the pull request's verified author can be trusted from here.")
        XCTAssertEqual(CIError(status: 404, code: "not_available").description, "This instance does not run checks for pull requests.")
        XCTAssertEqual(CIError(status: 404, code: "run_not_found").description, "This run was not found.")
        XCTAssertEqual(CIError(status: 403, code: "forbidden").description,
                       "Only the repository owner and organization writers can do that.")
    }

    @MainActor func testSecretsListSetReplaceAndDelete() async {
        let http = FakeHTTPClient()
        http.on("GET", path: "/api/repo/alice/tool/actions/secrets", json: [
            "ok": true, "secrets": .array([
                .object(["name": "TOKEN", "updatedAt": 1_000, "updatedBy": "alice"]),
                .object(["name": "API_KEY", "updatedAt": 2_000, "updatedBy": "bob"]),
                .object(["name": "bad name", "updatedAt": 3_000, "updatedBy": "eve"]),
            ]),
        ])
        http.on("PUT", path: "/api/repo/alice/tool/actions/secrets/DEPLOY_KEY", json: ["ok": true, "name": "DEPLOY_KEY", "updatedAt": 4_000])
        http.on("PUT", path: "/api/repo/alice/tool/actions/secrets/TOKEN", json: ["ok": true, "name": "TOKEN", "updatedAt": 5_000])
        http.on("DELETE", path: "/api/repo/alice/tool/actions/secrets/API_KEY", json: ["ok": true, "name": "API_KEY", "updatedAt": 6_000])
        let model = RepoActionSecretsModel(client: api(http), repo: repo)
        await model.load()
        XCTAssertEqual(model.secrets.map(\.name), ["API_KEY", "TOKEN"])
        XCTAssertEqual(model.secrets.first?.updatedBy, "bob")
        XCTAssertEqual(http.lastRequest?.headers["Authorization"], "Bearer session-token")

        let added = await model.save(name: " deploy_key ", value: "s3cret")
        XCTAssertTrue(added)
        let put = http.requests.last { $0.method == "PUT" }
        XCTAssertEqual(put?.url.path, "/api/repo/alice/tool/actions/secrets/DEPLOY_KEY")
        XCTAssertEqual(body(put), ["value": "s3cret"])
        XCTAssertEqual(put?.headers["Content-Type"], "application/json")
        XCTAssertEqual(model.notice?.text, "Added DEPLOY_KEY.")
        XCTAssertEqual(http.lastRequest?.method, "GET")

        let replaced = await model.save(name: "TOKEN", value: "new")
        XCTAssertTrue(replaced)
        XCTAssertEqual(model.notice?.text, "Replaced TOKEN.")

        let deleted = await model.delete("API_KEY")
        XCTAssertTrue(deleted)
        XCTAssertEqual(http.lastRequest?.method, "DELETE")
        XCTAssertEqual(http.lastRequest?.url.path, "/api/repo/alice/tool/actions/secrets/API_KEY")
        XCTAssertEqual(model.secrets.map(\.name), ["TOKEN"])
        XCTAssertEqual(model.notice?.text, "Deleted API_KEY.")
    }

    @MainActor func testSecretsValidateBeforeSendingAndExplainRefusals() async {
        XCTAssertTrue(RepoActionSecret.isValidName("_A1"))
        XCTAssertFalse(RepoActionSecret.isValidName("1ABC"))
        XCTAssertFalse(RepoActionSecret.isValidName("A-B"))
        XCTAssertTrue(RepoActionSecret.isValidName("A" + String(repeating: "B", count: 63)))
        XCTAssertFalse(RepoActionSecret.isValidName("A" + String(repeating: "B", count: 64)))
        XCTAssertNil(RepoActionSecret.valueProblem(String(repeating: "x", count: 48 * 1024)))
        XCTAssertNotNil(RepoActionSecret.valueProblem(String(repeating: "x", count: 48 * 1024 + 1)))
        XCTAssertNotNil(RepoActionSecret.valueProblem(""))

        let http = FakeHTTPClient()
        http.on("GET", path: "/api/repo/alice/tool/actions/secrets", status: 403, json: ["error": "forbidden"])
        let model = RepoActionSecretsModel(client: api(http), repo: repo)
        await model.load()
        XCTAssertEqual(model.error, "Only the repository owner and its admins can manage secrets.")
        XCTAssertFalse(model.loaded)

        let before = http.requests.count
        let badName = await model.save(name: "9LIVES", value: "x")
        XCTAssertFalse(badName)
        let emptyValue = await model.save(name: "OK", value: "")
        XCTAssertFalse(emptyValue)
        XCTAssertEqual(model.error, "Enter a value.")
        XCTAssertEqual(http.requests.count, before)

        http.on("PUT", path: "/api/repo/alice/tool/actions/secrets/MORE", status: 409, json: ["error": "too_many_secrets"])
        let full = await model.save(name: "MORE", value: "x")
        XCTAssertFalse(full)
        XCTAssertEqual(model.error, "This repository already has the most secrets it can have.")
        http.on("PUT", path: "/api/repo/alice/tool/actions/secrets/MORE", status: 400, json: ["error": "invalid_value"])
        _ = await model.save(name: "MORE", value: "x")
        XCTAssertEqual(model.error, "A secret value is 1 byte to 48 KiB of text.")
        http.on("PUT", path: "/api/repo/alice/tool/actions/secrets/MORE", status: 401, json: ["error": "invalid_session"])
        _ = await model.save(name: "MORE", value: "x")
        XCTAssertEqual(model.error, "Sign in again to manage this repository.")
        XCTAssertEqual(CIError(status: 400, code: "invalid_name").description,
                       "A secret name uses A-Z, 0-9 and _, does not start with a digit, and has at most 64 characters.")
    }

    func testApprovalNotificationParsesThePushPayload() {
        let payload: JSONObject = [
            "aps": ["category": "actions-approval", "interruption-level": "time-sensitive"], "alertId": "n1",
            "href": "/alice/tool/pull/9", "runId": "50100", "repo": "alice/tool", "number": "9",
            "head": .string(ActionsRunnerTests.head),
        ]
        let note = ActionsApprovalNotification(payload: payload)
        XCTAssertEqual(note?.repo, repo)
        XCTAssertEqual(note?.number, 9)
        XCTAssertEqual(note?.head, ActionsRunnerTests.head)
        XCTAssertEqual(note?.runId, 50100)
        XCTAssertEqual(note?.route, .pull(repo, number: 9, tab: .checks))
        XCTAssertEqual(note?.canApprove, true)
        XCTAssertNotNil(ActionsApprovalNotification(payload: [
            "category": "actions-approval", "meta": ["number": 9, "head": .string(ActionsRunnerTests.head)], "repo": "alice/tool",
        ]))
        XCTAssertNil(ActionsApprovalNotification(payload: ["aps": ["category": "ci-failed"], "repo": "alice/tool", "number": "9"]))
        XCTAssertNil(ActionsApprovalNotification(payload: ["aps": ["category": "actions-approval"], "repo": "alice/tool"]))
        XCTAssertNil(ActionsApprovalNotification(payload: ["aps": ["category": "actions-approval"], "repo": "tool", "number": "9"]))
        XCTAssertEqual(ActionsApprovalNotification(payload: [
            "aps": ["category": "actions-approval"], "repo": "alice/tool", "number": "9", "head": "short",
        ])?.canApprove, false)
    }

    @MainActor func testApproveFromTheNotificationOpensNothingOnSuccessAndThePullOnFailure() async {
        let http = FakeHTTPClient()
        http.on("POST", path: "/api/repo/alice/tool/pulls/9/checks", json: ["ok": true, "approved": 1])
        let note = ActionsApprovalNotification(repo: repo, number: 9, head: ActionsRunnerTests.head)
        let ok = await note.approve(client: api(http))
        XCTAssertNil(ok)
        XCTAssertEqual(body(http.lastRequest), ["action": "approve", "head": .string(ActionsRunnerTests.head)])

        for (status, code) in [(409, "stale_head"), (409, "not_awaiting_approval"), (403, "forbidden"), (401, "invalid_session")] {
            let failing = FakeHTTPClient()
            failing.on("POST", path: "/api/repo/alice/tool/pulls/9/checks", status: status, json: ["error": .string(code)])
            let route = await note.approve(client: api(failing))
            XCTAssertEqual(route, .pull(repo, number: 9, tab: .checks), code)
        }

        let unused = FakeHTTPClient()
        let route = await ActionsApprovalNotification(repo: repo, number: 9, head: "nothex").approve(client: api(unused))
        XCTAssertEqual(route, .pull(repo, number: 9, tab: .checks))
        XCTAssertTrue(unused.requests.isEmpty)
    }
}
