// After native/android api/PushBinder.kt and app/.../account/PushNotificationService.kt.
//
// Registers this phone with APNs and keeps the Worker's copy of its device token current. This
// is the transport of last resort: while the app is alive, AccountEventChannel delivers account
// events over a WebSocket. APNs covers the case that socket cannot: iOS has suspended or
// terminated the process.
//
// `PushPlatform` is the native half, which the app target implements over
// UNUserNotificationCenter; it reports back through `didReceiveToken`, `didFailToRegister` and
// `didOpen`. The notification service extension reads
// `GET /api/push/alert` itself with the session this service shares through
// `setCredentials` on every successful registration and clears on sign-out.
//
// Per project (docs/native/PARITY.md row #52, WS2): one APNs token serves the whole phone, but
// each project's instance (ForkMesh, ForkMesh Next, a self-hosted one) holds its own registration
// of it and its own shared-keychain item, keyed by host. `attach(api:projectId:)` points this
// app-wide service at another project without dropping the one being left, and a tapped push from
// another project's instance is parked until the app has switched there (`onForeignProjectOpen`).
// A project id is its normalized origin (`RelayProfile.normalizeOrigin`), the same id WS1's
// `ProjectScope.id` carries.

import FMAccount
import FMBase
import Foundation
import Observation

/// The native half. Implemented in the app target over UNUserNotificationCenter and the shared
/// keychain.
@MainActor
public protocol PushPlatform: AnyObject {
    /// iOS's authorization status: "authorized", "provisional", "ephemeral", "denied",
    /// "notDetermined" (nil reads as "unknown").
    func permissionStatus() async throws -> String?
    /// Asks for alert/badge/sound permission (the system prompt appears only once) and calls
    /// `registerForRemoteNotifications`. Answers whether it is granted.
    func requestPermission() async throws -> Bool?
    /// "development" for a sandbox token (Debug builds, Simulator), else "production".
    func environment() async throws -> String?
    /// The APNs token already issued this launch (lower-case hex), or ""/nil while pending.
    func getToken() async throws -> String?
    /// A tap that cold-launched the app, parked until the shell can route it.
    func takePendingOpen() async throws -> PushOpen?
    /// Puts `count` on the app icon.
    func setBadge(_ count: Int) async throws
    /// Shares relay origin, account and session with the notification service extension.
    func setCredentials(origin: String, account: String, session: String) async throws
    func clearCredentials() async throws
    /// Drops only the project at `origin`'s shared session: the extension keeps expanding banners
    /// for the projects this phone is still signed in to.
    func clearCredentials(origin: String) async throws
    /// The bundle id of the app that mints this phone's APNs token. The relay sends each token
    /// under its own app's apns-topic (`apns_device_tokens.bundle_id`); "" leaves the relay on the
    /// App Store app's id (com.forkmesh.forkmesh), which is wrong for any other build.
    func appBundleId() -> String
}

extension PushPlatform {
    public func clearCredentials(origin: String) async throws { try await clearCredentials() }
    public func appBundleId() -> String { "" }
}

@MainActor
@Observable
public final class PushNotificationService {
    @ObservationIgnored public private(set) var api: ApiService
    @ObservationIgnored let platform: PushPlatform?
    /// The project whose instance this service registers with (its normalized origin), or "" when
    /// not attached to one; see `attach(api:projectId:)`.
    public private(set) var projectId: String
    @ObservationIgnored public let listeners = Listeners()

    /// Push is iOS-only (Android would need FCM). False without a platform.
    public let supported: Bool

    public private(set) var token = ""
    /// `development` for a token minted against the APNs sandbox, `production` otherwise.
    public private(set) var environment = ""
    /// iOS's answer the last time permission was checked, or `unknown` before the first check.
    public private(set) var permission = "unknown"
    /// Whether the relay accepted this phone's current token for the current account.
    public private(set) var registered = false
    /// Whether the relay said it holds APNs provider credentials; nil until a registration or
    /// test answers, or on a relay that predates the field.
    public private(set) var relayConfigured: Bool?
    /// Last error for the settings screen (APNs refused a token, or the relay refused the
    /// registration). Empty when healthy.
    public var lastError = ""
    /// The most recent push tap no screen has acted on yet (see `takePendingOpen`).
    public private(set) var pendingOpen: PushOpen?
    /// A tap from another project's instance, held while the app switches there; it becomes
    /// `pendingOpen` once `onForeignProjectOpen` returns.
    public private(set) var awaitingProjectOpen: PushOpen?

