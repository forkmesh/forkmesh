// The startup order and the service graph, and the per-account machinery (the
// account event socket, push binding, the vault's session, the app badge and
// the lifecycle hooks). Every long-lived service is registered in FMBase's
// `Services`; the feature areas register theirs from here, in dependency
// order: Account -> Api -> Chat -> Code -> LocalAI -> Agents -> Tools.

import FMAccount
import FMApi
import FMBase
import FMCrypto
import FMNet
import FMTools
import FMVault
import Foundation
import Observation
import SwiftUI

/// Every service the app builds at startup and per account. Created once by
/// `AppBoot`.
@MainActor
final class AppServices {
    private(set) static var current: AppServices?

    let config: AppConfig
    let prefs: UserDefaultsStore
    let secure: KeychainStore
    let http: HTTPClient
    let webSockets: URLSessionWebSocketConnector
    let monitor: PerformanceMonitorService
    let frameSource: DisplayLinkFrameSource
    let settings: SettingsService
    let security: SecurityBootstrap
    let auth: AuthService
    let api: ApiService
    let inbox: InboxService?
    let notifications: NotificationService
    let pingSnapshot: PingSnapshot
    let userStatus: UserStatusService
    let accountEvents: AccountEventChannel
    let agentList: AgentListSignal
    let pushBridge: ApplePushBridge
    let push: PushNotificationService
    let vault: VaultService
    let signIn: SignInCoordinator
    let lifecycle: AppLifecycle
    /// Every destination's view, registered by the feature installers (Gate 2;
    /// docs/native/NAVIGATION.md section 3.1). Unregistered routes show the
    /// "Coming soon" placeholder that names the owning workstream.
    private(set) lazy var routes: RouteRegistry<AnyView> = RouteRegistry { route in
        AnyView(RoutePlaceholderView(route: route))
    }

    private var accountEventToken: String = ""
    private var vaultAccount: String = ""
    private var authToken: ListenerToken?
    private var badgeToken: ListenerToken?
    private var lifecycleToken: UUID?

    /// The Kit gallery and previews may want a flag here later; for now the
    /// only build switch is the design-preview login shortcut.
    static var isDebugBuild: Bool {
        #if DEBUG
        return true
        #else
        return false
        #endif
    }

    // MARK: Startup (main())

    /// The startup sequence, in order. The Keychain reads and the ML-KEM key
    /// generation of the identity bootstrap run off the main thread; the
    /// launch screen stays up until this returns.
    static func start(pushBridge: ApplePushBridge) async -> AppServices {
        if let existing = current {
            return existing
        }
        OSLogSink.install(minimumLevel: isDebugBuild ? .debug : .info)
        FontRegistration.registerBundledFonts()
        let config: AppConfig = AppConfig(infoDictionary: Bundle.main.infoDictionary)

        // Stall detection starts before anything else so a slow launch is
        // logged too; every stall goes to the unified log and
        // diagnostics/stalls.log in Application Support.
        let monitor: PerformanceMonitorService = PerformanceMonitorService(
            stallLog: StallLogFile.forDevice(build: config.build))
        let frameSource: DisplayLinkFrameSource = DisplayLinkFrameSource()
        monitor.installStallMonitors(frameSource: frameSource, lifecycleState: .resumed)

        monitor.timeSync("startup.freshInstall") {
            FlutterMigrationRunner.eraseLeftoversOfADeletedApp()
        }
        monitor.timeSync("startup.flutterMigration") {
            FlutterMigrationRunner.migrateBeforeLaunch()
        }
        let prefs: UserDefaultsStore = UserDefaultsStore()
        let secure: KeychainStore = KeychainStore()
        let settings: SettingsService = monitor.timeSync("startup.settings") {
            SettingsService.create(prefs: prefs, secureStore: secure, env: config.env)
        }
        let security: SecurityBootstrap = await monitor.track("startup.identity") {
            await Task.detached(priority: .userInitiated) {
                SecurityBootstrap.load(secureStore: secure, prefs: prefs)
            }.value
        }
        let services: AppServices = AppServices(
            config: config, prefs: prefs, secure: secure, monitor: monitor, frameSource: frameSource,
            settings: settings, security: security, pushBridge: pushBridge)
        current = services
        // Projects first: the project scope decides which storage namespace
        // every service below reads, so it goes in before the feature areas.
        services.installProjects()
        services.registerFeatures()
        services.wire()
        // The workstream hooks run after the base wiring, so each may extend
        // or replace what wire() set up (push callbacks, deep links, ...).
        services.installPush()
        services.installComposer()
        services.installAgentWatch()
        services.installWatchBridge()
        services.installHosts()
        services.installInbox()
        services.installAgentOps()
        services.installReview()
        services.installSurfaces()
        services.installChat()
        services.installCI()
        services.installPlanning()
        services.installDesktopRemote()
        services.installTeammates()
        services.installAccount()
        services.installPackages()
        services.installInsights()
        services.installCoves()
        services.installNavigation()
        FlutterMigrationRunner.retireWhenVerified(services)
        return services
    }

