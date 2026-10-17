// The encrypted chat room on the live relay.
//
// One WebSocket to the active relay's room. Every frame is wrapped in the relay-derived room
// cipher (RoomCrypto); everything but public #channel chat is additionally sealed device to device
// (RoomSeal) before the room key sees it. Presence goes out every minute, peers not heard from in
// three minutes drop off the roster, and a dropped socket comes back on a 1 s doubling backoff (to
// five minutes), or at once when the app returns to the foreground.
//
// The app calls `appLifecycleChanged(_:)` on scene phase changes. State is observed through
// Observation, plus `addListener` for code outside SwiftUI. Timers are tasks over an injectable
// `sleep`.

import FMAccount
import FMApi
import FMBase
import FMCrypto
import FMNet
import Foundation
import Observation

public enum RelayConnectionState: String, Sendable {
    case offline, connecting, connected
}

@MainActor
@Observable
public final class RelayService: OwnStatusAnnouncer {
    nonisolated public static let connectTimeout: TimeInterval = 15
    nonisolated public static let pingInterval: TimeInterval = 25
    /// Presence cadence and staleness window (matches the ServerNode fix: peers not heard from
    /// within the window are dropped so stale nodes don't show online).
    nonisolated public static let presenceInterval: TimeInterval = 60
    nonisolated public static let staleMs: Int64 = 180_000
    /// What every relay frame says it was sent by (`version`): the value the retired Flutter app sent.
    nonisolated public static let clientVersion = "flutter-1.0"

    @ObservationIgnored let settings: SettingsService
    @ObservationIgnored let identity: DeviceIdentity
    @ObservationIgnored let secureStore: SecureStore
    @ObservationIgnored let connector: WebSocketConnector
    @ObservationIgnored let performanceMonitor: PerformanceMonitorService?
    @ObservationIgnored let clock: Clock
    @ObservationIgnored let sleeper: @Sendable (TimeInterval) async throws -> Void
    @ObservationIgnored let platform: String

    /// Supplies the shared room-chat passphrase (fetched server-side from the relay, derived from
    /// DATA_KEY) for passphrase-free "shared" rooms. Wired to `ApiService.roomChatPassphrase` when
    /// a session exists (ChatFeature).
    @ObservationIgnored public var roomPassphraseProvider: (@MainActor () async throws -> String)?
    /// Mirrors every `log` line into the app-wide log.
    @ObservationIgnored public var onLogLine: ((String) -> Void)?
    @ObservationIgnored public var frameObserver: (@MainActor (JSONObject) -> Void)?
    @ObservationIgnored public var onMessage: (@MainActor (ChatMessage) -> Void)?
    @ObservationIgnored public var isExpired: ChatExpiry?
    @ObservationIgnored public var isReadElsewhere: (@MainActor (ChatMessage) -> Bool)?
    @ObservationIgnored public let listeners = Listeners()

    @ObservationIgnored private var sharedPassphrase = ""
    /// What the cached `sharedPassphrase` was fetched for: `<relay host>|<room>`. The relay belongs
    /// in the key: every relay serves a room called `world-general` and derives its key from its
    /// own DATA_KEY, so a room-only cache would decrypt one relay's chat with another's key.
    @ObservationIgnored private var sharedPassphraseScope = ""
    @ObservationIgnored private var openGeneration = 0

    @ObservationIgnored private var socket: WebSocketConnection?
    @ObservationIgnored private var crypto: RoomCrypto?
    // Created automatically from a private X25519 key in the device secure store. DMs and
    // protected traffic are sealed before the relay-derived room key sees them; public #channels
    // deliberately remain available to integrations.
    @ObservationIgnored private var seal: RoomSeal?
    // Sender ids already logged, so a room full of old frames not addressed to this installation
    // costs the log one line per sender device.
    @ObservationIgnored private var reportedSealKeyIds = Set<String>()
    @ObservationIgnored private var readerTask: Task<Void, Never>?
    @ObservationIgnored private var presenceTask: Task<Void, Never>?
    @ObservationIgnored private var keyReplyTask: Task<Void, Never>?
    @ObservationIgnored private var reconnectTask: Task<Void, Never>?
    @ObservationIgnored private var reconnectAttempt = 0
    @ObservationIgnored private var userStopped = false
    @ObservationIgnored private var disposed = false
    @ObservationIgnored private var outbox: [String] = []
    @ObservationIgnored private var draining = false

    public private(set) var state: RelayConnectionState = .offline
    public private(set) var channels: [String] = ["#general"]
    public private(set) var currentConversation = "#general"
    public private(set) var log: [String] = []
    /// Bumped by every change, and by the presence tick (so staleness is re-evaluated even with
    /// no inbound traffic).
    public private(set) var revision = 0
    /// Socket frames received since launch, for the telemetry strip. Counted without notifying.
    @ObservationIgnored public private(set) var framesReceived = 0

    // The signed-in member's own away flag and custom status. nil until known: hello/presence
    // frames then carry no `userStatus` at all, because an empty one would wipe the status peers
    // already hold for this account.
    private var ownStatusValue: UserStatus?
    @ObservationIgnored private var peers: [String: RelayPeer] = [:]
    private var rosterRevision = 0
    @ObservationIgnored private var liveAtTick = Set<String>()
    private var history: [String: [ChatMessage]] = [:]
    @ObservationIgnored private var seenIds = Set<String>()
    private var seenNotificationIds = Set<String>()
    private var unread: [String: Int] = [:]
    private var unseenBadge: [String: Int] = [:]

