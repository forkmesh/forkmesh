// A scriptable `WebSocketConnector` for tests, public and in the module so
// the relay, account-event and node-sync tests of other modules can use it.

import FMBase
import Foundation

public final class FakeWebSocketConnector: WebSocketConnector, @unchecked Sendable {
    public struct Attempt: Sendable {
        public let url: URL
        public let headers: HTTPHeaders
        public let protocols: [String]
        public let options: WebSocketOptions
    }

    private let lock = NSLock()
    private var recordedAttempts: [Attempt] = []
    private var opened: [FakeWebSocketConnection] = []
    private var failures: [Error] = []
    private var refuseAll: Error?
    private var prepare: (@Sendable (FakeWebSocketConnection) -> Void)?

    public init() {}

    /// Every `connect` call, oldest first, whether it succeeded or not.
    public var attempts: [Attempt] { locked { recordedAttempts } }

    /// Every connection handed out, oldest first.
    public var connections: [FakeWebSocketConnection] { locked { opened } }

    public var lastConnection: FakeWebSocketConnection? { connections.last }

    /// The next `connect` throws `error`; several calls queue up.
    public func failNext(_ error: Error = WebSocketError.handshakeFailed(status: 0, message: "refused")) {
        locked { failures.append(error) }
    }

    /// Every `connect` throws until called with nil.
    public func refuseConnections(_ error: Error?) { locked { refuseAll = error } }

    /// Runs on each new connection before `connect` returns: the place to
    /// queue greeting frames or install an `onSend` responder.
    public func onConnect(_ prepare: (@Sendable (FakeWebSocketConnection) -> Void)?) {
        locked { self.prepare = prepare }
    }

    public func connect(
        url: URL, headers: HTTPHeaders, protocols: [String], options: WebSocketOptions
    ) async throws -> WebSocketConnection {
        try Task.checkCancellation()
        let (connection, prepare) = try open(Attempt(url: url, headers: headers, protocols: protocols, options: options))
        prepare?(connection)
        return connection
    }

    private func open(_ attempt: Attempt) throws -> (FakeWebSocketConnection, (@Sendable (FakeWebSocketConnection) -> Void)?) {
        try locked {
            recordedAttempts.append(attempt)
            if !failures.isEmpty { throw failures.removeFirst() }
            if let refuseAll { throw refuseAll }
            let connection = FakeWebSocketConnection(url: attempt.url, negotiatedProtocol: attempt.protocols.first)
            opened.append(connection)
            return (connection, prepare)
        }
    }

    /// Polls until `count` connections were opened; false on timeout.
    public func waitForConnections(_ count: Int, timeout: TimeInterval = 2) async -> Bool {
        await poll(timeout) { self.connections.count >= count }
    }

    private func locked<T>(_ body: () throws -> T) rethrows -> T {
        lock.lock()
        defer { lock.unlock() }
        return try body()
    }
}

public final class FakeWebSocketConnection: WebSocketConnection, @unchecked Sendable {
    public let url: URL
    public let negotiatedProtocol: String?
    public let messages: AsyncThrowingStream<WebSocketMessage, Error>

    private let incoming: AsyncThrowingStream<WebSocketMessage, Error>.Continuation
    private let lock = NSLock()
    private var outgoing: [WebSocketMessage] = []
    private var pings = 0
    private var open = true
    private var code: Int?
    private var reason: String?
    private var clientClose: (code: Int, reason: String)?
    private var responder: (@Sendable (FakeWebSocketConnection, WebSocketMessage) -> Void)?
    private var sendFailure: Error?

    public init(url: URL, negotiatedProtocol: String? = nil) {
        self.url = url
        self.negotiatedProtocol = negotiatedProtocol
        var continuation: AsyncThrowingStream<WebSocketMessage, Error>.Continuation!
        messages = AsyncThrowingStream { continuation = $0 }
        incoming = continuation
    }

