import FMApi
import FMBase
import FMChat
import FMControl
import Foundation
import XCTest
@testable import FMWorkflows

@MainActor
final class FakeDesktopChannel: DesktopControlCalling {
    var requests: [(method: String, params: JSONObject)] = []
    var replies: [String: JSONObject] = [:]
    var failures: [String: Error] = [:]
    var handler: ((String, JSONObject) throws -> JSONObject?)?
    private var taps: [UUID: @MainActor (String, JSONObject) -> Void] = [:]

    func request(_ method: String, _ params: JSONObject, timeout: TimeInterval) async throws -> JSONObject {
        requests.append((method, params))
        if let handled = try handler?(method, params) { return handled }
        if let failure = failures[method] { throw failure }
        return replies[method] ?? [:]
    }

    func addEventTap(_ tap: @escaping @MainActor (String, JSONObject) -> Void) -> UUID {
        let id = UUID()
        taps[id] = tap
        return id
    }

    func removeEventTap(_ id: UUID) { taps.removeValue(forKey: id) }

    func emit(_ event: String) {
        for tap in taps.values { tap(event, [:]) }
    }

    func count(_ method: String) -> Int { requests.filter { $0.method == method }.count }
    func last(_ method: String) -> JSONObject? { requests.last { $0.method == method }?.params }
}

final class DesktopWorkflowsTests: XCTestCase {
    static let listJSON: JSONObject = [
        "desktop": "office",
        "workflows": [
            ["repo": "forkmesh/forkmesh", "id": "cfo", "name": "CFO", "specialAgent": true, "enabled": true, "steps": 2,
             "trigger": "Weekly", "lastRunAt": 1_700_000_000_000.0, "lastStatus": "success", "lastSummary": "Burn is fine"],
            ["repo": "forkmesh/forkmesh", "id": "clean", "name": "Clean", "specialAgent": false, "enabled": false,
             "running": true, "runningStep": "Measure the disk", "steps": 7],
        ],
    ]

    @MainActor func testListParsesRowsAndRefreshesOnChange() async {
        let channel = FakeDesktopChannel()
        channel.replies["workflows.list"] = Self.listJSON
        let store = DesktopWorkflowsStore(channel: channel)
        await store.refresh()
        XCTAssertEqual(store.desktop, "office")
        XCTAssertEqual(store.rows.map(\.id), ["forkmesh/forkmesh|cfo", "forkmesh/forkmesh|clean"])
        XCTAssertEqual(store.rows[0].kindLabel, "Special agent")
        XCTAssertEqual(store.rows[0].lastRunAt, 1_700_000_000_000)
        XCTAssertTrue(store.rows[1].running)
        XCTAssertFalse(store.rows[1].enabled)
        XCTAssertEqual(store.rows[1].kindLabel, "Desktop workflow")
        XCTAssertTrue(store.loaded)

        channel.emit("teammates.changed")
        channel.emit("workflows.changed")
        for _ in 0..<20 where channel.count("workflows.list") < 2 { await Task.yield() }
        XCTAssertEqual(channel.count("workflows.list"), 2)
        XCTAssertEqual(store.revision, 1)
    }

    @MainActor func testOlderDesktopIsUnsupported() async {
        let channel = FakeDesktopChannel()
        channel.failures["workflows.list"] = LocalControlException("unsupported")
        let store = DesktopWorkflowsStore(channel: channel)
        await store.refresh()
        XCTAssertTrue(store.unsupported)
        XCTAssertTrue(store.rows.isEmpty)
        XCTAssertTrue(store.error.contains("Update ForkMesh"))
    }

    @MainActor func testRunAsksForATokenThenSpendsIt() async throws {
        let channel = FakeDesktopChannel()
        channel.replies["workflows.list"] = Self.listJSON
        channel.replies["workflows.prepare"] = ["token": "t1", "summary": "Run CFO from forkmesh/forkmesh on the desktop now."]
        channel.replies["workflows.run"] = ["started": true, "message": "CFO is running."]
        let store = DesktopWorkflowsStore(channel: channel)
        await store.refresh()
        let row = try XCTUnwrap(store.rows.first)
        let confirmation = try await store.prepare("run", row, assignment: "  Check the burn  ")
        XCTAssertEqual(confirmation.summary, "Run CFO from forkmesh/forkmesh on the desktop now.")
        XCTAssertEqual(channel.last("workflows.prepare"),
                       ["action": "run", "repo": "forkmesh/forkmesh", "id": "cfo"])
        let message = try await store.perform(confirmation)
        XCTAssertEqual(message, "CFO is running.")
        XCTAssertEqual(channel.last("workflows.run"),
                       ["action": "run", "repo": "forkmesh/forkmesh", "id": "cfo", "token": "t1", "assignment": "Check the burn"])
        XCTAssertEqual(channel.count("workflows.list"), 2)
    }

