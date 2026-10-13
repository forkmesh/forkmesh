import FMAccount
import FMApi
import FMBase
import FMChat
import FMControl
import FMDiscord
import FMNet
import SwiftUI

@MainActor
struct ChatInstaller: AppServicesInstaller {
    func install(into services: AppServices) {
        installServices()
        registerRoutes(services)
    }

    private func installServices() {
        guard let hub = Services.get(ChatHub.self) else { return }
        let relay: RelayService = hub.relay
        hub.pings = InboxLocalPings.log
        let settings: SettingsService? = Services.get(SettingsService.self)
        let auth: AuthService? = Services.get(AuthService.self)
        let api: ApiService? = Services.get(ApiService.self)

        if let directory = ChatHistoryStore.defaultDirectory(), let settings = settings {
            hub.attachHistory(ChatHistoryStore(directory: directory)) {
                ChatHistoryCodec.scopeName(serverUrl: settings.serverUrl, room: settings.room)
            }
        }
        if let prefs = Services.get(KeyValueStore.self), let settings = settings {
            Services.put(
                ChannelGroupsStore(
                    prefs: prefs, projectId: settings.relays.activeId, serverUrl: settings.serverUrl, room: settings.room))
            hub.workspace = ChatWorkspace(store: prefs, scope: settings.relays.activeId)
        }
        if let api = api, let sockets = Services.get(WebSocketConnector.self), let secure = Services.get(SecureStore.self) {
            let office: OfficeChannelService = OfficeChannelService(
                api: api, connector: sockets, secureStore: secure, origin: { api.url("/") },
                selfId: { relay.selfId }, displayName: { auth?.session?.nodeName ?? "" })
            let direct: DirectMessageService = DirectMessageService(
                api: api, connector: sockets, secureStore: secure, origin: { api.url("/") },
                selfId: { relay.selfId }, displayName: { auth?.session?.nodeName ?? "" })
            hub.office = office
            hub.direct = direct
            installCalls(hub: hub, direct: direct, relay: relay, auth: auth)
            let live: DirectLiveEngine = installLive(hub: hub, direct: direct)
            let readMarks: ChatReadMarks = ChatReadMarks(api: api)
            hub.readMarks = readMarks
            let mesh: LocalMeshService = hub.mesh
            direct.meshSender = { (plain: JSONObject, keys: [String]) in await mesh.sendDirect(plain, to: keys) }
            mesh.onDirect = { (channel: String, keyId: String, plain: JSONObject) in
                _ = direct.acceptMeshDirect(channel, sealKeyId: keyId, plain: plain)
            }
            hub.forkbot = api
            hub.apps = api.appsClient
            Services.put(office)
            Services.put(direct)
            if !(auth?.session?.sessionToken ?? "").isEmpty {
                Task { await office.refresh() }
                Task { await direct.refresh() }
                Task { await hub.applyReadMarks() }
            }
            if let previous = Services.get(ChatAuthListener.self) {
                previous.auth?.removeListener(previous.token)
            }
            if let auth = auth {
                let token: ListenerToken = auth.addListener {
                    MainActor.assumeIsolated {
                        hub.resetAppCommands(clearReplies: (auth.session?.sessionToken ?? "").isEmpty)
                        if (auth.session?.sessionToken ?? "").isEmpty {
                            Task { await office.closeAll() }
                            Task { await direct.clear() }
                            DirectLiveMotionFeed.shared.stop()
                            live.clear()
                            readMarks.clear()
                        } else {
                            Task { await office.refresh() }
                            Task { await direct.refresh() }
                            Task { await hub.applyReadMarks() }
                        }
                    }
                }
                Services.put(ChatAuthListener(auth: auth, token: token))
            }
            Services.get(AppLifecycle.self)?.observe { (state: AppLifecycleState) in
                direct.appLifecycleChanged(state)
                if state == .hidden || state == .paused || state == .detached {
                    live.stopSharingMotion()
                    DirectLiveMotionFeed.shared.stop()
                }
                if state == .resumed && !(auth?.session?.sessionToken ?? "").isEmpty {
                    Task { await direct.refresh() }
                    Task { await hub.applyReadMarks() }
                }
            }
            if let events = Services.get(AccountEventChannel.self) {
                let previous: (@MainActor (String) -> Void)? = events.onTopic
                events.onTopic = { (topic: String) in
                    previous?(topic)
                    if topic == "direct-messages" {
                        Task { await direct.refresh() }
                    } else if topic == ChatReadMarks.topic {
                        Task { await hub.applyReadMarks() }
                    }
                }
                let previousConnected: (@MainActor () -> Void)? = events.onConnected
                events.onConnected = {
                    previousConnected?()
                    Task { await hub.applyReadMarks() }
                }
            }
        }
        _ = Services.put(
            ChatReplySender(relay: relay, extras: hub.extras, office: { hub.office }),
            as: (any NotificationReplySending).self)
        if let desktops = Services.get(DesktopControlHub.self) {
            Services.put(DiscordBridge(channel: desktops))
        }
        if let api = api, let auth = auth {
            installRetention(hub: hub, api: api, auth: auth)
        }
        hub.startRetentionPruning()
        if let prefs = Services.get(KeyValueStore.self), let auth = auth {
            ChatWelcome.schedule(hub: hub, auth: auth, announcer: ChatWelcomeAnnouncer(prefs: prefs))
        }
    }

