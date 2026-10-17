import FMApi
import FMBase
import FMNet
import XCTest
@testable import FMWorkflows

@MainActor
final class FakeWorkflowsApi {
    typealias Handler = (_ method: String, _ suffix: String, _ body: JSONObject?) throws -> JSONObject

    var handler: Handler
    private(set) var calls: [(method: String, suffix: String, body: JSONObject?)] = []

    init(_ handler: @escaping Handler = { method, suffix, _ in throw OrgApiException("unexpected", "unexpected \(method) \(suffix)") }) {
        self.handler = handler
    }

    var call: WorkflowsApiCall {
        { [self] method, suffix, body in
            self.calls.append((method, suffix, body))
            return try self.handler(method, suffix, body)
        }
    }
}

final class ScriptedWorkflowScript: WorkflowScript, @unchecked Sendable {
    var steps: [String]
    private(set) var inputs: [String] = []
    private(set) var replies: [String] = []
    private(set) var cancelled: [String] = []

    init(_ steps: [String]) {
        self.steps = steps
    }

    func start(_ input: String) -> String {
        inputs.append(input)
        return steps.removeFirst()
    }

    func resume(_ session: String, _ reply: String) -> String {
        replies.append(reply)
        return steps.removeFirst()
    }

    func cancel(_ session: String) { cancelled.append(session) }