    @MainActor func testPrepareWithoutTokenFails() async throws {
        let channel = FakeDesktopChannel()
        channel.replies["workflows.list"] = Self.listJSON
        let store = DesktopWorkflowsStore(channel: channel)
        await store.refresh()
        do {
            _ = try await store.prepare("stop", try XCTUnwrap(store.rows.last))
            XCTFail("expected bad_answer")
        } catch let error as LocalControlException {
            XCTAssertEqual(error.code, "bad_answer")
        }
    }

    @MainActor func testDetailParsesRuns() async throws {
        let channel = FakeDesktopChannel()
        channel.replies["workflows.list"] = Self.listJSON
        channel.replies["workflows.get"] = [
            "repo": "forkmesh/forkmesh", "id": "clean", "name": "Clean",
            "nodes": [["id": "run", "type": "workflow.manual", "name": "Run by hand"],
                      ["id": "usage", "type": "desktop.diskUsage", "name": "Measure the disk"]],
            "runs": [["id": "r1", "status": "error", "error": "Disk busy", "startedAtMs": 1000.0, "finishedAtMs": 4000.0,
                      "steps": [["nodeId": "usage", "name": "Measure the disk", "status": "error", "startedAtMs": 1000.0,
                                 "finishedAtMs": 4000.0, "logs": ["a", "b"]]]]],
            "active": ["id": "r2", "status": "running", "steps": [["nodeId": "usage", "status": "running"]]],
        ]
        let store = DesktopWorkflowsStore(channel: channel)
        await store.refresh()
        let detail = try await store.detail(try XCTUnwrap(store.rows.last))
        XCTAssertEqual(channel.last("workflows.get"), ["repo": "forkmesh/forkmesh", "id": "clean"])
        XCTAssertEqual(detail.nodes.map(\.name), ["Run by hand", "Measure the disk"])
        XCTAssertEqual(detail.runs.first?.summary, "Disk busy")
        XCTAssertEqual(detail.runs.first?.steps.first?.ms, 3000)
        XCTAssertEqual(detail.runs.first?.steps.first?.logs, ["a", "b"])
        XCTAssertEqual(detail.latest?.id, "r2")
    }

    static func cleanupDraft() -> WorkflowDraft {
        WorkflowDraft(
            id: "w1", name: "Free disk space",
            nodes: [
                WorkflowNode(id: "run", type: "core.manual", name: "Run"),
                WorkflowNode(id: "usage", type: "desktop.diskUsage", name: "Measure the disk"),
                WorkflowNode(id: "agents", type: "desktop.cleanAgents", name: "Remove agent leftovers"),
                WorkflowNode(id: "report", type: "desktop.diskReport", name: "Report free space"),
            ],
            connections: [
                WorkflowConnection(from: "run", to: "usage"),
                WorkflowConnection(from: "usage", to: "agents"),
                WorkflowConnection(from: "agents", to: "report"),
            ])
    }

    @MainActor func testRunOnDesktopDrivesEachStepAndRecordsTheRun() async throws {
        let channel = FakeDesktopChannel()
        channel.handler = { method, params in
            switch method {
            case "diag.prepare": return ["token": .string("tok-" + params.str("step"))]
            case "diag.run":
                switch params.str("step") {
                case "agents": return ["freedBytes": 2048, "items": 3, "freeBytes": 4096, "totalBytes": 0, "lines": ["removed 3"]]
                case "report": return ["freedBytes": 0, "freeBytes": 5_000_000, "totalBytes": 9_000_000]
                default: return ["freedBytes": 0, "freeBytes": 1024, "totalBytes": 9_000_000]
                }
            default: return nil
            }
        }
        let api = FakeWorkflowsApi { method, suffix, _ in
            XCTAssertEqual(method, "POST")
            XCTAssertEqual(suffix, "/w1/record")
            return ["run": ["id": "run-9", "trigger": "desktop", "status": "success", "nodes": [:]]]
        }
        let store = WorkflowsStore(call: api.call, engine: { throw WorkflowScriptError.unavailable }, secureStore: InMemorySecureStore())
        store.desktopChannel = channel
        XCTAssertTrue(store.canRunOnDesktop)
        let run = await store.runOnDesktop(Self.cleanupDraft())

        XCTAssertEqual(run.id, "run-9")
        XCTAssertEqual(run.place, .desktop)
        XCTAssertEqual(store.desktopRunning, "")
        XCTAssertEqual(channel.requests.map(\.method),
                       ["diag.prepare", "diag.run", "diag.prepare", "diag.run", "diag.prepare", "diag.run"])
        XCTAssertEqual(channel.requests[1].params,
                       ["action": "freeDiskSpace", "step": "usage", "token": "tok-usage"])
        XCTAssertEqual(channel.requests[3].params.str("step"), "agents")
        let body = try XCTUnwrap(api.calls.first?.body)
        let nodes = body.obj("nodes")
        XCTAssertEqual(nodes.obj("run").str("status"), "success")
        XCTAssertEqual(nodes.obj("agents").str("status"), "success")
        XCTAssertEqual(nodes.obj("agents").list("logs").compactMap(\.stringValue), ["removed 3"])
        let item = try XCTUnwrap(nodes.obj("agents").obj("outputs").obj("main").maps("items").first)
        XCTAssertEqual(item.str("freed"), "2.0 KB")
        XCTAssertEqual(item.int("items"), 3)
        XCTAssertEqual(body.obj("output"), ["freedBytes": 2048, "freeBytes": 5_000_000, "totalBytes": 9_000_000])
    }

