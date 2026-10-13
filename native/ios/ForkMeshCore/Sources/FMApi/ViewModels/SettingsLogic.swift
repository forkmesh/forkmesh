// The pure logic of the Settings screen: the profile, this-device, nodes,
// sessions, push and account cards' states and text, the display-name
// adoption and the web settings link. Follows native/android
// core/account/SettingsLogic.kt.

import FMAccount
import FMBase
import Foundation

/// The account name for a session: the node name, else the email, else "".
public func sessionAccountName(_ session: AuthSession?) -> String {
    let nodeName = session?.nodeName ?? ""
    return nodeName.isEmpty ? (session?.email ?? "") : nodeName
}

/// Signed in with a real Worker session (not the offline design preview).
public func hasRealSession(_ session: AuthSession?) -> Bool {
    guard let session else { return false }
    return session.sessionKind != "preview"
}

/// The Away switch and "Set a status" row need a real signed-in account.
public func showsUserStatus(_ session: AuthSession?, guest: Bool) -> Bool { !guest && hasRealSession(session) }

/// The Display name field shows the account name while it is empty or still
/// holds the preview node's name. Returns the text to put in the field, or
/// nil to leave it alone. Nothing is saved by this.
public func adoptedDisplayName(_ fieldText: String, session: AuthSession?) -> String? {
    let accountName = sessionAccountName(session)
    if accountName.isEmpty { return nil }
    return fieldText.isEmpty || fieldText == "preview-node" ? accountName : nil
}

/// `_ProfileAccountCard`'s two lines.
public struct ProfileCardText: Equatable, Sendable {
    public let title: String
    public let subtitle: String

    public init(_ title: String, _ subtitle: String) {
        self.title = title
        self.subtitle = subtitle
    }
}

public func profileCardText(_ session: AuthSession?, guest: Bool) -> ProfileCardText {
    let preview = session?.sessionKind == "preview"
    let nodeName = session?.nodeName ?? ""
    let email = session?.email ?? ""
    let title: String
    if guest {
        title = "Guest"
    } else if preview {
        title = "Preview mode"
    } else if !nodeName.isEmpty {
        title = nodeName
    } else if !email.isEmpty {
        title = email
    } else {
        title = "Not signed in"
    }
    let subtitle: String
    if guest {
        subtitle = "Exploring read-only without an account"
    } else if preview {
        subtitle = "This is local preview data. Sign out and log in to show your Worker account."
    } else if !email.isEmpty {
        subtitle = email
    } else {
        subtitle = "No Worker account session found on this device"
    }
    return ProfileCardText(title, subtitle)
}

/// What the "This device" card shows (`_ThisDeviceCard`).
public enum ThisDeviceCard: Equatable, Sendable {
    /// Signed out or previewing.
    case signInToRegister
    /// A session that never bound this phone's key.
    case notRegistered
    /// `canServeAsNode`: the "Serve as a node" switch exists only for an
    /// owner-signing mobile node.
    case mobileNode(label: String, deviceId: String, capabilities: String, lastSeen: String, canServeAsNode: Bool)
}

public func thisDeviceCard(_ session: AuthSession?, nowMillis: Int64 = SystemClock().nowMillis) -> ThisDeviceCard {
    guard let session, session.sessionKind != "preview" else { return .signInToRegister }
    if session.deviceId.isEmpty && !session.isMobileNode { return .notRegistered }
    let device = session.thisDevice
    let trimmed = (device?.label ?? "").dartTrimmed
    let label = trimmed.isEmpty ? "This phone" : trimmed
    let lastSeen: String
    if let device, device.lastSeen > 0 {
        lastSeen = Format.relativeTime(device.lastSeen, nowMillis: nowMillis)
    } else {
        lastSeen = "now"
    }
    return .mobileNode(
        label: label,
        deviceId: shortDeviceId(session.deviceId),
        capabilities: device?.capabilitySummary ?? capabilitySummary(session.capabilities),
        lastSeen: lastSeen,
        canServeAsNode: session.isMobileNode)
}

/// `_ThisDeviceCard._capabilitySummary`: what a session without a device row may do.
public func capabilitySummary(_ capabilities: [String]) -> String {
    var parts: [String] = []
    if capabilities.contains("publish_repo") { parts.append("Publishes") }
    if capabilities.contains("owner_sign") { parts.append("signs as owner") }
    if parts.isEmpty { return "Browse only" }
    let text = parts.joined(separator: " · ")
    return text.prefix(1).uppercased() + text.dropFirst()
}

