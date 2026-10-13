import FMApi
import FMBase
import FMControl
import FMPackages
import Foundation

public enum AssistantText {
    public static func argument(_ args: JSONObject, _ key: String) -> String? {
        guard let value = args[key], !value.isNull else { return nil }
        let text = value.text.trimmingCharacters(in: .whitespacesAndNewlines)
        return text.isEmpty ? nil : text
    }

    public static func pathSegment(_ id: String) -> String {
        id.addingPercentEncoding(withAllowedCharacters: .alphanumerics.union(CharacterSet(charactersIn: "-_."))) ?? id
    }

    public static func matches(_ query: String, _ fields: [String]) -> Bool {
        let words = query.lowercased().split(whereSeparator: \.isWhitespace)
        guard !words.isEmpty else { return true }
        let haystack = fields.joined(separator: " ").lowercased()
        return words.allSatisfy { haystack.contains($0) }
    }

    public static func list(_ heading: String, _ rows: [String], limit: Int = ForkMeshToolbox.maxRows, more: Bool = false) -> String {
        guard !rows.isEmpty else { return heading + "." }
        var lines = [heading]
        lines += rows.prefix(limit).map { "- " + $0 }
        if rows.count > limit {
            lines.append("… \(rows.count - limit) more not shown; narrow it with a query.")
        } else if more {
            lines.append("… the board has more tasks than were read.")
        }
        return lines.joined(separator: "\n")
    }

    public static func failures(_ failures: [String]) -> String {
        failures.isEmpty ? "" : "\nCould not read: " + failures.joined(separator: "; ")
    }

    public static func failure(_ error: Error) -> String {
        if error is PackagesError { return PackagesError.describe(error) }
        let text = "\(error)"
        if text.contains("org_member_required") { return "the user is not a member of an organization with a task board" }
        if text.contains("plugin_not_installed") { return "that package is not installed for this account" }
        return text
    }

    private static func formatter(_ format: String, _ timeZone: TimeZone) -> DateFormatter {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.calendar = Calendar(identifier: .gregorian)
        formatter.timeZone = timeZone
        formatter.dateFormat = format
        return formatter
    }

    private static func moment(_ ms: Int64) -> Date { Date(timeIntervalSince1970: Double(ms) / 1000) }

    public static func dayTime(_ date: Date, _ timeZone: TimeZone) -> String {
        formatter("EEE yyyy-MM-dd HH:mm", timeZone).string(from: date)
    }

    public static func date(_ ms: Int64, _ timeZone: TimeZone) -> String {
        ms > 0 ? formatter("yyyy-MM-dd", timeZone).string(from: moment(ms)) : "unknown"
    }

    public static func dateTime(_ ms: Int64, _ timeZone: TimeZone) -> String {
        ms > 0 ? formatter("EEE yyyy-MM-dd HH:mm", timeZone).string(from: moment(ms)) : "unknown"
    }

    public static func time(_ ms: Int64, _ timeZone: TimeZone) -> String {
        formatter("HH:mm", timeZone).string(from: moment(ms))
    }

    public static func isDone(_ task: JSONObject) -> Bool { task.str("status") == "done" }

    public static func isAssigned(_ task: JSONObject, to actor: String) -> Bool {
        task.str("assigneeKind", "user") == "user" && task.str("assignee").lowercased() == actor.lowercased() && !actor.isEmpty
    }

    public static func taskStatus(_ task: JSONObject) -> String {
        switch task.str("status") {
        case "done": return "done"
        case "active": return "in progress"
        default: return "open"
        }
    }

    public static func taskLine(_ task: JSONObject, _ timeZone: TimeZone) -> String {
        var parts = ["[\(task.str("id"))] \(task.str("title", "Untitled"))", taskStatus(task)]
        let assignee = task.str("assignee")
        if assignee.isEmpty {
            parts.append("unassigned")
        } else {
            parts.append("assigned to \(assignee)\(task.str("assigneeKind", "user") == "user" ? "" : " (agent)")")
        }
        if task.int("priority") > 0 { parts.append("priority \(task.int("priority"))") }
        if !task.str("department").isEmpty { parts.append(task.str("department")) }
        if !task.str("repository").isEmpty { parts.append(task.str("repository")) }
        parts.append("updated \(date(task.int64("updatedAt", task.int64("createdAt")), timeZone))")
        return parts.joined(separator: " · ")
    }

