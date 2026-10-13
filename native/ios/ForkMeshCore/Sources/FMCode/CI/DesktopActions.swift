import FMApi
import FMBase
import FMChat
import FMControl
import Foundation

public struct DesktopActionsLog: Equatable, Sendable {
    public var text: String
    public var from: Int
    public var next: Int
    public var reset: Bool

    public init(text: String, from: Int, next: Int, reset: Bool) {
        self.text = text
        self.from = from
        self.next = next
        self.reset = reset
    }

    public init(json: JSONObject) {
        self.init(
            text: json.str("text"), from: max(0, ModelJSON.int(json["from"])), next: max(0, ModelJSON.int(json["next"])),
            reset: json.bool("reset"))
    }
}

@MainActor
public final class DesktopActions {
    public let desktopName: String
    private let family: DesktopControlFamily

    public static let timeout: TimeInterval = 8
    public static let maxLogChars = 64 * 1024

    public init(channel: DesktopControlCalling, desktopName: String) {
        family = DesktopControlFamily(.repos, channel: channel)
        let name = desktopName.trimmingCharacters(in: .whitespaces)
        self.desktopName = name.isEmpty ? "your desktop" : name
    }

    public func summary(_ repo: RepoRef) async throws -> ActionsSummary {
        let reply = try await family.call("actions", ["repo": .string(repo.slug)], timeout: DesktopActions.timeout)
        return DesktopActions.summary(reply, node: desktopName)
    }

    public func log(_ repo: RepoRef, runId: Int, since: Int?) async throws -> DesktopActionsLog {
        var params: JSONObject = ["repo": .string(repo.slug), "run": .int(Int64(runId))]
        if let since { params["since"] = .int(Int64(since)) }
        return DesktopActionsLog(json: try await family.call("actions.log", params, timeout: DesktopActions.timeout))
    }

    public func control(_ repo: RepoRef, _ kind: ActionsControlKind, runId: Int?, workflow: String?, branch: String?,
                        enabled: Bool? = nil) async throws -> String
    {
        guard let action = DesktopActions.action(for: kind, enabled: enabled) else {
            throw LocalControlException("unsupported_action")
        }
        var params: JSONObject = ["repo": .string(repo.slug), "action": .string(action)]
        if let runId { params["run"] = .int(Int64(runId)) }
        if let workflow { params["path"] = .string(workflow) }
        if let branch { params["branch"] = .string(DesktopActions.bareBranch(branch)) }
        let prepared = try await family.call("prepare", params, timeout: DesktopActions.timeout)
        let token = prepared.str("token")
        guard !token.isEmpty else { throw LocalControlException("not_started") }
        let done = try await family.call("run", ["token": .string(token)], timeout: DesktopActions.timeout)
        return done.str("message")
    }

    @discardableResult
    public func onChange(_ tap: @escaping @MainActor () -> Void) -> UUID {
        family.addEventTap { suffix, _ in if suffix == "changed" { tap() } }
    }

    public func removeTap(_ id: UUID) { family.removeEventTap(id) }

    public nonisolated static func action(for kind: ActionsControlKind, enabled: Bool? = nil) -> String? {
        switch kind {
        case .dispatch: return "workflow.run"
        case .rerun: return "run.rerun"
        case .cancel: return "run.stop"
        case .approve: return "run.approve"
        case .reject: return "run.reject"
        case .setWorkflowEnabled: return enabled.map { $0 ? "workflow.enable" : "workflow.disable" }
        case .setRepositoryEnabled: return nil
        }
    }

    public nonisolated static func bareBranch(_ branch: String) -> String {
        let trimmed = branch.trimmingCharacters(in: .whitespaces)
        return trimmed.hasPrefix("refs/heads/") ? String(trimmed.dropFirst("refs/heads/".count)) : trimmed
    }

    public nonisolated static func summary(_ reply: JSONObject, node: String) -> ActionsSummary {
        let workflows: [JSONValue] = reply.maps("workflows").map { item in
            let runsOn = item.str("runsOn").split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) }
                .filter { !$0.isEmpty }
            return .object([
                "path": .string(item.str("path")), "name": .string(item.str("name")),
                "events": .array(item.strings("triggers").map(JSONValue.string)),
                "manual": .bool(item.bool("manual")), "enabled": .bool(!item.bool("disabled")),
                "runsOn": .array(runsOn.map(JSONValue.string)), "valid": .bool(item["valid"] != .bool(false)),
            ])
        }
        let runs: [JSONValue] = reply.maps("runs").map { item in
            var run = item
            run["workflowPath"] = .string(item.str("path"))
            let branch = item.str("branch")
            run["ref"] = .string(branch.isEmpty || branch.hasPrefix("refs/") ? branch : "refs/heads/\(branch)")
            run["pullNumber"] = .int(Int64(ModelJSON.int(item["pull"])))
            return .object(run)
        }
        return ActionsSummary(json: [
            "ok": true, "schemaVersion": 2, "node": .string(node), "controlAvailable": true,
            "actionsEnabled": .bool(reply.bool("enabled")), "workflows": .array(workflows), "runs": .array(runs),
        ])
    }

    public nonisolated static func explain(_ error: Error) -> String {
        switch (error as? LocalControlException)?.code ?? "" {
        case "unknown_repo": return "Your desktop does not have this repository."
        case "unknown_run": return "The desktop no longer has that run."
        case "wrong_state": return "That run has already moved on."
        case "actions_disabled": return "Actions are turned off for this repository on the desktop."
        case "workflow_disabled": return "That workflow is turned off on the desktop."
        case "no_mirror": return "The desktop has no mirror of this repository to run from."
        case "unsupported", "unknown_method", "unsupported_action": return "Update the desktop to do that from here."
        case "not_owner": return "The desktop is signed in to another account."
        case "not_paired": return "Pair this phone with your desktop first."
        case "": return "The desktop did not answer."
        case let code: return code.replacingOccurrences(of: "_", with: " ").capitalized + "."
        }
    }
}