/// This device's short id, cut by UTF-16 code units.
public func shortDeviceId(_ id: String) -> String {
    let units = Array(id.utf16)
    if units.isEmpty { return "—" }
    if units.count <= 10 { return id }
    let head = String(decoding: units[0..<6], as: UTF16.self)
    let tail = String(decoding: units[(units.count - 4)...], as: UTF16.self)
    return "\(head)…\(tail)"
}

/// `_DeviceRow._shortKey`.
public func shortDeviceKey(_ key: String) -> String {
    let units = Array(key.utf16)
    if units.count <= 18 { return key }
    let head = String(decoding: units[0..<9], as: UTF16.self)
    let tail = String(decoding: units[(units.count - 6)...], as: UTF16.self)
    return "\(head)...\(tail)"
}

/// `_AccountNodesCardState._count`: "1 desktop node", "2 mobile nodes".
public func countLabel(_ n: Int, _ noun: String) -> String { "\(n) \(noun)\(n == 1 ? "" : "s")" }

/// The "Nodes on this account" card: the refreshed registry, else the login
/// payload's inventory.
public struct NodesSummary: Equatable, Sendable {
    public let devices: [AccountDevice]
    public let desktopCount: Int
    public let mobileCount: Int

    public var headline: String {
        if devices.isEmpty { return "No nodes registered yet" }
        return "\(countLabel(desktopCount, "desktop node")) · \(countLabel(mobileCount, "mobile node"))"
    }

    /// The "remote commands stay locked" note appears once a desktop node is enrolled.
    public var showsDesktopNote: Bool { desktopCount > 0 }
}

public func nodesSummary(_ refreshed: [AccountDevice]?, session: AuthSession) -> NodesSummary {
    let devices = refreshed ?? session.devices
    return NodesSummary(
        devices: devices,
        desktopCount: devices.filter { $0.isDesktopNode && !$0.isRevoked }.count,
        mobileCount: devices.filter { $0.isMobileNode && !$0.isRevoked }.count)
}

public func nodesEmptyText(loading: Bool) -> String {
    loading
        ? "Loading nodes…"
        : "Log in from the ForkMesh desktop app or another phone with this account to register more nodes."
}

public func isCurrentDevice(_ device: AccountDevice, session: AuthSession) -> Bool {
    device.current || (!session.deviceId.isEmpty && device.id == session.deviceId)
}

/// The tint of a registry row: grey when revoked, green when it signs as
/// owner, amber otherwise.
public enum DeviceTone: Equatable, Sendable {
    case offline, success, warning
}

public func deviceTone(_ device: AccountDevice) -> DeviceTone {
    if device.isRevoked { return .offline }
    return device.canOwnerSign ? .success : .warning
}

public func deviceRowTitle(_ device: AccountDevice, isCurrent: Bool) -> String {
    isCurrent ? "\(device.displayName) (this device)" : device.displayName
}

public func deviceRowDetail(_ device: AccountDevice) -> String { "\(device.kindLabel) · \(device.capabilityLabel)" }

/// "Revoked 3d ago" or "Last seen 5m ago"; nil when neither is known.
public func deviceRowTimeline(_ device: AccountDevice, nowMillis: Int64 = SystemClock().nowMillis) -> String? {
    if device.revokedAt > 0 { return "Revoked \(Format.relativeTime(device.revokedAt, nowMillis: nowMillis))" }
    if device.lastSeen > 0 { return "Last seen \(Format.relativeTime(device.lastSeen, nowMillis: nowMillis))" }
    return nil
}

/// `_webSettingsUri`: the web app's node registry for the configured relay
/// origin, dropping a port that is the default for its scheme.
public func webSettingsURL(_ serverUrl: String) -> String {
    let fallback = "https://app.forkmesh.com/dashboard/settings/nodes"
    guard let components = URLComponents(string: serverUrl.dartTrimmed),
        let host = components.host, !host.isEmpty
    else { return fallback }
    let scheme = components.scheme?.lowercased() == "ws" ? "http" : "https"
    var portText = ""
    if let port = components.port {
        let isDefault = (scheme == "http" && port == 80) || (scheme == "https" && port == 443)
        if !isDefault { portText = ":\(port)" }
    }
    return "\(scheme)://\(host.lowercased())\(portText)/dashboard/settings/nodes"
}

// MARK: Sessions card (`_AccountSessionsCard`, `_SessionRow`)

public func sessionsHeadline(_ sessions: [AccountSession]?) -> String {
    guard let sessions else { return "Signed-in sessions" }
    return "\(sessions.count) active session\(sessions.count == 1 ? "" : "s")"
}

