import FMApi
import FMBase
import FMChat
import FMControl
import FMNet
import Foundation
import Observation

public typealias WorkflowsApiCall = @MainActor (_ method: String, _ suffix: String, _ body: JSONObject?) async throws -> JSONObject

public enum WorkflowsText {
    public static let title = "Workflows"
    public static let unavailableTitle = "Workflows are unavailable"
    public static let emptyTitle = "No workflows yet"
    public static let emptyMessage =
        "A workflow is a trigger followed by steps: HTTP requests, logic and 100 integrations. Start from an example or a blank workflow."
    public static let phoneRunNote =
        "Runs on this phone use JavaScriptCore and the credentials saved on this phone. Runs on ForkMesh use your account's credentials."
    public static let phoneEngineMissing = "This phone cannot run workflows itself. Run it on ForkMesh instead."
    public static let desktopRunNote =
        "These steps run on your desktop, in its forkmesh/forkmesh checkout. Only ForkMesh leftovers that nothing uses are deleted."
    public static let desktopMissing = "Pair this iPhone with your desktop to run these steps."

    static let errors: [String: String] = [
        "plugin_not_installed": "Install the Workflows plugin from the Market to use workflows.",
        "too_many_workflows": "You have 50 workflows, the most an account can keep.",
        "too_many_credentials": "You have 50 credentials, the most an account can keep.",
        "rate_limited": "This account ran 120 workflows in the last hour. Try again later.",
        "no_trigger": "Add a trigger before running this workflow.",
        "not_found": "This workflow no longer exists.",
        "too_many_nodes": "A workflow can have at most 60 steps.",
        "too_many_connections": "A workflow can have at most 120 connections.",
        "too_many_triggers": "A workflow can have only one trigger.",
        "duplicate_node_name": "Every step needs its own name.",
        "invalid_node": "A step has an invalid id.",
        "unknown_node_type": "A step uses an integration this version does not know.",
        "value_too_long": "A value is longer than 20,000 characters.",
        "invalid_credential": "A step points at an invalid credential.",
        "invalid_connection": "A connection is invalid: steps cannot connect to themselves or into the trigger.",
        "cycle": "Steps cannot connect in a loop.",
        "invalid_workflow": "This workflow could not be read.",
        "missing_credential_field": "Fill in every credential field.",
        "invalid_credential_url": "Credential URLs must be public https:// addresses.",
        "unknown_integration": "This integration is not available.",
        "no_credential_needed": "This integration does not need a credential.",
    ]

    public static func error(_ code: String) -> String { errors[code] ?? code.replacingOccurrences(of: "_", with: " ") }

    public static func describe(_ error: Error, _ fallback: String) -> String {
        guard let refusal = error as? OrgApiException else { return fallback }
        if let known = errors[refusal.code] { return known }
        return refusal.message.isEmpty ? fallback : refusal.message
    }

    public static func trigger(_ type: String) -> String {
        switch type {
        case "core.manual": return "Manual"
        case "core.webhook": return "Webhook"
        case "core.schedule": return "Schedule"
        default: return "No trigger"
        }
    }

    public static func status(_ status: String) -> String {
        switch status {
        case "success": return "Succeeded"
        case "error": return "Failed"
        case "skipped": return "Skipped"
        case "": return "Never run"
        default: return status.capitalized
        }
    }

    public static func every(_ minutes: Int) -> String {
        if minutes <= 0 { return "" }
        if minutes % 1440 == 0 { return minutes == 1440 ? "Every day" : "Every \(minutes / 1440) days" }
        if minutes % 60 == 0 { return minutes == 60 ? "Every hour" : "Every \(minutes / 60) hours" }
        return "Every \(minutes) minutes"
    }
}

public final class WorkflowPhoneCredentials: @unchecked Sendable {
    public static let prefix = "workflows.credential."

    private let store: SecureStore

    public init(store: SecureStore) {
        self.store = store
    }

    public func has(_ id: String) -> Bool { record(id) != nil }

