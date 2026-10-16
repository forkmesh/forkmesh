// The pure logic of the Notifications screen: kind icons, the filter chips,
// the body state, empty-state copy, the row labels, the ping-to-agent prompt
// and the choice of repository it goes to. Follows native/android
// core/account/NotificationsLogic.kt.

import FMBase
import Foundation

/// `FmIcons.notificationLine`, the app's MingCute bell, by the name `FmIcon`
/// draws it under.
public let notificationLineIcon = "mingcute:notification_line"

/// `notificationIconForKind`: every known server ping kind has its own glyph
/// (Material icon names); unknown future kinds get the generic bell.
public func notificationIconForKind(_ kind: String) -> String {
    switch kind.dartTrimmed.lowercased() {
    case "mention": return "alternate_email_rounded"
    case "subscribed": return "notifications_active_outlined"
    case "issue_opened": return "adjust"
    case "pull_submitted": return "call_merge_rounded"
    case "issue_assigned": return "assignment_ind_outlined"
    case "repo_shared": return "folder_special_outlined"
    case "bounty_funded", "bounty_paid", "tip_sent": return "payments_outlined"
    case "release_published": return "new_releases_outlined"
    case "host_online": return "cloud_done_outlined"
    case "host_offline": return "cloud_off_outlined"
    case "credits_refilled": return "account_balance_wallet_outlined"
    case "pending_inbox": return "inbox_outlined"
    case "mirror_request": return "sync_alt"
    case "pending_reward": return "account_balance_wallet_outlined"
    case "org_succession": return "account_tree_outlined"
    case "repository_hosted": return "cloud_done_outlined"
    case "organization_task_started", "organization_task_activity", "organization_task_update":
        return "check_circle_outline"
    case "calendar_invitation", "calendar_reminder", "calendar_rsvp": return "calendar_today_outlined"
    case "chat_webhook", "channel_unread", "direct_unread": return "chat_bubble_outline"
    case "account_email_sent": return "mark_email_read_outlined"
    case "operational_alert": return "warning_amber_outlined"
    case "error_group": return "error_outline"
    case "plugin_activity": return "extension_outlined"
    case "timer_finished": return "timer_outlined"
    case "desktop_ping": return "desktop_mac_outlined"
    case "repo_activity", "code_push": return "commit_outlined"
    case "security_alert": return "gpp_maybe_outlined"
    case "device_added": return "smartphone_outlined"
    case "hackathon_squad": return "groups_outlined"
    case "hackathon_reward": return "card_giftcard_outlined"
    case "badge": return "emoji_events_outlined"
    case "package_review": return "rate_review_outlined"
    default: return notificationLineIcon
    }
}

public enum PingTone: String, Sendable {
    case danger, success, warning, info, accent, neutral
}

public func notificationToneForKind(_ kind: String) -> PingTone {
    switch kind.dartTrimmed.lowercased() {
    case "error", "error_group", "operational_alert", "security_alert", "local_failure": return .danger
    case "agent-done", "agent_done", "host_online", "repository_hosted", "badge": return .success
    case "issue", "issue_opened", "issue_assigned", "host_offline", "agent_approval", "agent-approval": return .warning
    case "mention", "subscribed", "repo", "repo_shared", "repo_activity", "code_push", "pending_inbox",
         "mirror", "mirror_request", "calendar_invitation", "calendar_reminder", "calendar_rsvp",
         "plugin_activity", "timer_finished", "account_email_sent", "prompt", "device_added", "package_review":
        return .info
    case "action", "organization_task_started", "organization_task_activity", "organization_task_update",
         "pull", "pull_submitted", "review_requested", "pull_review_requested", "bounty_funded", "bounty_paid",
         "credits_refilled", "pending_reward", "tip_sent", "release", "release_published", "org_succession",
         "chat", "comment", "discussion", "chat_webhook", "channel_unread", "direct_unread", "hackathon_squad", "hackathon_reward":
        return .accent
    default: return .neutral
    }
}