    @MainActor func testFailedStepSkipsTheRestAndIsRecorded() async throws {
        let channel = FakeDesktopChannel()
        channel.handler = { method, params in
            if method == "diag.prepare" { return ["token": "t"] }
            if params.str("step") == "usage" { throw LocalControlException("cleanup_failed", message: "Permission denied") }
            return [:]
        }
        let api = FakeWorkflowsApi { _, _, body in
            ["run": ["id": "run-1", "trigger": "desktop", "status": "error", "error": .string(body?.str("error") ?? "")]]
        }
        let store = WorkflowsStore(call: api.call, engine: { throw WorkflowScriptError.unavailable }, secureStore: InMemorySecureStore())
        store.desktopChannel = channel
        _ = await store.runOnDesktop(Self.cleanupDraft())
        XCTAssertEqual(channel.count("diag.run"), 1)
        let nodes = try XCTUnwrap(api.calls.first?.body).obj("nodes")
        XCTAssertEqual(nodes.obj("usage").str("status"), "error")
        XCTAssertTrue(nodes.obj("usage").str("error").contains("Permission denied"))
        XCTAssertEqual(nodes.obj("agents").str("status"), "skipped")
        XCTAssertEqual(nodes.obj("report").str("status"), "skipped")
    }

    @MainActor func testUnsavedRunStillReportsTheResult() async {
        let channel = FakeDesktopChannel()
        channel.handler = { method, _ in method == "diag.prepare" ? ["token": "t"] : [:] }
        let api = FakeWorkflowsApi { _, _, _ in throw OrgApiException("offline", "offline") }
        let store = WorkflowsStore(call: api.call, engine: { throw WorkflowScriptError.unavailable }, secureStore: InMemorySecureStore())
        store.desktopChannel = channel
        let run = await store.runOnDesktop(Self.cleanupDraft())
        XCTAssertEqual(run.status, "success")
        XCTAssertEqual(run.place, .desktop)
        XCTAssertTrue(run.error.contains("could not save"))
        XCTAssertEqual(run.nodes.keys.count, 4)
    }

    @MainActor func testNoDesktopNoRun() async {
        let store = WorkflowsStore(call: FakeWorkflowsApi().call, engine: { throw WorkflowScriptError.unavailable },
                                   secureStore: InMemorySecureStore())
        XCTAssertFalse(store.canRunOnDesktop)
        let run = await store.runOnDesktop(Self.cleanupDraft())
        XCTAssertEqual(run.status, "error")
        XCTAssertEqual(run.error, WorkflowsText.desktopMissing)
    }

    func testDesktopStepsMatchTheServerSpec() throws {
        let url = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            .appendingPathComponent("Sources/FMWorkflows/Resources/workflow-spec.json")
        let spec = JSON.parseObject(try String(contentsOf: url, encoding: .utf8))
        var fromSpec: [String: String] = [:]
        for (type, value) in spec.obj("coreNodes") {
            let step = value.objectValue?.str("desktopStep") ?? ""
            if !step.isEmpty { fromSpec[type] = step }
        }
        XCTAssertEqual(fromSpec, WorkflowDesktopSteps.steps)
        XCTAssertTrue(WorkflowDesktopSteps.hasDesktopSteps(Self.cleanupDraft()))
        XCTAssertFalse(WorkflowDesktopSteps.hasDesktopSteps(.blank))
        XCTAssertEqual(WorkflowDesktopSteps.bytes(512), "512 B")
        XCTAssertEqual(WorkflowDesktopSteps.bytes(1_572_864), "1.5 MB")
    }

    func testServerDesktopRunsShowAsDesktop() {
        XCTAssertEqual(WorkflowRun(json: ["trigger": "desktop"], place: .server).place, .desktop)
        XCTAssertEqual(WorkflowRun(json: ["trigger": "manual"], place: .server).place, .server)
    }
}
