// After native/android api/AccountEventLoop.kt and app/.../account/AccountEventChannel.kt.
//
// One authenticated, payload-free event channel per signed-in account: the mobile half of the
// push channel the desktop node and the web dashboard already hold (the per-owner
// `ForkMeshNodes` Durable Object behind `/api/nodes/events`). The relay pushes
// `{"type":"event","topic"}` frames ("pings", "issues", "direct-messages", ...) when
// account-scoped state changes, and the app answers with the one authenticated read it would
// otherwise have made on a timer. Because the frames carry no payload, holding this socket
// grants no authority.
//
// A phone has its sockets torn down whenever the app leaves the foreground, so the channel also
// takes the app lifecycle (`appLifecycleChanged`) and comes back on resume instead of waiting
// out the backoff.

import FMAccount
import FMBase
import FMNet
import Foundation
import Observation

/// Where the channel gets its ticket and URL. `ApiService` conforms.
@MainActor
public protocol AccountEventTicketSource: AnyObject {
    func accountEventTicket() async -> EventTicket
    func accountEventsURL(_ ticket: String, node: String) -> URL
}

extension ApiService: AccountEventTicketSource {}

@MainActor
@Observable
public final class AccountEventChannel {
    nonisolated public static let initialReconnectDelay: TimeInterval = 2
    nonisolated public static let maxReconnectDelay: TimeInterval = 30

    @ObservationIgnored let api: AccountEventTicketSource
    @ObservationIgnored let sessionToken: @MainActor () -> String
    @ObservationIgnored let connector: WebSocketConnector
    /// App-level keepalive cadence (seconds). Matches the desktop node and stays well inside the
    /// Durable Object's 15-minute staleness reaper.
    @ObservationIgnored let keepaliveInterval: TimeInterval
    @ObservationIgnored let sleeper: @Sendable (TimeInterval) async throws -> Void

    /// The owned machine this channel wants to reach, when it wants one: naming a managed mirror
    /// asks the relay to join this channel to that node's own object. "" for the account channel.
    @ObservationIgnored public let nodeName: String

    /// Fired with the topic of each event frame.
    @ObservationIgnored public var onTopic: (@MainActor (String) -> Void)?
    /// Fired with every other frame the relay delivers (the `hello` capability frame, `terminal`
    /// frames), for callers that speak more than topics.
    @ObservationIgnored public var onFrame: (@MainActor (JSONObject) -> Void)?
    /// Fired on every successful (re)connect: where the catch-up read belongs.
    @ObservationIgnored public var onConnected: (@MainActor () -> Void)?
    /// Fired when the relay says the stored session proves nobody. The channel stays dark until
    /// `restart()`.
    @ObservationIgnored public var onSignedOut: (@MainActor () -> Void)?

    /// Whether a socket is open.
    public private(set) var connected = false
    /// Whether the channel is meant to be up (it may be between reconnects).
    public private(set) var started = false

    @ObservationIgnored private var socket: WebSocketConnection?
    @ObservationIgnored private var readerTask: Task<Void, Never>?
    @ObservationIgnored private var reconnectTask: Task<Void, Never>?
    @ObservationIgnored private var keepaliveTask: Task<Void, Never>?
    @ObservationIgnored private var reconnectDelay = AccountEventChannel.initialReconnectDelay
    @ObservationIgnored private var generation = 0
    @ObservationIgnored private var openingAttempt: Int?
    @ObservationIgnored private var disposed = false
    @ObservationIgnored private var outbox: [(WebSocketConnection, String)] = []
    @ObservationIgnored private var draining = false

    /// `connector` is FMNet's (`URLSessionWebSocketConnector` in the app); `sleep` drives the
    /// reconnect and keepalive timers (tests shorten it).
    public init(
        api: AccountEventTicketSource,
        sessionToken: @escaping @MainActor () -> String,
        connector: WebSocketConnector,
        keepalive: TimeInterval = 4 * 60,
        nodeName: String = "",
        sleep: @escaping @Sendable (TimeInterval) async throws -> Void = { seconds in
            try await Task.sleep(nanoseconds: UInt64(max(0, seconds) * 1_000_000_000))
        }
    ) {
        self.api = api
        self.sessionToken = sessionToken
        self.connector = connector
        self.keepaliveInterval = keepalive
        self.nodeName = nodeName
        self.sleeper = sleep
    }

    public func start() {
        if disposed || started { return }
        started = true
        reconnectDelay = AccountEventChannel.initialReconnectDelay
        Task { await self.open() }
    }

    public func stop() async {
        started = false
        // Invalidate any open() already awaiting a ticket, so a late arrival cannot install a
        // socket after the caller asked the channel to go down.
        generation += 1
        reconnectTask?.cancel()
        reconnectTask = nil
        await teardownSocket()
    }

