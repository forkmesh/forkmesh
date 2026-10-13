// NodeSyncService; the models are in NodeSyncModels.swift.
//
// The phone acting as its own ForkMesh node. A `mobile_node` bind registers this device's
// Ed25519 key with the `owner_sign` capability, and the Worker's `_owner_signing_pubkeys(owner)`
// unions every enabled `owner_sign` device key, so both node routes already authorize this phone:
//
// * `GET /api/sync?owner=&ts=&sig=`: the one consolidated read of every pending inbox item,
//   queued agent prompt and pinned repo state.
// * `GET /api/nodes/events?owner=&ts=&sig=`: the payload-free `{"type":"event","topic","repo"}`
//   advisory socket. `_node_events_authorization` takes either a browser `?ticket=` or exactly
//   the signed drain token above; a node signs, so this service signs.
//
// Both sign the same canonical, byte for byte with `_authorized_owner_signing_key`:
// `forkmesh-issues-pull-v1\n{owner}\n{ts}`.
//
// Foreground only, per the mobile node contract §1: no background modes, no timers or sockets
// while the app is not resumed. The app calls `appLifecycleChanged(_:)` on scene-phase changes.

import FMAccount
import FMApi
import FMBase
import FMCrypto
import FMNet
import Foundation
import Observation

/// Applies one drained inbox item to local state. Returning `true` is a promise that the item is
/// durably held by the app and may be deleted from the relay; returning `false` (or throwing)
/// leaves the row for a desktop.
public typealias NodeSyncApplier = @MainActor (NodeSyncItem) async throws -> Bool

/// `{ts, sig}` of one signed node request.
public struct NodeSyncToken: Equatable, Sendable {
    public let ts: String
    public let sig: String

    public init(ts: String, sig: String) {
        self.ts = ts
        self.sig = sig
    }
}

@MainActor
@Observable
public final class NodeSyncService {
    /// The signed drain token every node route shares.
    nonisolated public static let canonicalPrefix = "forkmesh-issues-pull-v1"

    /// Kinds this node will ever acknowledge (delete) on the relay.
    ///
    /// * issues / pulls / discussions: deleted by exact row id (`DELETE ...?ids=1,2`), and only
    ///   for items an `applier` confirmed it durably applied.
    /// * agentPrompts: never. An owner-sealed prompt is acknowledged only after a host decrypts
    ///   and runs it, and a mobile node has no `host_repo` capability.
    /// * aboutUpdate: never. The relay drains these on read regardless, which is why they are
    ///   carried locally (see `carried`).
    nonisolated public static let ackableKinds: [NodeSyncKind] = [.issues, .pulls, .discussions]

    nonisolated public static let initialReconnectDelay: TimeInterval = 2
    nonisolated public static let maxReconnectDelay: TimeInterval = 30

    /// Local-carry cap for the kinds the relay deletes on read.
    nonisolated public static let carriedLimit = 100

    /// Legacy, unscoped carry slot. Migrated into the active relay's slot on first load; nothing
    /// writes it any more.
    nonisolated public static let carriedPrefsKey = "nodeSync/carried"

    /// Where one relay's carried items live. Carried items are inbox rows the relay deleted on
    /// read, so they belong to the relay they were drained from and to no other.
    nonisolated public static func carriedPrefsKeyFor(_ relayId: String) -> String {
        "\(carriedPrefsKey)/\(relayId)"
    }

    /// The canonical `_authorized_owner_signing_key` verifies, byte for byte.
    nonisolated public static func syncCanonical(_ owner: String, _ ts: String) -> String {
        "\(canonicalPrefix)\n\(owner)\n\(ts)"
    }

    @ObservationIgnored private let settings: SettingsService
    @ObservationIgnored private let identity: DeviceIdentity
    @ObservationIgnored private let session: @MainActor () -> AuthSession?
    @ObservationIgnored private let http: HTTPClient
    @ObservationIgnored private let connector: WebSocketConnector
    @ObservationIgnored private let pollInterval: TimeInterval
    @ObservationIgnored private let requestTimeout: TimeInterval
    @ObservationIgnored private let nextInt: @MainActor (Int) -> Int
    @ObservationIgnored private let clock: Clock
    @ObservationIgnored private let sleeper: @Sendable (TimeInterval) async throws -> Void

