// The WebSocket surface the relay, account-event and node-sync services rely
// on: `connect` returns once the handshake is done, frames arrive as a stream
// that ends when the socket closes, `send` sends a frame, `close` closes with
// a status code.

import FMBase
import Foundation

public enum WebSocketMessage: Equatable, Sendable {
    case text(String)
    case data(Data)

    public var text: String? {
        if case .text(let value) = self { return value }
        return nil
    }

    public var data: Data? {
        if case .data(let value) = self { return value }
        return nil
    }
}

/// WebSocket close status codes (RFC 6455).
public enum WebSocketStatus {
    public static let normalClosure = 1000
    public static let goingAway = 1001
    public static let protocolError = 1002
    public static let noStatusReceived = 1005
    public static let abnormalClosure = 1006
    public static let policyViolation = 1008
    public static let messageTooBig = 1009
    public static let internalServerError = 1011
}

public enum WebSocketError: Error, Equatable, Sendable, CustomStringConvertible {
    /// The opening handshake failed; `status` is the HTTP status of a non-101 answer, else 0.
    case handshakeFailed(status: Int, message: String)
    case connectTimedOut
    /// A send or ping on a socket that is no longer open.
    case notConnected
    /// The connection broke without a close frame.
    case transport(String)

    public var description: String {
        switch self {
        case .handshakeFailed(let status, let message):
            return status == 0 ? "WebSocket handshake failed: \(message)" : "WebSocket handshake failed: HTTP \(status)"
        case .connectTimedOut: return "WebSocket connect timed out"
        case .notConnected: return "WebSocket is not connected"
        case .transport(let message): return message
        }
    }
}

public struct WebSocketOptions: Equatable, Sendable {
    /// `IOWebSocketChannel.connect(connectTimeout:)`.
    public var connectTimeout: TimeInterval
    /// `IOWebSocketChannel.connect(pingInterval:)`: a ping every interval, and
    /// a socket whose pong has not arrived by the next one is closed with
    /// 1001. nil sends no pings.
    public var pingInterval: TimeInterval?
    /// Larger incoming messages close the socket.
    public var maxMessageBytes: Int

    public init(connectTimeout: TimeInterval = 15, pingInterval: TimeInterval? = nil, maxMessageBytes: Int = 16 * 1024 * 1024) {
        self.connectTimeout = connectTimeout
        self.pingInterval = pingInterval
        self.maxMessageBytes = maxMessageBytes
    }
}

public protocol WebSocketConnection: AnyObject, Sendable {
    /// The subprotocol the server chose, if any.
    var negotiatedProtocol: String? { get }

    /// Incoming messages, for one consumer. Finishes normally when either
    /// side closes (`onDone`); throws `WebSocketError.transport` when the
    /// connection breaks (`onError`).
    var messages: AsyncThrowingStream<WebSocketMessage, Error> { get }

    /// The peer's close code and reason once `messages` has finished; nil
    /// while open, and when the connection broke without a close frame.
    var closeCode: Int? { get }
    var closeReason: String? { get }

    func send(_ message: WebSocketMessage) async throws

    /// Resolves when the pong arrives.
    func ping() async throws

    /// Idempotent. `messages` finishes afterwards.
    func close(code: Int, reason: String) async
}

extension WebSocketConnection {
    public func send(text: String) async throws { try await send(.text(text)) }
    public func send(data: Data) async throws { try await send(.data(data)) }

    /// `sink.add(jsonEncode(frame))`.
    public func send(json: JSONValue) async throws { try await send(.text(JSON.encode(json))) }

    public func close() async { await close(code: WebSocketStatus.normalClosure, reason: "") }
}

public protocol WebSocketConnector: Sendable {
    /// Returns once the opening handshake succeeded; throws `WebSocketError` otherwise.
    func connect(
        url: URL, headers: HTTPHeaders, protocols: [String], options: WebSocketOptions
    ) async throws -> WebSocketConnection
}

extension WebSocketConnector {
    /// `WebSocketChannel.connect(uri)` followed by `await channel.ready`.
    public func connect(url: URL, options: WebSocketOptions = WebSocketOptions()) async throws -> WebSocketConnection {
        try await connect(url: url, headers: [:], protocols: [], options: options)
    }
}