    /// `connector` is FMNet's (`URLSessionWebSocketConnector` in the app). `platform` is what
    /// frames say this device is ("ios").
    public init(
        settings: SettingsService,
        identity: DeviceIdentity,
        secureStore: SecureStore,
        connector: WebSocketConnector,
        performanceMonitor: PerformanceMonitorService? = nil,
        platform: String = AppBuild.currentPlatform,
        clock: Clock = SystemClock(),
        sleep: @escaping @Sendable (TimeInterval) async throws -> Void = { seconds in
            try await Task.sleep(nanoseconds: UInt64(max(0, seconds) * 1_000_000_000))
        }
    ) {
        self.settings = settings
        self.identity = identity
        self.secureStore = secureStore
        self.connector = connector
        self.performanceMonitor = performanceMonitor
        self.platform = platform
        self.clock = clock
        self.sleeper = sleep
    }

    @discardableResult
    public func addListener(_ listener: @escaping @Sendable () -> Void) -> ListenerToken { listeners.add(listener) }

    public func removeListener(_ token: ListenerToken) { listeners.remove(token) }

    private func notifyListeners() {
        revision &+= 1
        listeners.notify()
    }

    // MARK: Own status

    public var ownStatus: UserStatus { ownStatusValue ?? .empty }

    /// Adopts the local member's status and, when it changed, announces it at once with a
    /// presence frame instead of waiting out the presence interval.
    public func setOwnStatus(_ status: UserStatus?) {
        if status == ownStatusValue { return }
        ownStatusValue = status
        notifyListeners()
        if status != nil { Task { await self.sendPresence() } }
    }

    /// The status a live relay peer (or this device) last announced for `senderId`, or nil when
    /// that node is not live here (callers then fall back to the account directory).
    public func liveStatusFor(_ senderId: String) -> UserStatus? {
        if senderId.isEmpty { return nil }
        if senderId == nodeId { return ownStatusValue }
        _ = rosterRevision
        guard let peer = peers[senderId], peer.online else { return nil }
        if clock.nowMillis - peer.lastSeenMs > RelayService.staleMs { return nil }
        return peer.status
    }

    // MARK: Roster

    private var nodeId: String { identity.nodeId }

    private var sealAccount: String {
        let named = (accountProvider?() ?? "").trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        return named.isEmpty ? nodeId : named
    }

    /// This device's node id, as it appears in `senderId`.
    public var selfId: String { identity.nodeId }

    @ObservationIgnored public var accountProvider: (@MainActor () -> String)?

    public var persona: ChatPersona {
        ChatPersona.resolve(
            nodeKey: identity.publicKeyB64url, account: accountProvider?() ?? "", displayName: settings.displayName)
    }

    @ObservationIgnored private var announcedPersona: ChatPersona?

    public func identityChanged() {
        let now = persona
        if now == announcedPersona { return }
        announcedPersona = now
        notifyListeners()
        if state == .connected { Task { await self.sendPresence() } }
    }

    private var name: String { persona.name }

    public func roster() -> [Member] {
        _ = rosterRevision
        let now = clock.nowMillis
        let me = persona
        var list = [
            Member(
                id: nodeId, name: me.name, isSelf: true, online: state == .connected, platform: platform,
                owner: me.ownerUser, userStatus: ownStatus, timeZone: TimeZone.current.identifier)
        ]
        for (id, peer) in peers.sorted(by: { $0.value.order < $1.value.order }) {
            if !peer.online || now - peer.lastSeenMs > RelayService.staleMs { continue }
            list.append(
                Member(
                    id: id, name: peer.name, online: true, platform: peer.platform, version: peer.version,
                    mirrors: peer.mirrors, owner: peer.owner, nodeName: peer.nodeName,
                    userStatus: peer.status ?? .empty, diagnostics: peer.diagnostics, timeZone: peer.timeZone))
        }
        return list
    }

    /// Same count as `roster().filter(\.online)`, without building a Member per peer.
    public var onlineCount: Int {
        _ = rosterRevision
        let now = clock.nowMillis
        var count = state == .connected ? 1 : 0
        for peer in peers.values where peer.online && now - peer.lastSeenMs <= RelayService.staleMs {
            count += 1
        }
        return count
    }

    public func rosterGroups() -> [MemberGroup] { RelayService.groups(roster()) }

    nonisolated public static func groups(_ members: [Member]) -> [MemberGroup] {
        var order: [String] = []
        var byKey: [String: [Member]] = [:]
        let owners = Set(members.map { $0.owner.dartTrimmed.lowercased() }.filter { !$0.isEmpty })
        for member in members {
            var key = RelayService.identityKey(member)
            let named = RelayService.displayName(member).lowercased()
            if key.hasPrefix("name:") && owners.contains(named) { key = "owner:\(named)" }
            if byKey[key] == nil { order.append(key) }
            byKey[key, default: []].append(member)
        }
        var nameCounts: [String: Int] = [:]
        for key in order {
            let lower = RelayService.displayName(byKey[key]![0]).lowercased()
            nameCounts[lower, default: 0] += 1
        }
        var groups: [MemberGroup] = []
        for key in order {
            let nodes = byKey[key]!.sorted(by: RelayService.nodeOrder)
            let primary = nodes[0]
            let groupName = RelayService.displayName(primary)
            let collides = (nameCounts[groupName.lowercased()] ?? 0) > 1
            groups.append(
                MemberGroup(
                    name: groupName, members: nodes, isSelf: nodes.contains { $0.isSelf },
                    disambiguator: collides ? RelayService.shortIdentity(primary) : ""))
        }
        return groups
    }

    nonisolated static func displayName(_ m: Member) -> String {
        let owner = m.owner.dartTrimmed
        if !owner.isEmpty { return owner }
        let name = m.name.dartTrimmed
        if !name.isEmpty { return name }
        let nodeName = m.nodeName.dartTrimmed
        if !nodeName.isEmpty { return nodeName }
        return m.id
    }

