// The Pings settings page's wire shape (issue #347).
//
// `GET /api/notifications?settings=1` and every `POST /api/notifications` carrying `pingSettings`
// answer with the whole of this, so the page always re-renders from what the Worker stored rather
// than from what it asked for.

import FMBase
import Foundation

/// The trimmed text of the first key that is present and not null, or "".
private func pingField(_ json: JSONObject, _ keys: String...) -> String {
    for key in keys {
        if let value = json[key], !value.isNull { return value.text.dartTrimmed }
    }
    return ""
}

/// One ping kind row: its switch, its label, and whether it also mails.
public struct PingKindOption: Equatable, Hashable, Sendable {
    public let kind: String
    public let label: String
    /// The kind also has an email switch in `notificationPreferences`.
    public let email: Bool

    public init(kind: String, label: String, email: Bool = false) {
        self.kind = kind
        self.label = label
        self.email = email
    }

    public init(json: JSONObject) {
        self.init(
            kind: pingField(json, "kind"),
            label: pingField(json, "label", "kind"),
            email: json["email"] == .bool(true))
    }
}

/// A heading on the settings page and the kinds under it.
public struct PingKindGroup: Equatable, Hashable, Sendable {
    public let key: String
    public let label: String
    public let kinds: [PingKindOption]

    public init(key: String, label: String, kinds: [PingKindOption]) {
        self.key = key
        self.label = label
        self.kinds = kinds
    }

    public static let hiddenKinds: Set<String> = ["credits_refilled", "tip_sent"]

    public init(json: JSONObject) {
        let kinds = (json["kinds"]?.arrayValue ?? [])
            .compactMap { $0.objectValue }
            .map { PingKindOption(json: $0) }
            .filter { !$0.kind.isEmpty && !PingKindGroup.hiddenKinds.contains($0.kind) }
        let key = pingField(json, "key")
        self.init(key: key, label: key == "rewards" ? "Rewards" : pingField(json, "label", "key"), kinds: kinds)
    }
}

/// The daily window in which pings are filed but raise nothing.
public struct PingQuietHours: Equatable, Hashable, Sendable {
    public static let defaultStart = 22 * 60
    public static let defaultEnd = 7 * 60
    /// Monday = 0, the Worker's (and Python's) weekday numbering.
    public static let allDays = [0, 1, 2, 3, 4, 5, 6]

    public let enabled: Bool
    /// Minutes after local midnight. An end at or before the start crosses midnight (22:00 → 07:00).
    public let start: Int
    public let end: Int
    /// Weekdays a window may start on.
    public let days: [Int]
    /// IANA zone the minutes are in.
    public let timezone: String

    public init(
        enabled: Bool = false, start: Int = PingQuietHours.defaultStart, end: Int = PingQuietHours.defaultEnd,
        days: [Int] = PingQuietHours.allDays, timezone: String = "UTC"
    ) {
        self.enabled = enabled
        self.start = start
        self.end = end
        self.days = days
        self.timezone = timezone
    }

    public init(json raw: JSONValue?) {
        guard let json = raw?.objectValue else {
            self.init()
            return
        }
        func minute(_ value: JSONValue?, _ fallback: Int) -> Int {
            let parsed: Int?
            switch value {
            case .some(.int(let number)): parsed = Int(truncatingIfNeeded: number)
            case .some(.double(let number)) where number.isFinite: parsed = Int(number)
            case .some(let other): parsed = Int(other.text.dartTrimmed)
            case .none: parsed = nil
            }
            guard let minutes = parsed, minutes >= 0, minutes < 24 * 60 else { return fallback }
            return minutes
        }
        let days: [Int]
        if let rawDays = json["days"]?.arrayValue {
            var picked = Set<Int>()
            for day in rawDays {
                switch day {
                case .int(let number) where number >= 0 && number <= 6:
                    picked.insert(Int(number))
                case .double(let number) where number >= 0 && number <= 6:
                    picked.insert(Int(number))
                default:
                    break
                }
            }
            days = picked.sorted()
        } else {
            days = PingQuietHours.allDays
        }
        let zone = pingField(json, "timezone")
        self.init(
            enabled: json["enabled"] == .bool(true),
            start: minute(json["start"], PingQuietHours.defaultStart),
            end: minute(json["end"], PingQuietHours.defaultEnd),
            days: days,
            timezone: zone.isEmpty ? "UTC" : zone)
    }

