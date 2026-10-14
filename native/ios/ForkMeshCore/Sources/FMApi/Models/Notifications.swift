import Foundation
import FMBase

public struct NotificationPage: Equatable, Hashable, Sendable {
    public var notifications: [ForkNotification]
    public var unread: Int
    /// Every notification the Worker holds for this account, not just the rows on
    /// this page. The phone fetches one capped page and never scrolls past it, so
    /// anything that speaks for the whole inbox (clearing it) has to quote this
    /// rather than the page length. Defaults to the page length for a Worker that
    /// did not send it.
    public var total: Int

    public init(notifications: [ForkNotification], unread: Int, total: Int? = nil) {
        self.notifications = notifications
        self.unread = unread
        self.total = total ?? notifications.count
    }

    public init(json: JSONObject) {
        let raw = json["notifications"]?.arrayValue ?? json["items"]?.arrayValue ?? []
        self.init(
            notifications: raw.compactMap(\.objectValue).map(ForkNotification.init(json:)),
            unread: ModelJSON.int(json["unread"]),
            total: json.contains("total") ? ModelJSON.int(json["total"]) : nil
        )
    }
}

/// How the inbox groups a notification's kind.
public enum NotificationFilterGroup: String, Sendable, CaseIterable {
    case mentions = "Mentions"
    case system = "System"
    case repo = "Repo"
}

public struct ForkNotification: Equatable, Hashable, Sendable, Identifiable {
    public var id: String
    public var kind: String
    public var title: String
    public var body: String
    public var repo: String
    public var href: String
    public var actor: String
    public var source: String
    public var ts: Int
    public var readAt: Int
    public var meta: JSONObject

    public init(
        id: String, kind: String, title: String, body: String = "", repo: String = "", href: String = "",
        actor: String = "", source: String = "", ts: Int = 0, readAt: Int = 0, meta: JSONObject = JSONObject()
    ) {
        self.id = id
        self.kind = kind
        self.title = title
        self.body = body
        self.repo = repo
        self.href = href
        self.actor = actor
        self.source = source
        self.ts = ts
        self.readAt = readAt
        self.meta = meta
    }

    public init(json: JSONObject) {
        self.init(
            id: json.str("id"),
            kind: json.str("kind", "pending_inbox"),
            title: json.str("title", "Notification"),
            body: json.str("body"),
            repo: json.str("repo"),
            href: json.str("href"),
            actor: json.str("actor"),
            source: json.str("source"),
            ts: ModelJSON.int(json["ts"]),
            readAt: ModelJSON.int(ModelJSON.coalesce(json["readAt"], json["read_at"])),
            meta: json["meta"]?.objectValue ?? JSONObject()
        )
    }

    public var json: JSONObject {
        [
            "id": .string(id), "kind": .string(kind), "title": .string(title), "body": .string(body),
            "repo": .string(repo), "href": .string(href), "actor": .string(actor), "source": .string(source),
            "ts": .int(Int64(ts)), "readAt": .int(Int64(readAt)), "meta": .object(meta),
        ]
    }

    public var isUnread: Bool { readAt <= 0 }

    public var kindLabel: String {
        switch kind {
        case "mention": return "Mention"
        case "subscribed": return "Subscribed"
        case "issue_opened": return "New issue"
        case "pull_submitted": return "Pull request"
        case "issue_assigned": return "Assignment"
        case "repo_shared": return "Repo share"
        case "bounty_funded", "bounty_paid": return "Bounty"
        case "tip_sent": return "Tip"
        case "release_published": return "Release"
        case "host_online", "host_offline": return "Host"
        case "credits_refilled": return "Claude Code"
        case "pending_inbox": return "Inbox"
        case "mirror_request": return "Mirror request"
        case "pending_reward": return "Reward"
        case "org_succession": return "Organization"
        case "repository_hosted": return "Hosting"
        case "organization_task_started", "organization_task_activity", "organization_task_update": return "Task"
        case "calendar_invitation", "calendar_reminder", "calendar_rsvp": return "Calendar"
        case "chat_webhook", "channel_unread", "direct_unread": return "Chat"
        case "account_email_sent": return "Email"
        case "operational_alert": return "Alert"
        case "security_alert": return "Security"
        case "error_group": return "Error"
        case "device_added": return "New device"
        case "plugin_activity": return "Package"
        case "package_review": return "Package review"
        case "hackathon_squad", "hackathon_reward": return "Hackathon"
        case "timer_finished": return "Timer"
        case "desktop_ping": return "Desktop"
        case "repo_activity": return "Repo activity"
        case "code_push": return "Code push"
        default: return "Notification"
        }
    }

    /// "Mentions", "System" or "Repo": the raw value of `filter`.
    public var filterGroup: String { filter.rawValue }

    public var filter: NotificationFilterGroup {
        switch kind {
        case "mention":
            return .mentions
        case "host_online", "host_offline", "release_published", "bounty_funded", "bounty_paid",
             "credits_refilled", "timer_finished", "desktop_ping":
            return .system
        default:
            return .repo
        }
    }

    public func copyWith(readAt: Int? = nil) -> ForkNotification {
        var copy = self
        copy.readAt = readAt ?? self.readAt
        return copy
    }
}