    /// Applies a drained issue/pull/discussion locally. With no applier the node acks nothing at
    /// all and every row is left for a desktop. `LocalNodeSyncApplier.installOn` sets it.
    @ObservationIgnored public var applier: NodeSyncApplier?

    /// Fired after a sync that drained at least one new item, so the app can run the
    /// account-inbox read it already uses to notify (NotificationService).
    @ObservationIgnored public var onNewItems: (@MainActor () -> Void)?

    /// Fired with the Worker's `error` code when a signed node call is refused.
    @ObservationIgnored public var onUnauthorized: (@MainActor (String) -> Void)?

    /// `addListener` / `removeListener` for observers outside SwiftUI.
    @ObservationIgnored public let listeners = Listeners()

    public private(set) var connection: NodeSyncConnection = .offline
    public private(set) var payload = NodeSyncPayload()
    /// Items the relay deleted as it handed them over (About edits, legacy agent prompts, branch
    /// operations). Kept so a read is never a silent loss.
    public private(set) var carried: [NodeSyncItem] = []
    public private(set) var lastSyncAt: Date?
    public private(set) var lastError = ""
    public private(set) var syncing = false
    public private(set) var started = false
    public private(set) var ackedTotal = 0
    /// Bumped on every notify, so a view reading `mode` re-renders when the mode flips.
    public private(set) var revision = 0

    @ObservationIgnored private var lastMode: NodeSyncMode = .disabled
    @ObservationIgnored private var disposed = false
    @ObservationIgnored private var foreground = true
    @ObservationIgnored private var syncGeneration = 0
    @ObservationIgnored private var settingsToken: ListenerToken?

    @ObservationIgnored private var pollTask: Task<Void, Never>?
    @ObservationIgnored private var reconnectTask: Task<Void, Never>?
    /// Milliseconds.
    @ObservationIgnored private var reconnectDelayMs = Int(NodeSyncService.initialReconnectDelay * 1000)
    @ObservationIgnored private var socket: WebSocketConnection?
    @ObservationIgnored private var readerTask: Task<Void, Never>?
    @ObservationIgnored private var socketGeneration = 0

    /// - Parameters:
    ///   - session: the live session (`{ auth.session }`).
    ///   - connector: FMNet's (`URLSessionWebSocketConnector` in the app).
    ///   - random: `nextInt(n)`, a value in `0..<n`, for the reconnect jitter.
    ///   - sleep: drives the poll and reconnect timers (tests park it).
    public init(
        settings: SettingsService,
        identity: DeviceIdentity,
        session: @escaping @MainActor () -> AuthSession?,
        http: HTTPClient,
        connector: WebSocketConnector,
        pollInterval: TimeInterval = 60,
        requestTimeout: TimeInterval = 20,
        random: @escaping @MainActor (Int) -> Int = { Int.random(in: 0..<max(1, $0)) },
        clock: Clock = SystemClock(),
        sleep: @escaping @Sendable (TimeInterval) async throws -> Void = { seconds in
            try await Task.sleep(nanoseconds: UInt64(max(0, seconds) * 1_000_000_000))
        }
    ) {
        self.settings = settings
        self.identity = identity
        self.session = session
        self.http = http
        self.connector = connector
        self.pollInterval = pollInterval
        self.requestTimeout = requestTimeout
        self.nextInt = random
        self.clock = clock
        self.sleeper = sleep
    }

    @discardableResult
    public func addListener(_ listener: @escaping @Sendable () -> Void) -> ListenerToken { listeners.add(listener) }

    public func removeListener(_ token: ListenerToken) { listeners.remove(token) }

    // MARK: State

    /// Derived from the live session, so the status card is right before the first `start` and
    /// follows a sign-in or a desktop joining the account.
    public var mode: NodeSyncMode {
        _ = revision
        return computeMode()
    }

    public var repos: [NodeSyncRepoState] { payload.repos }
    public var items: [NodeSyncItem] { payload.items }
    public var isActingAsNode: Bool { mode == .active }
    public var pendingTotal: Int { payload.items.count }

