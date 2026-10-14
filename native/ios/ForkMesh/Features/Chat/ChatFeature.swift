// The Chat area's services, created once and registered in `Services` (RelayService,
// LocalMeshService, LocalControlService, RelaySwitchController, NodeSyncService), plus the two
// pieces of iOS glue the core cannot hold: the LocalAuthentication presence prompt and the
// relay-switch overlay.

import FMAccount
import FMApi
import FMBase
import FMChat
import FMCrypto
import FMLocalAI
import FMNet
import Foundation
import LocalAuthentication
import Network
import SwiftUI

enum ChatFeature {
    @MainActor private static var meshPathMonitor: NWPathMonitor?
    @MainActor private static var meshPathSignature: String?

    /// Builds and registers the chat services. Called by AppServices after Account and Api;
    /// everything it needs is already in `Services` (see docs/FMChat.md).
    @MainActor
    static func register() {
        ChatAvatarCache.shared.warm()
        guard let settings = Services.get(SettingsService.self),
            let auth = Services.get(AuthService.self),
            let api = Services.get(ApiService.self),
            let secure = Services.get(SecureStore.self),
            let prefs = Services.get(KeyValueStore.self),
            let http = Services.get(HTTPClient.self),
            let sockets = Services.get(WebSocketConnector.self)
        else {
            Log.error("chat", "ChatFeature.register: account services missing")
            return
        }
        let identity: DeviceIdentity = Services.get(DeviceIdentity.self) ?? ChatFeature.throwawayIdentity()
        let monitor: PerformanceMonitorService? = Services.get(PerformanceMonitorService.self)

        let relay = RelayService(
            settings: settings, identity: identity, secureStore: secure, connector: sockets, performanceMonitor: monitor)
        relay.onLogLine = { (line: String) in
            if line.lowercased().contains("failed") {
                Log.warning("relay", line)
            } else {
                Log.info("relay", line)
            }
        }
        // The key endpoint needs a session; without one the answer is known, and asking anyway
        // would send a 401 on every reconnect attempt.
        relay.roomPassphraseProvider = {
            if (auth.session?.sessionToken ?? "").isEmpty { return "" }
            return try await api.roomChatPassphrase()
        }
        relay.accountProvider = { auth.session?.nodeName ?? "" }

        let mesh = LocalMeshService(
            settings: settings, identity: identity, secureStore: secure, transport: CoreBluetoothMeshTransport(),
            historyFile: { defaultMeshHistoryFile() })
        mesh.accountProvider = { auth.session?.nodeName ?? "" }
        mesh.wakeTokenProvider = {
            if (auth.session?.sessionToken ?? "").isEmpty { return "" }
            let minted = try? await api.accountApiCall("POST", "/api/push/mesh-wake", [:])
            return minted?["token"]?.text ?? ""
        }
        mesh.wakeSender = { (tokens: [String]) in
            if (auth.session?.sessionToken ?? "").isEmpty { return }
            _ = try? await api.accountApiCall("POST", "/api/push/mesh", ["tokens": JSONValue(tokens)])
        }
        let control = LocalControlService(
            mesh: mesh, identity: identity, prefs: prefs, userName: { auth.session?.nodeName ?? "" })
        control.inferenceProvider = { Services.get(LocalInferenceService.self) }

        let hub = ChatHub(relay: relay, mesh: mesh, isGuest: { auth.isGuest })
        MeshAlerts.install(mesh: mesh, hub: hub)
        hub.onGuestBlocked = { (action: String) in
            guard let signIn = Services.get(SignInCoordinator.self) else { return }
            Task { _ = await signIn.requireAccount(action: action) }
        }

        let nodeSync = NodeSyncService(
            settings: settings, identity: identity, session: { auth.session }, http: http, connector: sockets)
        nodeSync.onNewItems = {
            if let pings = Services.get(NotificationService.self) {
                Task { await pings.refresh(silent: true) }
            }
        }
        nodeSync.onUnauthorized = { (code: String) in auth.handleUnauthorized(code: code) }

        let switcher = RelaySwitchController(settings: settings, auth: auth, api: api, relay: relay)
        switcher.onSuspend = {
            await nodeSync.stop()
            if let events = Services.get(AccountEventChannel.self) { await events.stop() }
            if let push = Services.get(PushNotificationService.self) { await push.stop() }
        }
        switcher.onResume = {
            // The new relay's session is in place by now.
            if !(auth.session?.sessionToken ?? "").isEmpty {
                if let events = Services.get(AccountEventChannel.self) { await events.restart() }
                if let push = Services.get(PushNotificationService.self) {
                    let accountKey: String = auth.session?.nodeName ?? ""
                    Task { await push.rebind(accountKey: accountKey) }
                }
            }
            nodeSync.refreshSession()
            // Carried items belong to the relay they were drained from.
            nodeSync.loadCarried()
            nodeSync.start()
            if let pings = Services.get(NotificationService.self) { Task { await pings.refresh(silent: true) } }
        }

        Services.put(relay)
        Services.put(relay, as: OwnStatusAnnouncer.self)
        Services.put(hub)
        Services.put(hub, as: ShellChatBridge.self)
        Services.put(mesh)
        Services.put(control)
        Services.put(control, as: ShellLocalDesktopBridge.self)
        Services.put(switcher)
        Services.put(nodeSync)
        Services.put(
            ChatLinkPreviewService(
                http: URLSessionHTTPClient(), store: prefs, file: ChatLinkPreviewService.defaultFile))

        UserPresence.instance.prompt = LocalAuthPresencePrompt()

        auth.addListener {
            MainActor.assumeIsolated {
                relay.identityChanged()
                nodeSync.refreshSession()
            }
        }
        settings.addListener {
            MainActor.assumeIsolated { relay.identityChanged() }
        }
        if let lifecycle = Services.get(AppLifecycle.self) {
            var meshBackgrounded = false
            lifecycle.observe { (state: AppLifecycleState) in
                relay.appLifecycleChanged(state)
                nodeSync.appLifecycleChanged(state)
                MeshAlerts.foreground = state == .resumed
                if state == .resumed && MeshAlerts.onScreen { hub.roomShown() }
                if state == .paused {
                    meshBackgrounded = true
                } else if state == .resumed {
                    let rebuild = meshBackgrounded
                    meshBackgrounded = false
                    Task {
                        if rebuild {
                            await mesh.resume()
                        } else if !mesh.running {
                            _ = await mesh.start()
                        }
                    }
                }
            }
        }
        watchMeshPath(mesh)

        // The mesh needs no account and no internet, so it starts first; then the relay, like
        // the Qt client; then the node drain (opt-in, foreground only).
        Task {
            _ = await mesh.start()
            await relay.connect()
        }
        nodeSync.loadCarried()
        nodeSync.start()
    }

