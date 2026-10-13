import FMApi
import FMBase
import FMChat
import FMControl
import FMNet
import Foundation
import XCTest

@testable import FMCode

@MainActor
final class FakeActionsChannel: DesktopControlCalling {
    var calls: [(String, JSONObject)] = []
    var replies: [String: JSONObject] = [:]
    var failures: [String: Error] = [:]
    private var taps: [UUID: @MainActor (String, JSONObject) -> Void] = [:]

    func request(_ method: String, _ params: JSONObject, timeout: TimeInterval) async throws -> JSONObject {
        calls.append((method, params))
        if let failure = failures[method] { throw failure }
        return replies[method] ?? [:]
    }

    func addEventTap(_ tap: @escaping @MainActor (String, JSONObject) -> Void) -> UUID {
        let id = UUID()
        taps[id] = tap
        return id
    }

    func removeEventTap(_ id: UUID) { taps.removeValue(forKey: id) }

    var tapCount: Int { taps.count }
    var methods: [String] { calls.map(\.0) }
}

@MainActor
final class DesktopActionsTests: XCTestCase {
    private let repo = RepoRef(owner: "alice", name: "tool")
    private static let commit = String(repeating: "b", count: 40)

    private func api(_ http: FakeHTTPClient) -> ApiService {
        let api = ApiService(settings: StaticApiSettings(serverUrl: "wss://ci.test/ws"), http: http, sleep: { _ in })
        api.sessionTokenProvider = { "session-token" }
        return api
    }

    private func unavailableServer() -> FakeHTTPClient {
        let http = FakeHTTPClient()
        http.on("GET", path: "/api/repo/alice/tool/actions/runs",
                json: ["ok": false, "state": "unavailable", "runs": .array([]), "retryable": true])
        return http
    }

    private static func desktopReply(runs: [JSONValue]) -> JSONObject {
        [
            "repo": "alice/tool", "enabled": true, "autoApprove": false, "mirror": true,
            "workflows": .array([
                .object(["path": ".forkmesh/ci.yml", "name": "CI", "valid": true, "triggers": .array(["push", "manual"]),
                         "manual": true, "disabled": false, "here": true, "runsOn": "desktop, linux"]),
                .object(["path": ".forkmesh/old.yml", "name": "Old", "valid": true, "triggers": .array(["push"]),
                         "manual": true, "disabled": true, "here": true]),
            ]),
            "runs": .array(runs),
        ]
    }

    private static let runningRun: JSONValue = .object([
        "id": 41, "workflow": "CI", "path": ".forkmesh/ci.yml", "status": "running", "commit": .string(commit),
        "branch": "main", "createdAtMs": 5_000, "startedAtMs": 6_000, "finishedAtMs": 0, "currentStep": 1,
        "steps": .array([.object(["name": "build", "status": "success"]), .object(["name": "test", "status": "running"]),
                         .object(["name": "deploy", "status": "pending"])]),
        "pull": 9, "pullAuthor": "bob",
    ])