    /// Pending item count per kind. Iterate `NodeSyncKind.allCases` for the display order.
    public var counts: [NodeSyncKind: Int] {
        var out: [NodeSyncKind: Int] = [:]
        for kind in NodeSyncKind.allCases { out[kind] = payload.countOf(kind) }
        return out
    }

    /// The account name this node signs as, or "" when it cannot sign.
    public var owner: String {
        guard let current = session(), current.isMobileNode else { return "" }
        return current.nodeName.dartTrimmed.lowercased()
    }

    /// True when a desktop node is enrolled and therefore owns the drain.
    public var hasDesktopNode: Bool { !(session()?.desktopDevices.isEmpty ?? true) }

    // MARK: Signing and URLs

    /// `{ts, sig}` for one signed node request at `nowMs` (default: now).
    public func signedToken(_ owner: String, nowMs: Int64? = nil) -> NodeSyncToken {
        let ts = String(nowMs ?? clock.nowMillis)
        let sig = identity.sign(utf8: NodeSyncService.syncCanonical(owner, ts))
        return NodeSyncToken(ts: ts, sig: sig)
    }

    /// `GET /api/sync?owner=&ts=&sig=` (http for a `ws` relay, else https).
    public func syncURL(nowMs: Int64? = nil) -> URL? {
        let account = owner
        let token = signedToken(account, nowMs: nowMs)
        return settings.server.httpURL("/api/sync", query: [("owner", account), ("ts", token.ts), ("sig", token.sig)])
    }

    /// `wss://.../api/nodes/events?owner=&ts=&sig=`. A browser trades its session for
    /// `/api/accounts/event-ticket`; a node presents the signed drain token instead, which is the
    /// branch `_node_events_authorization` takes when `?ticket=` is absent.
    public func eventsURL(nowMs: Int64? = nil) -> URL? {
        let account = owner
        let token = signedToken(account, nowMs: nowMs)
        return URL(string: settings.server.webSocket(
            "/api/nodes/events", query: [("owner", account), ("ts", token.ts), ("sig", token.sig)]))
    }

    // MARK: Lifecycle

    public func start() {
        if disposed || started { return }
        started = true
        if settingsToken == nil {
            settingsToken = settings.addListener { [weak self] in
                MainActor.assumeIsolated { self?.onSettingsChanged() }
            }
        }
        foreground = true
        evaluate(kick: true)
    }

    public func stop() async {
        started = false
        syncGeneration += 1
        pollTask?.cancel()
        pollTask = nil
        reconnectTask?.cancel()
        reconnectTask = nil
        await detachSocket()?.close(code: WebSocketStatus.normalClosure, reason: "")
        setConnection(.offline)
    }

    /// Re-reads the session and starts/stops accordingly. Wire this to AuthService's listener: a
    /// sign-in, a sign-out, or a desktop node showing up on the account all change whether this
    /// phone should be draining.
    public func refreshSession() {
        if disposed { return }
        evaluate(kick: true)
    }

    /// The app's lifecycle (scene phase). Anything but `.resumed` takes the timers and the socket
    /// down; `.resumed` comes back at once rather than serving out a backoff frozen while away.
    public func appLifecycleChanged(_ state: AppLifecycleState) {
        if disposed { return }
        let resumed = state == .resumed
        if resumed == foreground { return }
        foreground = resumed
        if resumed {
            reconnectDelayMs = Int(NodeSyncService.initialReconnectDelay * 1000)
            evaluate(kick: true)
        } else {
            // Contract §1: never run in the background.
            pollTask?.cancel()
            pollTask = nil
            reconnectTask?.cancel()
            reconnectTask = nil
            syncGeneration += 1
            closeSocketInBackground()
            setConnection(.offline)
        }
    }

    public func dispose() {
        disposed = true
        if let settingsToken { settings.removeListener(settingsToken) }
        settingsToken = nil
        pollTask?.cancel()
        pollTask = nil
        reconnectTask?.cancel()
        reconnectTask = nil
        closeSocketInBackground()
    }

    /// Follows the "Serve as a node" switch. Settings notifies for every stored preference, so
    /// only a change that actually flips the mode re-evaluates.
    private func onSettingsChanged() {
        if disposed || computeMode() == lastMode { return }
        evaluate(kick: true)
    }