    func call(_ name: String, _ arguments: String) -> String {
        if name == "safeUrl" { return arguments.contains("https://ok.example.com") ? #"{"result":"x"}"# : #"{"result":""}"# }
        if name == "clean" { return #"{"result":{"error":"cycle"}}"# }
        if name == "catalog" { return JSON.encode(.object(["result": .object(catalogJSON())])) }
        return #"{"result":null}"#
    }
}

final class RecordingTransport: WorkflowTransport, @unchecked Sendable {
    var replies: [WorkflowReply]
    private(set) var sent: [WorkflowRequest] = []

    init(_ replies: [WorkflowReply]) {
        self.replies = replies
    }

    func send(_ request: WorkflowRequest, allowed: @escaping @Sendable (String) -> Bool) async -> WorkflowReply {
        sent.append(request)
        return replies.isEmpty ? .noResponse : replies.removeFirst()
    }
}

let workflowId = String(repeating: "a", count: 32)
let credentialId = String(repeating: "c", count: 32)

func catalogJSON() -> JSONObject {
    [
        "core": [
            ["type": "core.manual", "name": "Manual trigger", "group": "Triggers", "trigger": true, "outputs": ["main"], "fields": []],
            ["type": "core.webhook", "name": "Webhook", "group": "Triggers", "trigger": true, "outputs": ["main"], "fields": []],
            ["type": "core.if", "name": "If", "group": "Flow", "outputs": ["true", "false"], "fields": [
                ["key": "operator", "label": "Operator", "type": "select", "default": "equals"],
            ]],
            ["type": "core.noop", "name": "No operation", "group": "Flow", "outputs": ["main"], "fields": []],
        ],
        "integrations": [
            ["type": "app.slack", "key": "slack", "name": "Slack", "category": "Communication",
             "credential": [["key": "token", "label": "Bot token", "secret": true]],
             "operations": [["key": "postMessage", "name": "Send a message", "method": "POST", "fields": [
                 ["key": "channel", "label": "Channel", "type": "string", "required": true],
             ]]]],
        ],
        "categories": ["Communication"],
        "operators": [["value": "equals", "label": "is equal to"]],
    ]
}

final class WorkflowDraftTests: XCTestCase {
    func testOrderFollowsConnectionsAndKeepsUnreachableSteps() {
        let draft = WorkflowDraft(json: [
            "id": .string(workflowId), "name": "Flow", "active": true,
            "nodes": [
                ["id": "b", "type": "core.noop", "name": "B"], ["id": "t", "type": "core.manual", "name": "T"],
                ["id": "a", "type": "core.noop", "name": "A"], ["id": "z", "type": "core.noop", "name": "Z"],
            ],
            "connections": [["from": "t", "to": "a"], ["from": "a", "to": "b"], ["from": "b", "to": "a"]],
        ])
        XCTAssertEqual(draft.ordered.map(\.id), ["t", "z", "b", "a"])
        XCTAssertEqual(draft.trigger?.id, "t")
        XCTAssertEqual(draft.body.bool("active"), true)
        XCTAssertEqual(draft.graph["active"], nil)
    }

    func testAddStepConnectsBranchesAndPicksUniqueNamesAndIds() {
        let catalog = WorkflowCatalog(json: catalogJSON())
        var draft = WorkflowDraft.blank
        let branch = draft.addStep(catalog.kind("core.if")!, after: "trigger")
        XCTAssertEqual(branch.params.str("operator"), "equals")
        let yes = draft.addStep(catalog.kind("core.noop")!, after: branch.id, output: "true")
        let no = draft.addStep(catalog.kind("core.noop")!, after: branch.id, output: "false")
        XCTAssertEqual([yes.name, no.name], ["No operation", "No operation 2"])
        XCTAssertNotEqual(yes.id, no.id)
        XCTAssertEqual(no.y - yes.y, WorkflowDraft.row)
        XCTAssertTrue(draft.connections.contains(WorkflowConnection(from: branch.id, output: "false", to: no.id)))
        let slack = draft.addStep(catalog.kind("app.slack")!, after: yes.id)
        XCTAssertEqual(slack.params.str("operation"), "postMessage")
        XCTAssertEqual(slack.x, yes.x + WorkflowDraft.column)
    }

    func testRemovingAStepJoinsItsParentToItsChildren() {
        let catalog = WorkflowCatalog(json: catalogJSON())
        var draft = WorkflowDraft.blank
        let middle = draft.addStep(catalog.kind("core.noop")!, after: "trigger")
        let last = draft.addStep(catalog.kind("core.noop")!, after: middle.id)
        draft.remove(middle.id)
        XCTAssertEqual(draft.connections, [WorkflowConnection(from: "trigger", to: last.id)])
        XCTAssertNil(draft.node(middle.id))
    }

    func testSettingTheTriggerReplacesTheOldOne() {
        let catalog = WorkflowCatalog(json: catalogJSON())
        var draft = WorkflowDraft.blank
        draft.setTrigger(catalog.kind("core.webhook")!)
        XCTAssertEqual(draft.nodes.filter(\.isTrigger).map(\.type), ["core.webhook"])
        XCTAssertEqual(draft.trigger?.name, "Webhook")
    }

    func testRunStepsFollowTheWorkflowOrder() {
        let draft = WorkflowDraft(json: [
            "nodes": [["id": "t", "type": "core.manual", "name": "Go"], ["id": "s", "type": "core.noop", "name": "Next"]],
            "connections": [["from": "t", "to": "s"]],
        ])
        let run = WorkflowRun(json: [
            "status": "error", "error": "Next: boom", "requests": 1,
            "nodes": [
                "s": ["status": "error", "error": "boom", "ms": 4],
                "t": ["status": "success", "ms": 1, "outputs": ["main": ["count": 2, "items": [["a": 1]]]]],
            ],
        ], place: .phone)
        let steps = run.steps(in: draft)
        XCTAssertEqual(steps.map(\.name), ["Go", "Next"])
        XCTAssertEqual(steps[0].itemCount, 2)
        XCTAssertEqual(steps[1].error, "boom")
        XCTAssertFalse(run.succeeded)
    }

    func testCatalogFindsFieldsByOperation() {
        let catalog = WorkflowCatalog(json: catalogJSON())
        XCTAssertEqual(catalog.triggers.map(\.type), ["core.manual", "core.webhook"])
        XCTAssertEqual(catalog.kind("app.slack")?.fields(operation: "nope").map(\.key), ["channel"])
        XCTAssertTrue(catalog.kind("app.slack")!.needsCredential)
        XCTAssertEqual(catalog.search("slack").map(\.type), ["app.slack"])
        XCTAssertEqual(WorkflowsText.every(120), "Every 2 hours")
        XCTAssertEqual(WorkflowsText.every(1440), "Every day")
    }
}

final class WorkflowEngineTests: XCTestCase {
    func testTheRunnerSendsEachRequestAndFeedsTheReplyBack() async {
        let script = ScriptedWorkflowScript([
            #"{"request":{"method":"POST","url":"https://ok.example.com/a","headers":{"X":"1"},"body":"{}"},"session":"1"}"#,
            #"{"request":{"method":"GET","url":"https://ok.example.com/b","headers":{},"body":null},"session":"1"}"#,
            #"{"done":{"status":"success","error":"","requests":2,"nodes":{},"trigger":"manual"}}"#,
        ])
        let engine = WorkflowEngine(script: script, spec: ["version": "v1", "userAgent": "UA"])
        let transport = RecordingTransport([WorkflowReply(status: 201, text: "ok"), WorkflowReply(status: 200, text: "[]")])
        let report = await engine.run(["name": "W"], credentials: [credentialId: ["integration": "slack"]], workflowId: workflowId,
                                      nowMs: 5, transport: transport)
        XCTAssertEqual(report.str("status"), "success")
        XCTAssertEqual(transport.sent.map(\.url), ["https://ok.example.com/a", "https://ok.example.com/b"])
        XCTAssertEqual(transport.sent[0].body, "{}")
        XCTAssertNil(transport.sent[1].body)
        XCTAssertEqual(script.replies, [#"{"status":201,"text":"ok"}"#, #"{"status":200,"text":"[]"}"#])
        let input = JSON.parseObject(script.inputs[0])
        XCTAssertEqual(input.str("workflowId"), workflowId)
        XCTAssertEqual(input.int("nowMs"), 5)
        XCTAssertEqual(input.obj("credentials").obj(credentialId).str("integration"), "slack")
        XCTAssertEqual(engine.clean(["name": "x"]).error, "cycle")
    }

    func testTheTransportChecksEveryRedirectAndSwitchesPostToGet() async {
        let client = FakeHTTPClient()
        client.enqueue(HTTPResponse(status: 302, headers: ["Location": "/next"], text: ""))
        client.enqueue(HTTPResponse(status: 200, text: "done"))
        let transport = HTTPWorkflowTransport(client: client, userAgent: "ForkMesh-Workflows/1.0")
        let reply = await transport.send(
            WorkflowRequest(method: "POST", url: "https://ok.example.com/start", headers: ["Content-Type": "application/json"], body: "{}"),
            allowed: { $0.hasPrefix("https://ok.example.com") })
        XCTAssertEqual(reply, WorkflowReply(status: 200, text: "done"))
        XCTAssertEqual(client.requests.map(\.url.absoluteString), ["https://ok.example.com/start", "https://ok.example.com/next"])
        XCTAssertEqual(client.requests.map(\.method), ["POST", "GET"])
        XCTAssertNil(client.requests[1].body)
        XCTAssertFalse(client.requests[0].followRedirects)
        XCTAssertEqual(client.requests[0].headers["User-Agent"], "ForkMesh-Workflows/1.0")

        client.reset()
        client.enqueue(HTTPResponse(status: 301, headers: ["Location": "https://10.0.0.1/admin"], text: ""))
        let blocked = await transport.send(WorkflowRequest(method: "GET", url: "https://ok.example.com/x"),
                                           allowed: { $0.hasPrefix("https://ok.example.com") })
        XCTAssertEqual(blocked, .noResponse)
        XCTAssertEqual(client.requests.count, 1)
    }

    func testTheTransportReportsNoResponseWhenTheRequestFails() async {
        let client = FakeHTTPClient()
        client.enqueue(error: HTTPError.timedOut)
        let transport = HTTPWorkflowTransport(client: client, userAgent: "UA")
        let reply = await transport.send(WorkflowRequest(method: "GET", url: "https://ok.example.com/x"), allowed: { _ in true })
        XCTAssertEqual(reply, .noResponse)
    }
}

final class WorkflowsStoreTests: XCTestCase {
    @MainActor func testLoadListsWorkflowsAndMapsRefusals() async {
        let api = FakeWorkflowsApi { _, suffix, _ in
            if suffix == "/catalog" { return catalogJSON() }
            return ["workflows": [["id": .string(workflowId), "name": "Daily", "active": true, "trigger": "core.schedule",
                                   "nodes": 3, "scheduleMinutes": 60, "lastStatus": "success"]]]
        }
        let store = WorkflowsStore(call: api.call, engine: { throw WorkflowScriptError.unavailable }, secureStore: InMemorySecureStore())
        await store.load()
        XCTAssertEqual(store.workflows.map(\.name), ["Daily"])
        XCTAssertEqual(store.workflows[0].steps, 3)
        XCTAssertEqual(store.catalog.kind("app.slack")?.name, "Slack")
        XCTAssertFalse(store.canRunOnPhone)

        api.handler = { _, _, _ in throw OrgApiException("plugin_not_installed", "") }
        let empty = WorkflowsStore(call: api.call, engine: { throw WorkflowScriptError.unavailable }, secureStore: InMemorySecureStore())
        await empty.load()
        XCTAssertEqual(empty.error, "Install the Workflows plugin from the Market to use workflows.")
        XCTAssertTrue(empty.showsError)
    }

    @MainActor func testSavingCreatesThenUpdatesAndRunsOnTheServer() async throws {
        let api = FakeWorkflowsApi { method, suffix, body in
            switch (method, suffix) {
            case ("POST", ""), ("POST", "/\(workflowId)"):
                var saved = body ?? [:]
                saved["id"] = .string(workflowId)
                return ["workflow": .object(saved)]
            case ("POST", "/\(workflowId)/run"):
                return ["run": ["id": "r1", "status": "success", "requests": 0, "startedAt": 10, "nodes": [:]]]
            case ("GET", "/\(workflowId)/runs"):
                return ["runs": [["id": "r1", "trigger": "manual", "status": "success", "startedAt": 10]]]
            default:
                throw OrgApiException("not_found", "")
            }
        }
        let store = WorkflowsStore(call: api.call, engine: { throw WorkflowScriptError.unavailable }, secureStore: InMemorySecureStore())
        var draft = WorkflowDraft.blank
        draft.name = "Mine"
        let created = try await store.save(draft)
        XCTAssertEqual(api.calls.last?.suffix, "")
        XCTAssertEqual(api.calls.last?.body?.str("name"), "Mine")
        XCTAssertEqual(api.calls.last?.body?.bool("active"), false)
        XCTAssertEqual(created.id, workflowId)
        _ = try await store.save(created)
        XCTAssertEqual(api.calls.last?.suffix, "/\(workflowId)")
        XCTAssertEqual(store.workflows.count, 1)
        let run = try await store.runOnServer(created)
        XCTAssertEqual(run.place, .server)
        XCTAssertEqual(store.workflows[0].lastStatus, "success")
        let phone = await store.runOnPhone(created)
        XCTAssertEqual(phone.error, WorkflowsText.phoneEngineMissing)
        let runs = try await store.runs(workflowId)
        XCTAssertEqual(runs.map(\.place), [.server], "a phone run that could not start is not kept")
    }

    @MainActor func testPhoneRunsUseOnlyCredentialsSavedOnThePhone() async {
        let script = ScriptedWorkflowScript([#"{"done":{"status":"success","error":"","requests":0,"nodes":{}}}"#])
        let api = FakeWorkflowsApi { _, _, _ in catalogJSON() }
        let secure = InMemorySecureStore()
        let store = WorkflowsStore(call: api.call, engine: { WorkflowEngine(script: script, spec: [:]) },
                                   transport: { _ in RecordingTransport([]) }, secureStore: secure,
                                   clock: FixedClock(epochMillis: 1_000))
        await store.loadCatalog()
        var draft = WorkflowDraft.blank
        var step = draft.addStep(WorkflowCatalog(json: catalogJSON()).kind("app.slack")!, after: "trigger")
        step.credential = credentialId
        draft.update(step)
        XCTAssertEqual(store.missingPhoneCredentials(draft).map(\.id), [step.id])
        store.savePhoneCredential(WorkflowCredential(json: ["id": .string(credentialId), "integration": "slack"]), values: ["token": "xoxb"])
        XCTAssertTrue(store.missingPhoneCredentials(draft).isEmpty)
        XCTAssertNotNil(secure.read(WorkflowPhoneCredentials.prefix + credentialId))
        let run = await store.runOnPhone(draft)
        XCTAssertTrue(run.succeeded)
        XCTAssertEqual(run.place, .phone)
        let input = JSON.parseObject(script.inputs[0])
        XCTAssertEqual(input.obj("credentials").obj(credentialId).obj("values").str("token"), "xoxb")
        XCTAssertEqual(store.phoneRuns["draft"]?.count, 1)
        store.removePhoneCredential(credentialId)
        XCTAssertNil(secure.read(WorkflowPhoneCredentials.prefix + credentialId))
    }

    @MainActor func testCreatingACredentialCanKeepACopyOnThePhone() async throws {
        let api = FakeWorkflowsApi { _, suffix, body in
            XCTAssertEqual(suffix, "/credentials")
            XCTAssertEqual(body?.obj("values").str("token"), "secret")
            return ["credential": ["id": .string(credentialId), "integration": "slack", "name": "Team", "hint": ["token": "••••"]]]
        }
        let store = WorkflowsStore(call: api.call, engine: { throw WorkflowScriptError.unavailable }, secureStore: InMemorySecureStore())
        let credential = try await store.createCredential(integration: "slack", name: "Team", values: ["token": "secret"], keepOnPhone: true)
        XCTAssertEqual(credential.hintText, "token ••••")
        XCTAssertTrue(store.phoneCredentialIds.contains(credentialId))
        XCTAssertEqual(store.credentials(for: "slack").count, 1)
    }
}

#if canImport(JavaScriptCore)
final class WorkflowConformanceTests: XCTestCase {
    func cases() throws -> JSONObject {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "workflow-cases", withExtension: "json", subdirectory: "Fixtures"))
        return JSON.parseObject(try Data(contentsOf: url))
    }

    func withoutMs(_ value: JSONValue) -> JSONValue {
        switch value {
        case .object(let object):
            return .object(JSONObject(object.filter { $0.key != "ms" }.map { ($0.key, withoutMs($0.value)) }))
        case .array(let items):
            return .array(items.map(withoutMs))
        default:
            return value
        }
    }

    func testJavaScriptCoreRunsEveryCaseLikeTheServer() async throws {
        let cases = try cases()
        let engine = try WorkflowEngine.onDevice()
        var failures: [String] = []
        for item in cases.maps("runs") {
            let responses = item.maps("responses").map { WorkflowReply(status: $0.int("status"), text: $0.str("text")) }
            let transport = RecordingTransport(responses)
            let report = await engine.run(
                item.obj("workflow"), payload: item["payload"], credentials: item.obj("credentials"),
                trigger: item.str("trigger"), workflowId: cases.str("workflowId"), nowMs: cases["nowMs"]?.int64Value,
                transport: transport)
            let expected = item.obj("expect")
            if withoutMs(.object(report)) != withoutMs(expected["report"] ?? .null) {
                failures.append(item.str("name"))
                continue
            }
            let sent: [JSONValue] = transport.sent.map {
                .object(["method": .string($0.method), "url": .string($0.url), "headers": .object($0.headers),
                         "body": $0.body.map(JSONValue.string) ?? .null])
            }
            if sent != expected.list("requests") { failures.append(item.str("name") + " requests") }
        }
        XCTAssertEqual(failures, [])
    }

    func testJavaScriptCoreAgreesOnUrlsCleaningAndTheCatalog() throws {
        let cases = try cases()
        let engine = try WorkflowEngine.onDevice()
        for item in cases.maps("safeUrl") {
            XCTAssertEqual(engine.isSafe(item.str("url")), !item.str("safe").isEmpty, item.str("url"))
        }
        for item in cases.maps("clean") {
            let checked = engine.call("clean", [item["input"] ?? .null]).objectValue ?? [:]
            XCTAssertEqual(checked.str("error"), item.str("error"))
            if let workflow = item["workflow"]?.objectValue { XCTAssertEqual(checked.obj("workflow"), workflow) }
        }
        let catalog = engine.catalog()
        let expected = WorkflowCatalog(json: cases.obj("catalog"))
        XCTAssertEqual(catalog, expected)
        XCTAssertEqual(catalog.integrations.count, 100)
    }
}
#endif

final class WorkflowSummaryTests: XCTestCase {
    func testStepSummariesNameTheOperationAndCondition() {
        let catalog = WorkflowCatalog(json: catalogJSON())
        let slack = WorkflowNode(id: "s", type: "app.slack", name: "Post", params: ["operation": "postMessage"])
        XCTAssertEqual(catalog.summary(slack), "Send a message")
        let branch = WorkflowNode(id: "i", type: "core.if", name: "If", params: ["left": "{{ $json.a }}", "operator": "equals", "right": "1"],
                                  disabled: true)
        XCTAssertEqual(catalog.summary(branch), "{{ $json.a }} is equal to 1 · Off")
        let draft = WorkflowDraft(nodes: [branch, slack], connections: [WorkflowConnection(from: "i", output: "true", to: "s")])
        XCTAssertEqual(catalog.branch(draft.inputs(of: "s")[0], in: draft), "If: true")
        XCTAssertEqual(catalog.outputs("core.if"), ["true", "false"])
    }
}
