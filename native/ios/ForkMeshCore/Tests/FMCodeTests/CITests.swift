import FMAccount
import FMApi
import FMBase
import FMCrypto
import FMNet
import Foundation
import XCTest

@testable import FMCode

final class CITests: XCTestCase {
    private let repo = RepoRef(owner: "alice", name: "tool")

    @MainActor private func api(_ http: FakeHTTPClient) -> ApiService {
        let api = ApiService(settings: StaticApiSettings(serverUrl: "wss://ci.test/ws"), http: http, sleep: { _ in })
        api.sessionTokenProvider = { "session-token" }
        return api
    }

    private func body(_ request: HTTPRequest?) -> JSONObject {
        guard let data = request?.body, let parsed = try? JSON.parse(data) else { return [:] }
        return parsed.objectValue ?? [:]
    }

    private static let commit = String(repeating: "a", count: 40)

    private static func runsPayload(controls: [JSONValue] = [], status: String = "failed") -> JSONValue {
        .object([
            "ok": true, "schemaVersion": 2, "node": "alice-mac", "updatedAt": 10, "controlAvailable": true,
            "actionsEnabled": true,
            "workflows": .array([
                .object(["path": ".forkmesh/ci.yml", "name": "CI", "events": .array(["push"]), "manual": true, "enabled": true,
                         "runsOn": .array([]), "valid": true]),
                .object(["path": ".forkmesh/nightly.yml", "name": "Nightly", "events": .array(["schedule"]), "manual": false,
                         "enabled": true, "runsOn": .array([]), "valid": true]),
            ]),
            "controls": .array(controls),
            "runs": .array([
                .object(["id": 7, "workflow": "CI", "workflowPath": ".forkmesh/ci.yml", "commit": .string(commit),
                         "ref": "refs/heads/main", "status": .string(status), "createdAt": 2_000, "startedAt": 2_000,
                         "finishedAt": 65_000, "logTail": "ok\n\u{1B}[31merror: boom\u{1B}[0m\n", "pullNumber": 3,
                         "steps": .array([.object(["name": "build", "status": "success"]), .object(["name": "test", "status": "failed"])])]),
                .object(["id": 8, "workflow": "CI", "workflowPath": ".forkmesh/ci.yml", "commit": .string(commit),
                         "ref": "refs/heads/feature", "status": "awaiting-approval", "createdAt": 3_000, "startedAt": 0,
                         "finishedAt": 0, "logTail": "", "pullNumber": 4, "steps": .array([])]),
            ]),
        ])
    }

    @MainActor func testRunsSummaryParsesV2AndSortsNewestFirst() async throws {
        let http = FakeHTTPClient()
        http.on("GET", path: "/api/repo/alice/tool/actions/runs", json: CITests.runsPayload())
        let summary = try await api(http).actionsRuns(repo)
        XCTAssertEqual(http.lastRequest?.headers["Authorization"], "Bearer session-token")
        XCTAssertEqual(summary.schemaVersion, 2)
        XCTAssertTrue(summary.controlAvailable)
        XCTAssertEqual(summary.runs.map(\.id), [8, 7])
        XCTAssertEqual(summary.runs[1].status, .failed)
        XCTAssertEqual(summary.runs[1].stepProgress.done, 2)
        XCTAssertEqual(summary.runs[1].durationLabel(nowMs: 0), "1m 3s")
        XCTAssertEqual(summary.workflows.filter(\.canDispatch).map(\.path), [".forkmesh/ci.yml"])
    }

    func testV1SummaryNeverOffersControl() {
        let summary = ActionsSummary(json: ["ok": true, "node": "n", "controlAvailable": true, "runs": .array([])])
        XCTAssertEqual(summary.schemaVersion, 1)
        XCTAssertFalse(summary.controlAvailable)
        XCTAssertNil(summary.actionsEnabled)
    }

