import Foundation
import FMBase

public struct RepoActions: Equatable, Hashable, Sendable {
    public var workflows: [ActionWorkflow]
    public var runs: [ActionRun]

    public init(workflows: [ActionWorkflow] = [], runs: [ActionRun] = []) {
        self.workflows = workflows
        self.runs = runs
    }

    public init(json: JSONObject) {
        self.init(
            workflows: json.maps("workflows").map(ActionWorkflow.init(json:)),
            runs: json.maps("runs").map(ActionRun.init(json:))
        )
    }
}

public struct ActionWorkflow: Equatable, Hashable, Sendable {
    public var path: String
    public var name: String
    public var triggers: [String]
    public var stepCount: Int
    public var manual: Bool
    public var valid: Bool
    public var error: String

    public init(
        path: String = "", name: String = "", triggers: [String] = [], stepCount: Int = 0,
        manual: Bool = false, valid: Bool = true, error: String = ""
    ) {
        self.path = path
        self.name = name
        self.triggers = triggers
        self.stepCount = stepCount
        self.manual = manual
        self.valid = valid
        self.error = error
    }

    public init(json: JSONObject) {
        let rawTriggers: [JSONValue]
        if let list = json["on"]?.arrayValue {
            rawTriggers = list
        } else if let list = json["triggers"]?.arrayValue {
            rawTriggers = list
        } else if let on = json["on"], on.stringValue != nil {
            rawTriggers = [on]
        } else {
            rawTriggers = []
        }
        self.init(
            path: json.str("path"),
            name: json.str("name"),
            triggers: rawTriggers.map { DartCore.toString($0) },
            stepCount: ModelJSON.int(ModelJSON.coalesce(json["stepCount"], json["steps"])),
            manual: json.bool("manual"),
            valid: json["valid"] != .bool(false),
            error: json.str("error")
        )
    }

    public var displayName: String { name.isEmpty ? path : name }
    public var triggerLabel: String { triggers.isEmpty ? "No triggers" : triggers.joined(separator: ", ") }
}

public struct ActionRun: Equatable, Hashable, Sendable, Identifiable {
    public var id: Int
    public var workflowPath: String
    public var workflowName: String
    public var commit: String
    public var ref: String
    public var status: String
    public var createdAtMs: Int
    public var startedAtMs: Int
    public var finishedAtMs: Int

    public init(
        id: Int, workflowPath: String = "", workflowName: String = "", commit: String = "", ref: String = "",
        status: String = "", createdAtMs: Int = 0, startedAtMs: Int = 0, finishedAtMs: Int = 0
    ) {
        self.id = id
        self.workflowPath = workflowPath
        self.workflowName = workflowName
        self.commit = commit
        self.ref = ref
        self.status = status
        self.createdAtMs = createdAtMs
        self.startedAtMs = startedAtMs
        self.finishedAtMs = finishedAtMs
    }

    public init(json: JSONObject) {
        self.init(
            id: ModelJSON.int(json["id"]),
            workflowPath: json.str("workflowPath"),
            workflowName: json.str("workflowName"),
            commit: json.str("commit"),
            ref: json.str("ref"),
            status: json.str("status"),
            createdAtMs: ModelJSON.int(json["createdAtMs"]),
            startedAtMs: ModelJSON.int(json["startedAtMs"]),
            finishedAtMs: ModelJSON.int(json["finishedAtMs"])
        )
    }

    public var isActive: Bool {
        let clean = ModelJSON.normalizedStatus(status)
        return clean == "queued" || clean == "running" || clean == "awaiting-approval"
    }

    public var displayName: String { workflowName.isEmpty ? "Action run #\(id)" : workflowName }

    public var statusLabel: String {
        switch ModelJSON.normalizedStatus(status) {
        case "awaiting-approval": return "Awaiting approval"
        case "queued": return "Queued"
        case "running": return "Running"
        case "success", "passed": return "Success"
        case "failed", "failure", "error": return "Failed"
        case "rejected": return "Rejected"
        case "cancelled", "canceled", "stopped": return "Cancelled"
        default: return status.isEmpty ? "Unknown" : ModelJSON.titleCase(status)
        }
    }

