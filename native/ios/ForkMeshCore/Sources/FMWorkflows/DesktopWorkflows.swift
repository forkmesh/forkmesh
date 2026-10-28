import FMBase
import FMChat
import FMControl
import Foundation
import Observation

public struct DesktopWorkflowRow: Equatable, Sendable, Identifiable {
    public var repo: String
    public var workflowId: String
    public var name: String
    public var description: String
    public var icon: String
    public var enabled: Bool
    public var specialAgent: Bool
    public var steps: Int
    public var trigger: String
    public var running: Bool
    public var runningStep: String
    public var hasSchedule: Bool
    public var scheduleActive: Bool
    public var nextRunAt: Int64
    public var lastRunAt: Int64
    public var lastStatus: String
    public var lastSummary: String
    public var runs: Int
    public var failedRuns: Int
    public var error: String

    public var id: String { "\(repo)|\(workflowId)" }

    public init(json: JSONObject) {
        repo = json.str("repo")
        workflowId = json.str("id")
        name = json.str("name", json.str("id"))
        description = json.str("description")
        icon = json.str("icon")
        enabled = json["enabled"]?.boolValue ?? true
        specialAgent = json.bool("specialAgent")
        steps = json.int("steps")
        trigger = json.str("trigger")
        running = json.bool("running") || json.bool("agentWorking")
        runningStep = json.str("runningStep")
        hasSchedule = json.bool("hasSchedule")
        scheduleActive = json.bool("scheduleActive")
        nextRunAt = json["nextRunAt"]?.int64Value ?? 0
        lastRunAt = json["lastRunAt"]?.int64Value ?? 0
        lastStatus = json.str("lastStatus")
        lastSummary = json.str("lastSummary")
        runs = json.int("runs")
        failedRuns = json.int("failedRuns")
        error = json.str("error")
    }

    public var kindLabel: String { specialAgent ? "Special agent" : "Desktop workflow" }
}

public struct DesktopWorkflowStepRun: Equatable, Sendable, Identifiable {
    public var nodeId: String
    public var name: String
    public var type: String
    public var status: String
    public var startedAt: Int64
    public var finishedAt: Int64
    public var output: String
    public var error: String
    public var logs: [String]

    public var id: String { nodeId }

    public init(json: JSONObject) {
        nodeId = json.str("nodeId")
        name = json.str("name", json.str("nodeId"))
        type = json.str("type")
        status = json.str("status")
        startedAt = json["startedAtMs"]?.int64Value ?? 0
        finishedAt = json["finishedAtMs"]?.int64Value ?? 0
        output = json.str("output")
        error = json.str("error")
        logs = json.list("logs").compactMap(\.stringValue)
    }

    public var ms: Int64 { finishedAt >= startedAt && startedAt > 0 ? finishedAt - startedAt : 0 }
}

public struct DesktopWorkflowRun: Equatable, Sendable, Identifiable {
    public var id: String
    public var trigger: String
    public var status: String
    public var startedAt: Int64
    public var finishedAt: Int64
    public var caption: String
    public var output: String
    public var error: String
    public var steps: [DesktopWorkflowStepRun]

    public init(json: JSONObject) {
        id = json.str("id")
        trigger = json.str("trigger")
        status = json.str("status")
        startedAt = json["startedAtMs"]?.int64Value ?? 0
        finishedAt = json["finishedAtMs"]?.int64Value ?? 0
        caption = json.str("caption")
        output = json.str("output")
        error = json.str("error")
        steps = json.maps("steps").map(DesktopWorkflowStepRun.init(json:))
    }

    public var summary: String {
        if !error.isEmpty { return error }
        if !caption.isEmpty { return caption }
        return output.split(separator: "\n").first.map(String.init) ?? ""
    }
}

public struct DesktopWorkflowDetail: Equatable, Sendable {
    public struct Step: Equatable, Sendable, Identifiable {
        public var id: String
        public var type: String
        public var name: String
    }

    public var repo: String
    public var workflowId: String
    public var name: String
    public var description: String
    public var enabled: Bool
    public var error: String
    public var nodes: [Step]
    public var runs: [DesktopWorkflowRun]
    public var active: DesktopWorkflowRun?

    public init(json: JSONObject) {
        repo = json.str("repo")
        workflowId = json.str("id")
        name = json.str("name", json.str("id"))
        description = json.str("description")
        enabled = json["enabled"]?.boolValue ?? true
        error = json.str("error")
        nodes = json.maps("nodes").map { Step(id: $0.str("id"), type: $0.str("type"), name: $0.str("name", $0.str("id"))) }
        runs = json.maps("runs").map(DesktopWorkflowRun.init(json:))
        let activeJSON = json.obj("active")
        active = activeJSON.isEmpty ? nil : DesktopWorkflowRun(json: activeJSON)
    }

    public var latest: DesktopWorkflowRun? { active ?? runs.first }
}

public struct DesktopWorkflowConfirmation: Equatable, Sendable, Identifiable {
    public var token: String
    public var action: String
    public var repo: String
    public var workflowId: String
    public var summary: String
    public var assignment: String

    public var id: String { token }
}