    public static func taskDetail(_ task: JSONObject, _ timeZone: TimeZone) -> String {
        var lines = ["Task " + taskLine(task, timeZone)]
        if !task.str("createdBy").isEmpty { lines.append("Created by \(task.str("createdBy")) on \(date(task.int64("createdAt"), timeZone))") }
        if task.int64("completedAt") > 0 { lines.append("Completed \(date(task.int64("completedAt"), timeZone))\(task.str("completionNote").isEmpty ? "" : ": \(task.str("completionNote"))")") }
        let checkin = task.obj("lastCheckin")
        if !checkin.isEmpty { lines.append("Last check-in: \(checkin.str("state")) \(checkin.str("note"))".trimmingCharacters(in: .whitespaces)) }
        if !task.str("details").isEmpty { lines.append("\nDetails:\n\(task.str("details"))") }
        let replies = task.maps("replies")
        if !replies.isEmpty {
            lines.append("\nReplies:")
            lines += replies.suffix(10).map { reply in
                "- \(reply.firstStr("author", "by", "actor")) (\(date(reply.int64("createdAt", reply.int64("at")), timeZone))): \(reply.firstStr("body", "text", "note"))"
            }
        }
        return lines.joined(separator: "\n")
    }

    public static func repoLine(_ repo: Repository) -> String {
        var parts = [repo.fullName]
        if repo.isPrivate { parts.append("private") }
        if !repo.language.isEmpty { parts.append(repo.language) }
        if repo.stars > 0 { parts.append("\(repo.stars) stars") }
        if !repo.description.isEmpty { parts.append(repo.description) }
        return parts.joined(separator: " · ")
    }

    public static func issueLine(_ issue: Issue, repo: String) -> String {
        var parts = ["\(repo)#\(issue.number) \(issue.title)", issue.isOpen ? "open" : "closed"]
        if !issue.assignees.isEmpty { parts.append("assigned to " + issue.assignees.joined(separator: ", ")) }
        if !issue.labels.isEmpty { parts.append(issue.labels.joined(separator: ", ")) }
        if issue.priority > 0 { parts.append("priority \(issue.priority)") }
        if !issue.milestone.isEmpty { parts.append("milestone \(issue.milestone)") }
        return parts.joined(separator: " · ")
    }

    public static func issueDetail(_ issue: Issue, repo: String) -> String {
        var lines = ["Issue " + issueLine(issue, repo: repo)]
        if !issue.author.isEmpty { lines.append("Opened by \(issue.author)") }
        if issue.progress > 0 { lines.append("Progress: \(issue.progress)%") }
        if !issue.body.isEmpty { lines.append("\n\(issue.body)") }
        return lines.joined(separator: "\n")
    }

    public static func isRunning(_ agent: AgentLaunchSession) -> Bool { agent.isActive }

    public static func agentLine(_ agent: AgentLaunchSession, _ timeZone: TimeZone) -> String {
        var parts = [agent.title.isEmpty ? "Agent run \(agent.sessionId)" : agent.title]
        parts.append(agent.displayStatus.isEmpty ? agent.status : agent.displayStatus)
        if !agent.repo.isEmpty { parts.append("\(agent.owner)/\(agent.repo)") }
        if !agent.provider.isEmpty { parts.append(agent.provider) }
        if !agent.targetNode.isEmpty { parts.append("on \(agent.targetNode)") }
        if agent.prNumber > 0 { parts.append("PR #\(agent.prNumber)\(agent.merged ? " merged" : "")") }
        if !agent.lastError.isEmpty { parts.append("error: \(agent.lastError)") }
        parts.append("updated \(dateTime(Int64(agent.updatedAtMs), timeZone))")
        return parts.joined(separator: " · ")
    }

