// Native half of ForkMesh's push support: FMApi's `PushPlatform` over
// UNUserNotificationCenter, plus the notification-center delegate that routes
// taps.
//
// Deliberately not Firebase Messaging: the Worker talks to APNs directly and
// routing pushes through a third party would undo the point of a payload-free
// alert. It also carries the session across the process boundary: the
// notification service extension expands each blind alert by reading the
// account's own inbox, and it can only see the shared keychain group
// (Shared/PushCredentialStore.swift).

import FMAccount
import FMApi
import FMBase
import FMChat
import FMCrypto
import FMPush
import Foundation
import UIKit
import UserNotifications

@MainActor
final class ApplePushBridge: NSObject, PushPlatform {
    private var cachedToken: String = ""
    /// A tap the shell has not collected yet (a cold launch arrives before
    /// anything is listening).
    private var parkedOpen: PushOpen?

    /// Set once AppServices built the service; taps and tokens that arrive
    /// earlier are parked here and handed over then.
    weak var service: PushNotificationService? {
        didSet { flushEarlyEvents() }
    }

    private var earlyToken: (token: String, environment: String)?
    private var earlyError: String?

    override init() {
        super.init()
        UNUserNotificationCenter.current().delegate = self
    }

    // MARK: PushPlatform

    func permissionStatus() async throws -> String? {
        let settings: UNNotificationSettings = await UNUserNotificationCenter.current().notificationSettings()
        return ApplePushBridge.describe(settings.authorizationStatus)
    }

    func requestPermission() async throws -> Bool? {
        let granted: Bool = try await UNUserNotificationCenter.current().requestAuthorization(
            options: [.alert, .sound, .badge])
        if granted {
            // Registering is what actually produces a device token; asking for
            // permission alone never yields one.
            UIApplication.shared.registerForRemoteNotifications()
        }
        return granted
    }

    func environment() async throws -> String? {
        return ApplePushBridge.apsEnvironment()
    }

    func getToken() async throws -> String? {
        return cachedToken
    }

    func takePendingOpen() async throws -> PushOpen? {
        let open: PushOpen? = parkedOpen
        parkedOpen = nil
        return open
    }

    /// The app icon badge. The service extension sets it as each push lands;
    /// this is how the app lowers it again once pings are read or deleted.
    func setBadge(_ count: Int) async throws {
        try await UNUserNotificationCenter.current().setBadgeCount(max(0, count))
    }

    /// One keychain item per project host, plus the unkeyed "latest" item a
    /// push that names no origin falls back to (PushCredentialStore).
    func setCredentials(origin: String, account: String, session: String) async throws {
        let deviceKeyName: String = ProjectKeyPolicy.secureKey(RoomSeal.privateKeyStoreKey, in: AppProjectScope.router.scope)
        let preview: String = UserDefaults.standard.string(forKey: PushPreviewMode.preferenceKey) ?? ""
        PushCredentialStore.save(
            origin: origin, account: account, session: session, deviceKeyName: deviceKeyName, preview: preview)
    }

    func clearCredentials() async throws {
        PushCredentialStore.clear()
    }

    /// Sign-out of one project drops only that project's item.
    func clearCredentials(origin: String) async throws {
        PushCredentialStore.clear(origin: origin)
    }

    /// The app that mints the token, which is the apns-topic the relay must
    /// send it under: com.forkmesh.forkmesh, the App Store app id. Never the
    /// extension's id: this runs in the app process.
    func appBundleId() -> String {
        return Bundle.main.bundleIdentifier ?? ""
    }

    // MARK: From the AppDelegate

    func didRegister(deviceToken: Data) {
        let token: String = deviceToken.map { String(format: "%02x", $0) }.joined()
        cachedToken = token
        let environment: String = ApplePushBridge.apsEnvironment()
        // Fires on first grant and again whenever APNs rotates the token
        // (restore from backup, reinstall), which is exactly when the relay
        // must hear about it.
        guard let service = service else {
            earlyToken = (token: token, environment: environment)
            return
        }
        Task { @MainActor in
            await service.didReceiveToken(token, environment: environment)
        }
    }

    func didFailToRegister(error: Error) {
        guard let service = service else {
            earlyError = error.localizedDescription
            return
        }
        service.didFailToRegister(error.localizedDescription)
    }

    private func flushEarlyEvents() {
        guard let service = service else { return }
        if let early = earlyToken {
            earlyToken = nil
            Task { @MainActor in
                await service.didReceiveToken(early.token, environment: early.environment)
            }
        }
        if let message = earlyError {
            earlyError = nil
            service.didFailToRegister(message)
        }
    }

    // MARK: Helpers

    static func describe(_ status: UNAuthorizationStatus) -> String {
        switch status {
        case .authorized: return "authorized"
        case .provisional: return "provisional"
        case .ephemeral: return "ephemeral"
        case .denied: return "denied"
        case .notDetermined: return "notDetermined"
        @unknown default: return "unknown"
        }
    }