    private init(
        config: AppConfig,
        prefs: UserDefaultsStore,
        secure: KeychainStore,
        monitor: PerformanceMonitorService,
        frameSource: DisplayLinkFrameSource,
        settings: SettingsService,
        security: SecurityBootstrap,
        pushBridge: ApplePushBridge
    ) {
        self.config = config
        self.prefs = prefs
        self.secure = secure
        self.monitor = monitor
        self.frameSource = frameSource
        self.settings = settings
        self.security = security
        self.pushBridge = pushBridge
        self.lifecycle = AppLifecycle.shared

        // Telemetry is the Tools feature's: every request goes through its
        // recording decorator, so
        // the telemetry strip counts and logs it.
        // It reads the performance monitor from Services, so that goes in first.
        Services.put(monitor)
        let telemetry: AppTelemetry = ToolsFeature.bootstrapTelemetry()
        let http: HTTPClient = telemetry.wrap(URLSessionHTTPClient())
        self.http = http
        let sockets: URLSessionWebSocketConnector = URLSessionWebSocketConnector()
        self.webSockets = sockets

        let auth: AuthService = monitor.timeSync("startup.auth") {
            AuthService.create(
                settings: settings, identity: security.identity, http: http, performanceMonitor: monitor,
                deviceInfo: DeviceInfoProvider.current())
        }
        self.auth = auth

        let api: ApiService = ApiService(settings: settings, http: http, performanceMonitor: monitor)
        // This device's owner-encryption identity. Enrolling its public bundle
        // lets the desktop seal agent snapshots to this phone as well.
        api.ownerCrypto = security.ownerKeys
        api.deployApprovalIdentity = security.identity
        self.api = api

        if let identity = security.identity {
            inbox = InboxService(settings: settings, identity: identity, http: http, performanceMonitor: monitor)
        } else {
            inbox = nil
        }
        pingSnapshot = PingSnapshot(store: prefs)
        notifications = NotificationService(
            api: api, nodeName: { auth.session?.nodeName ?? "" }, snapshot: pingSnapshot)
        userStatus = UserStatusService(api: api, sessionToken: { auth.session?.sessionToken ?? "" })
        accountEvents = AccountEventChannel(
            api: api, sessionToken: { auth.session?.sessionToken ?? "" }, connector: sockets)
        agentList = AgentListSignal()
        push = PushNotificationService(api: api, platform: pushBridge)

        // The Password Vault stays unlocked across screens and locks itself
        // when idle, after a minute in the background, and on sign-out.
        let request: VaultRequest = { (method: String, path: String, body: JSONObject?) async throws -> JSONObject in
            do {
                return try await api.accountApiCall(method, path, body)
            } catch let error as OrgApiException {
                throw VaultError(error.code)
            }
        }
        vault = VaultService(
            request: request,
            accountProvider: { auth.session?.nodeName ?? "" },
            store: secure,
            platform: nil,
            deviceAuth: LocalAuthVaultDeviceAuth())
        signIn = SignInCoordinator(auth: auth, debugBuild: AppServices.isDebugBuild)
    }

    // MARK: Registration