    nonisolated static func identityKey(_ m: Member) -> String {
        let owner = m.owner.dartTrimmed.lowercased()
        if !owner.isEmpty { return "owner:\(owner)" }
        if m.isSelf { return "self" }
        let name = displayName(m).lowercased()
        if !name.isEmpty { return "name:\(name)" }
        return "node:\(m.id)"
    }

    nonisolated static func shortIdentity(_ m: Member) -> String {
        let raw = Array(m.id)
        if raw.count <= 10 { return String(raw) }
        return "\(String(raw[0..<4]))…\(String(raw[(raw.count - 4)...]))"
    }

    nonisolated static func nodeOrder(_ a: Member, _ b: Member) -> Bool {
        if a.isSelf != b.isSelf { return a.isSelf }
        if a.online != b.online { return a.online }
        return a.id.utf16.lexicographicallyPrecedes(b.id.utf16)
    }

    // MARK: Messages and unread

    public func messages(_ conversation: String) -> [ChatMessage] { history[conversation] ?? [] }

    public var conversationsWithHistory: [String] { history.filter { !$0.value.isEmpty }.map(\.key).sorted() }

    public var recentMessages: [ChatMessage] {
        history.values.flatMap { $0 }.sorted { $0.timestamp > $1.timestamp }
    }

    public var unreadMessages: [ChatMessage] {
        recentMessages.filter { !$0.isSelf && unread[$0.conversation] != nil }
    }

    public var notificationMessages: [ChatMessage] { recentMessages.filter { !$0.isSelf } }

    public var unseenNotificationCount: Int {
        notificationMessages.filter { !seenNotificationIds.contains($0.id) }.count
    }

    public func markNotificationsSeen() {
        let before = seenNotificationIds.count
        for message in notificationMessages where !message.id.isEmpty {
            seenNotificationIds.insert(message.id)
        }
        if seenNotificationIds.count != before { notifyListeners() }
    }

    public var unreadConversations: [String] { Array(unread.keys) }

    public func hasUnread(_ conversation: String) -> Bool { (unread[conversation] ?? 0) > 0 }

    public func unreadCountFor(_ conversation: String) -> Int { unread[conversation] ?? 0 }

    public var totalUnread: Int { unread.values.reduce(0, +) }

    public var badgeUnread: Int {
        unseenBadge.reduce(0) { $0 + min($1.value, unread[$1.key] ?? 0) }
    }

    public func markBadgeSeen() {
        if unseenBadge.isEmpty { return }
        unseenBadge.removeAll()
        notifyListeners()
    }

    public func noteUnread(_ conversation: String) {
        if conversation.isEmpty || conversation == currentConversation { return }
        unread[conversation, default: 0] += 1
        unseenBadge[conversation, default: 0] += 1
        notifyListeners()
    }

    public func switchConversation(_ conversation: String) {
        currentConversation = conversation
        unread.removeValue(forKey: conversation)
        unseenBadge.removeValue(forKey: conversation)
        notifyListeners()
    }

    public func markConversationRead(_ conversation: String) {
        unseenBadge.removeValue(forKey: conversation)
        if unread.removeValue(forKey: conversation) != nil { notifyListeners() }
    }

    public func markAllRead() {
        unseenBadge.removeAll()
        if unread.isEmpty { return }
        unread.removeAll()
        notifyListeners()
    }

    // MARK: Connection

    public func connect() async {
        userStopped = false
        reconnectTask?.cancel()
        reconnectTask = nil
        await open()
    }

    /// The app's lifecycle. Mobile OSes freeze timers and tear down the socket while backgrounded,
    /// so on resume the relay reconnects at once (instead of waiting out the backoff) and
    /// re-announces presence so this node reappears online to peers without the 60 s delay.
    public func appLifecycleChanged(_ lifecycle: AppLifecycleState) {
        if lifecycle != .resumed || userStopped || disposed { return }
        reconnectTask?.cancel()
        reconnectTask = nil
        reconnectAttempt = 0
        if state == .connected {
            Task { await self.sendPresence() }
        } else {
            Task { await self.open() }
        }
    }

    private func open() async {
        if let monitor = performanceMonitor {
            let details: JSONObject = [
                "server": .string(ServerURL.parse(settings.serverUrl).host), "room": .string(settings.room),
            ]
            await monitor.track("relay.open", details: details) { await self.openUntracked() }
            return
        }
        await openUntracked()
    }