    public var refLabel: String {
        if ref.hasPrefix("refs/heads/") { return String(ref.dropFirst("refs/heads/".count)) }
        if ref.hasPrefix("refs/tags/") { return String(ref.dropFirst("refs/tags/".count)) }
        return ref
    }

    public var shortCommit: String { DartCore.length(commit) <= 8 ? commit : DartCore.prefix(commit, 8) }

    public var durationLabel: String {
        if startedAtMs <= 0 || finishedAtMs <= startedAtMs { return "" }
        let totalSeconds = (finishedAtMs - startedAtMs) / 1_000
        let minutes = totalSeconds / 60
        let seconds = totalSeconds % 60
        if minutes <= 0 { return "\(seconds)s" }
        return "\(minutes)m \(seconds)s"
    }
}

public struct ActionLog: Equatable, Hashable, Sendable {
    public var id: Int
    public var status: String
    public var log: String

    public init(id: Int, status: String = "", log: String = "") {
        self.id = id
        self.status = status
        self.log = log
    }

    public init(json: JSONObject) {
        self.init(id: ModelJSON.int(json["id"]), status: json.str("status"), log: json.str("log"))
    }

    public var statusLabel: String { ActionRun(id: id, status: status).statusLabel }
}

public struct DesktopCommandResult: Equatable, Hashable, Sendable {
    public var ok: Bool
    public var queued: Int

    public init(ok: Bool, queued: Int = 0) {
        self.ok = ok
        self.queued = queued
    }

    public init(json: JSONObject) {
        self.init(ok: json.bool("ok"), queued: ModelJSON.int(json["queued"]))
    }

    public static func path(_ owner: String, _ name: String) -> String {
        "/api/repo/\(owner)/\(name)/desktop-commands"
    }

    public static func routeAnswered(status: Int, code: String) -> Bool? {
        if status == 403 && code == "not_authorized" { return true }
        if status == 400 && (code == "invalid_json" || code == "unsupported_command") { return true }
        if status == 404 || status == 405 { return false }
        return nil
    }

    public static func errorMessage(status: Int, code: String) -> String {
        switch code {
        case "not_authorized":
            return "Only the repo owner can request desktop control for this repo."
        case "unsupported_command":
            return "The server does not accept that desktop request."
        case "signature_required", "invalid_signature":
            return "The server could not verify this device's signature. Pair this device with the owner key and try again."
        case "stale_request":
            return "This device's clock is more than five minutes off. Correct the time and try again."
        case "replayed_request":
            return "That request was already sent."
        case "command_queue_full":
            return "Fifty requests are already waiting for the desktop. Approve or decline them there first."
        default:
            if status == 404 || status == 405 { return "This server does not take desktop requests yet." }
            return "Desktop request was not queued (HTTP \(status))."
        }
    }
}

public enum ActionsRunStatus: String, Codable, Hashable, Sendable, CaseIterable {
    case awaitingApproval = "awaiting-approval"
    case queued, running, success, failed, rejected, cancelled, skipped
    case unknown

    public init(raw: String) {
        let clean = raw.trimmingCharacters(in: .whitespaces).lowercased()
        switch clean {
        case "awaiting-approval", "awaiting_approval": self = .awaitingApproval
        case "passed": self = .success
        case "failure", "error": self = .failed
        case "canceled", "stopped": self = .cancelled
        default: self = ActionsRunStatus(rawValue: clean) ?? .unknown
        }
    }

    public var label: String {
        switch self {
        case .awaitingApproval: return "Awaiting approval"
        case .queued: return "Queued"
        case .running: return "Running"
        case .success: return "Success"
        case .failed: return "Failed"
        case .rejected: return "Rejected"
        case .cancelled: return "Cancelled"
        case .skipped: return "Skipped"
        case .unknown: return "Unknown"
        }
    }

    public var isActive: Bool { self == .queued || self == .running || self == .awaitingApproval }
    public var isFinished: Bool { !isActive && self != .unknown }
}

public struct ActionsStep: Equatable, Hashable, Sendable {
    public var name: String
    public var status: String

    public init(name: String, status: String) {
        self.name = name
        self.status = status
    }

