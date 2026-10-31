import FMBase
import Foundation

public enum InboxSource: String, Codable, Hashable, Sendable {
    case ping, task, local
}

public enum InboxSyncState: String, Codable, Hashable, Sendable {
    case synced, pending, failed, localOnly
}

public enum InboxCategory: String, Codable, Hashable, Sendable, CaseIterable {
    case approval, review, assigned, mention, ping

    public static func of(kind: String) -> InboxCategory {
        switch kind.trimmingCharacters(in: .whitespaces).lowercased() {
        case "agent_approval", "agent-approval": return .approval
        case "pull_submitted", "review_requested", "pull_review_requested": return .review
        case "issue_assigned", "organization_task_started": return .assigned
        case "mention": return .mention
        default: return .ping
        }
    }

    public func matches(_ filter: InboxFilter) -> Bool {
        switch filter {
        case .all: return true
        case .pings: return true
        case .reviews: return self == .review
        case .assigned: return self == .assigned
        case .mentions: return self == .mention
        case .approvals: return self == .approval
        }
    }
}

extension InboxFilter {
    public var label: String {
        switch self {
        case .all: return "All"
        case .pings: return "Pings"
        case .reviews: return "Reviews"
        case .assigned: return "Assigned"
        case .mentions: return "Mentions"
        case .approvals: return "Approvals"
        }
    }

    public var emptyMessage: String {
        switch self {
        case .all: return "Nothing needs you right now."
        case .pings: return "No pings."
        case .reviews: return "No pull requests are waiting for you."
        case .assigned: return "Nothing is assigned to you."
        case .mentions: return "No one has mentioned you."
        case .approvals: return "No agent is waiting for your approval."
        }
    }
}

public struct InboxItem: Equatable, Hashable, Sendable, Identifiable {
    public var id: String
    public var source: InboxSource
    public var category: InboxCategory
    public var kind: String
    public var title: String
    public var body: String
    public var repo: String
    public var actor: String
    public var href: String
    public var ts: Int
    public var unread: Bool
    public var sync: InboxSyncState
    public var route: Route?
    public var remoteId: String
    public var sessionId: String
    public var approvalId: String
    public var desktopPingId: String
    public var desktopNode: String

    public init(
        id: String, source: InboxSource, category: InboxCategory, kind: String, title: String, body: String = "",
        repo: String = "", actor: String = "", href: String = "", ts: Int = 0, unread: Bool = true,
        sync: InboxSyncState = .synced, route: Route? = nil, remoteId: String = "", sessionId: String = "",
        approvalId: String = "", desktopPingId: String = "", desktopNode: String = ""
    ) {
        self.id = id
        self.source = source
        self.category = category
        self.kind = kind
        self.title = title
        self.body = body
        self.repo = repo
        self.actor = actor
        self.href = href
        self.ts = ts
        self.unread = unread
        self.sync = sync
        self.route = route
        self.remoteId = remoteId
        self.sessionId = sessionId
        self.approvalId = approvalId
        self.desktopPingId = desktopPingId
        self.desktopNode = desktopNode
    }

    public init(ping: ForkNotification) {
        let category = InboxCategory.of(kind: ping.kind)
        let sessionId = ping.meta.str("sessionId")
        let approvalId = ping.meta.str("approvalId")
        self.init(
            id: "ping:\(ping.id)", source: .ping, category: category, kind: ping.kind, title: ping.title, body: ping.body,
            repo: ping.repo, actor: ping.actor, href: ping.href, ts: ping.ts, unread: ping.isUnread,
            route: InboxRoutes.route(kind: ping.kind, href: ping.href, meta: ping.meta, repo: ping.repo),
            remoteId: ping.id, sessionId: sessionId, approvalId: approvalId,
            desktopPingId: ping.meta.str("pingId"), desktopNode: ping.meta.str("node"))
    }

    public var fromDesktop: Bool { source == .ping && kind == "desktop_ping" }

    public var icon: String {
        switch source {
        case .task: return "task_alt"
        case .local:
            if kind == LocalPingLog.chatKind { return "chat_bubble_outline" }
            return kind == "local_notice" ? "info_outline" : "error_outline"
        case .ping: return category == .approval ? "verified_user_outlined" : notificationIconForKind(kind)
        }
    }

    public var tone: PingTone {
        switch (source, category) {
        case (.task, _): return .accent
        case (.local, _):
            if kind == LocalPingLog.chatKind { return .accent }
            return kind == "local_notice" ? .info : .danger
        case (_, .approval): return .warning
        case (_, .review): return .accent
        case (_, .mention): return .info
        default: return notificationToneForKind(kind)
        }
    }

    public var kindLabel: String {
        switch (source, category) {
        case (.task, _): return "Task"
        case (.local, _): return kind == LocalPingLog.chatKind ? "Chat" : "On this phone"
        case (_, .approval): return "Approval"
        case (_, .review): return "Review"
        case (_, .assigned): return "Assigned"
        case (_, .mention): return "Mention"
        case (_, .ping): return ForkNotification(id: "", kind: kind, title: "").kindLabel
        }
    }
}