    public func record(_ id: String) -> JSONObject? {
        guard !id.isEmpty, let text = store.read(Self.prefix + id) else { return nil }
        let record = JSON.parseObject(text)
        return record.str("integration").isEmpty ? nil : record
    }

    public func save(_ id: String, integration: String, values: [String: String]) {
        let ordered = JSONObject(values.keys.sorted().map { ($0, JSONValue.string(values[$0] ?? "")) })
        store.write(Self.prefix + id, JSON.encode(.object(["integration": .string(integration), "values": .object(ordered)])))
    }

    public func remove(_ id: String) { store.delete(Self.prefix + id) }

    public func credentials(for draft: WorkflowDraft) -> JSONObject {
        var out = JSONObject()
        for node in draft.nodes where !node.credential.isEmpty {
            if let record = record(node.credential) { out[node.credential] = .object(record) }
        }
        return out
    }
}

@MainActor
@Observable
public final class WorkflowsStore {
    public static let phoneRunsKept = 30

    @ObservationIgnored public let call: WorkflowsApiCall
    @ObservationIgnored private let makeEngine: () throws -> WorkflowEngine
    @ObservationIgnored private let makeTransport: (WorkflowEngine) -> WorkflowTransport
    @ObservationIgnored public let phoneCredentials: WorkflowPhoneCredentials
    @ObservationIgnored private let clock: Clock
    @ObservationIgnored private var engineCache: WorkflowEngine?
    @ObservationIgnored private var engineTried = false
    @ObservationIgnored public var desktopChannel: DesktopControlCalling?

    public private(set) var workflows: [WorkflowSummary] = []
    public private(set) var templates: [WorkflowTemplate] = []
    public private(set) var credentials: [WorkflowCredential] = []
    public private(set) var catalog: WorkflowCatalog = .empty
    public private(set) var loading = true
    public private(set) var error = ""
    public private(set) var phoneRuns: [String: [WorkflowRun]] = [:]
    public private(set) var phoneCredentialIds: Set<String> = []
    public private(set) var desktopLive: [String: String] = [:]
    public private(set) var desktopRunning = ""

    public init(
        call: @escaping WorkflowsApiCall,
        engine: @escaping () throws -> WorkflowEngine = { try WorkflowEngine.onDevice() },
        transport: @escaping (WorkflowEngine) -> WorkflowTransport = { HTTPWorkflowTransport(client: URLSessionHTTPClient(), engine: $0) },
        secureStore: SecureStore,
        clock: Clock = SystemClock()
    ) {
        self.call = call
        makeEngine = engine
        makeTransport = transport
        phoneCredentials = WorkflowPhoneCredentials(store: secureStore)
        self.clock = clock
    }

    public convenience init(api: ApiService, secureStore: SecureStore) {
        self.init(call: { method, suffix, body in try await api.workflowsApi(method, suffix, body) }, secureStore: secureStore)
    }

    public var engine: WorkflowEngine? {
        if !engineTried {
            engineTried = true
            engineCache = try? makeEngine()
        }
        return engineCache
    }

    public var canRunOnPhone: Bool { engine != nil }

    public var showsSpinner: Bool { loading && workflows.isEmpty }
    public var showsError: Bool { !error.isEmpty && workflows.isEmpty }

    public func load() async {
        loading = true
        error = ""
        do {
            let data = try await call("GET", "", nil)
            workflows = data.maps("workflows").map(WorkflowSummary.init(json:))
            loading = false
        } catch {
            loading = false
            self.error = WorkflowsText.describe(error, "Workflows are unavailable right now.")
        }
        if catalog.core.isEmpty { await loadCatalog() }
    }

    public func loadCatalog() async {
        if let engine {
            catalog = engine.catalog()
            return
        }
        if let data = try? await call("GET", "/catalog", nil) { catalog = WorkflowCatalog(json: data) }
    }

    public func loadTemplates() async -> String {
        do {
            templates = try await call("GET", "/templates", nil).maps("templates").map(WorkflowTemplate.init(json:))
            return ""
        } catch {
            return WorkflowsText.describe(error, "Examples are unavailable right now.")
        }
    }