    private func openUntracked() async {
        // Resume, retry and the backoff timer can all start an open while another is still
        // deriving keys or dialing. Only the newest may install a socket; an older one must
        // neither tear the newer down nor schedule a reconnect.
        openGeneration += 1
        let generation = openGeneration
        await teardownSocket()
        if generation != openGeneration { return }
        setState(.connecting)
        var connection: WebSocketConnection?
        do {
            let room = settings.room
            let passphrase = settings.passphrase
            if !passphrase.isEmpty {
                crypto = await RelayService.deriveRoomCrypto(room, passphrase)
            } else {
                // Shared room: fetch the server-held key once per relay and room (cached). Fail
                // closed so the app never silently falls back to a weaker key.
                let scope = "\(ServerURL.parse(settings.serverUrl).host)|\(room)"
                if sharedPassphraseScope != scope { sharedPassphrase = "" }
                if sharedPassphrase.isEmpty, let provider = roomPassphraseProvider {
                    sharedPassphrase = try await provider()
                    sharedPassphraseScope = scope
                }
                if sharedPassphrase.isEmpty {
                    throw RelayError("Sign in to join chat - room key unavailable.")
                }
                crypto = await RelayService.deriveRoomCrypto(room, sharedPassphrase)
            }
            if generation != openGeneration { return }
            // No prompt or shared phrase: the private half is generated locally once and loaded
            // from secure storage on later connections.
            let store = secureStore
            let account = sealAccount
            let node = nodeId
            let loaded = try await Task.detached(priority: .userInitiated) {
                try RoomSeal.loadOrCreate(room, scope: room, policy: .open, acct: account, node: node, secureStore: store)
            }.value
            seal = loaded
            if generation != openGeneration { return }
            reportedSealKeyIds.removeAll()
            guard let url = URL(string: settings.serverUrl) else {
                throw RelayError("Invalid relay URL: \(settings.serverUrl)")
            }
            // A dial into a dead network would otherwise wait forever with no reconnect scheduled,
            // and a socket the OS silently dropped would look connected until the next send.
            // Pings surface both as a close.
            let opened = try await connector.connect(
                url: url,
                options: WebSocketOptions(
                    connectTimeout: RelayService.connectTimeout, pingInterval: RelayService.pingInterval))
            connection = opened
            if generation != openGeneration {
                await opened.close(code: WebSocketStatus.normalClosure, reason: "")
                return
            }
            socket = opened
            outbox.removeAll()
            setState(.connected)
            reconnectAttempt = 0
            logLine("Connected to \(ServerURL.parse(settings.serverUrl).host) · room \(settings.room)")
            readerTask = Task { [weak self] in
                do {
                    for try await message in opened.messages {
                        guard let self else { return }
                        await self.onFrame(message)
                    }
                } catch {}
                self?.onSocketDone(opened)
            }
            let persistKey = loaded.shouldPersistAdvertisement(now: clock.nowMillis)
            sendKeyAdvertisement(persist: persistKey)
            await sendHello()
            startPresenceTimer()
        } catch {
            if generation != openGeneration {
                if let connection { await connection.close() }
                return
            }
            logLine("Connect failed: \(RelayService.errorText(error))")
            await teardownSocket()
            onClosed()
        }
    }

    /// PBKDF2 costs seconds on a phone: derived off the main actor, cached per process.
    nonisolated static func deriveRoomCrypto(_ room: String, _ passphrase: String) async -> RoomCrypto {
        await Task.detached(priority: .userInitiated) {
            RoomCrypto.withPassphrase(room, passphrase)
        }.value
    }

    private func startPresenceTimer() {
        presenceTask?.cancel()
        let sleeper = self.sleeper
        presenceTask = Task { [weak self] in
            while !Task.isCancelled {
                do { try await sleeper(RelayService.presenceInterval) } catch { return }
                guard let self, !Task.isCancelled else { return }
                await self.sendPresence()
                self.noteRosterStaleness()
                self.notifyListeners()  // re-evaluate staleness even with no inbound traffic
            }
        }
    }

    /// Forget everything this service learned from the relay being left: history, roster,
    /// seen-notification set and log are all one relay's content, and the room key is derived per
    /// relay. Called by RelaySwitchController while the socket is already down.
    public func resetForRelaySwitch() {
        history.removeAll()
        seenNotificationIds.removeAll()
        forgetPeers()
        log.removeAll()
        sharedPassphrase = ""
        sharedPassphraseScope = ""
        crypto = nil
        seal = nil
        reportedSealKeyIds.removeAll()
        notifyListeners()
    }

    public func disconnect() async {
        userStopped = true
        openGeneration += 1
        reconnectTask?.cancel()
        reconnectTask = nil
        presenceTask?.cancel()
        _ = send(envelopeBase("bye"))
        await flushOutbox()
        await teardownSocket()
        forgetPeers()
        setState(.offline)
    }

    /// `dispose`: stops everything for good.
    public func dispose() {
        userStopped = true
        disposed = true
        presenceTask?.cancel()
        keyReplyTask?.cancel()
        reconnectTask?.cancel()
        readerTask?.cancel()
        let current = socket
        socket = nil
        if let current { Task { await current.close() } }
    }

    private func teardownSocket() async {
        presenceTask?.cancel()
        presenceTask = nil
        keyReplyTask?.cancel()
        keyReplyTask = nil
        readerTask?.cancel()
        readerTask = nil
        let current = socket
        socket = nil
        outbox.removeAll()
        if let current { await current.close(code: WebSocketStatus.normalClosure, reason: "") }
    }

    private func onSocketDone(_ connection: WebSocketConnection) {
        guard socket === connection else { return }
        onClosed()
    }

    private func onClosed() {
        presenceTask?.cancel()
        presenceTask = nil
        keyReplyTask?.cancel()
        keyReplyTask = nil
        let current = socket
        socket = nil
        outbox.removeAll()
        if let current { Task { await current.close() } }
        if userStopped || disposed {
            setState(.offline)
            return
        }
        setState(.connecting)
        scheduleReconnect()
    }

    private func scheduleReconnect() {
        if userStopped || disposed || reconnectTask != nil { return }
        // 1 s doubling to five minutes. Every attempt can cost a room-key request as well as the
        // dial, so a relay that keeps refusing is not asked twice a minute forever; returning to
        // the app reconnects at once.
        let delay = min(max(1 << min(max(reconnectAttempt, 0), 9), 1), 300)
        reconnectAttempt += 1
        let sleeper = self.sleeper
        reconnectTask = Task { [weak self] in
            do { try await sleeper(TimeInterval(delay)) } catch { return }
            guard let self else { return }
            self.reconnectTask = nil
            if !self.userStopped && !self.disposed { await self.open() }
        }
    }

    /// The seconds the next reconnect will wait, for tests.
    var nextReconnectDelay: Int { min(max(1 << min(max(reconnectAttempt, 0), 9), 1), 300) }

