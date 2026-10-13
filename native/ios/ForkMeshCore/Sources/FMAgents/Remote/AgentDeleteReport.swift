import FMBase
import Foundation

public struct AgentDeletePart: Equatable, Sendable {
    public var kind: String
    public var name: String
    public var status: String
    public var reason: String

    public init(kind: String, name: String = "", status: String, reason: String = "") {
        self.kind = kind
        self.name = name
        self.status = status
        self.reason = reason
    }

    public init?(json: JSONObject) {
        let kind = json.str("kind")
        let status = json.str("status")
        guard !kind.isEmpty, !status.isEmpty else { return nil }
        self.init(kind: kind, name: json.str("name"), status: status, reason: json.str("reason"))
    }

    public var label: String {
        switch kind {
        case "agent": return "the agent"
        case "branch": return name.isEmpty ? "its branch" : "branch \(name)"
        case "work": return "its worktree"
        case "issue": return "issue #\(name)"
        case "pull": return "pull request #\(name)"
        case "task": return "task \(name)"
        default: return name.isEmpty ? kind : "\(kind) \(name)"
        }
    }

    public var reasonText: String {
        switch reason {
        case "": return ""
        case "merged": return "it was merged"
        case "no_checkout": return "its repository is not on the desktop"
        case "owner_only": return "only an organization owner can delete it"
        case "forbidden": return "only an owner can delete branches"
        case "running": return "the run did not stop"
        case "relay": return "the phone cannot close it"
        default: return reason
        }
    }
}

public struct AgentDeleteReport: Equatable, Sendable {
    public static let attachedKinds = ["branch", "issue", "pull", "task"]

    public var parts: [AgentDeletePart]
    public var settled: Bool
    public var known: [String]

    public init(parts: [AgentDeletePart], settled: Bool = true, known: [String] = AgentDeleteReport.attachedKinds) {
        self.parts = parts
        self.settled = settled
        self.known = known
    }

    public init?(json: JSONObject) {
        let parts = json.maps("parts").compactMap(AgentDeletePart.init(json:))
        guard !parts.isEmpty else { return nil }
        self.init(parts: parts, settled: json["settled"]?.bool(or: true) ?? true)
    }

    public var agentKept: Bool { parts.contains { $0.kind == "agent" && $0.status == "kept" } }

    public func summary(place: String = "") -> String {
        let at = place.isEmpty ? "" : " \(place)"
        let others = parts.filter { $0.kind != "agent" }
        let agent = parts.first { $0.kind == "agent" }
        var lines: [String] = []
        let deleted = others.filter { $0.status == "deleted" }.map(\.label)
        if agent == nil || agent?.status == "deleted" {
            lines.append(deleted.isEmpty ? "Deleted the agent\(at)." : "Deleted the agent\(at) with \(Self.join(deleted)).")
        } else if !deleted.isEmpty {
            lines.append("Deleted \(Self.join(deleted))\(at).")
        }
        let closed = others.filter { $0.status == "closed" }.map(\.label)
        if !closed.isEmpty { lines.append("Closed \(Self.join(closed)).") }
        let gone = others.filter { $0.status == "absent" }.map(\.label)
        if !gone.isEmpty { lines.append("Already gone: \(Self.join(gone)).") }
        for part in parts where part.status == "kept" {
            let why = part.reasonText
            lines.append(why.isEmpty ? "Kept \(part.label)." : "Kept \(part.label): \(why).")
        }
        let pending = parts.filter { $0.status == "pending" }.map(\.label)
        if !pending.isEmpty { lines.append("Still deleting \(Self.join(pending)).") }
        let missing = known.filter { kind in !parts.contains { $0.kind == kind } }.map(Self.noun)
        if !missing.isEmpty { lines.append("No \(Self.join(missing, last: "or")) was attached.") }
        return lines.joined(separator: " ")
    }

    static func noun(_ kind: String) -> String {
        switch kind {
        case "pull": return "pull request"
        default: return kind
        }
    }

    static func join(_ items: [String], last: String = "and") -> String {
        guard let tail = items.last else { return "" }
        if items.count == 1 { return tail }
        return items.dropLast().joined(separator: ", ") + " \(last) " + tail
    }
}