    /// Fired when the user taps a push, so the inbox is re-read straight away.
    @ObservationIgnored public var onOpened: (@MainActor () -> Void)?
    /// The account this service binds tokens to (the session's node name), so a token arriving
    /// asynchronously from APNs still lands on the right one.
    @ObservationIgnored public var accountKeyProvider: (@MainActor () -> String)?
    /// The project the app shows now (defaults to `projectId`).
    @ObservationIgnored public var activeProjectProvider: (@MainActor () -> String)?
    /// Whether this phone knows the project a push names; a push from an unknown one opens in the
    /// active project.
    @ObservationIgnored public var knowsProject: (@MainActor (String) -> Bool)?
    /// Switch to the project a tapped push came from. The tap is delivered as `pendingOpen` when
    /// this returns, so the new project's shell routes it. Nil opens every tap where the app is.
    @ObservationIgnored public var onForeignProjectOpen: (@MainActor (PushOpen) async -> Void)?

    @ObservationIgnored private var registeredFor = ""
    @ObservationIgnored private var publishingFor = ""
    /// Set by `beginProjectSwitch`: the next `stop` is the relay switch stepping down, not a
    /// sign-out, so it must not unregister the project being left or drop its shared session.
    @ObservationIgnored private var leavingForSwitch = false
    @ObservationIgnored private var started = false
    @ObservationIgnored private var disposed = false

    /// `platform` nil (or `supported: false`) leaves the service inert.
    public init(api: ApiService, platform: PushPlatform?, supported: Bool? = nil, projectId: String = "") {
        self.api = api
        self.platform = platform
        self.supported = (supported ?? true) && platform != nil
        self.projectId = projectId
    }

    /// Point this app-wide service at another project's instance.
    ///
    /// The project being left keeps its registration and its shared session: its instance should
    /// go on reaching this phone. Only a sign-out (`stop`) drops a registration. Nothing is sent
    /// here; the new project's session binds through `rebind` as usual.
    public func attach(api: ApiService, projectId: String) {
        if api === self.api && projectId == self.projectId { return }
        self.api = api
        self.projectId = projectId
        started = false
        registered = false
        relayConfigured = nil
        lastError = ""
        notify()
    }

    @discardableResult
    public func addListener(_ listener: @escaping @Sendable () -> Void) -> ListenerToken { listeners.add(listener) }

    public func removeListener(_ token: ListenerToken) { listeners.remove(token) }

    // MARK: Taps

    public func takePendingOpen() -> PushOpen? {
        let open = pendingOpen
        pendingOpen = nil
        return open
    }

    /// Collect a tap that cold-launched the app. Call once the home shell can route it.
    public func collectLaunchOpen() async {
        guard supported, let platform else { return }
        do {
            if let parked = try await platform.takePendingOpen() { didOpen(parked) }
        } catch {
            log("could not collect a launch tap: \(error)")
        }
    }

    /// A push was tapped (the native `onOpened`). A tap from another known project's instance
    /// switches there first (`onForeignProjectOpen`), then opens.
    public func didOpen(_ open: PushOpen) {
        if let follow = onForeignProjectOpen, belongsToAnotherProject(open) {
            awaitingProjectOpen = open
            log("push opened for project \(open.projectId); switching there first")
            notify()
            Task { @MainActor in
                await follow(open)
                self.releaseProjectOpen()
            }
            return
        }
        deliverOpen(open)
    }

    /// Hand a tap parked for a project switch to the shell, whether or not the switch succeeded:
    /// a tap that cannot reach its project still opens the inbox where the app is.
    public func releaseProjectOpen() {
        guard let open = awaitingProjectOpen else { return }
        awaitingProjectOpen = nil
        deliverOpen(open)
    }