    @MainActor func testControlSendsValidatedRequestAndSettlesFromControls() async throws {
        let http = FakeHTTPClient()
        http.on("GET", path: "/api/repo/alice/tool/actions/runs", json: CITests.runsPayload())
        http.on("POST", path: "/api/repo/alice/tool/actions/control", status: 202, json: ["ok": true, "status": "accepted"])
        let clock = FixedClock(epochMillis: 100_000)
        let model = RepoActionsModel(client: api(http), repo: repo, clock: clock, makeRequestId: { "req_0123456789abcdef" })
        await model.load()
        XCTAssertEqual(model.actions(for: model.run(7)!), [.rerun])
        XCTAssertEqual(model.actions(for: model.run(8)!), [.approve, .reject])
        let sent = await model.send(.rerun, runId: 7)
        XCTAssertTrue(sent)
        XCTAssertEqual(body(http.lastRequest), [
            "requestId": "req_0123456789abcdef", "action": "rerun", "node": "alice-mac", "runId": 7,
        ])
        XCTAssertTrue(model.isPending(runId: 7))
        XCTAssertEqual(model.actions(for: model.run(7)!), [])

        http.on("GET", path: "/api/repo/alice/tool/actions/runs", json: CITests.runsPayload(controls: [
            .object(["requestId": "req_0123456789abcdef", "action": "rerun", "status": "rejected", "reason": "not_finished",
                     "runId": 7, "finishedAt": 101_000]),
        ]))
        await model.load()
        XCTAssertFalse(model.isPending(runId: 7))
        XCTAssertEqual(model.notice, ActionsNotice(text: "Re-run was refused: not finished.", good: false))
    }

    @MainActor func testDispatchBuildsBranchRefAndRejectsBadInput() async {
        let http = FakeHTTPClient()
        http.on("GET", path: "/api/repo/alice/tool/actions/runs", json: CITests.runsPayload())
        http.on("POST", path: "/api/repo/alice/tool/actions/control", status: 202, json: ["ok": true])
        let model = RepoActionsModel(client: api(http), repo: repo, makeRequestId: { "abcdefghijklmnop" })
        await model.load()
        let ok = await model.dispatch(workflow: model.dispatchableWorkflows[0], branch: "main")
        XCTAssertTrue(ok)
        XCTAssertEqual(body(http.lastRequest)["ref"], "refs/heads/main")
        XCTAssertEqual(body(http.lastRequest)["workflow"], ".forkmesh/ci.yml")
        let bad = await model.send(.dispatch, workflow: "../evil.yml", branch: "main")
        XCTAssertFalse(bad)
        let short = RepoActionsModel(client: api(http), repo: repo, makeRequestId: { "short" })
        await short.load()
        let refused = await short.send(.cancel, runId: 8)
        XCTAssertFalse(refused)
    }

    @MainActor func testControlFailureMapsForbidden() async {
        let http = FakeHTTPClient()
        http.on("GET", path: "/api/repo/alice/tool/actions/runs", json: CITests.runsPayload())
        http.on("POST", path: "/api/repo/alice/tool/actions/control", status: 403, json: ["error": "forbidden"])
        let model = RepoActionsModel(client: api(http), repo: repo)
        await model.load()
        let sent = await model.send(.cancel, runId: 8)
        XCTAssertFalse(sent)
        XCTAssertEqual(model.notice?.text, "Cancel: Only the repository owner and organization writers can do that.")
        XCTAssertFalse(model.needsFollowUp)
    }

    @MainActor func testRateLimitedLoadRecoversOnTheNextPoll() async {
        let http = FakeHTTPClient()
        http.enqueue(status: 429, json: ["error": "rate_limited"])
        http.on("GET", path: "/api/repo/alice/tool/actions/runs", json: ["ok": true, "node": "n", "runs": .array([])])
        let model = RepoActionsModel(client: api(http), repo: repo)
        await model.load()
        XCTAssertEqual(model.error, "Too many attempts. Try again later.")
        XCTAssertEqual(model.nextLoadDelaySeconds, RepoActionsModel.pollDelaySeconds)
        await model.load()
        XCTAssertNil(model.error)
        XCTAssertEqual(model.emptyMessage, "No workflow runs yet.")
    }

    @MainActor func testUnavailableSummaryAndWorkflowFilter() async {
        let http = FakeHTTPClient()
        http.on("GET", path: "/api/repo/alice/tool/actions/runs",
                json: ["ok": false, "state": "unavailable", "runs": .array([]), "retryable": true])
        let model = RepoActionsModel(client: api(http), repo: repo)
        await model.load()
        XCTAssertTrue(model.isUnavailable)
        XCTAssertNil(model.error)
        XCTAssertTrue(model.emptyMessage.hasPrefix("No node"))
        XCTAssertFalse(model.canControl)
    }