public func sessionsEmptyText(apiAvailable: Bool, loading: Bool, loaded: Bool) -> String {
    if !apiAvailable { return "Session management is unavailable in this build." }
    if loading || !loaded { return "Loading sessions…" }
    return "No active sessions found."
}

public func sessionRowTitle(_ session: AccountSession) -> String {
    session.current ? "\(session.deviceLabel) (this app)" : session.deviceLabel
}

/// "Active 5m ago · 203.0.113.9" (only the parts that are known).
public func sessionRowMeta(_ session: AccountSession, nowMillis: Int64 = SystemClock().nowMillis) -> String {
    var meta: [String] = []
    if session.lastSeenAt > 0 {
        meta.append("Active \(Format.relativeTime(session.lastSeenAt, nowMillis: nowMillis))")
    } else if session.createdAt > 0 {
        meta.append("Signed in \(Format.relativeTime(session.createdAt, nowMillis: nowMillis))")
    }
    if !session.ipAddress.isEmpty { meta.append(session.ipAddress) }
    return meta.joined(separator: " · ")
}

/// `_SessionRow._icon`, as a Material icon name.
public func sessionRowIcon(_ session: AccountSession) -> String {
    let label = session.deviceLabel.lowercased()
    if label.contains("desktop") { return "desktop_mac_outlined" }
    if label.contains("mobile") { return "phone_iphone" }
    if label.contains("browser") { return "language" }
    return "devices_other"
}

/// The confirmation a revoke asks for.
public struct RevokeConfirmation: Equatable, Sendable {
    /// A session id, or "others".
    public let target: String
    public let title: String
    public let body: String
    public let action: String
}

public func revokeOneConfirmation(_ session: AccountSession) -> RevokeConfirmation {
    RevokeConfirmation(
        target: session.id,
        title: session.current ? "Log out of this app?" : "Log out this session?",
        body: session.current
            ? "This ends the session this app is using. You will need to sign in again here."
            : "\(session.deviceLabel) will be signed out and must sign in again to use your account.",
        action: "Log out")
}

public func revokeOthersConfirmation(count: Int) -> RevokeConfirmation {
    RevokeConfirmation(
        target: "others",
        title: "Log out all other sessions?",
        body: "Every other browser, desktop app and phone signed in to your account (\(count)) will be signed out. "
            + "This app stays signed in.",
        action: "Log out others")
}

// MARK: Push card (`_PushNotificationsCard`)

/// Only on a platform with a push bridge, and never for a guest.
public func showsPushCard(supported: Bool, guest: Bool) -> Bool { supported && !guest }

public func showsPingSettings(session: AuthSession?, guest: Bool) -> Bool { !guest && hasRealSession(session) }

public func pushPermissionLabel(_ permission: String) -> String {
    switch permission {
    case "authorized": return "Allowed"
    case "provisional": return "Allowed quietly (delivered to Notification Center)"
    case "ephemeral": return "Allowed for this App Clip session"
    case "denied": return "Turned off in iOS Settings"
    case "notDetermined": return "Not asked yet"
    default: return "Not checked yet"
    }
}

public func pushTokenLabel(token: String, permission: String, environment: String, registered: Bool) -> String {
    if token.isEmpty { return permission == "denied" ? "Not requested" : "Waiting for Apple to issue one" }
    let gateway = environment == "development" ? "APNs sandbox" : "APNs production"
    return registered ? "Registered with the relay (\(gateway))" : "Issued, not yet registered (\(gateway))"
}

public func pushRelayLabel(_ relayConfigured: Bool?) -> String {
    switch relayConfigured {
    case .some(true): return "Can send pushes"
    case .some(false): return "No APNs credentials configured"
    case .none: return "Unknown until the next registration"
    }
}

public func pushCanEnable(token: String, permission: String) -> Bool { token.isEmpty && permission != "denied" }

public func pushSendLabel(sending: Bool) -> String { sending ? "Sending…" : "Send test notification" }

// MARK: Account card

/// "Signed in as …" on the sign-out card.
public func signedInAsName(_ session: AuthSession?) -> String {
    let nodeName = session?.nodeName ?? ""
    if !nodeName.isEmpty { return nodeName }
    return session?.email ?? "Current account"
}

/// The Account section: the guest card (ways out of guest mode) or the sign-out card.
public enum AccountCard: Equatable, Sendable {
    case guest, signOut
}

public func accountCard(guest: Bool) -> AccountCard { guest ? .guest : .signOut }