/// The filter chips, in order.
public let notificationFilters = ["All", "Unread", "Mentions", "Repo", "System"]

public func filterNotifications(_ items: [ForkNotification], _ filter: String) -> [ForkNotification] {
    items.filter { n in
        switch filter {
        case "Unread": return n.isUnread
        case "Mentions": return n.filterGroup == "Mentions"
        case "Repo": return n.filterGroup == "Repo"
        case "System": return n.filterGroup == "System"
        default: return true
        }
    }
}

/// Which body shows, what it is titled and what it says.
public enum NotificationsBody: Equatable, Sendable {
    /// The skeleton: loading with nothing to show yet.
    case loading
    /// The read failed and nothing is cached.
    case error(String)
    case empty(filter: String)
    case items(filter: String, items: [ForkNotification])

    /// `_bodyState`: the key the body cross-fades on; the filter is part of it.
    public var stateKey: String {
        switch self {
        case .loading: return "loading"
        case .error: return "error"
        case .empty(let filter): return "empty-\(filter)"
        case .items(let filter, _): return "list-\(filter)"
        }
    }
}

public func notificationsBody(
    loading: Bool,
    error: String,
    all: [ForkNotification],
    filter: String
) -> NotificationsBody {
    if loading && all.isEmpty { return .loading }
    if !error.isEmpty && all.isEmpty { return .error(error) }
    let visible = filterNotifications(all, filter)
    if visible.isEmpty { return .empty(filter: filter) }
    return .items(filter: filter, items: visible)
}

/// The empty state of one filter.
public struct NotificationsEmptyCopy: Equatable, Sendable {
    public let icon: String
    public let title: String
    public let message: String

    public init(filter: String) {
        let unread = filter == "Unread"
        icon = unread ? "mark_email_read_outlined" : notificationLineIcon
        title = unread ? "All caught up" : "No \(filter.lowercased()) notifications"
        message = unread
            ? "Unread issue, PR, discussion, host, release, and mention updates will appear here."
            : "Mainnode-backed ForkMesh updates will appear here when something needs your attention."
    }
}

public enum NotificationsCopy {
    public static let loadErrorTitle = "Could not load notifications"
    public static let refresh = "Refresh notifications"
    public static let deleteAll = "Delete all notifications"
    public static let deleteAllBody =
        "This removes them from your ForkMesh inbox on every device, read and unread alike. This cannot be undone."
    public static let deleteAllConfirm = "Delete all"
    public static let deleted = "Notifications deleted."
    public static let pingCopied =
        "The agent works in a repository on this phone; clone or create one first. The ping was copied."
    public static let pickRepoTitle = "Send ping to the agent in…"
}

public func notificationsUnreadLabel(_ unread: Int) -> String { "\(unread) unread" }

public func notificationsUpdatesLabel(_ count: Int) -> String { "\(count) \(count == 1 ? "update" : "updates")" }

/// "Mark all read" only shows while something is unread.
public func canMarkAllRead(_ unread: Int) -> Bool { unread > 0 }

/// "Delete all" is live only while notifications are loaded: that is what the
/// clear acts on, and an empty list has nothing to confirm. `loaded` is the
/// page size, not the filtered view: the clear ignores the filter.
public func canDeleteAllNotifications(_ loaded: Int) -> Bool { loaded > 0 }

/// `total` is the server-side inbox count, not the page on screen: the clear
/// removes every row the Worker holds.
public func notificationsDeleteAllTitle(_ total: Int) -> String {
    total == 1 ? "Delete 1 notification?" : "Delete all \(total) notifications?"
}

/// The error as `dartErrorText` renders it.
public func notificationsDeleteErrorText(_ error: Error?) -> String {
    "Could not delete notifications: \(error.map { dartErrorText($0) } ?? "unknown error")"
}

