import FMApi
import FMBase
import FMLocalAI
import FMPlanning
import Foundation

@MainActor
public final class ForkMeshToolbox: OnDeviceChatToolbox {
    public nonisolated static let maxRows = 25
    public nonisolated static let maxTaskPages = 4
    public nonisolated static let maxRepoScan = 6
    public nonisolated static let briefingTtl: TimeInterval = 60

    public let sources: AssistantSources
    let now: () -> Date
    let timeZone: TimeZone
    var orgCache: [OrgSummary]?
    var crmOrgCache: [String]?
    var briefingCache: (at: Date, text: String)?

    public init(sources: AssistantSources, timeZone: TimeZone = .current, now: @escaping () -> Date = { Date() }) {
        self.sources = sources
        self.timeZone = timeZone
        self.now = now
    }

    public var tools: [LocalToolSpec] { ForkMeshToolbox.specs }

    public func invalidate() {
        briefingCache = nil
        orgCache = nil
        crmOrgCache = nil
    }

    public func briefing() async -> String {
        if let cache = briefingCache, now().timeIntervalSince(cache.at) < ForkMeshToolbox.briefingTtl {
            return cache.text
        }
        var lines = ["Now: \(AssistantText.dayTime(now(), timeZone)) (\(timeZone.identifier))."]
        guard let account = sources.account() else {
            lines.append("Not signed in to ForkMesh on this phone, so account data is unavailable.")
            return lines.joined(separator: "\n")
        }
        lines.append("Signed in as \(account.username)\(account.email.isEmpty ? "" : " (\(account.email))").")
        let orgs = await organizations()
        if !orgs.isEmpty {
            lines.append("Organizations: " + orgs.map { "\($0.name) (\($0.role))" }.joined(separator: ", ") + ".")
        }
        do {
            let board = try await sources.tasks(0)
            let actor = board.str("actor", account.username).lowercased()
            let open = board.maps("tasks").filter { !AssistantText.isDone($0) }
            let mine = open.filter { AssistantText.isAssigned($0, to: actor) }
            lines.append("Open tasks assigned to you: \(mine.count)\(mine.isEmpty ? "." : ":")")
            lines += mine.prefix(8).map { "- " + AssistantText.taskLine($0, timeZone) }
            if open.count > mine.count {
                lines.append("Other open tasks on the board: \(open.count - mine.count)\(board.bool("hasMore") ? "+" : "").")
            }
        } catch {
            lines.append("Tasks: \(AssistantText.failure(error)).")
        }
        if let page = try? await sources.pings(10) {
            let unread = page.notifications.filter { $0.readAt == 0 }
            lines.append("Pings: \(page.unread) unread\(unread.isEmpty ? "." : ":")")
            lines += unread.prefix(3).map { "- " + AssistantText.pingLine($0, timeZone) }
        }
        if let agents = try? await sources.agents() {
            let running = agents.filter { AssistantText.isRunning($0) }
            lines.append("Agents: \(running.count) running of \(agents.count) recent\(running.isEmpty ? "." : ":")")
            lines += running.prefix(4).map { "- " + AssistantText.agentLine($0, timeZone) }
        }
        let text = lines.joined(separator: "\n")
        briefingCache = (now(), text)
        return text
    }

    public func approval(for call: LocalToolCall) async -> String? {
        let args = call.arguments
        switch call.name {
        case "delete_crm_contact":
            return await deleteApproval(args)
        case "message_agent":
            let text = AssistantText.argument(args, "text") ?? ""
            return "Send to agent \(await agentName(args)): \"\(OnDeviceChatTools.clip(text, limit: 300))\""
        case "stop_agent":
            return "Stop agent \(await agentName(args))"
        default:
            return nil
        }
    }