    /// Whether `open` names a project other than the active one that this phone knows.
    public func belongsToAnotherProject(_ open: PushOpen) -> Bool {
        let active = activeProjectProvider?() ?? projectId
        if open.projectId.isEmpty || open.isFor(active) { return false }
        return knowsProject?(open.projectId) ?? false
    }

    private func deliverOpen(_ open: PushOpen) {
        pendingOpen = open
        log("push opened: \(open)")
        onOpened?()
        notify()
    }

    // MARK: Registration

    /// Ask for permission (first launch) and bind the resulting token to the signed-in account.
    /// Safe to call on every sign-in.
    public func start(accountKey: String) async {
        guard supported, let platform else { return }
        started = true
        do {
            let status = try await platform.permissionStatus() ?? "unknown"
            permission = status
            if status == "notDetermined" {
                let granted = try await platform.requestPermission() ?? false
                permission = granted ? "authorized" : "denied"
                if !granted {
                    log("permission declined at the system prompt")
                    notify()
                    return
                }
            } else if status == "denied" {
                // Nothing to do until the user turns it back on in Settings.
                log("permission denied in iOS Settings; not registering")
                notify()
                return
            } else {
                // Already authorized: registering again makes iOS hand back the token.
                _ = try await platform.requestPermission()
            }
            environment = try await platform.environment() ?? "production"
            let cached = try await platform.getToken() ?? ""
            log("permission=\(permission) environment=\(environment) token=\(cached.isEmpty ? "pending from APNs" : "cached")")
            if !cached.isEmpty {
                token = cached
                await publish(accountKey)
            }
        } catch {
            lastError = "\(error)"
            log("native call failed: \(lastError)")
        }
        notify()
    }

    /// The project switch is about to stop the per-account machinery (RelaySwitchController's
    /// `onSuspend`). The project being left keeps its registration and its extension session, so
    /// its instance goes on reaching this phone; the next `stop` only steps down locally.
    public func beginProjectSwitch() {
        leavingForSwitch = true
    }

    /// The switch moved to `projectId` (its `onResume`, before the new session rebinds).
    public func finishProjectSwitch(api: ApiService, projectId: String) {
        leavingForSwitch = false
        attach(api: api, projectId: projectId)
        // Same ApiService re-pointed at the new relay: attach saw no change, but the fingerprint
        // (which names the origin) makes the rebind post to the new instance.
        started = false
        registered = false
        publishingFor = ""
        relayConfigured = nil
    }

    /// Drop this phone's registration (sign-out): badge to 0, shared session cleared, token
    /// unregistered at the relay.
    public func stop() async {
        started = false
        let current = token
        registeredFor = ""
        publishingFor = ""
        registered = false
        relayConfigured = nil
        if leavingForSwitch {
            // A relay switch, not a sign-out (see beginProjectSwitch). One-shot, so a switch that
            // fails half way cannot turn a later sign-out into a no-op.
            leavingForSwitch = false
            log("stepping down for a project switch; the project left keeps its registration")
            notify()
            return
        }
        notify()
        // A signed-out handset has no inbox, so nothing on the icon is unread.
        let api = self.api
        await setBadge(0)
        await clearCredentials(origin: api.gitOrigin)
        if current.isEmpty { return }
        await api.unregisterPushDevice(current)
    }

    private func publish(_ accountKey: String) async {
        if token.isEmpty || accountKey.isEmpty { return }
        // Refreshed on every rebind: a re-signed session rotates what the extension presents.
        await shareCredentials(accountKey)
        // Re-POST whenever the token or the account changes. The origin is part of it: the same
        // account name on two projects is two registrations, on two instances.
        let api = self.api
        let fingerprint = "\(api.gitOrigin)|\(accountKey):\(token):\(environment)"
        if fingerprint == registeredFor || fingerprint == publishingFor { return }
        publishingFor = fingerprint
        let result = await api.registerPushDevice(
            deviceToken: token, environment: environment.isEmpty ? "production" : environment,
            bundleId: platform?.appBundleId() ?? "")
        if publishingFor == fingerprint { publishingFor = "" }
        // A project switch while the POST was in flight: the answer is about the project that was
        // left, and says nothing about this one.
        if api !== self.api { return }
        if result.ok {
            registeredFor = fingerprint
            registered = true
            relayConfigured = result.relayConfigured
            lastError = ""
            let can = result.relayConfigured.map { "\($0)" } ?? "unknown (older relay)"
            log("registered a \(environment) token with the relay; relay can push: \(can)")
        } else {
            registered = false
            lastError = result.statusCode == 0
                ? "The relay could not be reached to register this phone."
                : "The relay refused the registration (HTTP \(result.statusCode))."
            log(lastError)
        }
        notify()
    }