    public func toJSON() -> JSONObject {
        [
            "enabled": .bool(enabled),
            "start": .int(Int64(start)),
            "end": .int(Int64(end)),
            "days": .array(days.map { .int(Int64($0)) }),
            "timezone": .string(timezone),
        ]
    }

    /// This window with the fields a `notificationQuietHours` patch names.
    public func merge(_ patch: JSONObject) -> PingQuietHours {
        var merged = toJSON()
        merged.merge(patch)
        return PingQuietHours(json: .object(merged))
    }
}

/// One platform /status monitor's alert delivery (administrators only).
public struct StatusMonitorAlert: Equatable, Hashable, Sendable, Identifiable {
    public let id: String
    public let label: String
    public let pings: Bool
    public let emails: Bool
    /// Repeat a still-down alert every five minutes.
    public let continual: Bool

    public init(id: String, label: String, pings: Bool = false, emails: Bool = false, continual: Bool = false) {
        self.id = id
        self.label = label
        self.pings = pings
        self.emails = emails
        self.continual = continual
    }

    public init(json: JSONObject) {
        let id = pingField(json, "id")
        let label = pingField(json, "label")
        self.init(
            id: id,
            label: label.isEmpty ? id : label,
            pings: json["pings"] == .bool(true),
            emails: json["emails"] == .bool(true),
            continual: json["continual"] == .bool(true))
    }

    public func merge(_ patch: JSONObject) -> StatusMonitorAlert {
        StatusMonitorAlert(
            id: id,
            label: label,
            pings: patch["pings"]?.boolValue ?? pings,
            emails: patch["emails"]?.boolValue ?? emails,
            continual: patch["continual"]?.boolValue ?? continual)
    }
}

/// Everything the Pings settings page shows.
public struct PingSettings: Equatable, Sendable {
    /// Master email switch.
    public let emailNotifications: Bool
    /// Email switch per emailable kind, plus `general_chat` (the #general chat digest, which is
    /// not a ping kind).
    public let emailPreferences: [String: Bool]
    /// `digest` or `instant` email pacing.
    public let delivery: String
    /// Millisecond end of a one-off pause, 0 when none.
    public let pauseUntil: Int64
    /// Ping kinds switched off entirely.
    public let mutedKinds: Set<String>
    public let quietHours: PingQuietHours
    /// Millisecond end of the quiet window in force right now, 0 when none.
    public let quietUntil: Int64
    public let catalog: [PingKindGroup]
    /// Nil for anyone who is not an administrator.
    public let statusMonitorAlerts: [StatusMonitorAlert]?
    /// A ping (and phone push) whenever code is pushed to a repository you own or follow.
    /// `codePushes` in the ping settings object.
    public let codePushes: Bool

    public init(
        emailNotifications: Bool = true,
        emailPreferences: [String: Bool] = [:],
        delivery: String = "digest",
        pauseUntil: Int64 = 0,
        mutedKinds: Set<String> = [],
        quietHours: PingQuietHours = PingQuietHours(),
        quietUntil: Int64 = 0,
        catalog: [PingKindGroup] = PingSettings.fallbackCatalog,
        statusMonitorAlerts: [StatusMonitorAlert]? = nil,
        codePushes: Bool = true
    ) {
        self.emailNotifications = emailNotifications
        self.emailPreferences = emailPreferences
        self.delivery = delivery
        self.pauseUntil = pauseUntil
        self.mutedKinds = mutedKinds
        self.quietHours = quietHours
        self.quietUntil = quietUntil
        self.catalog = catalog
        self.statusMonitorAlerts = statusMonitorAlerts
        self.codePushes = codePushes
    }