    /// Account and Api first (what main() builds), then every feature area in
    /// dependency order. Views and view models find all of it through
    /// `Services.get(...)` / `Services.require(...)`.
    private func registerFeatures() {
        // Account
        Services.put(config)
        Services.put(prefs, as: KeyValueStore.self)
        Services.put(secure, as: SecureStore.self)
        Services.put(monitor)
        Services.put(settings)
        security.register()
        Services.put(auth)
        Services.put(signIn)
        Services.put(lifecycle)
        // Api
        Services.put(http, as: HTTPClient.self)
        Services.put(webSockets, as: WebSocketConnector.self)
        Services.put(api)
        if let inbox = inbox {
            Services.put(inbox)
        }
        Services.put(notifications)
        Services.put(pingSnapshot)
        Services.put(userStatus)
        Services.put(accountEvents)
        Services.put(agentList)
        Services.put(push)
        Services.put(vault)
        Services.put(AgentPromptHandoff())
        Services.put(ShellPromptTargets())

        // The feature areas, each creating and registering its own services.
        ChatFeature.register()
        CodeFeature.register()
        LocalReposFeature.register()
        LocalAIFeature.register(store: prefs, http: http)
        AgentsFeature.register()
        ToolsFeature.register()

        // The chat relay announces the member's own status with every hello
        // and presence (FMChat's RelayService is an OwnStatusAnnouncer).
        if let announcer = Services.get(OwnStatusAnnouncer.self) {
            userStatus.relay = announcer
        }
    }

    // MARK: Workstream hooks (docs/native/PARITY.md, section 3.0)

    // One hook per parity workstream. Each forwards to an installer type that
    // lives in its own file, `AppServices+<Area>.swift`, owned by exactly one
    // workstream; the workstream fills in its installer's `install(into:)`
    // there and never edits this file. (A method declared here cannot be
    // overridden from an extension of the same type, hence the separate
    // types.) Installers see everything on AppServices that is not private.
    //
    //   installProjects()    -> ProjectsInstaller    AppServices+Projects.swift    (WS1)
    //   installPush()        -> PushInstaller        AppServices+Push.swift        (WS2)
    //   installComposer()    -> ComposerInstaller    AppServices+Composer.swift    (WS3)
    //   installAgentWatch()  -> AgentWatchInstaller  AppServices+AgentWatch.swift  (WS4)
    //   installWatchBridge() -> WatchBridgeInstaller AppServices+WatchBridge.swift (watch app)
    //   installHosts()       -> HostsInstaller         AppServices+Hosts.swift (WS7)
    //   installNavigation()  -> NavigationInstaller    AppServices+Navigation.swift (WS8)
    //   installInbox()       -> InboxInstaller         AppServices+Inbox.swift (WS9)
    //   installAgentOps()    -> AgentOpsInstaller      AppServices+AgentOps.swift (WS10)
    //   installReview()      -> ReviewInstaller        AppServices+Review.swift (WS11)
    //   installSurfaces()    -> SurfacesInstaller      AppServices+Surfaces.swift (WS12)
    //   installChat()        -> ChatInstaller          AppServices+Chat.swift (WS13)
    //   installCI()          -> CIInstaller            AppServices+CI.swift (WS14)
    //   installPlanning()    -> PlanningInstaller      AppServices+Planning.swift (WS15)
    //   installDesktopRemote()-> DesktopRemoteInstaller AppServices+DesktopRemote.swift (WS16)
    //   installTeammates()   -> TeammatesInstaller     AppServices+Teammates.swift (WS17)
    //   installAccount()     -> AccountInstaller       AppServices+Account.swift (WS18)
    //   installPackages()    -> PackagesInstaller      AppServices+Packages.swift (WS19)
    //   installInsights()    -> InsightsInstaller      AppServices+Insights.swift (WS20)
    //   installCoves()       -> CovesInstaller         AppServices+Coves.swift (WS21)
    //
    // Order (see start()): installProjects() before registerFeatures(); the
    // rest after wire(), in the order listed, except installNavigation(), which
    // runs last so the shell sees every route the other installers registered.

    func installProjects() {
        ProjectsInstaller().install(into: self)
    }

    func installPush() {
        PushInstaller().install(into: self)
    }

    func installComposer() {
        ComposerInstaller().install(into: self)
    }