    public func invoke(_ call: LocalToolCall) async -> OnDeviceToolResult {
        let args = call.arguments
        do {
            switch call.name {
            case "get_account": return OnDeviceToolResult(await account())
            case "list_tasks": return OnDeviceToolResult(try await listTasks(args))
            case "read_notes": return OnDeviceToolResult(try await readNotes(args))
            case "list_repositories": return OnDeviceToolResult(try await listRepositories(args))
            case "list_issues": return OnDeviceToolResult(try await listIssues(args))
            case "list_pull_requests": return OnDeviceToolResult(try await listPulls(args))
            case "read_chat": return OnDeviceToolResult(readChat(args))
            case "list_agents": return OnDeviceToolResult(try await listAgents(args))
            case "message_agent": return OnDeviceToolResult(try await messageAgent(args))
            case "stop_agent": return OnDeviceToolResult(try await stopAgent(args))
            case "list_pings": return OnDeviceToolResult(try await listPings(args))
            case "list_calendar": return OnDeviceToolResult(try await listCalendar(args))
            case "search_forkmesh": return OnDeviceToolResult(await search(args))
            case "read_latest": return OnDeviceToolResult(try await readLatest(args))
            case "crm_contacts": return OnDeviceToolResult(try await crmContacts(args))
            case "create_crm_contact": return OnDeviceToolResult(try await createCrmContact(args))
            case "update_crm_contact": return OnDeviceToolResult(try await updateCrmContact(args))
            case "delete_crm_contact": return OnDeviceToolResult(try await deleteCrmContact(args))
            case "draft_email": return try await draftEmail(args)
            case "search_web": return OnDeviceToolResult(try await searchWeb(args))
            case "read_web_page": return OnDeviceToolResult(try await readWebPage(args))
            default:
                return OnDeviceToolResult("There is no tool named \(call.name). Use one of: " + ForkMeshToolbox.specs.map(\.name).joined(separator: ", ") + ".")
            }
        } catch {
            return OnDeviceToolResult("\(call.name) failed: \(AssistantText.failure(error)).")
        }
    }

    func organizations() async -> [OrgSummary] {
        if let orgCache { return orgCache }
        guard let orgs = try? await sources.orgs() else { return [] }
        orgCache = orgs
        return orgs
    }

    private func account() async -> String {
        guard let account = sources.account() else { return "Not signed in to ForkMesh on this phone." }
        var lines = ["Username: \(account.username)"]
        if !account.email.isEmpty { lines.append("Email: \(account.email)") }
        if account.isAdmin { lines.append("ForkMesh administrator: yes") }
        let orgs = await organizations()
        lines.append("Organizations: " + (orgs.isEmpty ? "none" : orgs.map { "\($0.name) (\($0.role))" }.joined(separator: ", ")))
        return lines.joined(separator: "\n")
    }

    func allTasks() async throws -> (tasks: [JSONObject], actor: String, more: Bool) {
        var tasks: [JSONObject] = []
        var seen = Set<String>()
        var offset = 0
        var actor = sources.account()?.username.lowercased() ?? ""
        for _ in 0..<ForkMeshToolbox.maxTaskPages {
            let page = try await sources.tasks(offset)
            if offset == 0 { actor = page.str("actor", actor).lowercased() }
            for task in page.maps("tasks") where seen.insert(task.str("id")).inserted {
                tasks.append(task)
            }
            guard page.bool("hasMore"), page.int("nextOffset") > offset else { return (tasks, actor, false) }
            offset = page.int("nextOffset")
        }
        return (tasks, actor, true)
    }

