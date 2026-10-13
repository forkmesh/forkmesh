import FMApi
import FMBase
import FMTools
import Foundation
import Observation

public enum TaskListFilter: String, CaseIterable, Sendable {
    case mine, open, done

    public init(route: String?) {
        self = TaskListFilter(rawValue: (route ?? "").lowercased()) ?? .mine
    }

    public var index: Int { TaskListFilter.allCases.firstIndex(of: self) ?? 0 }
}

public struct TaskHistoryEntry: Equatable, Hashable, Sendable, Identifiable {
    public var id: String
    public var kind: String
    public var actor: String
    public var summary: String
    public var createdAtMs: Int64

    public init(json: JSONObject) {
        id = json.str("id")
        kind = json.str("kind")
        actor = json.str("actor")
        summary = json.str("summary")
        createdAtMs = json.int64("createdAt")
    }
}

public struct TaskAttachmentDraft: Equatable, Sendable {
    public static let maxCount = 4
    public static let maxBytes = 256 * 1024
    public static let maxTotalBytes = 1024 * 1024
    public static let mimes = ["image/png", "image/jpeg", "image/gif", "image/webp"]

    public var name: String
    public var mime: String
    public var data: Data

    public init(name: String, mime: String, data: Data) {
        self.name = name
        self.mime = mime
        self.data = data
    }

    public static func validate(_ drafts: [TaskAttachmentDraft]) -> String? {
        if drafts.count > maxCount { return "Attach at most \(maxCount) images." }
        if drafts.contains(where: { !mimes.contains($0.mime) }) { return "Attach PNG, JPEG, GIF or WebP images." }
        if drafts.contains(where: { $0.data.count > maxBytes }) { return "Each image must be under 256 KB." }
        if drafts.reduce(0, { $0 + $1.data.count }) > maxTotalBytes { return "Images must add up to under 1 MB." }
        return nil
    }

    public var json: JSONValue {
        .object([
            "name": .string(String(name.prefix(120))),
            "mime": .string(mime),
            "file": .string(data.base64EncodedString()),
        ])
    }
}

public struct TaskAgentInfo: Equatable, Sendable {
    public var provider: String
    public var status: String
    public var sessionId: String

    public init?(task: JSONObject) {
        let agent = task.obj("agent")
        let session = task.str("agentSessionId").isEmpty ? agent.str("sessionId") : task.str("agentSessionId")
        let status = agent.str("status")
        guard !session.isEmpty || !status.isEmpty else { return nil }
        provider = agent.str("provider")
        self.status = status
        sessionId = session
    }

    public var label: String {
        let state: String
        switch status {
        case "queued": state = "Queued"
        case "running": state = "Running"
        case "waiting": state = "Waiting for you"
        case "success": state = "Finished"
        case "failed": state = "Failed"
        case "stopped": state = "Stopped"
        default: state = status.isEmpty ? "Linked" : status.capitalized
        }
        return provider.isEmpty ? "Agent: \(state)" : "\(provider.capitalized): \(state)"
    }
}

public enum PlanningConversion {
    public static let taskDetailsMax = 4_000

    public static func title(noteTitle: String, markdown: String) -> String {
        let trimmed = noteTitle.trimmingCharacters(in: .whitespacesAndNewlines)
        if !trimmed.isEmpty { return String(trimmed.prefix(160)) }
        let line = markdown.split(separator: "\n").map { $0.trimmingCharacters(in: .whitespaces) }.first { !$0.isEmpty } ?? ""
        let cleaned = line.drop { $0 == "#" }.trimmingCharacters(in: .whitespaces)
        return String((cleaned.isEmpty ? "Untitled note" : cleaned).prefix(160))
    }

    public static func details(noteTitle: String, markdown: String, publicURL: String?, limit: Int = taskDetailsMax) -> String {
        var footer = "\n\n---\nConverted from the note \"\(title(noteTitle: noteTitle, markdown: markdown))\""
        if let url = publicURL, !url.isEmpty { footer += " (published at \(url))" }
        let room = max(0, limit - footer.count)
        let body = markdown.count > room ? String(markdown.prefix(max(0, room - 1))) + "…" : markdown
        return body + footer
    }

    public static func agentPrompt(task: JSONObject) -> String {
        let id = task.str("id")
        var text = "[task:\(id)] \(task.str("title"))"
        let details = task.str("details").trimmingCharacters(in: .whitespacesAndNewlines)
        if !details.isEmpty { text += "\n\n\(details)" }
        return text
    }

    public static func validRepository(_ value: String) -> RepoRef? {
        RepoRef(slug: value.trimmingCharacters(in: .whitespaces))
    }
}

public typealias IssueFiler = @MainActor (_ owner: String, _ name: String, _ title: String, _ body: String) async throws -> Void

@MainActor
@Observable
public final class TaskOperations {
    public private(set) var busy = false
    public private(set) var history: [TaskHistoryEntry] = []

    @ObservationIgnored private let call: ToolsApiCall

    public init(call: @escaping ToolsApiCall) {
        self.call = call
    }

    public convenience init(api: ApiService) {
        self.init(call: { method, suffix, body in try await api.tasksApi(method, suffix, body) })
    }

    private func run<T>(_ fallback: String, _ work: () async throws -> T) async -> Result<T, TaskOperationError> {
        if busy { return .failure(TaskOperationError(message: "")) }
        busy = true
        defer { busy = false }
        do {
            return .success(try await work())
        } catch {
            return .failure(TaskOperationError(message: PlanningErrors.text(error, fallback)))
        }
    }