    @MainActor func testCommitFilterShowsTheMergeCommitsRuns() async {
        let http = FakeHTTPClient()
        http.on("GET", path: "/api/repo/alice/tool/actions/runs", json: CITests.runsPayload())
        let model = RepoActionsModel(client: api(http), repo: repo)
        await model.load()
        XCTAssertEqual(model.runs.count, 2)
        model.commitFilter = String(CITests.commit.prefix(7))
        XCTAssertEqual(model.runs.count, 2)
        model.commitFilter = String(repeating: "b", count: 40)
        XCTAssertTrue(model.runs.isEmpty)
        XCTAssertTrue(model.commitEmptyMessage.hasPrefix("No runs for this commit yet."))
        model.commitFilter = nil
        XCTAssertEqual(model.runs.count, 2)
        XCTAssertTrue(RepoActionsModel.sameCommit(CITests.commit, "AAAAAAA"))
        XCTAssertFalse(RepoActionsModel.sameCommit("", CITests.commit))
        XCTAssertFalse(RepoActionsModel.sameCommit(CITests.commit, "aaa"))
    }

    func testLogBoundingAnsiAndFailureExcerpt() {
        let lines = CILog.lines("a\r\n\u{1B}[1;31mred\u{1B}[0m\n\n\n")
        XCTAssertEqual(lines, ["a", "red"])
        let big = (0..<1_000).map { "line \($0)" }.joined(separator: "\n")
        XCTAssertEqual(CILog.lines(big).count, CILog.maxLines)
        XCTAssertEqual(CILog.lines(big).last, "line 999")
        let huge = String(repeating: "x", count: 40_000)
        XCTAssertLessThanOrEqual(CILog.lines(huge, maxLines: 10).joined().utf8.count, CILog.maxBytes)
        let excerpt = CILog.failureExcerpt((0..<200).map { $0 == 150 ? "FATAL: nope" : "ok \($0)" }.joined(separator: "\n"), lines: 10)
        XCTAssertTrue(excerpt.contains("FATAL: nope"))
        XCTAssertEqual(excerpt.split(separator: "\n").count, 10)
    }

    func testFailedRunPromptKeepsTheFailureAtTheEndOfALongLog() {
        let build = (0..<180).map { "[ 33%] Building CXX object CMakeFiles/forkmesh-window-tests.dir/src/File\($0).cpp.o" }
        let log = (["esh-window-tests.dir/src/ClientErrorReports.cpp.o"] + build
            + ["!! Process crashed", "", "==> FAILED: Critical Qt client contracts exceeded the process time limit."])
            .joined(separator: "\n")
        XCTAssertGreaterThan(log.count, MessageHandoff.maxMessageLength)
        let run = ActionsRunSummary(id: 8092, workflow: "Desktop client CI tests", commit: "f86a1b189b07", ref: "refs/heads/main",
                                    status: .failed, logTail: log, steps: [ActionsStep(name: "Critical Qt client contracts", status: "failed")])
        let text = CIPrompts.fixFailedRun(run, repo: repo).text!
        XCTAssertTrue(text.contains("exceeded the process time limit"))
        XCTAssertTrue(text.contains("Failed steps: Critical Qt client contracts."))
        XCTAssertFalse(text.contains("ClientErrorReports"))
        XCTAssertFalse(MessageHandoff.cleaned(log).contains("exceeded the process time limit"))
    }

    func testFailedRunPromptAndPullChecks() {
        let summary = ActionsSummary(json: CITests.runsPayload().objectValue!)
        let run = summary.runs.first { $0.id == 7 }!
        let target = CIPrompts.fixFailedRun(run, repo: repo)
        XCTAssertEqual(target.repo, repo)
        XCTAssertEqual(target.pull, 3)
        XCTAssertTrue(target.text!.contains("Failed steps: test."))
        XCTAssertTrue(target.text!.contains("error: boom"))
        XCTAssertFalse(target.text!.contains("\u{1B}"))

        let checks = PullChecksSummary(runs: summary.runs, pull: 3)
        XCTAssertEqual(checks.state, "failed")
        XCTAssertEqual(checks.label, "1 of 1 checks failed")
        let waiting = PullChecksSummary(runs: summary.runs, pull: 4)
        XCTAssertEqual(waiting.awaitingApproval.map(\.id), [8])
        XCTAssertEqual(PullChecksSummary(runs: summary.runs, pull: 99).label, "No checks")
    }

