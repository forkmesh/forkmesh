import FMBase
import FMChat
import FMControl
import Foundation

public enum MergeQueueEntryState: String, Sendable, CaseIterable {
    case queued, updating, merging, blocked, failed

    public init(wire: String) { self = MergeQueueEntryState(rawValue: wire) ?? .queued }

    public var title: String {
        switch self {
        case .queued: return "Queued"
        case .updating: return "Updating"
        case .merging: return "Merging"
        case .blocked: return "Blocked"
        case .failed: return "Failed"
        }
    }

    public var isRunning: Bool { self == .updating || self == .merging }
    public var needsAttention: Bool { self == .blocked || self == .failed }
}

public struct MergeQueueEntry: Equatable, Sendable, Identifiable {
    public var id: Int { number }
    public let number: Int
    public let title: String
    public let state: MergeQueueEntryState
    public let detail: String
    public let queuedAtMs: Int64
    public let stateAtMs: Int64

    public init(number: Int, title: String = "", state: MergeQueueEntryState = .queued, detail: String = "",
                queuedAtMs: Int64 = 0, stateAtMs: Int64 = 0) {
        self.number = number
        self.title = title
        self.state = state
        self.detail = detail
        self.queuedAtMs = queuedAtMs
        self.stateAtMs = stateAtMs
    }

    public init?(json j: JSONObject) {
        let number = j.int("number")
        guard number > 0 else { return nil }
        self.init(
            number: number, title: String(j.str("title").prefix(200)), state: MergeQueueEntryState(wire: j.str("state")),
            detail: String(j.str("detail").prefix(200)), queuedAtMs: j.int64("queuedAtMs"), stateAtMs: j.int64("stateAtMs"))
    }
}

public struct MergeQueueRepo: Equatable, Sendable, Identifiable {
    public var id: String { "\(owner)/\(name)" }
    public let owner: String
    public let name: String
    public let enabled: Bool
    public let paused: Bool
    public let isOpen: Bool
    public let writable: Bool
    public let entries: [MergeQueueEntry]

    public init(owner: String, name: String, enabled: Bool, paused: Bool = false, isOpen: Bool = false,
                writable: Bool = true, entries: [MergeQueueEntry] = []) {
        self.owner = owner
        self.name = name
        self.enabled = enabled
        self.paused = paused
        self.isOpen = isOpen
        self.writable = writable
        self.entries = entries
    }

    public init?(json j: JSONObject) {
        let owner = j.str("owner")
        let name = j.str("name")
        guard !owner.isEmpty, !name.isEmpty else { return nil }
        self.init(
            owner: owner, name: name, enabled: j.bool("enabled"), paused: j.bool("paused"), isOpen: j.bool("open"),
            writable: j.optBool("writable") ?? true,
            entries: j.maps("entries").prefix(MergeQueueLimits.maxEntries).compactMap(MergeQueueEntry.init(json:)))
    }

    public var summary: String {
        if !enabled { return "Off" }
        if entries.isEmpty { return paused ? "Empty · paused" : "Empty" }
        var text = "\(entries.count) queued"
        let running = entries.filter { $0.state.isRunning }.count
        let blocked = entries.filter { $0.state.needsAttention }.count
        if running > 0 { text += " · \(running) in progress" }
        if blocked > 0 { text += " · \(blocked) blocked" }
        if paused { text += " · paused" }
        return text
    }

    public func place(of number: Int) -> Int? { entries.firstIndex { $0.number == number }.map { $0 + 1 } }
}

public enum MergeQueueAction: String, Sendable, CaseIterable {
    case enqueue, remove, move, pause, resume, enable, disable, clear, process

    public var needsNumber: Bool { self == .enqueue || self == .remove || self == .move }

    public var isDestructive: Bool { self == .clear || self == .disable }

    public var title: String {
        switch self {
        case .enqueue: return "Add to merge queue"
        case .remove: return "Remove from queue"
        case .move: return "Move"
        case .pause: return "Pause queue"
        case .resume: return "Resume queue"
        case .enable: return "Turn on merge queue"
        case .disable: return "Turn off merge queue"
        case .clear: return "Clear queue"
        case .process: return "Merge next now"
        }
    }
}

public struct MergeQueueRequest: Equatable, Sendable {
    public let owner: String
    public let name: String
    public let action: MergeQueueAction
    public let number: Int
    public let delta: Int

    public init(owner: String, name: String, action: MergeQueueAction, number: Int = 0, delta: Int = 0) {
        self.owner = owner
        self.name = name
        self.action = action
        self.number = number
        self.delta = delta
    }

    public var params: JSONObject {
        var out: JSONObject = ["owner": .string(owner), "name": .string(name), "action": .string(action.rawValue)]
        if action.needsNumber { out["number"] = .int(Int64(number)) }
        if action == .move { out["delta"] = .int(Int64(delta)) }
        return out
    }
}

public struct MergeQueueConfirmation: Equatable, Sendable {
    public let token: String
    public let summary: String
    public let expiresInMs: Int