    public var isDone: Bool { ["success", "failed", "skipped", "cancelled"].contains(status) }
}

public struct ActionsRunSummary: Equatable, Hashable, Sendable, Identifiable {
    public var id: Int
    public var workflow: String
    public var workflowPath: String
    public var commit: String
    public var ref: String
    public var status: ActionsRunStatus
    public var createdAtMs: Int
    public var startedAtMs: Int
    public var finishedAtMs: Int
    public var logTail: String
    public var pullNumber: Int
    public var steps: [ActionsStep]

    public init(
        id: Int, workflow: String = "", workflowPath: String = "", commit: String = "", ref: String = "",
        status: ActionsRunStatus = .unknown, createdAtMs: Int = 0, startedAtMs: Int = 0, finishedAtMs: Int = 0,
        logTail: String = "", pullNumber: Int = 0, steps: [ActionsStep] = []
    ) {
        self.id = id
        self.workflow = workflow
        self.workflowPath = workflowPath
        self.commit = commit
        self.ref = ref
        self.status = status
        self.createdAtMs = createdAtMs
        self.startedAtMs = startedAtMs
        self.finishedAtMs = finishedAtMs
        self.logTail = logTail
        self.pullNumber = pullNumber
        self.steps = steps
    }

    public init?(json: JSONObject) {
        let id = ModelJSON.int(json["id"])
        guard id > 0 else { return nil }
        self.init(
            id: id,
            workflow: json.firstStr("workflow", "workflowName"),
            workflowPath: json.str("workflowPath"),
            commit: json.str("commit"),
            ref: json.str("ref"),
            status: ActionsRunStatus(raw: json.str("status")),
            createdAtMs: ModelJSON.int(ModelJSON.coalesce(json["createdAt"], json["createdAtMs"])),
            startedAtMs: ModelJSON.int(ModelJSON.coalesce(json["startedAt"], json["startedAtMs"])),
            finishedAtMs: ModelJSON.int(ModelJSON.coalesce(json["finishedAt"], json["finishedAtMs"])),
            logTail: json.str("logTail"),
            pullNumber: ModelJSON.int(json["pullNumber"]),
            steps: json.maps("steps").prefix(64).map { ActionsStep(name: $0.str("name"), status: $0.str("status").lowercased()) }
        )
    }

    public var displayName: String { workflow.isEmpty ? "Run #\(id)" : workflow }

    public var refLabel: String {
        if ref.hasPrefix("refs/heads/") { return String(ref.dropFirst("refs/heads/".count)) }
        if ref.hasPrefix("refs/tags/") { return String(ref.dropFirst("refs/tags/".count)) }
        return ref
    }

    public var shortCommit: String { String(commit.prefix(8)) }

    public var stepProgress: (done: Int, total: Int) { (steps.filter(\.isDone).count, steps.count) }

    public func durationLabel(nowMs: Int) -> String {
        guard startedAtMs > 0 else { return "" }
        let end = finishedAtMs > 0 ? finishedAtMs : (status == .running ? nowMs : 0)
        guard end >= startedAtMs else { return "" }
        let seconds = (end - startedAtMs + 500) / 1_000
        if seconds < 60 { return "\(seconds)s" }
        let minutes = seconds / 60
        if minutes < 60 { return "\(minutes)m \(seconds % 60)s" }
        return "\(minutes / 60)h \(minutes % 60)m"
    }
}

public struct ActionsWorkflowSummary: Equatable, Hashable, Sendable, Identifiable {
    public var path: String
    public var name: String
    public var events: [String]
    public var manual: Bool
    public var enabled: Bool
    public var runsOn: [String]
    public var valid: Bool

    public var id: String { path }

    public init(path: String, name: String = "", events: [String] = [], manual: Bool = false, enabled: Bool = true,
                runsOn: [String] = [], valid: Bool = true) {
        self.path = path
        self.name = name
        self.events = events
        self.manual = manual
        self.enabled = enabled
        self.runsOn = runsOn
        self.valid = valid
    }