    /// A sign-in or account switch invalidates the ticket this socket was opened with; drop it
    /// and come back under the new identity.
    public func restart() async {
        if disposed { return }
        let wasStarted = started
        await stop()
        if wasStarted || !sessionToken().isEmpty { start() }
    }

    public func dispose() async {
        disposed = true
        await stop()
    }

    /// The app's lifecycle (scene phase). On `.resumed` the channel comes back at once rather
    /// than serving out a backoff frozen while backgrounded, or probes an open socket the OS may
    /// have killed without a close frame.
    public func appLifecycleChanged(_ state: AppLifecycleState) {
        if state != .resumed || disposed || !started { return }
        reconnectTask?.cancel()
        reconnectTask = nil
        reconnectDelay = AccountEventChannel.initialReconnectDelay
        if socket == nil {
            Task { await self.open() }
        } else {
            sendKeepalive()
        }
    }

    /// Send one JSON frame up the socket. False when there is no socket to send it on.
    @discardableResult
    public func send(_ frame: JSONObject) -> Bool {
        guard let socket else { return false }
        outbox.append((socket, JSON.encode(.object(frame))))
        if !draining {
            draining = true
            Task { await self.drainOutbox() }
        }
        return true
    }

    private func drainOutbox() async {
        while !outbox.isEmpty {
            let (target, text) = outbox.removeFirst()
            guard let socket, socket === target else { continue }
            do {
                try await socket.send(text: text)
            } catch {
                onClosed(socket)
            }
        }
        draining = false
    }

    private func open() async {
        if disposed || !started || socket != nil || openingAttempt == generation { return }
        // Signed out: stay dark instead of retrying forever, which is just a poll of the ticket
        // endpoint.
        if sessionToken().isEmpty {
            started = false
            return
        }
        generation += 1
        let attempt = generation
        openingAttempt = attempt
        defer { if openingAttempt == attempt { openingAttempt = nil } }
        let ticket = await api.accountEventTicket()
        if attempt != generation || disposed || !started { return }
        switch ticket.status {
        case .signedOut:
            started = false
            onSignedOut?()
            return
        case .unavailable:
            scheduleReconnect()
            return
        case .ok:
            break
        }
        let connection: WebSocketConnection
        do {
            connection = try await connector.connect(url: api.accountEventsURL(ticket.ticket, node: nodeName))
        } catch {
            scheduleReconnect()
            return
        }
        if attempt != generation || disposed || !started || socket != nil {
            await connection.close()
            return
        }
        socket = connection
        connected = true
        reconnectDelay = AccountEventChannel.initialReconnectDelay
        readerTask = Task { [weak self] in
            do {
                for try await message in connection.messages {
                    guard let self else { return }
                    self.handle(message)
                }
            } catch {}
            self?.onClosed(connection)
        }
        keepaliveTask?.cancel()
        let interval = keepaliveInterval
        let sleeper = self.sleeper
        keepaliveTask = Task { [weak self] in
            while !Task.isCancelled {
                do { try await sleeper(interval) } catch { return }
                if Task.isCancelled { return }
                self?.sendKeepalive()
            }
        }
        onConnected?()
    }

    private func handle(_ message: WebSocketMessage) {
        let text: String
        switch message {
        case .text(let value): text = value
        case .data(let data):
            guard let decoded = try? Utf8.decode(data) else { return }
            text = decoded
        }
        guard let frame = (try? JSON.parse(text))?.objectValue else { return }
        // The relay also sends a {"type":"hello","accepts":[...]} capability frame on accept, and
        // relays terminal frames; only event frames carry a topic.
        if frame["type"] != .string("event") {
            onFrame?(frame)
            return
        }
        let topic = frame.str("topic")
        if topic.isEmpty { return }
        onTopic?(topic)
    }

    private func sendKeepalive() {
        send(["type": "ping"])
    }

    /// The socket `connection` ended (or failed to send). Ignored when it is no longer current.
    private func onClosed(_ connection: WebSocketConnection) {
        guard let socket, socket === connection else { return }
        keepaliveTask?.cancel()
        keepaliveTask = nil
        readerTask?.cancel()
        readerTask = nil
        self.socket = nil
        connected = false
        Task { await connection.close() }
        if disposed || !started { return }
        scheduleReconnect()
    }

    private func teardownSocket() async {
        keepaliveTask?.cancel()
        keepaliveTask = nil
        readerTask?.cancel()
        readerTask = nil
        let current = socket
        socket = nil
        connected = false
        if let current { await current.close() }
    }

    private func scheduleReconnect() {
        if disposed || !started || reconnectTask != nil { return }
        let delay = reconnectDelay
        reconnectDelay = min(reconnectDelay * 2, AccountEventChannel.maxReconnectDelay)
        let sleeper = self.sleeper
        reconnectTask = Task { [weak self] in
            do { try await sleeper(delay) } catch { return }
            guard let self, !Task.isCancelled else { return }
            self.reconnectTask = nil
            if !self.disposed && self.started { await self.open() }
        }
    }
}