    private func listTasks(_ args: JSONObject) async throws -> String {
        if let id = AssistantText.argument(args, "id") {
            let task = try await sources.task(id)
            if task.isEmpty { return "No task with id \(id)." }
            return AssistantText.taskDetail(task, timeZone)
        }
        let filter = (AssistantText.argument(args, "filter") ?? "mine").lowercased()
        let query = AssistantText.argument(args, "query")
        let board = try await allTasks()
        var rows = board.tasks
        switch filter {
        case "open": rows = rows.filter { !AssistantText.isDone($0) }
        case "done": rows = rows.filter(AssistantText.isDone)
        case "all": break
        default: rows = rows.filter { !AssistantText.isDone($0) && AssistantText.isAssigned($0, to: board.actor) }
        }
        if let query { rows = rows.filter { AssistantText.matches(query, [$0.str("title"), $0.str("details"), $0.str("assignee"), $0.str("repository")]) } }
        let heading = "Tasks (\(filter == "mine" ? "open, assigned to \(board.actor)" : filter)\(query.map { ", matching \"\($0)\"" } ?? "")): \(rows.count)"
        if rows.isEmpty {
            return heading + (board.tasks.isEmpty ? ". The task board is empty." : ". The board has \(board.tasks.count) tasks in total; try filter \"open\" or \"all\".")
        }
        return AssistantText.list(heading, rows.map { AssistantText.taskLine($0, timeZone) }, more: board.more)
    }

    private func readNotes(_ args: JSONObject) async throws -> String {
        let notes = try await sources.notes()
        let wanted = AssistantText.argument(args, "id") ?? AssistantText.argument(args, "title")
        if let wanted {
            let lower = wanted.lowercased()
            let hit = notes.first { $0.str("id") == wanted }
                ?? notes.first { $0.str("title").lowercased() == lower }
                ?? notes.first { $0.str("title").lowercased().contains(lower) }
            let id = hit?.str("id") ?? wanted
            let note = try await sources.note(id)
            if note.isEmpty { return "No note matches \"\(wanted)\"." }
            let title = note.str("title", hit?.str("title") ?? "Untitled")
            return "Note \"\(title)\" [\(id)], updated \(AssistantText.date(note.int64("updatedAt"), timeZone)):\n\(note.str("markdown"))"
        }
        var rows = notes
        if let query = AssistantText.argument(args, "query") { rows = rows.filter { AssistantText.matches(query, [$0.str("title")]) } }
        rows.sort { $0.int64("updatedAt") > $1.int64("updatedAt") }
        return AssistantText.list("Notes: \(rows.count)", rows.map { note in
            "[\(note.str("id"))] \(note.str("title", "Untitled")) · \(note.str("visibility", "private")) · updated \(AssistantText.date(note.int64("updatedAt"), timeZone))"
        })
    }

    func myRepositories() async throws -> [Repository] {
        let me = sources.account()?.username.lowercased() ?? ""
        let orgs = Set(await organizations().map { $0.name.lowercased() })
        let all = try await sources.repositories()
        var seen = Set<String>()
        return all.filter { repo in
            let owners = [repo.owner.lowercased(), repo.displayOwner.lowercased(), repo.organization.lowercased()]
            return owners.contains(me) || owners.contains { orgs.contains($0) }
        }
        .sorted { $0.updatedMs > $1.updatedMs }
        .filter { seen.insert($0.fullName.lowercased()).inserted }
    }

    private func listRepositories(_ args: JSONObject) async throws -> String {
        if let query = AssistantText.argument(args, "query") {
            let hits = try await sources.repositories().filter { AssistantText.matches(query, [$0.fullName, $0.owner, $0.description, $0.language]) }
            return AssistantText.list("Repositories on ForkMesh matching \"\(query)\": \(hits.count)", hits.map(AssistantText.repoLine))
        }
        let mine = try await myRepositories()
        return AssistantText.list("Your and your organizations' repositories: \(mine.count)", mine.map(AssistantText.repoLine))
    }

    func repositoryTargets(_ args: JSONObject) async throws -> [(owner: String, name: String)] {
        if let named = AssistantText.argument(args, "repository") {
            let parts = named.split(separator: "/").map(String.init)
            if parts.count == 2 {
                let all = try await sources.repositories()
                if let repo = all.first(where: { $0.fullName.lowercased() == named.lowercased() || "\($0.owner)/\($0.name)".lowercased() == named.lowercased() }) {
                    return [(repo.owner, repo.name)]
                }
                return [(parts[0], parts[1])]
            }
            let mine = try await myRepositories().filter { $0.name.lowercased() == named.lowercased() }
            if let repo = mine.first { return [(repo.owner, repo.name)] }
            let any = try await sources.repositories().first { $0.name.lowercased() == named.lowercased() }
            return any.map { [($0.owner, $0.name)] } ?? []
        }
        return try await myRepositories().prefix(ForkMeshToolbox.maxRepoScan).map { ($0.owner, $0.name) }
    }

    private func listIssues(_ args: JSONObject) async throws -> String {
        let targets = try await repositoryTargets(args)
        if targets.isEmpty { return "No repository matches \(AssistantText.argument(args, "repository") ?? "your account")." }
        if let number = args["number"]?.intValue, let target = targets.first {
            guard let issue = try await sources.issues(target.owner, target.name).first(where: { $0.number == number }) else {
                return "\(target.owner)/\(target.name) has no issue #\(number)."
            }
            return AssistantText.issueDetail(issue, repo: "\(target.owner)/\(target.name)")
        }
        let state = (AssistantText.argument(args, "state") ?? "open").lowercased()
        let query = AssistantText.argument(args, "query")
        var rows: [String] = []
        var failures: [String] = []
        for target in targets {
            do {
                var issues = try await sources.issues(target.owner, target.name)
                if state == "open" { issues = issues.filter(\.isOpen) } else if state == "closed" { issues = issues.filter { !$0.isOpen } }
                if let query { issues = issues.filter { AssistantText.matches(query, [$0.title, $0.body, $0.labels.joined(separator: " "), $0.assignees.joined(separator: " ")]) } }
                rows += issues.sorted { $0.number > $1.number }.map { AssistantText.issueLine($0, repo: "\(target.owner)/\(target.name)") }
            } catch {
                failures.append("\(target.owner)/\(target.name): \(AssistantText.failure(error))")
            }
        }
        let scope = targets.count == 1 ? "\(targets[0].owner)/\(targets[0].name)" : "your \(targets.count) most recent repositories"
        return AssistantText.list("Issues (\(state)) in \(scope): \(rows.count)", rows) + AssistantText.failures(failures)
    }

    private func listPulls(_ args: JSONObject) async throws -> String {
        let targets = try await repositoryTargets(args)
        if targets.isEmpty { return "No repository matches \(AssistantText.argument(args, "repository") ?? "your account")." }
        if let number = args["number"]?.intValue, let target = targets.first {
            guard let pull = try await sources.pulls(target.owner, target.name).first(where: { $0.number == number }) else {
                return "\(target.owner)/\(target.name) has no pull request #\(number)."
            }
            return "Pull request \(target.owner)/\(target.name)#\(pull.number): \(pull.title)\nStatus: \(pull.status) · \(pull.head) → \(pull.base)\n\n\(pull.body)\n\nPatch:\n\(OnDeviceChatTools.clip(pull.patch, limit: 3000))"
        }
        let state = (AssistantText.argument(args, "state") ?? "open").lowercased()
        var rows: [String] = []
        var failures: [String] = []
        for target in targets {
            do {
                var pulls = try await sources.pulls(target.owner, target.name)
                if state != "all" { pulls = pulls.filter { state == "open" ? $0.status == "open" : $0.status != "open" } }
                rows += pulls.sorted { $0.number > $1.number }.map { "\(target.owner)/\(target.name)#\($0.number) \($0.title) · \($0.status) · \($0.head) → \($0.base)" }
            } catch {
                failures.append("\(target.owner)/\(target.name): \(AssistantText.failure(error))")
            }
        }
        let scope = targets.count == 1 ? "\(targets[0].owner)/\(targets[0].name)" : "your \(targets.count) most recent repositories"
        return AssistantText.list("Pull requests (\(state)) in \(scope): \(rows.count)", rows) + AssistantText.failures(failures)
    }

    private func readChat(_ args: JSONObject) -> String {
        var messages = sources.chatMessages()
        let conversation = AssistantText.argument(args, "conversation")
        if let conversation {
            let wanted = conversation.lowercased()
            messages = messages.filter { $0.conversation.lowercased() == wanted || $0.conversation.lowercased().dropFirst() == wanted }
        }
        if let query = AssistantText.argument(args, "query") {
            messages = messages.filter { AssistantText.matches(query, [$0.text, $0.senderName]) }
        }
        if messages.isEmpty {
            let known = Set(sources.chatConversations() + sources.chatMessages().map(\.conversation)).sorted()
            return "No chat messages found on this phone\(conversation.map { " in \($0)" } ?? ""). Conversations: \(known.isEmpty ? "none yet" : known.joined(separator: ", "))."
        }
        let limit = min(max(args["limit"]?.intValue ?? 20, 1), 50)
        let shown = messages.prefix(limit).reversed()
        return AssistantText.list("Chat messages, oldest first (\(shown.count) of \(messages.count)):", shown.map { message in
            "\(AssistantText.dayTime(message.timestamp, timeZone)) \(message.conversation) \(message.isSelf ? "you" : message.senderName): \(message.text)\(message.fileName.isEmpty ? "" : " [file \(message.fileName)]")"
        }, limit: limit)
    }

    private func listPings(_ args: JSONObject) async throws -> String {
        let page = try await sources.pings(40)
        var pings = page.notifications
        if args["unread_only"]?.isTrue == true { pings = pings.filter { $0.readAt == 0 } }
        return AssistantText.list("Pings (\(page.unread) unread): \(pings.count)", pings.map { AssistantText.pingLine($0, timeZone) })
    }

    private func listCalendar(_ args: JSONObject) async throws -> String {
        let ahead = min(max(args["days_ahead"]?.intValue ?? 7, 0), 90)
        let back = min(max(args["days_back"]?.intValue ?? 0, 0), 90)
        let day: Int64 = 86_400_000
        let nowMs = Int64(now().timeIntervalSince1970 * 1000)
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = timeZone
        let startOfToday = Int64(calendar.startOfDay(for: now()).timeIntervalSince1970 * 1000)
        let start = startOfToday - Int64(back) * day
        let end = max(startOfToday + Int64(ahead + 1) * day, nowMs + 1)
        let events = try await sources.calendar(start, end)
        let occurrences = CalendarMath.occurrences(events, from: start, to: end, timeZone: timeZone)
        return AssistantText.list("Calendar from \(AssistantText.date(start, timeZone)) for \(back + ahead + 1) days: \(occurrences.count) events", occurrences.map { item in
            let event = item.event
            let when = event.allDay ? "\(AssistantText.date(item.startAt, timeZone)) all day" : "\(AssistantText.dateTime(item.startAt, timeZone))–\(AssistantText.time(item.endAt, timeZone))"
            var line = "\(when) \(event.title)"
            if !event.location.isEmpty { line += " @ \(event.location)" }
            if !event.attendees.isEmpty { line += " with " + event.attendees.map(\.name).joined(separator: ", ") }
            return line
        }, limit: 60)
    }

    private func search(_ args: JSONObject) async -> String {
        guard let query = AssistantText.argument(args, "query") else { return "search_forkmesh needs a query." }
        var sections: [String] = []
        if let board = try? await allTasks() {
            let hits = board.tasks.filter { AssistantText.matches(query, [$0.str("title"), $0.str("details")]) }
            if !hits.isEmpty { sections.append(AssistantText.list("Tasks", hits.map { AssistantText.taskLine($0, timeZone) }, limit: 8)) }
        }
        if let notes = try? await sources.notes() {
            let hits = notes.filter { AssistantText.matches(query, [$0.str("title")]) }
            if !hits.isEmpty { sections.append(AssistantText.list("Notes", hits.map { "[\($0.str("id"))] \($0.str("title"))" }, limit: 8)) }
        }
        if let repos = try? await myRepositories() {
            let hits = repos.filter { AssistantText.matches(query, [$0.fullName, $0.description]) }
            if !hits.isEmpty { sections.append(AssistantText.list("Repositories", hits.map(AssistantText.repoLine), limit: 8)) }
            var issueHits: [String] = []
            for repo in repos.prefix(ForkMeshToolbox.maxRepoScan) {
                guard let issues = try? await sources.issues(repo.owner, repo.name) else { continue }
                issueHits += issues.filter { AssistantText.matches(query, [$0.title, $0.body]) }.map { AssistantText.issueLine($0, repo: "\(repo.owner)/\(repo.name)") }
            }
            if !issueHits.isEmpty { sections.append(AssistantText.list("Issues", issueHits, limit: 8)) }
        }
        let chat = sources.chatMessages().filter { AssistantText.matches(query, [$0.text]) }
        if !chat.isEmpty {
            sections.append(AssistantText.list("Chat", chat.map { "\(AssistantText.dayTime($0.timestamp, timeZone)) \($0.conversation) \($0.isSelf ? "you" : $0.senderName): \($0.text)" }, limit: 8))
        }
        if let page = try? await sources.pings(40) {
            let hits = page.notifications.filter { AssistantText.matches(query, [$0.title, $0.body, $0.repo]) }
            if !hits.isEmpty { sections.append(AssistantText.list("Pings", hits.map { AssistantText.pingLine($0, timeZone) }, limit: 8)) }
        }
        if let agents = try? await sources.agents() {
            let hits = agents.filter { AssistantText.matches(query, [$0.title, $0.repo, $0.branchName]) }
            if !hits.isEmpty { sections.append(AssistantText.list("Agent runs", hits.map { AssistantText.agentLine($0, timeZone) }, limit: 8)) }
        }
        if sections.isEmpty { return "Nothing in tasks, notes, repositories, issues, chat, pings or agents matches \"\(query)\"." }
        return sections.joined(separator: "\n\n")
    }
}