    func installAgentWatch() {
        AgentWatchInstaller().install(into: self)
    }

    func installWatchBridge() {
        WatchBridgeInstaller().install(into: self)
    }

    func installHosts() {
        HostsInstaller().install(into: self)
    }

    func installNavigation() {
        NavigationInstaller().install(into: self)
    }

    func installInbox() {
        InboxInstaller().install(into: self)
    }

    func installAgentOps() {
        AgentOpsInstaller().install(into: self)
    }

    func installReview() {
        ReviewInstaller().install(into: self)
    }

    func installSurfaces() {
        SurfacesInstaller().install(into: self)
    }

    func installChat() {
        ChatInstaller().install(into: self)
    }

    func installCI() {
        CIInstaller().install(into: self)
    }

    func installPlanning() {
        PlanningInstaller().install(into: self)
    }

    func installDesktopRemote() {
        DesktopRemoteInstaller().install(into: self)
    }

    func installTeammates() {
        TeammatesInstaller().install(into: self)
    }

    func installAccount() {
        AccountInstaller().install(into: self)
    }

    func installPackages() {
        PackagesInstaller().install(into: self)
    }

    func installInsights() {
        InsightsInstaller().install(into: self)
    }

    func installCoves() {
        CovesInstaller().install(into: self)
    }

    // MARK: Wiring (main() and _ForkMeshAppState.initState)

    private func wire() {
        let auth: AuthService = self.auth
        let settings: SettingsService = self.settings
        let api: ApiService = self.api

        // Let API calls carry the logged-in account's session token.
        api.sessionTokenProvider = { auth.session?.sessionToken ?? "" }
        // A 401 clears the local session; `device_revoked` also rotates the
        // local key and arms the "signed out from the web" login banner.
        api.onSessionInvalid = { (code: String) in auth.handleUnauthorized(code: code) }
        // A relay-wide route that answers 404 is a feature this relay does not
        // serve; recording it lets the screens hide that section.
        api.onFeatureMissing = { (feature: RelayFeature) in
            settings.relays.markFeatureMissing(settings.relays.activeId, feature)
        }
        // Enroll (or re-confirm) this device as an owner device whenever a
        // session is available; registration is idempotent on the relay.
        Task { await api.enrollOwnerDeviceWhenSignedIn(auth) }

        // Push: bind tokens to the signed-in account; a tap re-reads the inbox.
        push.accountKeyProvider = { auth.session?.nodeName ?? "" }
        let notifications: NotificationService = self.notifications
        push.onOpened = {
            Task { await notifications.refresh(silent: true) }
        }
        pushBridge.service = push
        seedDeliveredPings()

        // Live account push: the relay wakes the inbox the moment a
        // notification is enqueued; the inbox poll stays as the safety net.
        let agentList: AgentListSignal = self.agentList
        accountEvents.onTopic = { (topic: String) in
            // "pings" is the alert inbox; an issue comment or close moves the
            // same unread count under "issues".
            if topic == "pings" || topic == "issues" {
                Task { await notifications.refresh(silent: true) }
            }
            // A desktop pushed a changed agent-session snapshot.
            if topic == "agents" || topic == "cloud-agents" {
                agentList.ring()
            }
            if topic == "cloud-agents-live" {
                agentList.ringLive()
            }
        }
        accountEvents.onConnected = {
            Task { await notifications.refresh(silent: true, catchUp: true) }
            // A run started while the socket was down rang no bell.
            agentList.ring()
        }

        vaultAccount = auth.session?.nodeName ?? ""
        authToken = auth.addListener { [weak self] in
            MainActor.assumeIsolated {
                self?.sessionChanged()
            }
        }
        badgeToken = notifications.addListener { [weak self] in
            MainActor.assumeIsolated {
                self?.syncAppBadge()
            }
        }
        lifecycleToken = lifecycle.observe { [weak self] (state: AppLifecycleState) in
            self?.lifecycleChanged(state)
        }

        let userStatus: UserStatusService = self.userStatus
        Task { await userStatus.syncSession() }
        syncAccountEventChannel()
    }

    // MARK: Session (auth listeners)