public enum DesktopWorkflowsText {
    public static let sectionTitle = "On your desktop"
    public static let accountTitle = "On ForkMesh"
    public static let offline = "Connect to your desktop to see and run its workflows and special agents."

    public static func describe(_ error: Error) -> String {
        if let control = error as? LocalControlException {
            let known = describe(code: control.code)
            if let known { return ControlErrorText.withReason(known, control) }
            if !control.message.isEmpty { return control.message }
        }
        return ControlErrorText.describe(error)
    }

    public static func describe(code: String) -> String? {
        switch code {
        case "unsupported", "unknown_method": return "Update ForkMesh on the desktop to run its workflows from this phone."
        case "unknown_repo": return "The desktop no longer has that repository."
        case "unknown_workflow": return "That workflow is no longer on the desktop."
        case "not_started": return "The desktop did not start it."
        case "not_running": return "It is not running."
        case "bad_request": return "The desktop could not read that request."
        case "no_checkout": return "The desktop has no forkmesh/forkmesh checkout to run the cleanup from."
        case "no_python": return "python3 is not installed on the desktop."
        case "cleanup_failed": return "The cleanup step failed on the desktop."
        case "busy": return "Another disk cleanup step is still running on the desktop."
        default: return nil
        }
    }
}

@MainActor
@Observable
public final class DesktopWorkflowsStore {
    @ObservationIgnored public let family: DesktopControlFamily
    @ObservationIgnored private var tapId: UUID?
    @ObservationIgnored private var refreshing = false
    @ObservationIgnored private var again = false

    public private(set) var rows: [DesktopWorkflowRow] = []
    public private(set) var desktop = ""
    public private(set) var loaded = false
    public private(set) var loading = false
    public private(set) var error = ""
    public private(set) var unsupported = false
    public private(set) var revision = 0

    public init(channel: DesktopControlCalling) {
        family = DesktopControlFamily(.workflows, channel: channel)
        tapId = family.addEventTap { [weak self] suffix, _ in
            guard let self, suffix == "changed" else { return }
            self.revision += 1
            Task { await self.refresh() }
        }
    }

    public func refresh() async {
        if refreshing {
            again = true
            return
        }
        refreshing = true
        loading = true
        defer {
            refreshing = false
            loading = false
        }
        repeat {
            again = false
            do {
                let reply = try await family.call("list")
                rows = reply.maps("workflows").map(DesktopWorkflowRow.init(json:))
                desktop = reply.str("desktop")
                error = ""
                unsupported = false
            } catch {
                unsupported = DesktopControlFamily.isUnsupported(error)
                self.error = DesktopWorkflowsText.describe(error)
            }
            loaded = true
        } while again
    }

    public func row(_ id: String) -> DesktopWorkflowRow? { rows.first { $0.id == id } }

    public func detail(_ row: DesktopWorkflowRow) async throws -> DesktopWorkflowDetail {
        DesktopWorkflowDetail(json: try await family.call("get", ["repo": .string(row.repo), "id": .string(row.workflowId)]))
    }

    public func prepare(_ action: String, _ row: DesktopWorkflowRow, assignment: String = "") async throws
        -> DesktopWorkflowConfirmation
    {
        let reply = try await family.call(
            "prepare", ["action": .string(action), "repo": .string(row.repo), "id": .string(row.workflowId)])
        let token = reply.str("token")
        guard !token.isEmpty else { throw LocalControlException("bad_answer") }
        return DesktopWorkflowConfirmation(
            token: token, action: action, repo: row.repo, workflowId: row.workflowId,
            summary: reply.str("summary"), assignment: assignment)
    }

    @discardableResult
    public func perform(_ confirmation: DesktopWorkflowConfirmation) async throws -> String {
        var params: JSONObject = [
            "action": .string(confirmation.action), "repo": .string(confirmation.repo),
            "id": .string(confirmation.workflowId), "token": .string(confirmation.token),
        ]
        let assignment = confirmation.assignment.trimmingCharacters(in: .whitespacesAndNewlines)
        if !assignment.isEmpty { params["assignment"] = .string(assignment) }
        let reply = try await family.call("run", params, timeout: 45)
        await refresh()
        return reply.str("message")
    }

    public func stop() {
        if let tapId { family.removeEventTap(tapId) }
        tapId = nil
    }
}

public enum WorkflowDesktopSteps {
    public static let steps: [String: String] = [
        "desktop.diskUsage": "usage",
        "desktop.cleanAgents": "agents",
        "desktop.cleanTests": "tests",
        "desktop.cleanVersions": "versions",
        "desktop.cleanCaches": "caches",
        "desktop.diskReport": "report",
    ]
    public static let stepTimeout: TimeInterval = 20 * 60

    public static func step(_ type: String) -> String? { steps[type] }

    public static func hasDesktopSteps(_ draft: WorkflowDraft) -> Bool {
        draft.nodes.contains { steps[$0.type] != nil }
    }

    public static func bytes(_ value: Int64) -> String {
        var size = Double(max(0, value))
        for unit in ["B", "KB", "MB", "GB"] {
            if size < 1024 { return unit == "B" ? "\(Int64(size)) B" : String(format: "%.1f %@", size, unit) }
            size /= 1024
        }
        return String(format: "%.1f TB", size)
    }
}