    public static func agentRowLine(_ row: UnifiedAgentRow, _ timeZone: TimeZone) -> String {
        var parts = ["[\(row.id)] \(row.title.isEmpty ? "Untitled agent" : row.title)", row.statusLabel.isEmpty ? row.status : row.statusLabel]
        if !row.repoLabelText.isEmpty { parts.append(row.repoLabelText) }
        parts.append(row.isDesktop ? "desktop \(row.node)" : "cloud\(row.node.isEmpty ? "" : " on \(row.node)")")
        if !row.provider.isEmpty { parts.append(row.provider + (row.model.isEmpty ? "" : " \(row.model)")) }
        if !row.branchName.isEmpty { parts.append("branch \(row.branchName)") }
        if row.prNumber > 0 { parts.append("PR #\(row.prNumber)\(row.merged ? " merged" : "")") }
        if row.canPrompt { parts.append("can take messages") }
        if row.updatedAtMs > 0 { parts.append("updated \(dateTime(Int64(row.updatedAtMs), timeZone))") }
        return parts.joined(separator: " · ")
    }

    public static func crmContactLine(_ contact: CrmContact) -> String {
        var parts = ["[\(contact.id)] \(contact.displayName)"]
        if !contact.email.isEmpty { parts.append(contact.email) }
        if !contact.subtitle.isEmpty { parts.append(contact.subtitle) }
        parts.append("\(contact.flow)/\(contact.stage)")
        if contact.value > 0 { parts.append("value \(contact.value)") }
        if !contact.nextStep.isEmpty { parts.append("next: \(contact.nextStep)") }
        if !contact.tags.isEmpty { parts.append("tags: " + contact.tags.joined(separator: ", ")) }
        return parts.joined(separator: " · ")
    }

    public static func crmContactDetail(_ contact: CrmContact, activity: [CrmActivity], _ timeZone: TimeZone) -> String {
        var lines = ["Contact " + crmContactLine(contact)]
        let fields: [(String, String)] = [
            ("Phone", contact.phone), ("Website", contact.website), ("Location", contact.location),
            ("LinkedIn", contact.linkedin), ("GitHub", contact.github), ("Twitter", contact.twitter),
            ("Mastodon", contact.mastodon), ("Bluesky", contact.bluesky), ("Owner", contact.owner), ("Source", contact.source),
        ]
        lines += fields.filter { !$0.1.isEmpty }.map { "\($0.0): \($0.1)" }
        if contact.lastContactedAt > 0 { lines.append("Last contacted: \(date(contact.lastContactedAt, timeZone))") }
        lines.append("Created \(date(contact.createdAt, timeZone))\(contact.createdBy.isEmpty ? "" : " by \(contact.createdBy)"), updated \(date(contact.updatedAt, timeZone))")
        if !contact.notes.isEmpty { lines.append("\nNotes:\n\(contact.notes)") }
        let recent = activity.sorted { $0.at > $1.at }.prefix(10)
        if !recent.isEmpty {
            lines.append("\nEmail activity:")
            lines += recent.map { item in
                "- \(date(item.at, timeZone)) \(item.incoming ? "received" : "sent") \"\(item.subject)\" · \(item.status)\(item.failed && !item.error.isEmpty ? " (\(item.error))" : "")"
            }
        }
        return lines.joined(separator: "\n")
    }

    public static func pingLine(_ ping: ForkNotification, _ timeZone: TimeZone) -> String {
        var parts = [ping.title]
        if !ping.body.isEmpty { parts.append(ping.body) }
        if !ping.actor.isEmpty { parts.append("from \(ping.actor)") }
        if !ping.repo.isEmpty { parts.append(ping.repo) }
        parts.append(dateTime(Int64(ping.ts), timeZone))
        if ping.readAt == 0 { parts.append("unread") }
        return parts.joined(separator: " · ")
    }
}