    private func evaluate(kick: Bool = false) {
        let next = computeMode()
        let changed = next != lastMode
        lastMode = next
        if next != .active || !started || !foreground {
            pollTask?.cancel()
            pollTask = nil
            reconnectTask?.cancel()
            reconnectTask = nil
            syncGeneration += 1
            closeSocketInBackground()
            if connection != .offline { connection = .offline }
            if changed { notify() }
            return
        }
        if pollTask == nil { pollTask = makePollTask() }
        if socket == nil && reconnectTask == nil { openSocket() }
        if changed { notify() }
        if kick { Task { await self.syncNow(manual: false) } }
    }

    private func computeMode() -> NodeSyncMode {
        if !settings.serveAsNode { return .disabled }
        guard let current = session(), current.isMobileNode else { return .disabled }
        if current.nodeName.dartTrimmed.isEmpty { return .disabled }
        if !current.desktopDevices.isEmpty { return .standby }
        return .active
    }

    private func makePollTask() -> Task<Void, Never> {
        let interval = pollInterval
        let sleeper = self.sleeper
        return Task { [weak self] in
            while !Task.isCancelled {
                do { try await sleeper(interval) } catch { return }
                guard let self, !Task.isCancelled else { return }
                Task { await self.syncNow(manual: false) }
            }
        }
    }

    // MARK: The signed sync

    /// One signed `GET /api/sync`, followed by the ack of whatever the applier accepted. `manual`
    /// marks a user-driven "Sync now", which runs even from standby so the card can still show
    /// the account's state.
    public func syncNow(manual: Bool = true) async {
        if disposed || syncing { return }
        let account = owner
        if account.isEmpty || !settings.serveAsNode { return }
        if !manual && mode != .active { return }
        syncGeneration += 1
        let generation = syncGeneration
        syncing = true
        notify()
        defer {
            if !disposed {
                syncing = false
                notify()
            }
        }
        do {
            guard let url = syncURL() else { throw NodeSyncURLError(relay: settings.serverUrl) }
            let response = try await http.send(HTTPRequest(
                method: "GET", url: url, headers: ["accept": "application/json"], timeout: requestTimeout))
            if generation != syncGeneration || disposed { return }
            if response.status == 401 || response.status == 403 {
                let code = NodeSyncService.errorCode(response.body)
                lastError = code.isEmpty ? "unauthorized" : code
                onUnauthorized?(code)
                return
            }
            if response.status < 200 || response.status >= 300 {
                lastError = "HTTP \(response.status)"
                return
            }
            let next = NodeSyncPayload.parse(try JSON.parse(response.body))
            let fresh = newItemCount(next)
            payload = next
            lastSyncAt = clock.now
            lastError = ""
            carry(next)
            if generation != syncGeneration || disposed { return }
            await ackApplied(next, account: account)
            if fresh > 0 { onNewItems?() }
        } catch {
            if generation == syncGeneration && !disposed {
                lastError = dartErrorText(error)
            }
        }
    }

    private func newItemCount(_ next: NodeSyncPayload) -> Int {
        func key(_ item: NodeSyncItem) -> String { "\(item.repoFullName):\(item.kind.rawValue):\(item.id)" }
        let seen = Set(payload.items.map(key))
        return next.items.filter { !seen.contains(key($0)) }.count
    }

    // MARK: Carried items

    private var carriedKey: String { NodeSyncService.carriedPrefsKeyFor(settings.relays.activeId) }

    /// Persist the kinds the relay deletes as it serves them. Nothing else is stored: every other
    /// row is still on the relay until it is acked.
    private func carry(_ next: NodeSyncPayload) {
        let drained = next.items.filter { $0.kind == .about || $0.kind == .agentPrompts || $0.kind == .branchOps }
        if drained.isEmpty { return }
        let merged = carried + drained
        carried = merged.count > NodeSyncService.carriedLimit
            ? Array(merged.suffix(NodeSyncService.carriedLimit))
            : merged
        settings.prefs.set(JSON.encode(.array(carried.map { .object($0.toJSON()) })), forKey: carriedKey)
    }