    /// Re-bind after a sign-in or account switch, without re-prompting.
    public func rebind(accountKey: String) async {
        guard supported else { return }
        if token.isEmpty {
            await start(accountKey: accountKey)
            return
        }
        // Bound again (after a project switch, say): a token APNs rotates from here on must reach
        // this project too.
        started = true
        await publish(accountKey)
    }

    /// The settings screen's "Enable notifications".
    public func enable() async {
        await rebind(accountKey: accountKeyProvider?() ?? "")
    }

    /// Ask the relay to send one real alert to every device registered for this account.
    public func sendTest() async -> PushTestResult {
        let result = await api.sendTestPush()
        if let configured = result.configured { relayConfigured = configured }
        log("test push: HTTP \(result.statusCode) — \(result.summary)")
        notify()
        return result
    }

    /// Put `count` (at least 0) on the app icon. Deliberately not deduplicated.
    public func setBadge(_ count: Int) async {
        guard supported, let platform else { return }
        do {
            try await platform.setBadge(max(0, count))
        } catch {
            log("could not set the app badge: \(error)")
        }
    }

    // MARK: Native callbacks

    /// APNs issued (or rotated) a token (the native `onToken`).
    public func didReceiveToken(_ token: String, environment: String? = nil) async {
        self.token = token
        if let environment { self.environment = environment }
        lastError = ""
        log("APNs issued a \(self.environment) token")
        if started { await publish(accountKeyProvider?() ?? "") }
        notify()
    }

    /// APNs refused to issue a token (the native `onError`).
    public func didFailToRegister(_ message: String) {
        lastError = message
        log("APNs registration failed: \(lastError)")
        notify()
    }

    // MARK: Extension credentials

    private func shareCredentials(_ accountKey: String) async {
        let session = api.sessionTokenProvider?() ?? ""
        if accountKey.isEmpty || session.isEmpty {
            await clearCredentials(origin: api.gitOrigin)
            return
        }
        guard let platform else { return }
        do {
            try await platform.setCredentials(origin: api.gitOrigin, account: accountKey, session: session)
            log("shared the session with the notification extension")
        } catch {
            log("could not share the session: \(error)")
        }
    }

    /// Only this project's copy: the extension keeps expanding banners for the projects this phone
    /// is still signed in to.
    private func clearCredentials(origin: String) async {
        guard let platform else { return }
        try? await platform.clearCredentials(origin: origin)
    }

    private func notify() {
        if disposed { return }
        listeners.notify()
    }

    /// Debug trace of the registration flow; never carries the token itself.
    private func log(_ message: String) { Log.debug("push", message) }

    public func dispose() { disposed = true }
}

/// Where a tapped push should take the user. The notification service extension adds the inbox
/// row it drew (`alertId`, deep link `href`) to the delivered notification; any field may be
/// missing, and the app then opens the inbox.
public struct PushOpen: Equatable, Hashable, Sendable, CustomStringConvertible {
    /// `chat` or `direct-messages` for a sealed conversation, else empty.
    public let topic: String
    /// The id of the inbox notification the banner showed.
    public let alertId: String
    /// Site-relative path of the record the banner was about (only ever a same-site path).
    public let href: String
    /// The instance that sent the push, as the payload named it (`origin`, or `host` from a relay
    /// that sends a bare host). Empty from a relay that names neither; the tap then opens in the
    /// active project.
    public let origin: String
    public let agent: String
    public let title: String
    public let subtitle: String
    public let body: String
    public let kind: String
    public let repo: String
    public let receivedMs: Int
    public let alarm: String
    public let conversation: String
    public let channelId: String