public func notificationDeleteErrorText(_ error: Error) -> String {
    "Could not delete notification: \(dartErrorText(error))"
}

/// `_timeLabel`: local "h:mm AM", or "" without a timestamp.
public func notificationTimeLabel(_ ms: Int, timeZone: TimeZone = .current) -> String {
    if ms <= 0 { return "" }
    var calendar = Calendar(identifier: .gregorian)
    calendar.timeZone = timeZone
    let parts = calendar.dateComponents([.hour, .minute], from: Date(timeIntervalSince1970: Double(ms) / 1000))
    let hour = parts.hour ?? 0
    let meridiem = hour < 12 ? "AM" : "PM"
    return String(format: "%d:%02d", hour % 12 == 0 ? 12 : hour % 12, parts.minute ?? 0) + " \(meridiem)"
}

public func pingDetailWhen(_ ms: Int, timeZone: TimeZone = .current) -> String {
    if ms <= 0 { return "" }
    var calendar = Calendar(identifier: .gregorian)
    calendar.timeZone = timeZone
    let p = calendar.dateComponents([.year, .month, .day, .weekday], from: Date(timeIntervalSince1970: Double(ms) / 1000))
    let weekdays = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"]
    let months = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]
    let weekday = weekdays[((p.weekday ?? 1) - 1) % 7]
    let month = months[((p.month ?? 1) - 1) % 12]
    return "\(weekday), \(month) \(p.day ?? 1), \(p.year ?? 0) · \(notificationTimeLabel(ms, timeZone: timeZone))"
}

public func notificationPreview(_ notification: ForkNotification) -> String {
    notification.body.isEmpty ? "Open context in ForkMesh to review this update." : notification.body
}

/// The row's pills: the kind, the repository and the @actor when known.
public func notificationPills(_ notification: ForkNotification) -> [String] {
    var pills = [notification.kindLabel]
    if !notification.repo.isEmpty { pills.append(notification.repo) }
    if !notification.actor.isEmpty { pills.append("@\(notification.actor)") }
    return pills
}

public func notificationContextLabel(_ notification: ForkNotification) -> String? {
    notification.href.isEmpty ? nil : "Open context: \(notification.href)"
}

/// `_pingWhen`: "yyyy-MM-dd HH:mm" in local time.
public func pingWhen(_ ms: Int, timeZone: TimeZone = .current) -> String {
    var calendar = Calendar(identifier: .gregorian)
    calendar.timeZone = timeZone
    let p = calendar.dateComponents(
        [.year, .month, .day, .hour, .minute], from: Date(timeIntervalSince1970: Double(ms) / 1000))
    return String(
        format: "%04d-%02d-%02d %02d:%02d", p.year ?? 0, p.month ?? 0, p.day ?? 0, p.hour ?? 0, p.minute ?? 0)
}

/// `pingAgentPrompt`: one ping, written out for the coding agent's composer.
/// Only the fields the ping has are listed, so a bare notice stays two lines
/// long. The shape matches the desktop's Send-to-prompt text.
public func pingAgentPrompt(_ ping: ForkNotification, timeZone: TimeZone = .current) -> String {
    var head: [String] = []
    let kind = ping.kindLabel.dartTrimmed
    if !kind.isEmpty { head.append(kind) }
    if ping.ts > 0 { head.append(pingWhen(ping.ts, timeZone: timeZone)) }
    var lines = [head.isEmpty ? "ForkMesh ping" : "ForkMesh ping (\(head.joined(separator: " · ")))"]
    let title = ping.title.dartTrimmed
    if !title.isEmpty { lines.append(title) }
    let body = ping.body.dartTrimmed
    if !body.isEmpty { lines.append(contentsOf: ["", body]) }
    var context: [String] = []
    let repo = ping.repo.dartTrimmed
    let actor = ping.actor.dartTrimmed
    let href = ping.href.dartTrimmed
    if !repo.isEmpty { context.append("Repository: \(repo)") }
    if !actor.isEmpty { context.append("From: \(actor)") }
    if !href.isEmpty { context.append("Link: \(href)") }
    if !context.isEmpty { lines.append(contentsOf: [""] + context) }
    return lines.joined(separator: "\n")
}