    /// Restores the active relay's locally carried items. Call at startup and again after every
    /// relay switch, so the in-memory carry always belongs to the relay the app is on.
    public func loadCarried() {
        carried = []
        let prefs = settings.prefs
        let key = carriedKey
        // A carry written before relays were scoped was, by definition, drained from whichever
        // relay the app was then pointed at, which the relay directory has made the active one.
        let legacy = prefs.string(forKey: NodeSyncService.carriedPrefsKey) ?? ""
        if !legacy.isEmpty {
            if (prefs.string(forKey: key) ?? "").isEmpty { prefs.set(legacy, forKey: key) }
            prefs.remove(NodeSyncService.carriedPrefsKey)
        }
        let raw = prefs.string(forKey: key) ?? ""
        if raw.isEmpty {
            notify()
            return
        }
        // A corrupt cache is ignored; it is a convenience, not the source of truth.
        guard let rows = (try? JSON.parse(raw))?.arrayValue else { return }
        carried = rows.compactMap { $0.objectValue.flatMap(NodeSyncItem.fromJSON) }
        notify()
    }

    public func clearCarried() {
        carried = []
        settings.prefs.remove(carriedKey)
        notify()
    }

    // MARK: Acks

    /// Offers every ackable item to `applier` and deletes exactly the rows it accepted. With no
    /// applier installed this is a no-op and the whole inbox is left for a desktop node.
    private func ackApplied(_ next: NodeSyncPayload, account: String) async {
        guard let apply = applier else { return }
        // Insertion-ordered repo -> kind -> ids.
        var accepted: [(repo: String, kinds: [(kind: NodeSyncKind, ids: [Int])])] = []
        for item in next.items {
            if !NodeSyncService.ackableKinds.contains(item.kind) || item.id <= 0 { continue }
            let ok: Bool
            do {
                ok = try await apply(item)
            } catch {
                // An applier that threw did not apply anything: leave the row.
                ok = false
            }
            if !ok { continue }
            let repoIndex: Int
            if let found = accepted.firstIndex(where: { $0.repo == item.repoFullName }) {
                repoIndex = found
            } else {
                accepted.append((item.repoFullName, []))
                repoIndex = accepted.count - 1
            }
            if let kindIndex = accepted[repoIndex].kinds.firstIndex(where: { $0.kind == item.kind }) {
                accepted[repoIndex].kinds[kindIndex].ids.append(item.id)
            } else {
                accepted[repoIndex].kinds.append((item.kind, [item.id]))
            }
        }
        for repoEntry in accepted {
            for kindEntry in repoEntry.kinds where !kindEntry.ids.isEmpty {
                await deleteInbox(account: account, repoFullName: repoEntry.repo, kind: kindEntry.kind, ids: kindEntry.ids)
            }
        }
    }

    private func deleteInbox(account: String, repoFullName: String, kind: NodeSyncKind, ids: [Int]) async {
        guard let slash = repoFullName.firstIndex(of: "/"), slash > repoFullName.startIndex else { return }
        let repoOwner = String(repoFullName[..<slash])
        let repoName = String(repoFullName[repoFullName.index(after: slash)...])
        // The DELETE is authorized by `_authorized_repo_inbox_signing_key`, which verifies the
        // same canonical against the URL's owner segment.
        let token = signedToken(repoOwner)
        do {
            guard let url = settings.server.httpURL(
                "/api/repo/\(repoOwner)/\(repoName)/\(kind.wireName)",
                query: [
                    ("owner", account), ("ts", token.ts), ("sig", token.sig),
                    ("ids", ids.map(String.init).joined(separator: ",")),
                ])
            else { throw NodeSyncURLError(relay: settings.serverUrl) }
            let response = try await http.send(HTTPRequest(
                method: "DELETE", url: url, headers: ["accept": "application/json"], timeout: requestTimeout))
            if response.status >= 200 && response.status < 300 {
                ackedTotal += ids.count
                return
            }
            lastError = "ack \(kind.wireName): HTTP \(response.status)"
        } catch {
            lastError = "ack \(kind.wireName): \(dartErrorText(error))"
        }
    }

    // MARK: The advisory socket

    /// Marks the socket as connecting synchronously, then connects in a task.
    private func openSocket() {
        if disposed || !started || !foreground { return }
        if socket != nil || mode != .active { return }
        socketGeneration += 1
        let attempt = socketGeneration
        setConnection(.connecting)
        let url = eventsURL()
        Task { await self.finishOpen(attempt, url) }
    }

