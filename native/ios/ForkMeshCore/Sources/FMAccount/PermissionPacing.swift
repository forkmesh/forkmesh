import FMBase
import Foundation

public struct PermissionReason: Equatable, Sendable, Identifiable {
    public let id: String
    public let icon: String
    public let title: String
    public let reason: String
    public let hint: String

    public init(id: String, icon: String, title: String, reason: String, hint: String) {
        self.id = id
        self.icon = icon
        self.title = title
        self.reason = reason
        self.hint = hint
    }
}

public enum PermissionPacing {
    public static let introKey = "permissions/intro"
    public static let nearbyKey = "permissions/nearby-opened"
    public static let pending = "pending"
    public static let seen = "seen"

    public static let title = "We only ask when it matters"
    public static let titleLines = ["We only ask", "when it matters"]
    public static let summary =
        "ForkMesh never asks for anything up front. Each permission comes up the first time a feature needs it, so you always know why."
    public static let footnote = "Say no to any of them and the rest of the app keeps working. You can change your mind in iOS Settings."

    public static let reasons: [PermissionReason] = [
        PermissionReason(
            id: "notifications", icon: "bell.badge.fill", title: "Notifications",
            reason: "After you send your first message, so you hear when someone replies.", hint: "After your first message"),
        PermissionReason(
            id: "nearby", icon: "dot.radiowaves.left.and.right", title: "Bluetooth and local network",
            reason: "When you open #mesh, to chat with people nearby without the internet.", hint: "When you open #mesh"),
        PermissionReason(
            id: "camera", icon: "video.fill", title: "Camera and microphone",
            reason: "When you start a call, record a message or dictate.", hint: "When you start a call or record"),
        PermissionReason(
            id: "photos", icon: "photo.fill", title: "Photos",
            reason: "When you attach or save a picture. Only the ones you pick.", hint: "When you attach a picture"),
        PermissionReason(
            id: "health", icon: "heart.fill", title: "Health and motion",
            reason: "Only if you turn on Pedometer, Water Intake or Sleep.", hint: "Only if you turn on a tracker"),
    ]

    public static func settle(_ prefs: KeyValueStore, freshInstall: Bool) {
        if prefs.string(forKey: introKey) != nil { return }
        prefs.set(freshInstall ? pending : seen, forKey: introKey)
        if !freshInstall { prefs.set(true, forKey: nearbyKey) }
    }

    public static func needsIntro(_ prefs: KeyValueStore) -> Bool {
        prefs.string(forKey: introKey) == pending
    }

    public static func finishIntro(_ prefs: KeyValueStore) {
        prefs.set(seen, forKey: introKey)
    }

    public static func nearbyAllowed(_ prefs: KeyValueStore) -> Bool {
        prefs.bool(forKey: nearbyKey) ?? false
    }

    public static func markNearbyOpened(_ prefs: KeyValueStore) {
        prefs.set(true, forKey: nearbyKey)
    }
}