    @MainActor func testWebhooksCrudRevealsSecretOnceAndValidates() async {
        let http = FakeHTTPClient()
        let hook: JSONValue = .object([
            "id": "wh1", "url": "https://ci.example/hook", "events": .array(["pull.merged"]), "description": "",
            "active": true, "managed": false, "lastStatus": "", "lastDeliveryAt": 0, "consecutiveFailures": 0,
        ])
        http.on("POST", path: "/api/repo/alice/tool/webhooks") { request in
            let action = (try? JSON.parse(request.body ?? Data()))?.objectValue?.str("action") ?? ""
            switch action {
            case "list":
                return HTTPResponse(status: 200, json: ["ok": true, "webhooks": .array([hook]), "deliveries": .array([]),
                                                         "events": .array([]), "limits": ["maxWebhooks": 20]])
            case "create":
                var created = hook.objectValue!
                created["secret"] = "s3cret"
                return HTTPResponse(status: 201, json: ["ok": true, "webhook": .object(created)])
            case "test":
                return HTTPResponse(status: 200, json: ["ok": false, "status": 500, "outcome": "http_error", "url": "https://ci.example/…"])
            default:
                return HTTPResponse(status: 200, json: ["ok": true, "deleted": "wh1"])
            }
        }
        let model = RepoWebhooksModel(client: api(http), repo: repo)
        await model.load()
        XCTAssertEqual(model.webhooks.map(\.id), ["wh1"])
        XCTAssertEqual(model.eventTypes.count, 4)

        var draft = RepoWebhookDraft(url: "http://insecure", events: ["pull.merged"])
        let refused = await model.create(draft)
        XCTAssertFalse(refused)
        XCTAssertEqual(model.error, "Use an https:// address.")
        draft.url = "https://ci.example/hook"
        let created = await model.create(draft)
        XCTAssertTrue(created)
        XCTAssertEqual(model.revealedSecret?.secret, "s3cret")
        model.dismissSecret()
        XCTAssertNil(model.revealedSecret)

        let result = await model.test("wh1")
        XCTAssertEqual(result, "Test failed: http_error.")
        let updated = await model.update("wh1", draft: draft)
        XCTAssertTrue(updated)
        XCTAssertTrue(http.requests.contains { self.body($0)["action"] == "update" && self.body($0)["active"] == .bool(true) })
        let deleted = await model.delete("wh1")
        XCTAssertTrue(deleted)
        XCTAssertTrue(model.webhooks.isEmpty)
    }

    @MainActor func testSecurityHistoryTriageAndFallback() async {
        let http = FakeHTTPClient()
        let finding: JSONValue = .object([
            "id": "0123456789abcdef", "category": "dependency", "severity": "high", "ruleId": "CVE-1", "path": "Package.swift",
            "line": 4, "summary": "Old lib", "recommendation": "Upgrade", "falsePositiveStatus": "unreviewed",
        ])
        let low: JSONValue = .object([
            "id": "fedcba9876543210", "category": "code", "severity": "low", "ruleId": "R2", "path": "a.swift",
            "summary": "Minor", "recommendation": "Fix", "falsePositiveStatus": "unreviewed",
        ])
        http.on("GET", path: "/api/repo/alice/tool/security-scans/history", json: [
            "ok": true, "history": .array([.object([
                "scanId": .string(String(repeating: "b", count: 64)), "receivedAt": "2026-09-01T00:00:00Z",
                "clipboard": ["status": "completed", "commitHash": .string(CITests.commit),
                              "findings": ["critical": 0, "high": 1, "medium": 0, "low": 1, "informational": 0]],
                "rich": ["findings": .array([low, finding])],
            ])]),
        ])
        http.on("GET", path: "/api/repo/alice/tool/security-scans/lease", json: ["ok": true, "due": true, "status": "idle"])
        http.on("PATCH", path: "/api/repo/alice/tool/security-scans/triage", json: ["ok": true, "findings": .array([])])
        let model = RepoSecurityModel(client: api(http), repo: repo)
        await model.load()
        XCTAssertEqual(model.latest?.headline, "1 high · 1 low")
        XCTAssertEqual(model.latest?.worst, .high)
        XCTAssertEqual(model.findings.map(\.ruleId), ["CVE-1", "R2"])
        XCTAssertEqual(model.findings.first?.location, "Package.swift:4")
        XCTAssertEqual(model.scanStatus?.due, true)
        model.severityFilter = .low
        XCTAssertEqual(model.findings.map(\.ruleId), ["R2"])
        let bad = await model.triage(model.findings[0], status: "maybe")
        XCTAssertFalse(bad)
        let ok = await model.triage(model.findings[0], status: "dismissed", note: "test file")
        XCTAssertTrue(ok)
        XCTAssertEqual(model.findings[0].reviewStatus, "dismissed")
        XCTAssertEqual(body(http.lastRequest)["findingId"], "fedcba9876543210")

        let publicOnly = FakeHTTPClient()
        publicOnly.on("GET", path: "/api/repo/alice/tool/security-scans/history", status: 403, json: ["error": "forbidden"])
        publicOnly.on("GET", path: "/api/repo/alice/tool/security-scans/latest", json: [
            "ok": true, "latest": ["scanId": "x", "clipboard": ["findings": ["critical": 0, "high": 0, "medium": 0, "low": 0, "informational": 0]]],
        ])
        let fallback = RepoSecurityModel(client: api(publicOnly), repo: repo)
        await fallback.load()
        XCTAssertEqual(fallback.latest?.headline, "No findings")
        XCTAssertEqual(fallback.latest?.hasDetail, false)
    }