    private func finishOpen(_ attempt: Int, _ url: URL?) async {
        let connection: WebSocketConnection
        do {
            guard let url else { throw NodeSyncURLError(relay: settings.serverUrl) }
            connection = try await connector.connect(url: url, options: WebSocketOptions())
        } catch {
            if attempt == socketGeneration { scheduleReconnect() }
            return
        }
        if attempt != socketGeneration || disposed || !started || !foreground || mode != .active {
            await connection.close(code: WebSocketStatus.normalClosure, reason: "")
            setConnection(.offline)
            return
        }
        socket = connection
        reconnectDelayMs = Int(NodeSyncService.initialReconnectDelay * 1000)
        readerTask = Task { [weak self] in
            do {
                for try await message in connection.messages {
                    guard let self else { return }
                    self.onFrame(message)
                }
            } catch {}
            self?.onSocketClosed(connection)
        }
        setConnection(.live)
        // A reconnect may have missed frames: catch up with one signed read.
        Task { await self.syncNow(manual: false) }
    }

    private func onFrame(_ message: WebSocketMessage) {
        let text: String
        switch message {
        case .text(let value): text = value
        case .data(let data):
            guard let decoded = try? Utf8.decode(data) else { return }
            text = decoded
        }
        // {"type":"hello","accepts":[...]} is a capability handshake, not an event.
        guard let frame = (try? JSON.parse(text))?.objectValue, frame["type"] == .string("event") else { return }
        Task { await self.syncNow(manual: false) }
    }

    private func onSocketClosed(_ connection: WebSocketConnection) {
        guard let current = socket, current === connection else { return }
        closeSocketInBackground()
        setConnection(.offline)
        if disposed || !started || !foreground { return }
        if mode != .active { return }
        scheduleReconnect()
    }

    /// Forgets the socket and stops reading it, synchronously. The caller closes the returned
    /// connection.
    private func detachSocket() -> WebSocketConnection? {
        socketGeneration += 1
        readerTask?.cancel()
        readerTask = nil
        let current = socket
        socket = nil
        return current
    }

    /// Detaches the socket and closes it without waiting.
    private func closeSocketInBackground() {
        guard let current = detachSocket() else { return }
        Task { await current.close(code: WebSocketStatus.normalClosure, reason: "") }
    }

    private func scheduleReconnect() {
        if disposed || !started || !foreground { return }
        if mode != .active { return }
        if reconnectTask != nil { return }
        let delayMs = jittered(reconnectDelayMs)
        reconnectDelayMs = min(reconnectDelayMs * 2, Int(NodeSyncService.maxReconnectDelay * 1000))
        setConnection(.offline)
        let sleeper = self.sleeper
        reconnectTask = Task { [weak self] in
            do { try await sleeper(TimeInterval(delayMs) / 1000) } catch { return }
            guard let self, !Task.isCancelled else { return }
            self.reconnectTask = nil
            if !self.disposed && self.started && self.foreground { self.openSocket() }
        }
    }

    /// Full jitter within ±25%, so a fleet coming back from a relay deploy does not reconnect in
    /// lockstep.
    private func jittered(_ baseMs: Int) -> Int {
        let span = Int((Double(baseMs) * 0.25).rounded())
        if span <= 0 { return baseMs }
        return baseMs - span + nextInt(span * 2 + 1)
    }

    private func setConnection(_ next: NodeSyncConnection) {
        if connection == next { return }
        connection = next
        notify()
    }

    private func notify() {
        if disposed { return }
        revision &+= 1
        listeners.notify()
    }

    /// `'${decoded['error']}'` of a JSON error body, or "".
    nonisolated static func errorCode(_ body: Data) -> String {
        guard !body.isEmpty, let value = try? JSON.parse(body), let error = value["error"], !error.isNull else { return "" }
        return error.text
    }
}

/// The relay URL could not make a request URL.
public struct NodeSyncURLError: Error, CustomStringConvertible, Sendable {
    public let relay: String
    public var description: String { "Invalid relay URL: \(relay)" }
}