    public init(json j: JSONObject) {
        token = j.str("token")
        summary = j.str("summary")
        expiresInMs = j.int("expiresInMs", 60_000)
    }
}

public struct MergeQueueResult: Equatable, Sendable {
    public let message: String
    public let repo: MergeQueueRepo?

    public init(json j: JSONObject) {
        message = j.str("message")
        repo = j["queue"]?.objectValue.flatMap(MergeQueueRepo.init(json:))
    }
}

public enum MergeQueueRules {
    public static func actions(for repo: MergeQueueRepo) -> [MergeQueueAction] {
        guard repo.writable else { return [] }
        if !repo.enabled { return [.enable] }
        var out: [MergeQueueAction] = [repo.paused ? .resume : .pause]
        if !repo.entries.isEmpty && !repo.paused { out.append(.process) }
        if !repo.entries.isEmpty { out.append(.clear) }
        out.append(.disable)
        return out
    }

    public static func entryActions(_ entry: MergeQueueEntry, in repo: MergeQueueRepo) -> [(MergeQueueAction, Int)] {
        guard repo.writable, repo.enabled, !entry.state.isRunning, let place = repo.place(of: entry.number) else { return [] }
        var out: [(MergeQueueAction, Int)] = []
        if place > 1 { out.append((.move, -1)) }
        if place < repo.entries.count { out.append((.move, 1)) }
        out.append((.remove, 0))
        return out
    }

    public static func canEnqueue(pullStatus: String, repo: MergeQueueRepo?, number: Int) -> Bool {
        guard let repo, repo.enabled, repo.writable else { return false }
        return pullStatus.dartTrimmed.lowercased() == "open" && repo.place(of: number) == nil
    }

    public static func errorMessage(_ code: String) -> String {
        switch code {
        case "unsupported", "unknown_method": return "This desktop does not know the merge queue yet. Update ForkMesh on the desktop."
        case "not_owner": return "Only the desktop's owner can change its merge queue."
        case "unknown_repo": return "The desktop does not have that repository."
        case "queue_off": return "The merge queue is off for this repository."
        case "read_only": return "This repository is a read-only mirror on the desktop; the owning node merges it."
        case "not_open_pull": return "Only open pull requests can join the queue."
        case "already_queued": return "That pull request is already in the queue."
        case "not_queued": return "That pull request is not in the queue."
        case "repo_not_open": return "Open this repository on the desktop to run its queue now; it keeps running while open."
        case "busy": return "The desktop is merging right now. Try again in a moment."
        case "paused": return "The queue is paused. Resume it first."
        case "queue_full": return "The queue is full."
        case "bad_request": return "The desktop could not read that request."
        case "missing", "expired", "mismatch", "confirmation_missing", "confirmation_expired", "confirmation_mismatch":
            return "The confirmation expired. Try again."
        default: return code.isEmpty ? "The desktop refused." : code.replacingOccurrences(of: "_", with: " ")
        }
    }
}

public enum MergeQueueLimits {
    public static let maxRepos = 100
    public static let maxEntries = 100
}

@MainActor
public final class MergeQueueRemote {

    let family: DesktopControlFamily

    public init(channel: DesktopControlCalling) {
        family = DesktopControlFamily(.mergeQueue, channel: channel)
    }

    public func list(owner: String = "", name: String = "") async throws -> [MergeQueueRepo] {
        var params: JSONObject = [:]
        if !owner.isEmpty && !name.isEmpty {
            params["owner"] = .string(owner)
            params["name"] = .string(name)
        }
        let reply = try await family.call("list", params)
        return reply.maps("repos").prefix(MergeQueueLimits.maxRepos).compactMap(MergeQueueRepo.init(json:))
    }

    public func prepare(_ request: MergeQueueRequest) async throws -> MergeQueueConfirmation {
        let confirmation = MergeQueueConfirmation(json: try await family.call("prepare", request.params))
        if confirmation.token.isEmpty { throw LocalControlException("confirmation_missing") }
        return confirmation
    }

    public func run(_ request: MergeQueueRequest, token: String) async throws -> MergeQueueResult {
        var params = request.params
        params["token"] = .string(token)
        return MergeQueueResult(json: try await family.call("run", params, timeout: 60))
    }

    public func perform(_ request: MergeQueueRequest) async throws -> MergeQueueResult {
        let confirmation = try await prepare(request)
        return try await run(request, token: confirmation.token)
    }

    @discardableResult
    public func onChange(_ handler: @escaping @MainActor (String, String) -> Void) -> UUID {
        family.addEventTap { suffix, data in
            guard suffix == "changed" else { return }
            handler(data.str("owner"), data.str("name"))
        }
    }

    public func removeTap(_ id: UUID) { family.removeEventTap(id) }

    public nonisolated static func message(for error: Error) -> String {
        if let error = error as? LocalControlException { return MergeQueueRules.errorMessage(error.code) }
        return "\(error)"
    }
}