    public func loadHistory(_ taskId: String) async -> String? {
        switch await run("The history could not be loaded.", { try await call("GET", "/\(taskId)/history", nil) }) {
        case .success(let data):
            history = data.maps("history").prefix(200).map(TaskHistoryEntry.init(json:))
            return nil
        case .failure(let error): return error.message
        }
    }

    public func quickAdd(title: String, attachments: [TaskAttachmentDraft] = []) async -> Result<JSONObject, TaskOperationError> {
        let trimmed = title.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return .failure(TaskOperationError(message: "Give the task a title.")) }
        if let problem = TaskAttachmentDraft.validate(attachments) { return .failure(TaskOperationError(message: problem)) }
        var body: JSONObject = [
            "title": .string(String(trimmed.prefix(160))), "department": "general", "destination": "department",
            "assigneeKind": "unassigned",
        ]
        if !attachments.isEmpty { body["attachments"] = .array(attachments.map(\.json)) }
        return await run("The task was not created.") { try await call("POST", "", body).obj("task") }
    }

    public func followUp(parentId: String, title: String) async -> Result<JSONObject, TaskOperationError> {
        let trimmed = title.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return .failure(TaskOperationError(message: "Give the follow-up a title.")) }
        return await run("The follow-up was not created.") {
            try await call("POST", "", ["title": .string(String(trimmed.prefix(160))), "parentTaskId": .string(parentId)]).obj("task")
        }
    }

    public func assignToAgent(_ taskId: String, repository: String) async -> Result<JSONObject, TaskOperationError> {
        guard let repo = PlanningConversion.validRepository(repository) else {
            return .failure(TaskOperationError(message: "Choose a repository as owner/name."))
        }
        return await run("The task was not assigned.") {
            try await call("PATCH", "/\(taskId)", ["assigneeKind": "agent", "repository": .string(repo.slug)]).obj("task")
        }
    }

    public func setPriority(_ taskId: String, _ priority: Int) async -> Result<JSONObject, TaskOperationError> {
        guard (1...99).contains(priority) else { return .failure(TaskOperationError(message: "Priority runs from 1 to 99.")) }
        return await run("The priority was not changed.") {
            try await call("PATCH", "/\(taskId)", ["priority": .int(Int64(priority))]).obj("task")
        }
    }

    public func linkAgentSession(_ taskId: String, sessionId: String) async -> Result<JSONObject, TaskOperationError> {
        let clean = sessionId.trimmingCharacters(in: .whitespaces)
        guard !clean.isEmpty, clean.count <= 128 else { return .failure(TaskOperationError(message: "That agent run cannot be linked.")) }
        return await run("The agent run was not linked.") {
            try await call("PATCH", "/\(taskId)", ["agentSessionId": .string(clean)]).obj("task")
        }
    }

    public func moveToIssue(_ task: JSONObject, repository: String, file: IssueFiler) async -> String? {
        guard let repo = PlanningConversion.validRepository(repository) else { return "Choose a repository as owner/name." }
        let id = task.str("id")
        let title = task.str("title")
        var body = task.str("details")
        let note = task.str("completionNote")
        if !note.isEmpty { body += "\n\n\(note)" }
        body += "\n\n---\nMoved from organization task \(id)."
        let outcome = await run("The issue was not filed.") { () async throws -> Bool in
            try await file(repo.owner, repo.name, title, body)
            _ = try await call("DELETE", "/\(id)", [:])
            return true
        }
        if case .failure(let error) = outcome { return error.message }
        return nil
    }

    public func deleteClosed(_ tasks: [JSONObject]) async -> (deleted: Int, failed: Int) {
        if busy { return (0, 0) }
        busy = true
        defer { busy = false }
        var deleted = 0
        var failed = 0
        for task in tasks.prefix(200) where task.str("status") == "done" {
            do {
                _ = try await call("DELETE", "/\(task.str("id"))", [:])
                deleted += 1
            } catch {
                failed += 1
            }
        }
        return (deleted, failed)
    }
}

public struct TaskOperationError: Error, Equatable, Sendable {
    public var message: String
}

@MainActor
public enum NoteConversions {
    public static func toTask(noteTitle: String, markdown: String, publicURL: String?, call: ToolsApiCall) async -> Result<JSONObject, TaskOperationError> {
        let body: JSONObject = [
            "title": .string(PlanningConversion.title(noteTitle: noteTitle, markdown: markdown)),
            "details": .string(PlanningConversion.details(noteTitle: noteTitle, markdown: markdown, publicURL: publicURL)),
            "department": "general", "destination": "department", "assigneeKind": "unassigned",
        ]
        do {
            return .success(try await call("POST", "", body).obj("task"))
        } catch {
            return .failure(TaskOperationError(message: PlanningErrors.text(error, "The task was not created.")))
        }
    }

    public static func toIssue(noteTitle: String, markdown: String, publicURL: String?, repository: String, file: IssueFiler) async -> String? {
        guard let repo = PlanningConversion.validRepository(repository) else { return "Choose a repository as owner/name." }
        do {
            try await file(
                repo.owner, repo.name, PlanningConversion.title(noteTitle: noteTitle, markdown: markdown),
                PlanningConversion.details(noteTitle: noteTitle, markdown: markdown, publicURL: publicURL, limit: 60_000))
            return nil
        } catch {
            return "The issue was not filed."
        }
    }
}