public enum InboxRoutes {
    public static func route(kind: String, href: String, meta: JSONObject, repo: String) -> Route? {
        if InboxCategory.of(kind: kind) == .approval {
            let approval = meta.str("approvalId")
            let session = meta.str("sessionId")
            if !approval.isEmpty && !session.isEmpty { return .approval(id: "\(session)/\(approval)") }
            if !session.isEmpty { return .agent(id: session) }
        }
        if kind == "desktop_ping", !meta.str("sessionId").isEmpty {
            return .agent(id: meta.str("sessionId"))
        }
        let number = meta.int("number")
        if let link = NotificationDeepLink.parse(href) {
            return route(link)
        }
        if let ref = RepoRef(slug: repo) {
            switch kind {
            case "pull_submitted", "pull_attention":
                return number > 0 ? .pull(ref, number: number, tab: .conversation) : .repo(ref, section: .pulls)
            case "issue_assigned", "issue_opened", "mention":
                return number > 0 ? .issue(ref, number: number) : .repo(ref, section: .issues)
            case "release_published": return .repo(ref, section: .releases)
            default: return .repo(ref, section: nil)
            }
        }
        return nil
    }

    public static func route(_ link: NotificationDeepLink) -> Route {
        let ref = RepoRef(owner: link.repo.owner, name: link.repo.name)
        switch link.initialTab {
        case .issues: return link.number > 0 ? .issue(ref, number: link.number) : .repo(ref, section: .issues)
        case .pulls: return link.number > 0 ? .pull(ref, number: link.number, tab: .conversation) : .repo(ref, section: .pulls)
        case .discussions: return link.number > 0 ? .discussion(ref, number: link.number) : .repo(ref, section: .discussions)
        case .commits:
            if !link.reference.isEmpty { return .commit(ref, sha: link.reference) }
            return .commits(ref, ref: link.branch)
        case .code:
            if !link.reference.isEmpty { return .commit(ref, sha: link.reference) }
            return .repo(ref, section: nil)
        case .mirrors: return .repo(ref, section: .mirrors)
        case .releases: return .repo(ref, section: .releases)
        case .about: return .repo(ref, section: .about)
        case .actions: return link.reference.isEmpty ? .repo(ref, section: .actions) : .run(ref, id: link.reference)
        case .agents: return link.number > 0 ? .agent(id: String(link.number)) : .repo(ref, section: .agents)
        case .worktrees: return .repo(ref, section: .worktrees)
        }
    }

    public static func approvalParts(_ id: String) -> (sessionId: String, approvalId: String)? {
        let parts = id.split(separator: "/", omittingEmptySubsequences: false)
        guard parts.count == 2 else { return nil }
        let session = String(parts[0])
        let approval = String(parts[1])
        guard !session.isEmpty, !approval.isEmpty else { return nil }
        return (session, approval)
    }
}

public enum InboxFeed {
    public static let maxItems = 300

    public static func taskItem(_ task: JSONObject, actor: String) -> InboxItem? {
        let id = task.str("id")
        guard !id.isEmpty, task.str("assigneeKind") == "user", task.str("assignee").lowercased() == actor.lowercased(),
              task.str("status") != "done" else { return nil }
        let updated = max(task.int("updatedAt"), task.int("createdAt"))
        let details = task.str("details").trimmingCharacters(in: .whitespacesAndNewlines)
        return InboxItem(
            id: "task:\(id)@\(updated)", source: .task, category: .assigned, kind: "organization_task",
            title: task.str("title", "Organization task"), body: String(details.prefix(280)),
            repo: task.str("repository"), actor: task.str("createdBy"), ts: updated, route: .task(id: id), remoteId: id)
    }

    public static func merge(pings: [InboxItem], tasks: [InboxItem], local: [InboxItem]) -> [InboxItem] {
        var seen = Set<String>()
        var out: [InboxItem] = []
        for item in pings + tasks + local where seen.insert(item.id).inserted {
            out.append(item)
        }
        out.sort { a, b in
            let aPinned = a.category == .approval && a.unread
            let bPinned = b.category == .approval && b.unread
            if aPinned != bPinned { return aPinned }
            return a.ts != b.ts ? a.ts > b.ts : a.id < b.id
        }
        return Array(out.prefix(maxItems))
    }

    public static func filter(_ items: [InboxItem], _ filter: InboxFilter, unreadOnly: Bool = false) -> [InboxItem] {
        items.filter { item in
            if unreadOnly && !item.unread { return false }
            if filter == .pings { return item.source != .task }
            return item.category.matches(filter)
        }
    }

    public static func unreadCounts(_ items: [InboxItem]) -> [InboxFilter: Int] {
        var counts: [InboxFilter: Int] = [:]
        for filter in InboxFilter.allCases {
            counts[filter] = self.filter(items, filter, unreadOnly: true).count
        }
        return counts
    }
}