    private static func millis(_ value: JSONValue?) -> Int64 {
        switch value {
        case .some(.int(let number)): return number
        case .some(.double(let number)) where number.isFinite: return Int64(number)
        case .some(let other): return Int64(other.text.dartTrimmed) ?? 0
        case .none: return 0
        }
    }

    public init(json: JSONObject) {
        var prefs: [String: Bool] = [:]
        if let raw = json["notificationPreferences"]?.objectValue {
            for (key, value) in raw {
                if case .bool(let flag) = value { prefs[key] = flag }
            }
        }
        var muted = Set<String>()
        for kind in json["pingMutedKinds"]?.arrayValue ?? [] {
            let clean = kind.text.dartTrimmed
            if !clean.isEmpty { muted.insert(clean) }
        }
        let catalog = (json["pingKindCatalog"]?.arrayValue ?? [])
            .compactMap { $0.objectValue }
            .map { PingKindGroup(json: $0) }
            .filter { !$0.kinds.isEmpty }
        var monitors: [StatusMonitorAlert]?
        if let raw = json["statusMonitorAlerts"]?.arrayValue {
            monitors = raw.compactMap { $0.objectValue }
                .map { StatusMonitorAlert(json: $0) }
                .filter { !$0.id.isEmpty }
        }
        let delivery = pingField(json, "notificationDelivery")
        self.init(
            emailNotifications: json["emailNotifications"] != .bool(false),
            emailPreferences: prefs,
            delivery: delivery == "instant" ? "instant" : "digest",
            pauseUntil: PingSettings.millis(json["notificationPauseUntil"]),
            mutedKinds: muted,
            quietHours: PingQuietHours(json: json["notificationQuietHours"]),
            quietUntil: PingSettings.millis(json["notificationQuietUntil"]),
            catalog: catalog.isEmpty ? PingSettings.fallbackCatalog : catalog,
            statusMonitorAlerts: monitors,
            // Absent from a relay that predates code-push pings: those pings are on by default,
            // like every other kind.
            codePushes: json["codePushes"] != .bool(false))
    }

    /// Kinds that mail unless switched off; the Worker's own defaults.
    static let emailOffByDefault: Set<String> = ["host_online", "host_offline"]

    public func pingEnabled(_ kind: String) -> Bool { !mutedKinds.contains(kind) }

    public func emailEnabled(_ kind: String) -> Bool {
        emailPreferences[kind] ?? !PingSettings.emailOffByDefault.contains(kind)
    }

    public func isPaused(_ now: Date = Date()) -> Bool { pauseUntil > now.epochMillis }

    public func isQuiet(_ now: Date = Date()) -> Bool { quietUntil > now.epochMillis }

    public func option(_ kind: String) -> PingKindOption? {
        for group in catalog {
            for option in group.kinds where option.kind == kind { return option }
        }
        return nil
    }

    /// What the Worker will answer for `patch`, so a switch moves the moment it is flipped. The
    /// answer then replaces this guess.
    public func applyPatch(_ patch: JSONObject) -> PingSettings {
        var muted = mutedKinds
        if let mutedPatch = patch["pingMutedKinds"]?.objectValue {
            for (kind, value) in mutedPatch {
                if value == .bool(true) {
                    muted.remove(kind)
                } else {
                    muted.insert(kind)
                }
            }
        } else if let mutedList = patch["pingMutedKinds"]?.arrayValue {
            muted = Set(mutedList.map { $0.text })
        }
        var prefs = emailPreferences
        if let prefsPatch = patch["notificationPreferences"]?.objectValue {
            for (key, value) in prefsPatch {
                if case .bool(let flag) = value { prefs[key] = flag }
            }
        }
        var monitors = statusMonitorAlerts
        if let current = monitors, let monitorPatch = patch["statusMonitorAlerts"]?.objectValue {
            monitors = current.map { (monitor: StatusMonitorAlert) -> StatusMonitorAlert in
                if let change = monitorPatch[monitor.id]?.objectValue {
                    return monitor.merge(change)
                }
                return monitor
            }
        }
        let quietPatch = patch["notificationQuietHours"]?.objectValue
        let quiet = quietPatch.map { quietHours.merge($0) } ?? quietHours
        var pause = pauseUntil
        switch patch["notificationPauseUntil"] {
        case .some(.int(let number)): pause = number
        case .some(.double(let number)) where number.isFinite: pause = Int64(number)
        default: break
        }
        return PingSettings(
            emailNotifications: patch["emailNotifications"]?.boolValue ?? emailNotifications,
            emailPreferences: prefs,
            delivery: patch["notificationDelivery"]?.stringValue ?? delivery,
            pauseUntil: pause,
            mutedKinds: muted,
            quietHours: quiet,
            // A reshaped window is only known to be quiet once the Worker says so.
            quietUntil: quietPatch != nil && !quiet.enabled ? 0 : quietUntil,
            catalog: catalog,
            statusMonitorAlerts: monitors,
            codePushes: patch["codePushes"]?.boolValue ?? codePushes)
    }