    // MARK: Sending

    private func envelopeBase(_ type: String) -> JSONObject {
        let me = persona
        var m: JSONObject = [
            "type": .string(type),
            "id": .string(RelayService.makeId(nodeId, clock)),
            "senderId": .string(nodeId),
            "sender": .string(me.name),
            "ts": .int(clock.nowMillis),
            "platform": .string(platform),
            "version": .string(RelayService.clientVersion),
        ]
        me.stamp(&m)
        announcedPersona = me
        return m
    }

    /// Wraps, seals and queues one frame. False when there is no live socket or sealing failed
    /// (the latter also drops the socket).
    @discardableResult
    private func send(_ plain: JSONObject) -> Bool {
        if let monitor = performanceMonitor {
            var details: JSONObject = ["type": .string(plain["type"]?.text ?? "")]
            if let conversation = plain["conversation"], !conversation.isNull {
                details["conversation"] = .string(conversation.text)
            }
            return monitor.timeSync("relay.send.\(plain["type"]?.stringValue ?? "unknown")", details: details) {
                sendUntracked(plain)
            }
        }
        return sendUntracked(plain)
    }

    private func sendUntracked(_ plain: JSONObject) -> Bool {
        guard let crypto, socket != nil, state == .connected else { return false }
        do {
            let persist = plain["persist"]?.boolValue == true
            var payload = plain
            if persist { payload.removeValue(forKey: "persist") }
            guard let seal else { throw RelayError("End-to-end encryption unavailable.") }
            let framed: JSONObject
            if RelayService.isControllerReadableFrame(payload) {
                framed = payload
            } else if let to = RelayService.directRecipient(payload) {
                framed = try seal.seal(payload, forNodes: [to], now: clock.nowMillis)
            } else {
                framed = try seal.seal(payload, now: clock.nowMillis)
            }
            if seal.needsAdvertisement(now: clock.nowMillis) { replyWithKeySoon() }
            var env = try crypto.encrypt(framed)
            if persist {
                env["persist"] = true
                // The relay only sees ciphertext and retains key advertisements too, so a message
                // says in the clear that it is one: that is what lets the relay wake the room's
                // phones for it and for nothing else.
                let type = payload["type"]?.stringValue ?? ""
                if type == "chat" || type == "thread-reply" { env["notify"] = true }
            }
            enqueue(JSON.encode(.object(env)))
            return true
        } catch {
            logLine("Send failed: \(RelayService.errorText(error))")
            onClosed()
            return false
        }
    }

    /// Frames go out in the order they were queued.
    private func enqueue(_ text: String) {
        outbox.append(text)
        if draining { return }
        draining = true
        Task { await self.drain() }
    }

    private func drain() async {
        while !outbox.isEmpty {
            guard let connection = socket else {
                outbox.removeAll()
                break
            }
            let text = outbox.removeFirst()
            do {
                try await connection.send(text: text)
            } catch {
                if socket === connection { onClosed() }
            }
        }
        draining = false
    }

    private func flushOutbox() async {
        if outbox.isEmpty && !draining { return }
        if !draining {
            draining = true
            await drain()
            return
        }
        for _ in 0..<200 where draining {
            try? await Task.sleep(nanoseconds: 5_000_000)
        }
    }

    private func sendKeyAdvertisement(persist: Bool) {
        guard let crypto, let seal, socket != nil, state == .connected else { return }
        let bundle = seal.keyFrame(now: clock.nowMillis)
        guard !bundle.isEmpty, var envelope = try? crypto.encrypt(bundle) else { return }
        if persist { envelope["persist"] = true }
        enqueue(JSON.encode(.object(envelope)))
    }

    private func replyWithKeySoon() {
        if keyReplyTask != nil { return }
        let sleeper = self.sleeper
        keyReplyTask = Task { [weak self] in
            do { try await sleeper(0.025) } catch { return }
            guard let self else { return }
            self.keyReplyTask = nil
            self.sendKeyAdvertisement(persist: false)
        }
    }

    /// Hello and presence frames carry the sender's away flag and custom status (web and desktop
    /// do the same), once this device knows it.
    private func withOwnStatus(_ frame: JSONObject) -> JSONObject {
        var frame = frame
        if let status = ownStatusValue { frame["userStatus"] = .object(status.toJSON()) }
        frame["timeZone"] = .string(TimeZone.current.identifier)
        return frame
    }

    private func sendHello() async {
        var hello = withOwnStatus(envelopeBase("hello"))
        hello["channels"] = JSONValue(channels)
        send(hello)
    }

    private func sendPresence() async {
        if state != .connected { return }
        send(withOwnStatus(envelopeBase("presence")))
    }

    /// Sends `text` to the current conversation. The message shows at once and is taken back when
    /// the send fails.
    @discardableResult
    public func sendMessage(_ text: String) async -> Bool {
        await sendMessage(text, to: currentConversation)
    }

    @discardableResult
    public func sendMessage(_ text: String, to conversation: String) async -> Bool {
        let trimmed = text.dartTrimmed
        if trimmed.isEmpty || !(conversation.hasPrefix("#") || conversation.hasPrefix("@")) { return false }
        var msg = envelopeBase(conversation.hasPrefix("@") ? "dm" : "chat")
        msg["text"] = .string(trimmed)
        msg["conversation"] = .string(conversation)
        if conversation.hasPrefix("#") {
            msg["channel"] = .string(conversation)
            msg["persist"] = true
        } else {
            msg["to"] = .string(String(conversation.dropFirst()))
        }
        let id = msg["id"]?.stringValue ?? ""
        let ts = Date(epochMillis: msg["ts"]?.int64Value ?? clock.nowMillis)
        let appended = appendMessage(
            ChatMessage(
                id: id, conversation: conversation, senderId: nodeId, senderName: name, text: trimmed, timestamp: ts,
                isSelf: true))
        seenIds.insert(id)
        if appended { notifyListeners() }
        let sent = send(msg)
        if !sent {
            removeMessage(conversation, id)
            seenIds.remove(id)
            notifyListeners()
        }
        return sent
    }