/// `pingNamesLocalRepo`: whether the on-device repository (its folder `name`
/// and, once published or cloned, `publishedAs` "owner/name") is the one a
/// ping names.
public func pingNamesLocalRepo(_ pingRepo: String, name: String, publishedAs: String?) -> Bool {
    let wanted = pingRepo.dartTrimmed.lowercased()
    if wanted.isEmpty { return false }
    let published = (publishedAs ?? "").dartTrimmed.lowercased()
    if !published.isEmpty { return published == wanted }
    let last = wanted.split(separator: "/", omittingEmptySubsequences: false).last.map(String.init) ?? wanted
    return name.dartTrimmed.lowercased() == last
}

/// One on-device repository, as the ping hand-off sees it.
public struct PingRepoCandidate: Equatable, Sendable {
    public let name: String
    public let publishedAs: String?

    public init(name: String, publishedAs: String?) {
        self.name = name
        self.publishedAs = publishedAs
    }
}

/// Where a ping goes (`_agentRepoFor`).
public enum PingRepoChoice: Equatable, Sendable {
    /// No repository on the phone: copy the prompt instead.
    case copy
    /// The index into the candidates to open, chosen without asking.
    case open(Int)
    /// Ask, listing these indices in this order (the named ones first).
    case pick([Int])
}

/// The repository a ping goes to without asking when there is one obvious
/// answer: the only clone the ping names, else the repository whose session is
/// running, else the only repository on the phone. Otherwise the person picks,
/// with the repositories the ping names listed first.
public func pingRepoChoice(_ pingRepo: String, candidates: [PingRepoCandidate], runningRepo: String?) -> PingRepoChoice {
    if candidates.isEmpty { return .copy }
    let named = candidates.indices.filter {
        pingNamesLocalRepo(pingRepo, name: candidates[$0].name, publishedAs: candidates[$0].publishedAs)
    }
    if named.count == 1 { return .open(named[0]) }
    if named.isEmpty {
        if let running = candidates.indices.first(where: { candidates[$0].name == runningRepo }) {
            return .open(running)
        }
        if candidates.count == 1 { return .open(0) }
    }
    let rest = candidates.indices.filter { !named.contains($0) }
    return .pick(named + rest)
}

/// The picker row's subtitle.
public func pingRepoSubtitle(_ candidate: PingRepoCandidate, runningRepo: String?) -> String {
    if candidate.name == runningRepo { return "Agent running — the ping steers it" }
    return candidate.publishedAs ?? "Only on this phone"
}

/// What tapping a ping opens (`_openNotification`): the repository record its
/// link names, else the web page it links to, else the repository it
/// mentions, else the ping written out in full.
public func notificationTapLink(_ notification: ForkNotification) -> NotificationDeepLink? {
    if let link = NotificationDeepLink.parse(notification.href) { return link }
    if notification.href.dartTrimmed.isEmpty {
        return NotificationDeepLink.parse("/\(notification.repo.dartTrimmed)")
    }
    return nil
}

public func pingAgeLabel(_ ms: Int, now: Int) -> String {
    if ms <= 0 { return "" }
    let seconds = (now - ms) / 1000
    if seconds < 60 { return "just now" }
    if seconds < 3600 { return "\(seconds / 60) min ago" }
    if seconds < 86400 { return "\(seconds / 3600) h ago" }
    if seconds < 86400 * 30 { return "\(seconds / 86400) d ago" }
    return ""
}

public struct PingDetailFact: Equatable, Hashable, Sendable {
    public let label: String
    public let value: String

    public init(_ label: String, _ value: String) {
        self.label = label
        self.value = value
    }
}