    func testDesktopRunsShowWhenNoNodeAnswers() async {
        let channel = FakeActionsChannel()
        channel.replies["repos.actions"] = DesktopActionsTests.desktopReply(runs: [DesktopActionsTests.runningRun])
        let http = unavailableServer()
        let model = RepoActionsModel(
            client: api(http), repo: repo, desktop: DesktopActions(channel: channel, desktopName: "studio"))
        await model.load()
        XCTAssertEqual(channel.methods, ["repos.actions"])
        XCTAssertEqual(channel.calls[0].1["repo"], "alice/tool")
        XCTAssertTrue(http.requests.isEmpty)
        XCTAssertTrue(model.fromDesktop)
        XCTAssertFalse(model.isUnavailable)
        XCTAssertTrue(model.canControl)
        XCTAssertTrue(model.hasActiveRuns)
        XCTAssertEqual(model.nextLoadDelaySeconds, RepoActionsModel.followUpDelaySeconds)
        let run = model.run(41)!
        XCTAssertEqual(run.status, .running)
        XCTAssertEqual(run.workflowPath, ".forkmesh/ci.yml")
        XCTAssertEqual(run.ref, "refs/heads/main")
        XCTAssertEqual(run.refLabel, "main")
        XCTAssertEqual(run.pullNumber, 9)
        XCTAssertEqual(run.steps.map(\.name), ["build", "test", "deploy"])
        XCTAssertEqual(run.stepProgress.done, 1)
        XCTAssertEqual(model.actions(for: run), [.cancel])
        XCTAssertEqual(model.workflows.map(\.enabled), [true, false])
        XCTAssertEqual(model.workflows[0].runsOn, ["desktop", "linux"])
        XCTAssertEqual(model.workflows[0].events, ["push", "manual"])
        XCTAssertEqual(model.dispatchableWorkflows.map(\.path), [".forkmesh/ci.yml"])
        XCTAssertEqual(channel.tapCount, 1)
        model.stop()
        XCTAssertEqual(channel.tapCount, 0)
    }

    func testEmptyDesktopDefersToTheServerThenBacksItUp() async {
        let channel = FakeActionsChannel()
        channel.replies["repos.actions"] = DesktopActionsTests.desktopReply(runs: [])
        let http = unavailableServer()
        let model = RepoActionsModel(
            client: api(http), repo: repo, desktop: DesktopActions(channel: channel, desktopName: "studio"))
        await model.load()
        XCTAssertEqual(http.requests.count, 1)
        XCTAssertTrue(model.fromDesktop)
        XCTAssertFalse(model.isUnavailable)
        XCTAssertEqual(model.emptyMessage, "No workflow runs yet.")
        XCTAssertEqual(model.workflows.count, 2)
    }

    func testUnknownRepoOnTheDesktopFallsBackToTheServer() async {
        let channel = FakeActionsChannel()
        channel.failures["repos.actions"] = LocalControlException("unknown_repo")
        let http = unavailableServer()
        let model = RepoActionsModel(
            client: api(http), repo: repo, desktop: DesktopActions(channel: channel, desktopName: "studio"))
        await model.load()
        XCTAssertFalse(model.fromDesktop)
        XCTAssertTrue(model.isUnavailable)
        XCTAssertEqual(
            model.emptyMessage,
            "No node that hosts this repository is answering right now. Your desktop does not have this repository.")
    }

    func testDesktopLogAppendsAndResets() async {
        let channel = FakeActionsChannel()
        channel.replies["repos.actions"] = DesktopActionsTests.desktopReply(runs: [DesktopActionsTests.runningRun])
        let model = RepoActionsModel(
            client: api(unavailableServer()), repo: repo, desktop: DesktopActions(channel: channel, desktopName: "studio"))
        await model.load()
        channel.replies["repos.actions.log"] = ["id": 41, "status": "running", "text": "one\n", "from": 0, "next": 4, "reset": false]
        await model.follow(runId: 41)
        XCTAssertEqual(model.run(41)?.logTail, "one\n")
        XCTAssertNil(channel.calls.last?.1["since"])
        channel.replies["repos.actions.log"] = ["id": 41, "status": "running", "text": "two\n", "from": 4, "next": 8, "reset": false]
        await model.loadLog(runId: 41)
        XCTAssertEqual(channel.calls.last?.1["since"], 4)
        XCTAssertEqual(model.run(41)?.logTail, "one\ntwo\n")
        channel.replies["repos.actions.log"] = ["id": 41, "status": "running", "text": "fresh\n", "from": 0, "next": 6, "reset": true]
        await model.loadLog(runId: 41)
        XCTAssertEqual(model.run(41)?.logTail, "fresh\n")
    }