    public func loadCredentials() async -> String {
        do {
            credentials = try await call("GET", "/credentials", nil).maps("credentials").map(WorkflowCredential.init(json:))
            phoneCredentialIds = Set(credentials.map(\.id).filter(phoneCredentials.has))
            return ""
        } catch {
            return WorkflowsText.describe(error, "Credentials are unavailable right now.")
        }
    }

    public func open(_ id: String) async throws -> WorkflowDraft {
        WorkflowDraft(json: try await call("GET", "/\(id)", nil).obj("workflow"))
    }

    public func draft(fromTemplate key: String) async throws -> WorkflowDraft {
        let template = try await call("GET", "/templates/\(key)", nil).obj("template")
        var draft = WorkflowDraft(json: template.obj("workflow"))
        if draft.name.isEmpty || draft.name == "Untitled workflow" { draft.name = template.str("name", draft.name) }
        return draft
    }

    public func check(_ draft: WorkflowDraft) -> String {
        guard let engine else { return "" }
        let checked = engine.clean(draft.graph)
        return checked.error.isEmpty ? "" : WorkflowsText.error(checked.error)
    }

    public func save(_ draft: WorkflowDraft) async throws -> WorkflowDraft {
        let answer = draft.id.isEmpty
            ? try await call("POST", "", draft.body)
            : try await call("POST", "/\(draft.id)", draft.body)
        let saved = WorkflowDraft(json: answer.obj("workflow"))
        upsert(WorkflowSummary(json: answer.obj("workflow")))
        return saved
    }

    public func setActive(_ id: String, _ active: Bool) async throws {
        var draft = try await open(id)
        draft.active = active
        _ = try await save(draft)
    }

    public func delete(_ id: String) async throws {
        _ = try await call("POST", "/\(id)/delete", nil)
        workflows.removeAll { $0.id == id }
        phoneRuns[id] = nil
    }

    public func runOnServer(_ draft: WorkflowDraft) async throws -> WorkflowRun {
        let run = WorkflowRun(json: try await call("POST", "/\(draft.id)/run", [:]).obj("run"), place: .server)
        if let index = workflows.firstIndex(where: { $0.id == draft.id }) {
            workflows[index].lastStatus = run.status
            workflows[index].lastRunAt = run.startedAt
        }
        return run
    }

    public func runOnPhone(_ draft: WorkflowDraft, payload: JSONValue? = nil) async -> WorkflowRun {
        let started = clock.nowMillis
        guard let engine else {
            return WorkflowRun(json: ["status": "error", "error": .string(WorkflowsText.phoneEngineMissing)], place: .phone,
                               id: "phone-\(started)", startedAt: started, finishedAt: started)
        }
        let linked = await linkedWorkflows(draft, depth: engine.maxWorkflowDepth - 1)
        var credentials = phoneCredentials.credentials(for: draft)
        for child in linked { credentials.merge(phoneCredentials.credentials(for: child)) }
        let report = await engine.run(
            draft.graph, payload: payload, credentials: credentials, trigger: "manual", workflowId: draft.id,
            workflows: linked.map { (child: WorkflowDraft) -> JSONValue in
                .object(["id": .string(child.id), "name": .string(child.name), "workflow": .object(child.graph)])
            },
            transport: makeTransport(engine))
        let run = WorkflowRun(json: report, place: .phone, id: "phone-\(started)", startedAt: started, finishedAt: clock.nowMillis)
        let key = draft.id.isEmpty ? "draft" : draft.id
        phoneRuns[key] = Array(([run] + (phoneRuns[key] ?? [])).prefix(Self.phoneRunsKept))
        return run
    }

    public var canRunOnDesktop: Bool { desktopChannel != nil }