public func pingMetaLabel(_ key: String) -> String {
    var words = ""
    var previous: Character?
    for ch in key.dartTrimmed {
        if ch == "_" || ch == "-" || ch == "." {
            words.append(" ")
        } else {
            if ch.isUppercase, let p = previous, p.isLowercase || p.isNumber { words.append(" ") }
            words.append(ch)
        }
        previous = ch
    }
    let collapsed = words.split(separator: " ").joined(separator: " ").lowercased()
    guard let first = collapsed.first else { return "" }
    return first.uppercased() + collapsed.dropFirst()
}

private func pingMetaValue(_ value: JSONValue) -> String? {
    switch value {
    case .string(let text):
        let trimmed = text.dartTrimmed
        return trimmed.isEmpty ? nil : trimmed
    case .bool(let flag):
        return flag ? "Yes" : "No"
    case .int(let number):
        return String(number)
    case .double(let number):
        if number.rounded() == number && abs(number) < 1e15 { return String(Int64(number)) }
        return String(number)
    default:
        return nil
    }
}

public func pingMetaFacts(_ meta: JSONObject) -> [PingDetailFact] {
    meta.sorted { $0.key.lowercased() < $1.key.lowercased() }.compactMap { entry in
        let label = pingMetaLabel(entry.key)
        guard !label.isEmpty, let text = pingMetaValue(entry.value) else { return nil }
        return PingDetailFact(label, text)
    }
}

public func pingDetailFacts(_ ping: ForkNotification, now: Int, timeZone: TimeZone = .current) -> [PingDetailFact] {
    var facts = [PingDetailFact("Kind", ping.kindLabel)]
    if ping.ts > 0 {
        let when = pingDetailWhen(ping.ts, timeZone: timeZone)
        let age = pingAgeLabel(ping.ts, now: now)
        facts.append(PingDetailFact("Received", age.isEmpty ? when : "\(when) · \(age)"))
    }
    facts.append(PingDetailFact("Status", ping.isUnread ? "Unread" : "Read · \(pingDetailWhen(ping.readAt, timeZone: timeZone))"))
    let repo = ping.repo.dartTrimmed
    let actor = ping.actor.dartTrimmed
    let source = ping.source.dartTrimmed
    let href = ping.href.dartTrimmed
    if !repo.isEmpty { facts.append(PingDetailFact("Repository", repo)) }
    if !actor.isEmpty { facts.append(PingDetailFact("From", "@\(actor)")) }
    if !source.isEmpty { facts.append(PingDetailFact("Source", source)) }
    if !href.isEmpty { facts.append(PingDetailFact("Link", href)) }
    facts.append(contentsOf: pingMetaFacts(ping.meta))
    let id = ping.id.dartTrimmed
    if !id.isEmpty { facts.append(PingDetailFact("ID", id)) }
    return facts
}

public func pingOpenLabel(_ target: NotificationTapTarget) -> String? {
    switch target {
    case .context:
        return nil
    case .web:
        return "Open in browser"
    case .repo(let link):
        switch link.initialTab {
        case .issues where link.number > 0: return "Open issue #\(link.number)"
        case .pulls where link.number > 0: return "Open pull request #\(link.number)"
        case .discussions where link.number > 0: return "Open discussion #\(link.number)"
        case .commits where !link.reference.isEmpty: return "Open commit \(link.reference.prefix(7))"
        case .commits where !link.branch.isEmpty: return "Open commits on \(link.branch)"
        case .releases: return "Open releases"
        default: return "Open \(link.repo.owner)/\(link.repo.name)"
        }
    }
}

public enum PingDetailCopy {
    public static let title = "Ping"
    public static let deleteLabel = "Delete ping"
    public static let deleteTitle = "Delete this ping?"
    public static let deleteBody = "It is removed from your ForkMesh inbox on every device. This cannot be undone."
    public static let deleted = "Ping deleted."
}