    /// Removes one of this device's own messages for every peer whose device syncs the room.
    /// Only ever offered for a message this node sent; `handle` still checks authorship on the
    /// way in.
    @discardableResult
    public func deleteMessage(_ message: ChatMessage) async -> Bool {
        let conversation = message.conversation
        var del = envelopeBase("delete")
        del["conversation"] = .string(conversation)
        del["target"] = .string(message.id)
        if conversation.hasPrefix("#") {
            del["channel"] = .string(conversation)
            del["persist"] = true
        } else {
            del["to"] = .string(String(conversation.dropFirst()))
        }
        removeMessage(conversation, message.id)
        seenIds.insert(del["id"]?.stringValue ?? "")
        notifyListeners()
        return send(del)
    }

    public func makeFrame(_ type: String) -> JSONObject { envelopeBase(type) }

    @discardableResult
    public func sendFrame(_ frame: JSONObject) -> Bool { send(frame) }

    public func applyFrame(_ frame: JSONObject) { handle(frame) }

    public var historySnapshot: [String: [ChatMessage]] { history }

    public func restoreHistory(_ messages: [ChatMessage]) {
        var changed = false
        for message in messages where !message.id.isEmpty && !seenIds.contains(message.id) {
            if isExpired?(message.conversation, message.timestamp) == true { continue }
            seenIds.insert(message.id)
            var list = history[message.conversation] ?? []
            var at = list.count
            while at > 0 && RelayService.compareMessages(list[at - 1], message) > 0 { at -= 1 }
            list.insert(message, at: at)
            history[message.conversation] = list
            if message.conversation.hasPrefix("#") && !channels.contains(message.conversation) {
                channels.append(message.conversation)
            }
            changed = true
        }
        if changed { notifyListeners() }
    }

    @discardableResult
    public func sendCoveInvite(
        inviteeAccount: String, coveId: String, coveName: String, inviterName: String, shareUrl: String, ts: Int64
    ) -> Bool {
        let invitee = inviteeAccount.trimmingCharacters(in: .whitespacesAndNewlines)
        if invitee.isEmpty { return false }
        var frame = envelopeBase("cove-invite")
        frame["invitee"] = .string(String(invitee.prefix(120)))
        frame["coveId"] = .string(String(coveId.prefix(80)))
        frame["coveName"] = .string(String(coveName.prefix(160)))
        frame["inviterName"] = .string(String(inviterName.prefix(80)))
        frame["shareUrl"] = .string(String(shareUrl.prefix(512)))
        frame["coveTs"] = .int(ts)
        seenIds.insert(frame["id"]?.text ?? "")
        return send(frame)
    }

    @discardableResult
    public func sendCoveOpen(
        creatorKey: String, coveId: String, coveName: String, openerKey: String, openerName: String, ts: Int64,
        signature: String
    ) -> Bool {
        let creator = creatorKey.trimmingCharacters(in: .whitespacesAndNewlines)
        if creator.isEmpty { return false }
        var frame = envelopeBase("cove-open")
        frame["creator"] = .string(String(creator.prefix(120)))
        frame["coveId"] = .string(String(coveId.prefix(80)))
        frame["coveName"] = .string(String(coveName.prefix(160)))
        frame["openerKey"] = .string(String(openerKey.prefix(120)))
        frame["openerName"] = .string(String(openerName.prefix(80)))
        frame["coveTs"] = .int(ts)
        frame["sig"] = .string(String(signature.prefix(200)))
        seenIds.insert(frame["id"]?.text ?? "")
        return send(frame)
    }

    public func addChannel(_ channel: String) {
        if channel.isEmpty || channels.contains(channel) { return }
        channels.append(channel)
        notifyListeners()
    }

    public func removeChannel(_ channel: String) {
        let before = channels.count
        channels.removeAll { $0 == channel }
        history.removeValue(forKey: channel)
        unread.removeValue(forKey: channel)
        unseenBadge.removeValue(forKey: channel)
        if channels.count != before { notifyListeners() }
    }

    // MARK: Receiving

    private func onFrame(_ message: WebSocketMessage) async {
        framesReceived += 1
        guard let crypto else { return }
        let text: String
        switch message {
        case .text(let value): text = value
        case .data(let value): text = String(decoding: value, as: UTF8.self)
        }
        guard case .object(let env)? = try? JSON.parse(text) else { return }
        guard let decrypted = crypto.decrypt(env) else { return }
        guard let plain = openSealedFrame(decrypted) else { return }
        handle(plain)
    }

    nonisolated static func directRecipient(_ frame: JSONObject) -> String? {
        guard (frame["type"]?.text ?? "") == "dm" else { return nil }
        let to = frame["to"]?.text ?? ""
        return to.isEmpty ? nil : to
    }

    nonisolated static func isControllerReadableFrame(_ frame: JSONObject) -> Bool {
        if (frame["type"]?.text ?? "") != "chat" || frame["private"]?.boolValue == true { return false }
        return (frame["channel"]?.text ?? "").hasPrefix("#")
    }