    public func runOnDesktop(_ draft: WorkflowDraft) async -> WorkflowRun {
        let started = clock.nowMillis
        guard let channel = desktopChannel else {
            return WorkflowRun(json: ["status": "error", "error": .string(WorkflowsText.desktopMissing)], place: .desktop,
                               id: "desktop-\(started)", startedAt: started, finishedAt: started)
        }
        let family = DesktopControlFamily(.diagnostics, channel: channel)
        desktopRunning = draft.id
        desktopLive = [:]
        defer {
            desktopRunning = ""
            desktopLive = [:]
        }
        var report = JSONObject()
        var failed = ""
        var freed: Int64 = 0
        var free: Int64?
        var total: Int64?
        let passed: JSONObject = ["status": "success", "ms": 0, "outputs": ["main": ["count": 1, "items": [[:]]]]]
        for node in draft.ordered {
            let stepStarted = clock.nowMillis
            if node.isTrigger {
                report[node.id] = .object(passed)
                desktopLive[node.id] = "success"
                continue
            }
            let fed = draft.inputs(of: node.id).contains { report[$0.from]?.objectValue?.str("status") == "success" }
            if !failed.isEmpty || !fed {
                report[node.id] = .object(["status": "skipped"])
                desktopLive[node.id] = "skipped"
                continue
            }
            guard !node.disabled, let step = WorkflowDesktopSteps.step(node.type) else {
                report[node.id] = .object(passed)
                desktopLive[node.id] = "success"
                continue
            }
            desktopLive[node.id] = "running"
            do {
                let prepared = try await family.call("prepare", ["action": "freeDiskSpace", "step": .string(step)], timeout: 30)
                let result = try await family.call(
                    "run", ["action": "freeDiskSpace", "step": .string(step), "token": .string(prepared.str("token"))],
                    timeout: WorkflowDesktopSteps.stepTimeout)
                let stepFreed = result["freedBytes"]?.int64Value ?? 0
                let stepFree = result["freeBytes"]?.int64Value ?? 0
                freed += stepFreed
                if let stepTotal = result["totalBytes"]?.int64Value, stepTotal > 0 {
                    free = stepFree
                    total = stepTotal
                }
                let item: JSONObject = [
                    "step": .string(step), "freedBytes": .int(stepFreed), "freed": .string(WorkflowDesktopSteps.bytes(stepFreed)),
                    "items": .int(result["items"]?.int64Value ?? 0), "freeBytes": .int(stepFree),
                    "free": .string(WorkflowDesktopSteps.bytes(stepFree)),
                ]
                report[node.id] = .object([
                    "status": "success", "ms": .int(clock.nowMillis - stepStarted),
                    "outputs": ["main": ["count": 1, "items": [.object(item)]]],
                    "logs": .array(result.list("lines").map { .string($0.text) }),
                ])
                desktopLive[node.id] = "success"
            } catch {
                let text = DesktopWorkflowsText.describe(error)
                failed = "\(node.name): \(text)"
                report[node.id] = .object(["status": "error", "ms": .int(clock.nowMillis - stepStarted), "error": .string(text)])
                desktopLive[node.id] = "error"
            }
        }
        let finished = clock.nowMillis
        var output: JSONObject = ["freedBytes": .int(freed)]
        if let free, let total {
            output["freeBytes"] = .int(free)
            output["totalBytes"] = .int(total)
        }
        let local = WorkflowRun(
            json: ["status": .string(failed.isEmpty ? "success" : "error"), "error": .string(failed), "nodes": .object(report),
                   "trigger": "desktop"],
            place: .desktop, id: "desktop-\(started)", startedAt: started, finishedAt: finished)
        guard !draft.id.isEmpty else { return local }
        do {
            let body: JSONObject = [
                "startedAt": .int(started), "finishedAt": .int(finished), "nodes": .object(report), "output": .object(output),
            ]
            let run = WorkflowRun(json: try await call("POST", "/\(draft.id)/record", body).obj("run"), place: .desktop)
            if let index = workflows.firstIndex(where: { $0.id == draft.id }) {
                workflows[index].lastStatus = run.status
                workflows[index].lastRunAt = run.startedAt
            }
            return run
        } catch {
            var unsaved = local
            unsaved.error = [failed, "The run finished but ForkMesh could not save it."].filter { !$0.isEmpty }.joined(separator: " ")
            return unsaved
        }
    }