/// Deleting an account needs a real Worker session.
public func showsDeleteAccount(_ session: AuthSession?) -> Bool { hasRealSession(session) }

/// The delete-account dialog's confirmation rule: nil when "DELETE" was typed.
public func deleteAccountConfirmError(_ confirmText: String) -> String? {
    confirmText.dartTrimmed != "DELETE" ? "Type DELETE to confirm account deletion." : nil
}

public func deleteAccountFailure(_ error: Error) -> String {
    if let auth = error as? AuthException { return auth.message }
    return "Could not delete account. Check your password and try again."
}

/// Every fixed sentence of the Settings screen.
public enum SettingsCopy {
    public static let room =
        "Public #channels are controller-readable so ForkBot, MCP, and webhooks can participate. Direct messages are "
        + "automatically encrypted to known participant devices beyond the controller room key. A brand-new device "
        + "cannot open protected messages sent before its first visit."
    public static let importTokens =
        "Personal access tokens used to authenticate clones when importing a repo from GitHub/GitLab (raises rate "
        + "limits). Stored locally only."
    public static let signOutTitle = "Sign out?"
    public static let signOutBody =
        "This clears the saved mobile session on this device. Your identity key, relay settings, and tokens stay local."
    public static let signedOut = "Signed out. Sign in with another account."
    public static let remoteSignedOut = "This device was signed out too."
    public static let signOutCard =
        "Sign out clears the saved session on this device. Your local identity key and relay settings stay until you "
        + "remove them."
    public static let deleteTitle = "Delete account?"
    public static let deleteBody =
        "This permanently deletes your ForkMesh account on the server. This cannot be undone."
    public static let deleteCard =
        "Permanently delete your ForkMesh account and associated server data. You will need your password."
    public static let accountDeleted = "Account deleted."
    public static let push =
        "While ForkMesh is closed, iOS wakes it with a payload-free alert for pings, issue activity and direct "
        + "messages — Apple is told an account had activity and nothing else. The phone then fills the banner in over "
        + "your signed session, so you see who acted, on which repository, and what they wrote."
    public static let pushDenied = "Turn notifications back on for ForkMesh in iOS Settings, then reopen the app."
    public static let sponsored =
        "One text-only, human-reviewed message from a ForkMesh member at the bottom of Activity. Chosen by placement "
        + "alone: nothing about you or your account is sent."
    public static let iosUdid =
        "Apple does not let apps read the UDID. Find UDID opens a ForkMesh page in Safari that installs a one-time "
        + "profile; iOS sends the UDID to that page, and its Open in ForkMesh button brings it back here. Nothing is "
        + "stored on the server."
    public static let thisPhone = "This phone publishes and signs as you. It never hosts or mirrors repositories."
    public static let sessions =
        "Browsers, desktop apps and phones logged in to your account. Log out any you don't recognise or no longer use."
    public static let desktopNote =
        "Remote commands stay locked until a desktop approval flow is added. This inventory only shows which nodes "
        + "could later run trusted work."
    public static let nodesRefreshFailed = "Could not refresh the node list. Showing the last known nodes."
    public static let sessionsLoadFailed = "Could not load your sessions. Tap refresh to try again."
    public static let sessionRevokeFailed = "Could not log that session out. Try again."
    public static let signInToRegisterTitle = "Sign in to register this phone"
    public static let signInToRegisterBody =
        "Logging in binds this phone's key to your account as a mobile node so it can publish repositories and sign "
        + "as you."
    public static let notRegisteredTitle = "Not registered as a mobile node"
    public static let notRegisteredBody =
        "This session was created without binding this phone's key. Sign out and sign in again to register it as a "
        + "mobile node that can publish and sign as you."
    public static let signInForNodesTitle = "Sign in to see your nodes"
    public static let signInForNodesBody =
        "Every desktop and mobile node bound to your account is listed here. Add or revoke nodes from the web app."

    /// The "Serve as a node" description, or the warning when the relay does
    /// not serve the node drain.
    public static func serveAsNode(supported: Bool, relayLabel: String) -> String {
        supported
            ? "Read this account's inbox and apply submissions while the app is open. Nothing runs in the background, "
                + "and a desktop node always takes priority."
            : "\(relayLabel) does not serve the node drain, so this phone cannot act as a node there. "
                + "Switch to a relay that does."
    }

    /// `_GuestAccountCard`'s body; `channel` is `guestChatChannel`.
    public static func guestAccount(channel: String) -> String {
        "Browse repositories, issues, pull requests and chat in #\(channel). Sign in to open issues, comment, "
            + "review, publish and manage organizations."
    }
}
