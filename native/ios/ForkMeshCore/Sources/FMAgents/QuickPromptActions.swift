import FMApi
import FMBase
import Foundation

public enum QuickPromptAction: String, CaseIterable, Sendable {
    case bugReport = "bug-report"
    case task
    case issue = "manual"
    case activityPub = "activitypub"
    case note

    public static let provider = "prompt-action"
    public static let storeKey = "agent/quick/action"
    public static let assigneeKey = "tasks/composerAssignee"

    public var choice: AgentModelChoice {
        AgentModelChoice(provider: Self.provider, alias: rawValue, id: "", label: label, note: note)
    }

    public var label: String {
        switch self {
        case .bugReport: return "Bug report"
        case .task: return "Task · assign"
        case .issue: return "Manual · create issue"
        case .activityPub: return "ActivityPub · post"
        case .note: return "Take a note"
        }
    }

    public var note: String {
        switch self {
        case .bugReport: return "Send this to the ForkMesh team instead of starting an agent"
        case .task: return "Create an organization task for the member you pick"
        case .issue: return "File an issue from this prompt instead of starting an agent"
        case .activityPub: return "Publish this prompt to the repository's ActivityPub followers"
        case .note: return "Save this prompt as a note instead of starting an agent"
        }
    }

    public var takesPhotos: Bool { self == .bugReport }

    public var needsRepository: Bool { self == .issue || self == .activityPub }

    public var needsAccount: Bool { self != .bugReport }

    public init?(choice: AgentModelChoice) {
        guard choice.provider == Self.provider else { return nil }
        self.init(rawValue: choice.alias)
    }

    public static func titleAndBody(_ prompt: String, limit: Int = 160) -> (title: String, body: String) {
        let text = prompt.dartTrimmed
        let lines = text.split(separator: "\n", maxSplits: 1, omittingEmptySubsequences: false)
        var title = String(lines.first ?? "").split(whereSeparator: \.isWhitespace).joined(separator: " ")
        var body = lines.count > 1 ? String(lines[1]).dartTrimmed : ""
        if title.isEmpty {
            title = "Update"
            body = text
        }
        if title.count > limit {
            title = String(title.prefix(limit - 1)).dartTrimmed + "…"
        }
        return (title, body)
    }
}

public struct QuickActionError: Error, Equatable, Sendable, CustomStringConvertible {
    public let message: String
    public init(_ message: String) { self.message = message }
    public var description: String { message }
}

public struct QuickTaskPeople: Equatable, Sendable {
    public let actor: String
    public let members: [String]

    public init(actor: String, members: [String]) {
        self.actor = actor
        self.members = members
    }

    public var names: [String] {
        var out: [String] = []
        for name in [actor] + members where !name.isEmpty && !out.contains(name) { out.append(name) }
        return out
    }
}

@MainActor
public protocol QuickPromptActions: AnyObject {
    func reportBug(_ text: String, photos: [PromptAttachment]) async throws
    func taskPeople() async throws -> QuickTaskPeople
    func createTask(title: String, details: String, assignee: String, repository: String) async throws
    func createIssue(owner: String, name: String, title: String, body: String) async throws
    func postToActivityPub(owner: String, name: String, title: String, body: String) async throws
    func takeNote(title: String, markdown: String) async throws
}