    public func linkedWorkflows(_ draft: WorkflowDraft, depth: Int) async -> [WorkflowDraft] {
        var found: [String: WorkflowDraft] = [:]
        var queue: [(draft: WorkflowDraft, level: Int)] = [(draft, 0)]
        while !queue.isEmpty {
            let (current, level) = queue.removeFirst()
            guard level < depth else { continue }
            for node in current.nodes where node.type == "core.runWorkflow" {
                let ref = node.text("workflow").trimmingCharacters(in: .whitespacesAndNewlines)
                let targets = ref.contains("{{") ? workflows : Array(workflows.filter { (summary: WorkflowSummary) in
                    summary.id == ref || summary.name.trimmingCharacters(in: .whitespacesAndNewlines).lowercased() == ref.lowercased()
                }.prefix(1))
                for summary in targets where found[summary.id] == nil && summary.id != draft.id {
                    guard let child = try? await open(summary.id) else { continue }
                    found[summary.id] = child
                    queue.append((child, level + 1))
                }
            }
        }
        if draft.nodes.contains(where: { (node: WorkflowNode) in node.type == "core.runWorkflow" }), !draft.id.isEmpty,
           found[draft.id] == nil {
            found[draft.id] = draft
        }
        return workflows.compactMap { (summary: WorkflowSummary) in found[summary.id] }
    }

    public func runs(_ id: String) async throws -> [WorkflowRun] {
        let server = try await call("GET", "/\(id)/runs", nil).maps("runs").map { WorkflowRun(json: $0, place: .server) }
        return ((phoneRuns[id] ?? []) + server).sorted { $0.startedAt > $1.startedAt }
    }

    public func run(_ id: String, _ runId: String) async throws -> WorkflowRun {
        if let local = phoneRuns[id]?.first(where: { $0.id == runId }) { return local }
        return WorkflowRun(json: try await call("GET", "/\(id)/runs/\(runId)", nil).obj("run"), place: .server)
    }

    public func missingPhoneCredentials(_ draft: WorkflowDraft) -> [WorkflowNode] {
        draft.nodes.filter { node in
            guard !node.disabled, let kind = catalog.kind(node.type), kind.needsCredential else { return false }
            return node.credential.isEmpty || !phoneCredentials.has(node.credential)
        }
    }

    public func createCredential(integration: String, name: String, values: [String: String], keepOnPhone: Bool) async throws -> WorkflowCredential {
        let body: JSONObject = [
            "integration": .string(integration), "name": .string(name),
            "values": .object(JSONObject(values.keys.sorted().map { ($0, JSONValue.string(values[$0] ?? "")) })),
        ]
        let credential = WorkflowCredential(json: try await call("POST", "/credentials", body).obj("credential"))
        if keepOnPhone { phoneCredentials.save(credential.id, integration: integration, values: values) }
        credentials.insert(credential, at: 0)
        if keepOnPhone { phoneCredentialIds.insert(credential.id) }
        return credential
    }

    public func savePhoneCredential(_ credential: WorkflowCredential, values: [String: String]) {
        phoneCredentials.save(credential.id, integration: credential.integration, values: values)
        phoneCredentialIds.insert(credential.id)
    }

    public func removePhoneCredential(_ id: String) {
        phoneCredentials.remove(id)
        phoneCredentialIds.remove(id)
    }

    public func deleteCredential(_ id: String) async throws {
        _ = try await call("POST", "/credentials/\(id)/delete", nil)
        credentials.removeAll { $0.id == id }
        removePhoneCredential(id)
    }

    public func credentials(for integration: String) -> [WorkflowCredential] {
        credentials.filter { $0.integration == integration }
    }

    private func upsert(_ summary: WorkflowSummary) {
        guard !summary.id.isEmpty else { return }
        if let index = workflows.firstIndex(where: { $0.id == summary.id }) {
            workflows[index] = summary
        } else {
            workflows.insert(summary, at: 0)
        }
    }
}