    func testControlsGoThroughPrepareAndRun() async {
        let channel = FakeActionsChannel()
        channel.replies["repos.actions"] = DesktopActionsTests.desktopReply(runs: [DesktopActionsTests.runningRun])
        channel.replies["repos.prepare"] = ["token": "tok-1", "action": "run.stop"]
        channel.replies["repos.run"] = ["started": true, "message": "Stopping run #41."]
        let model = RepoActionsModel(
            client: api(unavailableServer()), repo: repo, desktop: DesktopActions(channel: channel, desktopName: "studio"))
        await model.load()
        let ok = await model.send(.cancel, runId: 41)
        XCTAssertTrue(ok)
        XCTAssertEqual(channel.methods, ["repos.actions", "repos.prepare", "repos.run", "repos.actions"])
        XCTAssertEqual(channel.calls[1].1, ["repo": "alice/tool", "action": "run.stop", "run": 41])
        XCTAssertEqual(channel.calls[2].1, ["token": "tok-1"])
        XCTAssertEqual(model.notice, ActionsNotice(text: "Stopping run #41.", good: true))
        XCTAssertFalse(model.needsFollowUp)

        channel.replies["repos.prepare"] = ["token": "tok-2"]
        channel.replies["repos.run"] = ["started": true, "message": "Started run #42."]
        let dispatched = await model.dispatch(workflow: model.dispatchableWorkflows[0], branch: "feature/x")
        XCTAssertTrue(dispatched)
        XCTAssertEqual(
            channel.calls[4].1,
            ["repo": "alice/tool", "action": "workflow.run", "path": ".forkmesh/ci.yml", "branch": "feature/x"])

        channel.failures["repos.prepare"] = LocalControlException("wrong_state")
        let refused = await model.send(.approve, runId: 41)
        XCTAssertFalse(refused)
        XCTAssertEqual(model.notice, ActionsNotice(text: "Approve: That run has already moved on.", good: false))
        let switched = await model.send(.setWorkflowEnabled, workflow: ".forkmesh/ci.yml", enabled: false)
        XCTAssertFalse(switched)
    }

    func testWorkflowSwitchGoesThroughPrepareAndRun() async {
        let channel = FakeActionsChannel()
        channel.replies["repos.actions"] = DesktopActionsTests.desktopReply(runs: [DesktopActionsTests.runningRun])
        channel.replies["repos.prepare"] = ["token": "tok-off"]
        channel.replies["repos.run"] = ["started": true, "message": "Turned off .forkmesh/ci.yml."]
        let model = RepoActionsModel(
            client: api(unavailableServer()), repo: repo, desktop: DesktopActions(channel: channel, desktopName: "studio"))
        await model.load()
        XCTAssertTrue(model.canSwitchWorkflows)
        XCTAssertEqual(model.manualWorkflows.map(\.path), [".forkmesh/ci.yml"])
        let ci = model.workflows[0]
        XCTAssertEqual(ci.eventsLabel, "Push · Manual")
        XCTAssertEqual(model.latestRun(of: ci)?.id, 41)
        let ok = await model.setEnabled(ci, false)
        XCTAssertTrue(ok)
        XCTAssertEqual(channel.calls[1].1, ["repo": "alice/tool", "action": "workflow.disable", "path": ".forkmesh/ci.yml"])
        XCTAssertEqual(channel.calls[2].1, ["token": "tok-off"])
        XCTAssertEqual(model.notice, ActionsNotice(text: "Turned off .forkmesh/ci.yml.", good: true))
        XCTAssertFalse(model.isSwitching(ci))
        XCTAssertEqual(DesktopActions.action(for: .setWorkflowEnabled, enabled: true), "workflow.enable")
        XCTAssertNil(DesktopActions.action(for: .setWorkflowEnabled))

        channel.failures["repos.prepare"] = LocalControlException("unsupported_action")
        let old = await model.setEnabled(ci, false)
        XCTAssertFalse(old)
        XCTAssertEqual(model.notice, ActionsNotice(text: "Workflow switch: Update the desktop to do that from here.", good: false))
    }
}