    @MainActor func testReleaseAnnounceSignsManualPostAndMirrorsParse() async throws {
        let http = FakeHTTPClient()
        http.on("POST", path: "/api/repo/alice/tool/ap-publish", status: 202, json: ["ok": true, "mode": "manual_immediate"])
        http.on("GET", path: "/api/repo/alice/tool/mirrors", json: [
            "ok": true, "mirrors": .array([
                .object(["node": "m1", "status": "offline", "behind": 0, "artifactCount": 2]),
                .object(["node": "m2", "status": "online", "behind": 3, "branch": "main", "commit": .string(CITests.commit),
                         "latencyMs": 42, "artifactCount": 5]),
            ]),
        ])
        let identity = try Identity.fromSeed([UInt8](repeating: 7, count: 32))
        let model = RepoReleasesModel(client: api(http), repo: repo)
        let release = RepoRelease(tag: "v1.0", assets: [RepoReleaseAsset(name: "app.dmg", sha256: "ff", size: 10, os: "macos", arch: "arm64")])
        let draft = ReleaseAnnouncementDraft(release: release, repo: repo)
        XCTAssertEqual(draft.title, "tool v1.0")
        let message = await model.announce(draft, tag: "v1.0", account: "Alice", identity: identity, nowMillis: 123)
        XCTAssertEqual(message, "Announced on the Fediverse.")
        let sent = try XCTUnwrap(http.lastRequest)
        let query = URLComponents(url: sent.url, resolvingAgainstBaseURL: false)?.queryItems ?? []
        XCTAssertEqual(query.first { $0.name == "owner" }?.value, "alice")
        XCTAssertEqual(query.first { $0.name == "ts" }?.value, "123")
        let expected = identity.sign(Array(Utf8.encode(CIRequests.announceCanonical(account: "alice", ts: "123"))))
        XCTAssertEqual(query.first { $0.name == "sig" }?.value, expected)
        XCTAssertEqual(body(sent)["manual"], .bool(true))
        XCTAssertEqual(body(sent)["kind"], "release")

        let missing = await model.announce(draft, tag: "v1.0", account: "", identity: nil, nowMillis: 1)
        XCTAssertEqual(missing, "Sign in on this device to announce a release.")

        let mirrors = RepoMirrorsModel(client: api(http), repo: repo)
        await mirrors.load()
        XCTAssertEqual(mirrors.nodes.map(\.node), ["m2", "m1"])
        XCTAssertEqual(mirrors.nodes[0].light, "behind")
        XCTAssertEqual(mirrors.nodes[0].subtitle, "main @ aaaaaaaa · 3 behind · 42 ms")
        XCTAssertEqual(mirrors.totalArtifacts, 5)
        mirrors.onlineOnly = true
        XCTAssertEqual(mirrors.shown.count, 1)
        XCTAssertFalse(mirrors.syncLabel.isEmpty)
    }
}