    /// Public #channel chat accepts controller-readable application frames for integrations. DMs
    /// and all other traffic remain recipient-sealed.
    private func openSealedFrame(_ frame: JSONObject) -> JSONObject? {
        guard let seal else { return nil }
        if RoomSeal.isKeyFrame(frame) {
            guard let result = seal.acceptBundle(frame, now: clock.nowMillis), result.isNew || result.updated else {
                return nil
            }
            if result.isNew { replyWithKeySoon() }
            for reopened in seal.reopenHeld(now: clock.nowMillis) {
                if let plain = reopened.authenticatedPlain { handle(plain) }
            }
            return nil
        }
        if !RoomSeal.isSealedFrame(frame) {
            if RelayService.isControllerReadableFrame(frame) { return frame }
            noteSealMismatch("")
            return nil
        }
        let result = seal.open(frame, now: clock.nowMillis)
        if case .notForMe = result { noteSealMismatch(RoomSeal.frameKeyId(frame)) }
        return result.authenticatedPlain
    }

    /// Empty means an unsealed frame was refused in a protected conversation. A key id means old
    /// ciphertext was not addressed to this device.
    private func noteSealMismatch(_ keyId: String) {
        if !reportedSealKeyIds.insert(keyId).inserted { return }
        if keyId.isEmpty {
            logLine("Automatic end-to-end encryption refused an unencrypted frame in \(settings.room).")
            return
        }
        logLine(
            "An end-to-end encrypted message from device \(keyId) was not addressed to this device, "
                + "usually because it predates the first visit.")
    }

    private func handle(_ m: JSONObject) {
        let type = m["type"]?.text ?? ""
        let senderId = m["senderId"]?.text ?? ""
        let sender = m["sender"]?.text ?? ""
        if type != "typing" {
            let id = m["id"]?.text ?? ""
            if !id.isEmpty {
                if seenIds.contains(id) {
                    if !senderId.isEmpty && senderId != nodeId { rememberPeer(senderId, sender, m) }
                    return
                }
                seenIds.insert(id)
            }
        }
        if !senderId.isEmpty && senderId != nodeId { rememberPeer(senderId, sender, m) }

        switch type {
        case "hello":
            for value in m["channels"]?.arrayValue ?? [] {
                let channel = value.text
                if !channel.isEmpty && !channels.contains(channel) { channels.append(channel) }
            }
            if (m["to"]?.text ?? "").isEmpty && senderId != nodeId {
                var reply = withOwnStatus(envelopeBase("hello"))
                reply["channels"] = JSONValue(channels)
                reply["to"] = .string(senderId)
                send(reply)
            }
        case "chat":
            let channel = m["channel"].map { $0.isNull ? "#general" : $0.text } ?? "#general"
            if !channels.contains(channel) { channels.append(channel) }
            receive(toMessage(m, channel))
        case "dm":
            let to = m["to"]?.text ?? ""
            if to == nodeId {
                receive(toMessage(m, "@\(senderId)"))
            } else if senderId == nodeId && !to.isEmpty {
                appendMessage(toMessage(m, "@\(to)"))
            }
        case "delete":
            let conversation = m["conversation"]?.text ?? ""
            let target = m["target"]?.text ?? ""
            if conversation.isEmpty || target.isEmpty { break }
            // Only the original author may take a message back.
            if let original = history[conversation]?.first(where: { $0.id == target }), original.senderId == senderId {
                removeMessage(conversation, target)
            }
        case "bye":
            if peers.removeValue(forKey: senderId) != nil {
                rosterRevision &+= 1
                logLine("\(sender) left")
            }
        default:
            break
        }
        frameObserver?(m)
        notifyListeners()
    }

    private func toMessage(_ m: JSONObject, _ conversation: String) -> ChatMessage {
        let senderId = ChatFrameText.shownSender(m)
        let ts: Date
        if let raw = m["ts"], let millis = raw.doubleValue {
            ts = Date(epochMillis: Int64(millis))
        } else {
            ts = clock.now
        }
        return ChatMessage(
            id: m["id"]?.text ?? "", conversation: conversation, senderId: senderId, senderName: m["sender"]?.text ?? "",
            text: m["text"]?.text ?? "", timestamp: ts, isSelf: senderId == nodeId, fileName: m["fileName"]?.text ?? "")
    }

    private func receive(_ msg: ChatMessage) {
        if appendMessage(msg) && !msg.isSelf { onMessage?(msg) }
    }

    @discardableResult
    private func appendMessage(_ msg: ChatMessage) -> Bool {
        if isExpired?(msg.conversation, msg.timestamp) == true { return false }
        var list = history[msg.conversation] ?? []
        if list.contains(where: { $0.id == msg.id }) { return false }
        // Messages nearly always arrive in order, so find the slot from the end rather than
        // re-sorting the whole conversation for every message.
        var at = list.count
        while at > 0 && RelayService.compareMessages(list[at - 1], msg) > 0 { at -= 1 }
        list.insert(msg, at: at)
        history[msg.conversation] = list
        if !msg.isSelf && msg.conversation != currentConversation && isReadElsewhere?(msg) != true {
            unread[msg.conversation, default: 0] += 1
            unseenBadge[msg.conversation, default: 0] += 1
        }
        return true
    }

    @discardableResult
    public func pruneExpired() -> Bool {
        guard let isExpired else { return false }
        var changed = false
        for (conversation, list) in history {
            let kept = list.filter { !isExpired(conversation, $0.timestamp) }
            if kept.count == list.count { continue }
            changed = true
            let keptIds = Set(kept.map(\.id))
            for message in list where !keptIds.contains(message.id) { seenNotificationIds.remove(message.id) }
            history[conversation] = kept.isEmpty ? nil : kept
            if let count = unread[conversation] {
                let left = min(count, kept.filter { !$0.isSelf }.count)
                unread[conversation] = left > 0 ? left : nil
            }
        }
        if changed { notifyListeners() }
        return changed
    }