    private func sessionChanged() {
        syncAccountEventChannel()
        syncVaultSession()
        let userStatus: UserStatusService = self.userStatus
        Task { await userStatus.syncSession() }
    }

    /// The event channel follows the session: a sign-in needs a ticket minted
    /// under the new identity, and a sign-out takes the socket down rather
    /// than reconnect with a token that now proves nobody.
    private func syncAccountEventChannel() {
        let token: String = auth.session?.sessionToken ?? ""
        if token == accountEventToken {
            return
        }
        accountEventToken = token
        let events: AccountEventChannel = accountEvents
        let push: PushNotificationService = self.push
        if token.isEmpty {
            Task {
                await events.stop()
                await push.stop()
            }
        } else {
            let account: String = auth.session?.nodeName ?? ""
            Task {
                await events.restart()
                // Binds this phone's APNs token to the account that just signed
                // in, prompting for permission the first time only.
                await push.rebind(accountKey: account)
            }
        }
    }

    /// A vault never follows a sign-in: signing out, or into another account,
    /// locks it and drops this device's quick-unlock copy.
    private func syncVaultSession() {
        let account: String = auth.session?.nodeName ?? ""
        if account == vaultAccount {
            return
        }
        vaultAccount = account
        let vault: VaultService = self.vault
        Task {
            if account.isEmpty {
                await vault.signedOut()
            } else {
                try? await vault.refresh()
            }
        }
    }

    /// The red number on the app icon follows the ping inbox's unread count.
    private func syncAppBadge() {
        if !notifications.unreadCountKnown {
            return
        }
        let count: Int = notifications.unreadCount
        let push: PushNotificationService = self.push
        Task { await push.setBadge(count) }
    }

    // MARK: Lifecycle (didChangeAppLifecycleState)

    func seedDeliveredPings() {
        let notifications: NotificationService = self.notifications
        let bridge: ApplePushBridge = pushBridge
        Task { @MainActor in
            for open in await bridge.deliveredOpens() {
                if let ping = open.ping { notifications.insertFromPush(ping) }
            }
        }
    }

    private func lifecycleChanged(_ state: AppLifecycleState) {
        monitor.appLifecycleChanged(state)
        accountEvents.appLifecycleChanged(state)
        if state == .paused || state == .hidden {
            vault.appPaused()
        }
        if state != .resumed {
            return
        }
        vault.appResumed()
        seedDeliveredPings()
        notifications.appResumed()
        let account: String = auth.session?.nodeName ?? ""
        if !account.isEmpty {
            // A registration can fail while offline, and notifications can be
            // enabled in iOS Settings while the app is backgrounded; rebinding
            // on resume retries both without another sign-in.
            let push: PushNotificationService = self.push
            let userStatus: UserStatusService = self.userStatus
            Task {
                await push.rebind(accountKey: account)
                // The status may have been changed (or lapsed) on another device.
                do {
                    try await userStatus.refreshIfStale()
                } catch {
                    Log.warning("user-status", "User status read failed: \(error)")
                }
            }
        }
    }

    // MARK: Deep links

    func openURL(_ url: URL) {
        if SurfacesFeature.openURL(url) { return }
        if url.scheme == IosUdidLink.scheme, let link = IosUdidLink.tryParse(url) {
            _ = settings.acceptIosUdidLink(link)
            return
        }
        AppNavigator.shared?.openIncoming(url)
    }
}

/// One workstream's startup hook (see "Workstream hooks" in AppServices).
/// The default does nothing; a conforming type implements `install(into:)`
/// in its own `AppServices+<Area>.swift` file.
@MainActor
protocol AppServicesInstaller {
    func install(into services: AppServices)
}

extension AppServicesInstaller {
    func install(into services: AppServices) {}
}

/// The launch sequence as an observable, so the root view can show the launch
/// background until the services exist.
@MainActor
@Observable
final class AppBoot {
    private(set) var services: AppServices?
    @ObservationIgnored private var starting: Bool = false

    func start(pushBridge: ApplePushBridge) async {
        if starting || services != nil {
            return
        }
        starting = true
        services = await AppServices.start(pushBridge: pushBridge)
    }
}