    public init?(json: JSONObject) {
        let path = json.str("path")
        guard !path.isEmpty else { return nil }
        self.init(
            path: path, name: json.str("name"), events: json.strings("events"), manual: json.bool("manual"),
            enabled: json["enabled"] != .bool(false), runsOn: json.strings("runsOn"), valid: json["valid"] != .bool(false))
    }

    public var displayName: String { name.isEmpty ? path : name }
    public var canDispatch: Bool { manual && enabled && valid }

    public var eventsLabel: String {
        var labels: [String] = []
        for event in events {
            let label = ActionsWorkflowSummary.eventLabel(event)
            if !labels.contains(label) { labels.append(label) }
        }
        if manual && !labels.contains("Manual") { labels.append("Manual") }
        return labels.isEmpty ? "No triggers" : labels.joined(separator: " · ")
    }

    public static func eventLabel(_ event: String) -> String {
        switch event.trimmingCharacters(in: .whitespaces).lowercased() {
        case "push": return "Push"
        case "pull_request", "pull-request", "pr": return "Pull request"
        case "release": return "Release"
        case "manual", "workflow_dispatch": return "Manual"
        case "schedule": return "Schedule"
        case "": return "Unknown"
        case let other: return other.replacingOccurrences(of: "_", with: " ").capitalized
        }
    }
}

public struct ActionsControlResult: Equatable, Hashable, Sendable {
    public var requestId: String
    public var action: String
    public var applied: Bool
    public var reason: String
    public var runId: Int
    public var finishedAtMs: Int

    public init(requestId: String, action: String, applied: Bool, reason: String = "", runId: Int = 0, finishedAtMs: Int = 0) {
        self.requestId = requestId
        self.action = action
        self.applied = applied
        self.reason = reason
        self.runId = runId
        self.finishedAtMs = finishedAtMs
    }

    public init?(json: JSONObject) {
        let requestId = json.str("requestId")
        guard !requestId.isEmpty else { return nil }
        self.init(
            requestId: requestId, action: json.str("action"), applied: json.str("status") == "applied",
            reason: json.str("reason"), runId: ModelJSON.int(json["runId"]), finishedAtMs: ModelJSON.int(json["finishedAt"]))
    }
}

public struct ActionsSummary: Equatable, Hashable, Sendable {
    public var available: Bool
    public var node: String
    public var updatedAtMs: Int
    public var schemaVersion: Int
    public var controlAvailable: Bool
    public var actionsEnabled: Bool?
    public var workflows: [ActionsWorkflowSummary]
    public var runs: [ActionsRunSummary]
    public var controls: [ActionsControlResult]

    public init(
        available: Bool = true, node: String = "", updatedAtMs: Int = 0, schemaVersion: Int = 1,
        controlAvailable: Bool = false, actionsEnabled: Bool? = nil, workflows: [ActionsWorkflowSummary] = [],
        runs: [ActionsRunSummary] = [], controls: [ActionsControlResult] = []
    ) {
        self.available = available
        self.node = node
        self.updatedAtMs = updatedAtMs
        self.schemaVersion = schemaVersion
        self.controlAvailable = controlAvailable
        self.actionsEnabled = actionsEnabled
        self.workflows = workflows
        self.runs = runs
        self.controls = controls
    }

    public init(json: JSONObject) {
        let v2 = ModelJSON.int(json["schemaVersion"]) == 2
        let runs = json.maps("runs").prefix(50).compactMap(ActionsRunSummary.init(json:)).sorted {
            $0.createdAtMs != $1.createdAtMs ? $0.createdAtMs > $1.createdAtMs : $0.id > $1.id
        }
        self.init(
            available: json["ok"] == .bool(true),
            node: String(json.str("node").prefix(80)),
            updatedAtMs: max(0, ModelJSON.int(json["updatedAt"])),
            schemaVersion: v2 ? 2 : 1,
            controlAvailable: v2 && json["controlAvailable"] == .bool(true),
            actionsEnabled: v2 ? json["actionsEnabled"] == .bool(true) : nil,
            workflows: v2 ? json.maps("workflows").prefix(100).compactMap(ActionsWorkflowSummary.init(json:)) : [],
            runs: Array(runs),
            controls: json.maps("controls").prefix(100).compactMap(ActionsControlResult.init(json:))
        )
    }
}
