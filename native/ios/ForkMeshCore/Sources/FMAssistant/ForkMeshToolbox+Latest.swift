import FMApi
import FMBase
import FMLocalAI
import FMPlanning
import Foundation

extension ForkMeshToolbox {
    public nonisolated static let latestKinds = ["inbox", "commit", "chat", "agent", "task", "note", "issue", "pull", "event"]

    public nonisolated static func latestKind(_ raw: String) -> String? {
        let word = raw.lowercased().trimmingCharacters(in: .whitespacesAndNewlines)
        if latestKinds.contains(word) { return word }
        let aliases: [String: String] = [
            "ping": "inbox", "pings": "inbox", "notification": "inbox", "notifications": "inbox", "mention": "inbox",
            "mentions": "inbox", "commits": "commit", "message": "chat", "messages": "chat", "chats": "chat", "dm": "chat",
            "agents": "agent", "run": "agent", "tasks": "task", "todo": "task", "notes": "note", "issues": "issue", "bug": "issue",
            "pulls": "pull", "pr": "pull", "prs": "pull", "pull request": "pull", "pull requests": "pull", "events": "event",
            "meeting": "event", "meetings": "event", "calendar": "event",
        ]
        return aliases[word]
    }

    func readLatest(_ args: JSONObject) async throws -> String {
        guard let what = ForkMeshToolbox.latestKind(AssistantText.argument(args, "what") ?? "") else {
            return "read_latest needs what: one of " + ForkMeshToolbox.latestKinds.joined(separator: ", ") + "."
        }
        let count = min(max(args["count"]?.intValue ?? 1, 1), 5)
        switch what {
        case "inbox": return try await latestPings(count)
        case "commit": return try await latestCommits(args, count)
        case "chat": return latestChats(count)
        case "agent": return try await latestAgents(count)
        case "task": return try await latestTasks(count)
        case "note": return try await latestNotes(count)
        case "issue": return try await latestIssues(args, count)
        case "pull": return try await latestPulls(args, count)
        default: return try await nextEvents(count)
        }
    }

    static func spokenList(_ one: String, _ many: String, _ rows: [String]) -> String {
        rows.count == 1 ? "\(one): \(rows[0])" : "\(many):\n" + rows.map { "- \($0)" }.joined(separator: "\n")
    }

    private func latestPings(_ count: Int) async throws -> String {
        let page = try await sources.pings(max(count, 10))
        let pings = page.notifications.sorted { $0.ts > $1.ts }.prefix(count)
        guard !pings.isEmpty else { return "Your inbox is empty." }
        let rows = pings.map { ping -> String in
            var parts = [AssistantText.dateTime(Int64(ping.ts), timeZone)]
            if !ping.actor.isEmpty { parts.append("from \(ping.actor)") }
            var text = ping.title
            if !ping.body.isEmpty { text += ". " + OnDeviceChatTools.clip(ping.body, limit: 240) }
            if !ping.repo.isEmpty { text += " (\(ping.repo))" }
            return parts.joined(separator: " ") + ": " + text + (ping.readAt == 0 ? " · unread" : "")
        }
        let unread = page.unread > 0 ? " You have \(page.unread) unread." : ""
        return ForkMeshToolbox.spokenList("The newest in your inbox", "The \(rows.count) newest in your inbox", rows) + unread
    }

    private func latestChats(_ count: Int) -> String {
        let messages = sources.chatMessages().sorted { $0.timestamp > $1.timestamp }.prefix(count)
        guard !messages.isEmpty else { return "No chat messages have reached this phone yet." }
        let rows = messages.map { message in
            "\(AssistantText.dayTime(message.timestamp, timeZone)) in \(message.conversation), \(message.isSelf ? "you" : message.senderName) said: "
                + OnDeviceChatTools.clip(message.text.isEmpty ? "[file \(message.fileName)]" : message.text, limit: 240)
        }
        return ForkMeshToolbox.spokenList("The last chat message", "The last \(rows.count) chat messages", rows)
    }

    public nonisolated static func commitDate(_ row: JSONObject) -> Int64 {
        for key in ["date", "time", "timestamp", "createdAt", "committedDate", "authorDate", "ts"] {
            let value = row.int64(key)
            if value > 0 { return value > 100_000_000_000 ? value : value * 1000 }
            if let text = row[key]?.stringValue, let ms = DartCore.dateTimeTryParseMs(text.dartTrimmed) { return Int64(ms) }
        }
        return 0
    }

    public nonisolated static func commitAuthor(_ row: JSONObject) -> String {
        if let author = row["author"]?.objectValue { return author.firstStr("name", "email") }
        return row.firstStr("author", "authorName", "committer")
    }