    /// Which APNs gateway minted this token, read from the embedded
    /// provisioning profile rather than `#if DEBUG` (a Profile build is
    /// release-compiled yet development-signed). App Store builds embed no
    /// profile and fall through to production; the Simulator also embeds none
    /// but only ever mints sandbox tokens.
    static func apsEnvironment() -> String {
        #if targetEnvironment(simulator)
        return "development"
        #else
        return apsEnvironmentFromEmbeddedProfile()
        #endif
    }

    private static func apsEnvironmentFromEmbeddedProfile() -> String {
        guard let url = Bundle.main.url(forResource: "embedded", withExtension: "mobileprovision"),
            let data = try? Data(contentsOf: url),
            let raw = String(data: data, encoding: .isoLatin1)
        else {
            return "production"
        }
        guard let range = raw.range(of: "<key>aps-environment</key>") else {
            return "production"
        }
        let tail: Substring = raw[range.upperBound...].prefix(200)
        return tail.contains("development") ? "development" : "production"
    }

    /// The routing fields of a notification's userInfo.
    fileprivate func receivedTap(_ userInfo: [AnyHashable: Any]) {
        let open: PushOpen = PushOpen(arguments: userInfo)
        if let service = service {
            service.didOpen(open)
        } else {
            parkedOpen = open
        }
    }
}

extension ApplePushBridge {
    nonisolated static func openFields(_ notification: UNNotification) -> [String: String] {
        let content: UNNotificationContent = notification.request.content
        let info: [AnyHashable: Any] = content.userInfo
        var fields: [String: String] = [:]
        for key in ["topic", "alertId", "href", "origin", "host", "kind", "repo", "sessionId", "alarm", "conversation", "channelId"] {
            if let value = info[key] as? String, !value.isEmpty {
                fields[key] = value
            }
        }
        fields["category"] = content.categoryIdentifier
        fields["title"] = content.title
        fields["subtitle"] = content.subtitle
        fields["body"] = content.body
        fields["receivedMs"] = String(Int64(notification.date.timeIntervalSince1970 * 1000))
        return fields
    }

    func deliveredOpens() async -> [PushOpen] {
        let delivered: [UNNotification] = await UNUserNotificationCenter.current().deliveredNotifications()
        return delivered.map { PushOpen(arguments: ApplePushBridge.openFields($0)) }
    }
}

extension ApplePushBridge: UNUserNotificationCenterDelegate {
    /// Show the banner even while the app is in the foreground: by then the
    /// service extension has replaced the placeholder with the real event.
    /// Room chat is the exception: a public room's socket is anonymous, so the
    /// relay cannot leave the sender's own phone out of the fan-out, and the
    /// open app's live socket has already shown the message.
    nonisolated func userNotificationCenter(
        _ center: UNUserNotificationCenter,
        willPresent notification: UNNotification,
        withCompletionHandler completionHandler: @escaping (UNNotificationPresentationOptions) -> Void
    ) {
        let topic: String? = notification.request.content.userInfo["topic"] as? String
        if topic == "chat" {
            let local: Bool = notification.request.content.userInfo["meshLocal"] != nil
            completionHandler(local ? [.banner, .sound, .list] : [])
            return
        }
        let fields: [String: String] = ApplePushBridge.openFields(notification)
        let open: PushOpen = PushOpen(arguments: fields)
        Task { @MainActor in
            if let ping = open.ping {
                Services.get(NotificationService.self)?.insertFromPush(ping)
            }
            if open.isDirectMessage, let direct = Services.get(DirectMessageService.self) {
                Task { await direct.refresh() }
            }
            DesktopAlarm.shared.ring(open)
        }
        completionHandler(open.isAlarm ? [.banner, .list] : [.banner, .sound])
    }

    /// A tap tells the shell where to go: the service extension leaves the row
    /// it drew (`alertId`, `href`) on the delivered notification, and the push
    /// itself names the conversation `topic` and the project that sent it
    /// (`origin`, or `host` from a relay that sends a bare host).
    nonisolated func userNotificationCenter(
        _ center: UNUserNotificationCenter,
        didReceive response: UNNotificationResponse,
        withCompletionHandler completionHandler: @escaping () -> Void
    ) {
        let info: [AnyHashable: Any] = response.notification.request.content.userInfo
        let action: String = response.actionIdentifier
        if action != UNNotificationDefaultActionIdentifier && action != UNNotificationDismissActionIdentifier {
            let category: String = response.notification.request.content.categoryIdentifier
            let text: String? = (response as? UNTextInputNotificationResponse)?.userText
            var copy: [String: String] = [:]
            for key in NotificationPayload.userInfoKeys {
                if let value = info[key] as? String {
                    copy[key] = value
                } else if let number = info[key] as? NSNumber {
                    copy[key] = number.stringValue
                }
            }
            let actionFields: [String: String] = copy
            Task { @MainActor in
                await NotificationActions.handle(
                    actionIdentifier: action, category: category, userInfo: actionFields, text: text)
                completionHandler()
            }
            return
        }
        let routing: [String: String] = ApplePushBridge.openFields(response.notification)
        Task { @MainActor in
            self.receivedTap(routing)
        }
        completionHandler()
    }
}
