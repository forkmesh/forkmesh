import FMBase
import FMChat
import FMControl
import Foundation

public struct RemoteConfirmation: Equatable, Sendable, Identifiable {
    public var family: DesktopControlFamilyName
    public var token: String
    public var action: String
    public var subject: String
    public var summary: String
    public var typeToConfirm: String
    public var expiresInMs: Int
    public var params: JSONObject

    public var id: String { token }

    public init(
        family: DesktopControlFamilyName, token: String, action: String, subject: String = "",
        summary: String = "", typeToConfirm: String = "", expiresInMs: Int = 60000, params: JSONObject = [:]
    ) {
        self.family = family
        self.token = token
        self.action = action
        self.subject = subject
        self.summary = summary
        self.typeToConfirm = typeToConfirm
        self.expiresInMs = expiresInMs
        self.params = params
    }

    public init?(family: DesktopControlFamilyName, json: JSONObject, params: JSONObject) {
        let token = json.str("token")
        guard !token.isEmpty else { return nil }
        self.init(
            family: family, token: token, action: json.str("action"), subject: json.str("subject"),
            summary: json.str("summary"), typeToConfirm: json.str("typeToConfirm"),
            expiresInMs: json.int("expiresInMs", 60000), params: params)
    }

    public var needsTypedConfirmation: Bool { !typeToConfirm.isEmpty }

    public var needsDeviceAuthentication: Bool { needsTypedConfirmation }

    public func accepts(typed: String) -> Bool {
        typeToConfirm.isEmpty || typed.trimmingCharacters(in: .whitespacesAndNewlines) == typeToConfirm
    }

    public func runParams(typed: String) -> JSONObject {
        var out = params
        out["action"] = .string(action)
        out["token"] = .string(token)
        if needsTypedConfirmation {
            out["confirmText"] = .string(typed.trimmingCharacters(in: .whitespacesAndNewlines))
        }
        return out
    }
}

public struct RemoteActionOutcome: Equatable, Sendable, Identifiable {
    public var action: String
    public var subject: String
    public var ok: Bool
    public var message: String
    public var atMs: Int

    public var id: String { "\(action):\(subject):\(atMs)" }

    public init(action: String, subject: String, ok: Bool, message: String, atMs: Int) {
        self.action = action
        self.subject = subject
        self.ok = ok
        self.message = message
        self.atMs = atMs
    }

    public init(json: JSONObject, atMs: Int) {
        self.init(
            action: json.str("action"), subject: json.firstStr("subject", "root"), ok: json.bool("ok"),
            message: json.str("message"), atMs: atMs)
    }
}

public struct RemoteStarted: Equatable, Sendable {
    public var action: String
    public var subject: String
    public var message: String

    public init(json: JSONObject) {
        action = json.str("action")
        subject = json.str("subject")
        message = json.str("message")
    }
}

public enum RemoteErrorText {
    public static func describe(_ error: Error) -> String {
        if let control = error as? LocalControlException { return describe(code: control.code) }
        return ControlErrorText.describe(error)
    }

    public static func describe(code: String) -> String {
        switch code {
        case "unsupported", "unknown_method":
            return "This desktop's ForkMesh is too old for that. Update it."
        case "unknown_root": return "The desktop no longer has that repository or worktree."
        case "outside_root": return "That link points outside the repository, so it is not shown."
        case "not_allowed": return "Git's own files are not shown."
        case "sensitive_file": return "That file looks like a key or credential, so it stays on the desktop."
        case "not_found": return "The file is gone."
        case "not_a_file": return "That is a folder."
        case "not_a_directory": return "That is not a folder."
        case "bad_path": return "That path is not valid."
        case "root_unavailable": return "The repository folder is missing on the desktop."
        case "git_status_failed": return "Git could not read the working tree."
        case "git_unavailable": return "Git is not available on the desktop."
        case "host_exists": return "The desktop already has a host with that name."
        case "bad_host": return "That host name is not valid."
        case "bad_address": return "Enter an IP address or a host name."
        case "bad_user": return "Enter a valid SSH user name."
        case "bad_code": return "The link code has six digits."
        case "bad_mode": return "Pick what to deploy."
        case "bad_repo", "unknown_repo": return "The desktop does not have that repository."
        case "bad_params": return "That setting is out of range."
        case "not_admin": return "Only a platform admin can delete a node."
        case "protected_node": return "The main node cannot be deleted."
        case "cannot_sign": return "Sign in to the account on the desktop first."
        case "nothing_to_resume": return "There is no paused deployment."
        case "fleet_target_off": return "Turn on the fleet target first."
        case "no_mirror_targets": return "That repository has no mirror remotes or managed hosts."
        case "host_unreachable": return "The desktop could not reach that host."
        case "not_started": return "The desktop could not start it."
        case "not_running": return "That agent is no longer running."
        case "nothing_to_send": return "There are no stalls to send, or no checkout to run an agent in."
        case "unknown_stall": return "That stall record is gone."
        case "agent_not_started": return "The desktop could not start an agent."
        case "ps_failed", "ps_unavailable": return "The desktop could not list its processes."
        case "bad_session": return "That agent run is not valid."
        case "already_running": return "The desktop is already updating or restarting."
        case "unknown_action": return "The desktop does not know that action."
        default: return ControlErrorText.describe(code: code)
        }
    }
}

public enum RemoteFormat {
    public static func bytes(_ value: Double) -> String {
        if value < 0 { return "—" }
        let units = ["B", "KB", "MB", "GB", "TB"]
        var amount = value
        var index = 0
        while amount >= 1024, index < units.count - 1 {
            amount /= 1024
            index += 1
        }
        if index == 0 { return "\(Int(amount)) B" }
        return String(format: amount >= 100 ? "%.0f %@" : "%.1f %@", amount, units[index])
    }

    public static func money(_ value: Double) -> String {
        String(format: "$%.2f", value)
    }

    public static func duration(ms: Int) -> String {
        let seconds = max(0, ms / 1000)
        if seconds < 60 { return "\(seconds)s" }
        let minutes = seconds / 60
        if minutes < 60 { return "\(minutes)m \(seconds % 60)s" }
        let hours = minutes / 60
        if hours < 48 { return "\(hours)h \(minutes % 60)m" }
        return "\(hours / 24)d \(hours % 24)h"
    }

    public static func percent(_ value: Double) -> String {
        value < 0 ? "—" : String(format: "%.0f%%", value)
    }
}