    private func latestCommits(_ args: JSONObject, _ count: Int) async throws -> String {
        guard let target = try await repositoryTargets(args).first else {
            return "No repository matches \(AssistantText.argument(args, "repository") ?? "your account")."
        }
        let rows = try await sources.commits(target.owner, target.name)
        let dated = rows.enumerated().sorted { left, right in
            let a = ForkMeshToolbox.commitDate(left.element)
            let b = ForkMeshToolbox.commitDate(right.element)
            return a == b ? left.offset < right.offset : a > b
        }.map(\.element).prefix(count)
        let repo = "\(target.owner)/\(target.name)"
        guard !dated.isEmpty else { return "\(repo) has no commits yet." }
        let lines = dated.map { row -> String in
            let subject = row.firstStr("subject", "message", "title").components(separatedBy: .newlines).first ?? ""
            var parts: [String] = []
            let ms = ForkMeshToolbox.commitDate(row)
            if ms > 0 { parts.append(AssistantText.dateTime(ms, timeZone)) }
            let author = ForkMeshToolbox.commitAuthor(row)
            if !author.isEmpty { parts.append("by \(author)") }
            let sha = row.firstStr("sha", "hash", "id", "oid")
            let short = sha.isEmpty ? "" : " (\(sha.prefix(7)))"
            return (parts.isEmpty ? "" : parts.joined(separator: " ") + ": ") + OnDeviceChatTools.clip(subject, limit: 200) + short
        }
        return ForkMeshToolbox.spokenList("The last commit in \(repo)", "The last \(lines.count) commits in \(repo)", lines)
    }

    private func latestAgents(_ count: Int) async throws -> String {
        let agents = try await sources.agents().sorted { max($0.updatedAtMs, $0.createdAtMs) > max($1.updatedAtMs, $1.createdAtMs) }.prefix(count)
        guard !agents.isEmpty else { return "You have no agent runs yet." }
        let rows = agents.map { AssistantText.agentLine($0, timeZone) }
        return ForkMeshToolbox.spokenList("Your most recent agent", "Your \(rows.count) most recent agents", rows)
    }

    private func latestTasks(_ count: Int) async throws -> String {
        let board = try await allTasks()
        let stamp: (JSONObject) -> Int64 = { max($0.int64("createdAt"), $0.int64("updatedAt")) }
        let tasks = board.tasks.sorted { stamp($0) > stamp($1) }.prefix(count)
        guard !tasks.isEmpty else { return "The task board is empty." }
        let rows = tasks.map { AssistantText.taskLine($0, timeZone) }
        return ForkMeshToolbox.spokenList("The newest task", "The \(rows.count) newest tasks", rows)
    }

    private func latestNotes(_ count: Int) async throws -> String {
        let notes = try await sources.notes().sorted { $0.int64("updatedAt") > $1.int64("updatedAt") }.prefix(count)
        guard let first = notes.first else { return "You have no notes yet." }
        if notes.count == 1 {
            let note = try await sources.note(first.str("id"))
            let text = OnDeviceChatTools.clip(note.str("markdown"), limit: 400)
            return "Your last note, \"\(note.str("title", first.str("title", "Untitled")))\", updated \(AssistantText.date(first.int64("updatedAt"), timeZone))" + (text.isEmpty ? "." : ": \(text)")
        }
        let rows = notes.map { "\($0.str("title", "Untitled")), updated \(AssistantText.date($0.int64("updatedAt"), timeZone))" }
        return ForkMeshToolbox.spokenList("Your last note", "Your \(rows.count) last notes", rows)
    }

    private func latestIssues(_ args: JSONObject, _ count: Int) async throws -> String {
        guard let target = try await repositoryTargets(args).first else {
            return "No repository matches \(AssistantText.argument(args, "repository") ?? "your account")."
        }
        let repo = "\(target.owner)/\(target.name)"
        let issues = try await sources.issues(target.owner, target.name).sorted { $0.number > $1.number }.prefix(count)
        guard !issues.isEmpty else { return "\(repo) has no issues." }
        let rows = issues.map { AssistantText.issueLine($0, repo: repo) }
        return ForkMeshToolbox.spokenList("The newest issue in \(repo)", "The \(rows.count) newest issues in \(repo)", rows)
    }

    private func latestPulls(_ args: JSONObject, _ count: Int) async throws -> String {
        guard let target = try await repositoryTargets(args).first else {
            return "No repository matches \(AssistantText.argument(args, "repository") ?? "your account")."
        }
        let repo = "\(target.owner)/\(target.name)"
        let pulls = try await sources.pulls(target.owner, target.name).sorted { $0.number > $1.number }.prefix(count)
        guard !pulls.isEmpty else { return "\(repo) has no pull requests." }
        let rows = pulls.map { "#\($0.number) \($0.title) · \($0.status)\($0.authorName.isEmpty && $0.author.isEmpty ? "" : " by \($0.authorName.isEmpty ? $0.author : $0.authorName)")" }
        return ForkMeshToolbox.spokenList("The newest pull request in \(repo)", "The \(rows.count) newest pull requests in \(repo)", rows)
    }

    private func nextEvents(_ count: Int) async throws -> String {
        let nowMs = Int64(now().timeIntervalSince1970 * 1000)
        let events = try await sources.calendar(nowMs - 3_600_000, nowMs + 30 * 86_400_000)
            .filter { $0.endAt >= nowMs || $0.startAt >= nowMs }
            .sorted { $0.startAt < $1.startAt }
            .prefix(count)
        guard !events.isEmpty else { return "Nothing on your calendar for the next 30 days." }
        let rows = events.map { event in
            "\(event.title.isEmpty ? "Untitled event" : event.title), \(event.allDay ? AssistantText.date(event.startAt, timeZone) : AssistantText.dateTime(event.startAt, timeZone))"
        }
        return ForkMeshToolbox.spokenList("Your next event", "Your next \(rows.count) events", rows)
    }
}