    private func installCalls(hub: ChatHub, direct: DirectMessageService, relay: RelayService, auth: AuthService?) {
        let calls: DirectCallEngine = DirectCallEngine(
            selfId: { relay.selfId },
            accountName: { (auth?.session?.nodeName ?? "").trimmingCharacters(in: .whitespaces).lowercased() },
            partnerFor: { (conversation: String) in direct.record(for: conversation)?.otherUser ?? "" },
            sendSignal: { (conversation: String, type: String, extra: JSONObject) in
                await direct.sendCallFrame(conversation, type: type, extra: extra)
            })
        calls.peerFactory = WebCallPeerFactory()
        calls.onMissed = { (conversation: String, media: DirectCallKind) in
            Task { _ = await direct.send(conversation, text: "📞 Missed \(media.rawValue) call") }
        }
        let prefs: KeyValueStore? = Services.get(KeyValueStore.self)
        calls.setFace(prefs?.string(forKey: DirectCallEngine.faceKey) ?? "")
        calls.onFaceChanged = { (face: String) in prefs?.set(face, forKey: DirectCallEngine.faceKey) }
        direct.onCallFrame = { (plain: JSONObject, conversation: String) in
            Task { await calls.handleFrame(plain, conversation: conversation) }
        }
        hub.calls = calls
        Services.put(calls)
    }

    private func installLive(hub: ChatHub, direct: DirectMessageService) -> DirectLiveEngine {
        let live: DirectLiveEngine = direct.makeLiveEngine()
        live.onTap = { (_: String, _: DirectLiveTap) in DirectLiveHaptics.tapped() }
        live.onStroke = { (_: String, _: DirectLiveStroke) in DirectLiveHaptics.drawn() }
        hub.live = live
        Services.put(live)
        return live
    }

    private func installRetention(hub: ChatHub, api: ApiService, auth: AuthService) {
        let retention: ChatRetentionService = ChatRetentionService(
            api: api, store: Services.get(KeyValueStore.self), sessionToken: { auth.session?.sessionToken ?? "" },
            account: { auth.session?.nodeName ?? "" })
        Services.put(retention)
        hub.applyRetention(retention.retention)
        retention.addListener {
            MainActor.assumeIsolated { hub.applyRetention(retention.retention) }
        }
        auth.addListener {
            Task { @MainActor in await retention.syncSession() }
        }
        Services.get(AppLifecycle.self)?.observe { (state: AppLifecycleState) in
            if state != .resumed { return }
            hub.pruneExpired()
            Task {
                do {
                    try await retention.refresh()
                } catch {
                    Log.warning("chat-retention", "Chat retention read failed: \(error)")
                }
            }
        }
        Task { await retention.syncSession() }
    }

    private func registerRoutes(_ services: AppServices) {
        services.routes.register(.chat, by: "WS13") { _ in AnyView(ChatScreen()) }
        services.routes.register(.channel, by: "WS13") { route in
            guard case let .channel(id) = route else { return AnyView(RoutePlaceholderView(route: route)) }
            return AnyView(ChatConversationDestination(conversation: id))
        }
        services.routes.register(.dm, by: "WS13") { route in
            guard case let .dm(user) = route else { return AnyView(RoutePlaceholderView(route: route)) }
            return AnyView(ChatConversationDestination(conversation: user.hasPrefix("@") ? user : "@\(user)"))
        }
        services.routes.register(.thread, by: "WS13") { route in
            guard case let .thread(channel, id) = route, !id.isEmpty else { return AnyView(RoutePlaceholderView(route: route)) }
            return AnyView(ChatThreadScreen(channel: channel.hasPrefix("#") ? channel : "#\(channel)", rootId: id))
        }
        services.routes.register(.discord, by: "WS13") { route in
            guard case let .discord(guild, channel) = route else { return AnyView(RoutePlaceholderView(route: route)) }
            return AnyView(DiscordRouteScreen(guild: guild, channel: channel))
        }
    }
}

struct ChatConversationDestination: View {
    let conversation: String
    @State private var resolved: Bool = false

    var body: some View {
        ChatScreen()
            .onAppear {
                if resolved { return }
                resolved = true
                ChatFeature.hub()?.switchConversation(conversation)
            }
    }
}

@MainActor
enum ChatWelcome {
    static func schedule(hub: ChatHub, auth: AuthService, announcer: ChatWelcomeAnnouncer) {
        Task {
            for _ in 0..<60 {
                if hub.relay.state == .connected { break }
                try? await Task.sleep(nanoseconds: 1_000_000_000)
            }
            guard hub.relay.state == .connected, let session = auth.session, session.sessionKind != "preview" else { return }
            try? await Task.sleep(nanoseconds: 3_000_000_000)
            let account: String = session.nodeName
            let already: Bool = hub.relay.messages(ChatWelcomeAnnouncer.channel).contains {
                $0.senderName.lowercased() == account.lowercased() && $0.text == ChatWelcomeAnnouncer.text
            }
            let should: Bool = announcer.shouldAnnounce(
                account: account, isGuest: auth.isGuest, emailVerified: session.emailVerified, createdAtMs: session.createdAt,
                nowMs: Date().epochMillis, alreadyInRoom: already)
            if !should { return }
            announcer.markAnnounced(account: account)
            hub.relay.addChannel(ChatWelcomeAnnouncer.channel)
            _ = await hub.relay.sendMessage(ChatWelcomeAnnouncer.text, to: ChatWelcomeAnnouncer.channel)
        }
    }
}

@MainActor
final class ChatAuthListener {
    weak var auth: AuthService?
    let token: ListenerToken

    init(auth: AuthService, token: ListenerToken) {
        self.auth = auth
        self.token = token
    }
}