    /// Rendered when a relay answers without a catalog (one that predates it): the Worker's groups
    /// and labels at the time of writing, admin rows left out because only the Worker knows who is
    /// an administrator.
    public static let fallbackCatalog: [PingKindGroup] = [
        PingKindGroup(key: "conversations", label: "Conversations", kinds: [
            PingKindOption(kind: "mention", label: "Mentions", email: true),
            PingKindOption(kind: "subscribed", label: "Activity on things you follow", email: true),
            PingKindOption(kind: "chat_webhook", label: "Chat webhook posts"),
            PingKindOption(kind: "channel_unread", label: "Unread channel messages", email: true),
            PingKindOption(kind: "direct_unread", label: "Unread direct messages", email: true),
            PingKindOption(kind: "calendar_invitation", label: "Calendar invitations", email: true),
            PingKindOption(kind: "calendar_reminder", label: "Calendar reminders", email: true),
            PingKindOption(kind: "calendar_rsvp", label: "Calendar replies", email: true),
        ]),
        PingKindGroup(key: "repositories", label: "Repositories", kinds: [
            PingKindOption(kind: "issue_opened", label: "New issues", email: true),
            PingKindOption(kind: "pull_submitted", label: "New pull requests", email: true),
            PingKindOption(kind: "issue_assigned", label: "Issue assignments", email: true),
            PingKindOption(kind: "pending_inbox", label: "Inbox updates", email: true),
            PingKindOption(kind: "repo_activity", label: "Pushes and merges"),
            PingKindOption(kind: "release_published", label: "Releases", email: true),
            PingKindOption(kind: "repo_shared", label: "Repositories shared with you", email: true),
            PingKindOption(kind: "security_advisory", label: "Security advisories", email: true),
            PingKindOption(kind: "mirror_request", label: "Mirror requests", email: true),
            PingKindOption(kind: "repository_hosted", label: "Imports finished hosting"),
        ]),
        PingKindGroup(key: "organizations", label: "Organizations", kinds: [
            PingKindOption(kind: "org_succession", label: "Organization succession", email: true),
            PingKindOption(kind: "organization_task_update", label: "Tasks"),
        ]),
        PingKindGroup(key: "rewards", label: "Rewards", kinds: [
            PingKindOption(kind: "bounty_funded", label: "Bounties funded", email: true),
            PingKindOption(kind: "bounty_paid", label: "Bounties paid", email: true),
            PingKindOption(kind: "pending_reward", label: "Pending rewards", email: true),
        ]),
        PingKindGroup(key: "devices", label: "Devices & apps", kinds: [
            PingKindOption(kind: "host_online", label: "Host came online", email: true),
            PingKindOption(kind: "host_offline", label: "Host went offline", email: true),
            PingKindOption(kind: "timer_finished", label: "Timers and alarms"),
            PingKindOption(kind: "desktop_ping", label: "Pings from your desktop"),
            PingKindOption(kind: "plugin_activity", label: "App package activity"),
        ]),
    ]
}