    // MARK: What the code under test did

    /// Everything the client sent, oldest first.
    public var sent: [WebSocketMessage] { locked { outgoing } }
    public var sentTexts: [String] { sent.compactMap(\.text) }

    /// Sent text frames decoded as JSON objects (anything else is skipped).
    public var sentJSON: [JSONObject] { sentTexts.compactMap { (try? JSON.parse($0))?.objectValue } }

    public var pingCount: Int { locked { pings } }
    public var isOpen: Bool { locked { open } }
    public var closeCode: Int? { locked { code } }
    public var closeReason: String? { locked { reason } }

    /// The code and reason the client closed with, or nil if the client has not closed.
    public var closedByClient: (code: Int, reason: String)? { locked { clientClose } }

    // MARK: Playing the server

    public func receive(_ message: WebSocketMessage) { incoming.yield(message) }
    public func receive(text: String) { receive(.text(text)) }
    public func receive(json: JSONValue) { receive(.text(JSON.encode(json))) }

    /// The server closes: `messages` finishes and later sends throw.
    public func serverClose(code: Int = WebSocketStatus.normalClosure, reason: String = "") {
        let first: Bool = locked {
            guard open else { return false }
            open = false
            self.code = code
            self.reason = reason
            return true
        }
        if first { incoming.finish() }
    }

    /// The connection breaks: `messages` throws.
    public func fail(_ error: Error = WebSocketError.transport("connection reset")) {
        let first: Bool = locked {
            guard open else { return false }
            open = false
            return true
        }
        if first { incoming.finish(throwing: error) }
    }

    /// Called for each message the client sends, e.g. to answer a ping frame.
    public func onSend(_ responder: (@Sendable (FakeWebSocketConnection, WebSocketMessage) -> Void)?) {
        locked { self.responder = responder }
    }

    /// Sends throw `error` while it is set, without closing the socket.
    public func failSends(with error: Error?) { locked { sendFailure = error } }

    /// Polls until the client has sent `count` messages; false on timeout.
    public func waitForSent(_ count: Int, timeout: TimeInterval = 2) async -> Bool {
        await poll(timeout) { self.sent.count >= count }
    }

    public func waitUntilClosed(timeout: TimeInterval = 2) async -> Bool {
        await poll(timeout) { !self.isOpen }
    }

    // MARK: WebSocketConnection

    public func send(_ message: WebSocketMessage) async throws {
        let responder = try record(message)
        responder?(self, message)
    }

    private func record(_ message: WebSocketMessage) throws -> (@Sendable (FakeWebSocketConnection, WebSocketMessage) -> Void)? {
        try locked {
            guard open else { throw WebSocketError.notConnected }
            if let sendFailure { throw sendFailure }
            outgoing.append(message)
            return responder
        }
    }

    public func ping() async throws {
        try countPing()
    }

    private func countPing() throws {
        try locked {
            guard open else { throw WebSocketError.notConnected }
            pings += 1
        }
    }

    public func close(code: Int, reason: String) async {
        let first = markClosed(code: code, reason: reason)
        if first { incoming.finish() }
    }

    private func markClosed(code: Int, reason: String) -> Bool {
        locked {
            guard open else { return false }
            open = false
            clientClose = (code, reason)
            // A well-behaved server echoes the close code.
            self.code = code
            self.reason = reason
            return true
        }
    }

    private func locked<T>(_ body: () throws -> T) rethrows -> T {
        lock.lock()
        defer { lock.unlock() }
        return try body()
    }
}

/// Test helper: re-checks `condition` every few milliseconds.
func poll(_ timeout: TimeInterval, _ condition: @escaping @Sendable () -> Bool) async -> Bool {
    let deadline = Date().addingTimeInterval(timeout)
    while !condition() {
        if Date() >= deadline { return false }
        try? await Task.sleep(nanoseconds: 5_000_000)
    }
    return true
}
