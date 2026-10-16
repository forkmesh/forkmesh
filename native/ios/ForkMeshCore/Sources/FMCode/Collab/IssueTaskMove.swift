import FMApi
import FMBase
import Foundation

public enum IssueTaskMoveError: Error, Equatable {
    case taskNotCreated(String)
    case issueKept(String, taskTrashed: Bool)

    public var message: String {
        switch self {
        case .taskNotCreated(let why):
            return "The task could not be created, so the issue was kept. \(why)"
        case .issueKept(let why, let trashed):
            return trashed
                ? "\(why) The new task was moved to trash, so the issue is unchanged."
                : "\(why) The new task could not be removed; delete it from Tasks."
        }
    }
}

public enum IssueTaskMove {
    public typealias TasksCall = @MainActor (String, String, JSONObject?) async throws -> JSONObject

    public static let maxTitle = 160
    public static let maxDetails = 4000

    public static func taskBody(owner: String, name: String, issue: Issue, mover: String) -> JSONObject {
        let segment = try? NSRegularExpression(pattern: "^[A-Za-z0-9._-]{1,100}$")
        let fits = { (value: String) -> Bool in
            segment?.firstMatch(in: value, range: NSRange(value.startIndex..., in: value)) != nil
        }
        let repository = fits(owner) && fits(name) ? "\(owner)/\(name)" : ""
        let who = mover.dartTrimmed.isEmpty ? "a repository manager" : mover.dartTrimmed
        let source = "Moved from issue \(repository.isEmpty ? name : repository)#\(issue.number) by \(who)."
        var title = issue.title.dartTrimmed
        if title.isEmpty { title = "Issue #\(issue.number)" }
        if title.count > maxTitle { title = String(title.prefix(maxTitle - 1)) + "…" }
        var text = issue.body.dartTrimmed
        let room = maxDetails - source.count - 2
        if text.count > room { text = String(text.prefix(room - 1)) + "…" }
        return [
            "title": .string(title),
            "details": .string(text.isEmpty ? source : "\(text)\n\n\(source)"),
            "repository": .string(repository),
            "department": "general",
            "destination": "department",
            "assigneeKind": "unassigned",
        ]
    }

    @MainActor @discardableResult
    public static func move(
        _ issue: Issue, owner: String, name: String, mover: String,
        writer: CollabRecordWriter, tasks: TasksCall
    ) async throws -> JSONObject {
        let task: JSONObject
        do {
            task = try await tasks("POST", "", taskBody(owner: owner, name: name, issue: issue, mover: mover)).obj("task")
        } catch {
            throw IssueTaskMoveError.taskNotCreated(dartErrorText(error))
        }
        let id = task.str("id")
        do {
            try await writer.sendIssue(issue.number, .deleteOwnIssue())
        } catch {
            let why = dartErrorText(error)
            var trashed = false
            if !id.isEmpty {
                trashed = (try? await tasks(
                    "DELETE", "/\(id)", ["reason": .string("Issue #\(issue.number) could not be deleted.")])) != nil
            }
            throw IssueTaskMoveError.issueKept(why, taskTrashed: trashed)
        }
        return task
    }
}
