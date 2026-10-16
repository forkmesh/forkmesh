// The shared collaboration copy and rules: the pending and thread subscription
// notes, a signed action's failure text, badge colours, chips, and the thread
// subscription button's signing node, label and icon. After native/android
// core/.../collab/Collaboration.kt.

import FMApi
import FMBase
import Foundation

/// Semantic colour roles; the app resolves them against the palette.
public enum CollabTone: String, Sendable, CaseIterable {
    case success, danger, accent, accentEdge, secondary, tertiary
}

/// An icon (Material icon name) and label pill, `_Chip`.
public struct CollabChip: Equatable, Hashable, Sendable {
    public let icon: String
    public let label: String

    public init(_ icon: String, _ label: String) {
        self.icon = icon
        self.label = label
    }
}

/// An inline notice sentence, and whether it is shown in the danger colour.
public struct CollabNotice: Equatable, Sendable {
    public let message: String
    public let isError: Bool

    public init(_ message: String, isError: Bool = false) {
        self.message = message
        self.isError = isError
    }
}

public enum CollabCopy {
    /// `_pendingNote`: the notice after a signed inbox write.
    public static let pendingNote = "Submitted to the inbox - pending the repo owner applying it."

    /// `_threadSubscriptionNote`.
    public static let threadSubscriptionNote =
        "Thread notifications are signed with your node key and delivered through the mainnode notification inbox."

    /// `_LoadingCard(label: 'Loading…')`.
    public static let loading = "Loading…"

    /// `_EmptyCard`'s title.
    public static let emptyTitle = "Nothing here yet"

    /// `_run`'s failure notice: "Failed: $e".
    public static func runFailure(_ error: Error) -> String {
        "Failed: \(dartErrorText(error))"
    }

    /// `_copyText`'s confirmation.
    public static func copied(_ label: String) -> String { "\(label) copied" }
}

/// `_badgeColor`: the tone of a row badge from its words.
public func collabBadgeTone(_ text: String) -> CollabTone {
    let normalized = text.lowercased()
    if normalized.contains("open") || normalized.contains("live") || normalized.contains("signed") {
        return .success
    }
    if normalized.contains("closed") || normalized.contains("offline") || normalized.contains("request") {
        return .danger
    }
    if normalized.contains("merged") { return .accent }
    return .secondary
}

/// The account behind the session, as far as signing is concerned: nil
/// fields mean no session (`auth?.session == null`).
public struct CollabAccount: Equatable, Sendable {
    public let nodeName: String?
    public let pubkey: String?
    public let canOwnerSign: Bool

    public init(nodeName: String?, pubkey: String?, canOwnerSign: Bool = false) {
        self.nodeName = nodeName
        self.pubkey = pubkey
        self.canOwnerSign = canOwnerSign
    }

    public static let signedOut = CollabAccount(nodeName: nil, pubkey: nil, canOwnerSign: false)

    /// `session.nodeName.trim()`, "" without a session: the account the
    /// Issues tab's "Assigned to me" matches.
    public var filterAccount: String { nodeName?.dartTrimmed ?? "" }
}

/// `_ThreadSubscriptionButton`.
public enum ThreadSubscription {
    public static let subscribed = "Subscribed to thread notifications."
    public static let unsubscribed = "Unsubscribed from thread notifications."
    public static let guestAction = "follow threads"

    /// `_signedAccountNode`: the account node that may sign, or "" when there
    /// is no session or identity, no node name, or the session is bound to
    /// another key.
    public static func signedAccountNode(_ account: CollabAccount, identityPublicKey: String?) -> String {
        guard let nodeName = account.nodeName, let identityPublicKey else { return "" }
        let node = nodeName.dartTrimmed.lowercased()
        if node.isEmpty { return "" }
        if account.pubkey != identityPublicKey { return "" }
        return node
    }

    public static func label(guest: Bool, canSign: Bool, subscribed: Bool) -> String {
        if guest { return "Sign in for thread alerts" }
        if !canSign { return "Thread alerts unavailable" }
        return subscribed ? "Unsubscribe thread" : "Subscribe thread"
    }

    public static func icon(guest: Bool, canSign: Bool, subscribed: Bool) -> String {
        if guest || !canSign { return "lock_outline" }
        return subscribed ? "notifications_off_outlined" : "notifications_active_outlined"
    }

    public static func toggled(_ subscribed: Bool) -> String {
        subscribed ? self.subscribed : unsubscribed
    }
}