    private func removeMessage(_ conversation: String, _ id: String) {
        guard let list = history[conversation] else { return }
        let next = list.filter { $0.id != id }
        if next.count == list.count { return }
        history[conversation] = next.isEmpty ? nil : next
        seenNotificationIds.remove(id)
    }

    nonisolated static func compareMessages(_ a: ChatMessage, _ b: ChatMessage) -> Int {
        if a.timestamp < b.timestamp { return -1 }
        if a.timestamp > b.timestamp { return 1 }
        if a.id == b.id { return 0 }
        return a.id.utf16.lexicographicallyPrecedes(b.id.utf16) ? -1 : 1
    }

    private func rememberPeer(_ id: String, _ senderName: String, _ m: JSONObject) {
        let before = peers[id]
        var peer = before ?? RelayPeer(order: RelayService.nextPeerOrder())
        if !senderName.isEmpty { peer.name = senderName }
        let platform = m["platform"]?.text ?? ""
        if !platform.isEmpty { peer.platform = platform }
        let version = m["version"]?.text ?? ""
        if !version.isEmpty { peer.version = version }
        let ownerValue = m["ownerUser"].flatMap { $0.isNull ? nil : $0 } ?? m["owner"]
        let owner = ownerValue?.text ?? ""
        if !owner.isEmpty { peer.owner = owner }
        let nodeName = m["nodeName"]?.text ?? ""
        if !nodeName.isEmpty { peer.nodeName = nodeName }
        if let mirrors = m["mirrors"]?.arrayValue { peer.mirrors = mirrors.map(\.text) }
        if case .array(let findings)? = m["dg"] { peer.diagnostics = .array(Array(findings.prefix(64))) }
        rememberPeerStatus(&peer, m)
        let zone = m["timeZone"]?.text ?? ""
        if RelayService.validTimeZone(zone) { peer.timeZone = zone }
        peer.online = true
        let now = clock.nowMillis
        peer.lastSeenMs = now
        peers[id] = peer
        if RelayService.rosterChanged(before, peer, now: now) { rosterRevision &+= 1 }
    }

    nonisolated public static func validTimeZone(_ identifier: String) -> Bool {
        !identifier.isEmpty && identifier.count <= 64 && TimeZone(identifier: identifier) != nil
    }

    nonisolated static func rosterChanged(_ before: RelayPeer?, _ after: RelayPeer, now: Int64) -> Bool {
        guard var before else { return true }
        if !before.online || now - before.lastSeenMs > staleMs { return true }
        before.lastSeenMs = after.lastSeenMs
        return before != after
    }

    private func forgetPeers() {
        if peers.isEmpty { return }
        peers.removeAll()
        rosterRevision &+= 1
    }

    private func noteRosterStaleness() {
        let now = clock.nowMillis
        let live = Set(peers.filter { $0.value.online && now - $0.value.lastSeenMs <= RelayService.staleMs }.keys)
        if live != liveAtTick {
            liveAtTick = live
            rosterRevision &+= 1
        }
    }

    /// Only a live hello or presence frame speaks for a peer's status. A frame without the field
    /// leaves a known status alone; a replayed or stale frame, or one older than the status
    /// already held, cannot roll it back.
    private func rememberPeerStatus(_ peer: inout RelayPeer, _ m: JSONObject) {
        let type = m["type"]?.text ?? ""
        if type != "hello" && type != "presence" { return }
        guard case .object? = m["userStatus"] else { return }
        let now = clock.nowMillis
        let ts = m["ts"]?.doubleValue.map { Int64($0) } ?? now
        if now - ts > RelayService.staleMs || ts < peer.statusTs { return }
        peer.status = UserStatus(json: m["userStatus"])
        peer.statusTs = ts
    }

    private func setState(_ next: RelayConnectionState) {
        if state == next { return }
        state = next
        notifyListeners()
    }

    private func logLine(_ line: String) {
        let stamp = RelayService.stampFormatter.string(from: clock.now)
        log.append("\(stamp)  \(line)")
        if log.count > 500 { log.removeFirst() }
        onLogLine?(line)
        notifyListeners()
    }

    /// `DateTime.now().toIso8601String().substring(11, 19)`: local HH:mm:ss.
    nonisolated static let stampFormatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "HH:mm:ss"
        return formatter
    }()

    private static var seq = 0
    private static var peerOrder = 0

    private static func nextPeerOrder() -> Int {
        peerOrder += 1
        return peerOrder
    }

    /// `<nodeId>-<microseconds>-<seq>`.
    static func makeId(_ nodeId: String, _ clock: Clock) -> String {
        seq += 1
        let micros = Int64((clock.now.timeIntervalSince1970 * 1_000_000).rounded())
        return "\(nodeId)-\(micros)-\(seq)"
    }

    /// The text of what `openUntracked` and `send` catch: "Exception: message" for the
    /// service's own failures, the error's text otherwise.
    nonisolated static func errorText(_ error: Error) -> String {
        if let relay = error as? RelayError { return "Exception: \(relay.message)" }
        return dartErrorText(error)
    }
}

/// A failure the relay service raises itself.
public struct RelayError: Error, Equatable, CustomStringConvertible {
    public let message: String

    public init(_ message: String) { self.message = message }

    public var description: String { "Exception: \(message)" }
}

/// What the relay has told this device about one peer node.
struct RelayPeer: Equatable {
    /// First-heard order, so the roster stays in insertion order.
    var order: Int
    var name = ""
    var platform = ""
    var version = ""
    var owner = ""
    var nodeName = ""
    var mirrors: [String] = []
    var status: UserStatus?
    var statusTs: Int64 = 0
    var diagnostics: JSONValue?
    var timeZone = ""
    var online = true
    var lastSeenMs: Int64 = 0
}