    public init(
        topic: String = "", alertId: String = "", href: String = "", origin: String = "", agent: String = "",
        title: String = "", subtitle: String = "", body: String = "", kind: String = "", repo: String = "",
        receivedMs: Int = 0, alarm: String = "", conversation: String = "", channelId: String = ""
    ) {
        self.topic = topic
        self.alertId = alertId
        self.href = href
        self.origin = origin
        self.agent = agent
        self.title = title
        self.subtitle = subtitle
        self.body = body
        self.kind = kind
        self.repo = repo
        self.receivedMs = receivedMs
        self.alarm = PushOpen.alarms.contains(alarm) ? alarm : ""
        self.conversation = conversation
        self.channelId = channelId
    }

    public static let alarms: Set<String> = ["power"]

    /// The tapped notification's `userInfo` (or the bridge's map). Values print with string
    /// interpolation, trimmed; an href that is not a same-site path is dropped.
    public init(arguments: [AnyHashable: Any]) {
        func field(_ key: String) -> String {
            guard let value = arguments[AnyHashable(key)], !(value is NSNull) else { return "" }
            return "\(value)".dartTrimmed
        }
        let href = field("href")
        let origin = field("origin")
        let category = NotificationCategoryId(rawValue: field("category"))
        let opensAgent = category == .agentFinished || category == .agentFailed
        self.init(
            topic: field("topic"), alertId: field("alertId"),
            href: href.hasPrefix("/") && !href.hasPrefix("//") ? href : "",
            origin: origin.isEmpty ? field("host") : origin, agent: opensAgent ? field("sessionId") : "",
            title: field("title"), subtitle: field("subtitle"), body: field("body"), kind: field("kind"),
            repo: field("repo"), receivedMs: DartCore.intTryParse(field("receivedMs")) ?? 0,
            alarm: field("alarm"), conversation: field("conversation"), channelId: field("channelId"))
    }

    public var ping: ForkNotification? {
        if alertId.isEmpty || title.isEmpty || isChat || isDirectMessage { return nil }
        var subtitleRepo = subtitle
        var actor = ""
        if let range = subtitle.range(of: " · by ", options: .backwards) {
            subtitleRepo = String(subtitle[..<range.lowerBound])
            actor = String(subtitle[range.upperBound...])
        } else if subtitle.hasPrefix("by ") {
            subtitleRepo = ""
            actor = String(subtitle.dropFirst(3))
        }
        return ForkNotification(
            id: alertId, kind: kind.isEmpty ? "pending_inbox" : kind, title: title, body: body,
            repo: repo.isEmpty ? subtitleRepo.dartTrimmed : repo, href: href, actor: actor.dartTrimmed, ts: receivedMs)
    }

    public var isChat: Bool { topic == "chat" }
    public var isAlarm: Bool { !alarm.isEmpty }
    public var isDirectMessage: Bool { topic == "direct-messages" }
    /// A push about code pushed to a repository (topic `code-push`). Its href names the
    /// repository's commits on the pushed branch (`/owner/repo/commits/<branch>`).
    public var isCodePush: Bool { topic == "code-push" }

    /// The project `origin` belongs to (a project id is its normalized origin), or "" when the
    /// push did not say.
    public var projectId: String { origin.isEmpty ? "" : (RelayProfile.normalizeOrigin(origin) ?? "") }

    /// Whether this tap belongs to `activeProjectId`'s project.
    public func isFor(_ activeProjectId: String) -> Bool { projectId.isEmpty || projectId == activeProjectId }

    /// The repository screen the href names, if it names one.
    public var deepLink: NotificationDeepLink? { href.isEmpty ? nil : NotificationDeepLink.parse(href) }

    public var description: String {
        "PushOpen(topic: \(topic), alertId: \(alertId.isEmpty ? "-" : "set"), href: \(href), "
            + "origin: \(origin.isEmpty ? "-" : origin), agent: \(agent.isEmpty ? "-" : agent), "
            + "conversation: \(conversation.isEmpty ? "-" : "set"))"
    }
}