    @MainActor
    private static func watchMeshPath(_ mesh: LocalMeshService) {
        let monitor: NWPathMonitor = NWPathMonitor()
        monitor.pathUpdateHandler = { (path: NWPath) in
            let signature: String =
                "\(path.status)|" + path.availableInterfaces.map { $0.name }.joined(separator: ",") + "|"
                + path.gateways.map { "\($0)" }.joined(separator: ",")
            Task { @MainActor in
                let previous: String? = ChatFeature.meshPathSignature
                ChatFeature.meshPathSignature = signature
                if previous == nil || previous == signature { return }
                await mesh.networkChanged()
            }
        }
        monitor.start(queue: DispatchQueue(label: "forkmesh.mesh.path"))
        meshPathMonitor = monitor
    }

    /// Chat needs some node identity; when the device key could not be loaded the chat runs
    /// under a key that lives only as long as this launch.
    @MainActor
    private static func throwawayIdentity() -> DeviceIdentity {
        Log.warning("chat", "device identity unavailable; chat uses a temporary key")
        return (try? Identity.fromSeed(Ed25519.generateSeed()))!
    }

    @MainActor static func hub() -> ChatHub? { Services.get(ChatHub.self) }
}

/// The platform presence prompt: Face ID / Touch ID with the passcode fallback.
@MainActor
final class LocalAuthPresencePrompt: PresencePrompt {
    func authenticate(reason: String) async -> Bool? {
        let context: LAContext = LAContext()
        var probeError: NSError?
        // deviceOwnerAuthentication = biometrics OR passcode; nil when neither is set up.
        guard context.canEvaluatePolicy(.deviceOwnerAuthentication, error: &probeError) else {
            return nil
        }
        do {
            return try await context.evaluatePolicy(.deviceOwnerAuthentication, localizedReason: reason)
        } catch let error as LAError {
            // A user cancel is a real "no"; anything else falls back to the dialog.
            if error.code == .userCancel || error.code == .systemCancel || error.code == .appCancel {
                return false
            }
            return nil
        } catch {
            return nil
        }
    }
}

/// Covers the app while a relay switch runs, with the phase line, and
/// swallows taps so nothing opens against a relay that is halfway gone. Wrap the app root in it.
struct RelaySwitchScope<Content: View>: View {
    private let content: Content

    init(@ViewBuilder content: () -> Content) {
        self.content = content()
    }

    var body: some View {
        ZStack {
            content
            RelaySwitchOverlayHost()
        }
    }
}

extension View {
    func relaySwitchScope() -> some View {
        RelaySwitchScope { self }
    }
}

private struct RelaySwitchOverlayHost: View {
    @State private var controller: RelaySwitchController? = Services.get(RelaySwitchController.self)

    var body: some View {
        if let controller = controller {
            RelaySwitchOverlay(controller: controller)
        }
    }
}

private struct RelaySwitchOverlay: View {
    @Environment(\.fmPalette) private var fm
    let controller: RelaySwitchController

    var body: some View {
        let busy: Bool = controller.covering
        ZStack {
            if busy {
                fm.bgBase.opacity(fm.dark ? 0.88 : 0.92)
                    .ignoresSafeArea()
                card
            }
        }
        .allowsHitTesting(busy)
        .animation(.easeOut(duration: 0.18), value: busy)
    }

    private var card: some View {
        VStack(spacing: 0) {
            FmCircularProgressIndicator(size: 28, strokeWidth: 2.4)
            Text(controller.overlayTitle)
                .fmText(.titleMedium)
                .multilineTextAlignment(.center)
                .padding(.top, FmSpace.x5)
            Text(controller.statusLine)
                .fmText(.bodySmall, color: fm.textSecondary)
                .multilineTextAlignment(.center)
                .padding(.top, FmSpace.x2)
                .id(controller.phase.rawValue)
        }
        .padding(FmSpace.x6)
        .accessibilityElement(children: .combine)
        .accessibilityLabel(controller.statusLine.isEmpty ? "Switching relay" : controller.statusLine)
    }
}
