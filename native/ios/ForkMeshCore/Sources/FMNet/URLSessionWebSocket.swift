// The production `WebSocketConnector`, over URLSessionWebSocketTask: the OS
// speaks RFC 6455, this file only adapts it to the surface in WebSocket.swift.
//
// Apple platforms only. swift-corelibs-foundation declares the same API, but
// whether it works depends on how the host's libcurl was built, so Linux
// builds get the protocol and the fake; FM_LINUX_WEBSOCKET exists to
// type-check this file on a machine without a Mac.

#if canImport(Darwin) || FM_LINUX_WEBSOCKET
import FMBase
import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif

public final class URLSessionWebSocketConnector: WebSocketConnector, @unchecked Sendable {
    private let router = SocketRouter()
    private let session: URLSession

    public init(configuration: URLSessionConfiguration = URLSessionHTTPClient.defaultConfiguration) {
        session = URLSession(configuration: configuration, delegate: router, delegateQueue: nil)
    }

    deinit { session.invalidateAndCancel() }

    public func connect(
        url: URL, headers: HTTPHeaders, protocols: [String], options: WebSocketOptions
    ) async throws -> WebSocketConnection {
        try Task.checkCancellation()
        var request = URLRequest(url: url)
        request.timeoutInterval = options.connectTimeout
        for field in headers { request.addValue(field.value, forHTTPHeaderField: field.name) }
        // webSocketTask(with:protocols:) takes no request, so no headers; the
        // header form is what it sets anyway.
        if !protocols.isEmpty {
            request.setValue(protocols.joined(separator: ", "), forHTTPHeaderField: "Sec-WebSocket-Protocol")
        }
        let task = session.webSocketTask(with: request)
        task.maximumMessageSize = options.maxMessageBytes
        let connection = URLSessionWebSocketConnection(task: task, options: options)
        router.register(connection, for: task)
        try await withTaskCancellationHandler {
            try await connection.open()
        } onCancel: {
            task.cancel()
        }
        return connection
    }
}

final class URLSessionWebSocketConnection: WebSocketConnection, @unchecked Sendable {
    private enum State { case connecting, open, closed }

    let messages: AsyncThrowingStream<WebSocketMessage, Error>

    private let task: URLSessionWebSocketTask
    private let options: WebSocketOptions
    private let incoming: AsyncThrowingStream<WebSocketMessage, Error>.Continuation
    private let lock = NSLock()
    private var state = State.connecting
    private var opening: CheckedContinuation<Void, Error>?
    private var chosenProtocol: String?
    private var code: Int?
    private var reason: String?
    private var awaitingPong = false
    private var pinger: Task<Void, Never>?

    init(task: URLSessionWebSocketTask, options: WebSocketOptions) {
        self.task = task
        self.options = options
        var continuation: AsyncThrowingStream<WebSocketMessage, Error>.Continuation!
        messages = AsyncThrowingStream { continuation = $0 }
        incoming = continuation
    }

    var negotiatedProtocol: String? { locked { chosenProtocol } }
    var closeCode: Int? { locked { code } }
    var closeReason: String? { locked { reason } }

    // MARK: Opening

    func open() async throws {
        let timeout = DispatchWorkItem { [weak self] in self?.failOpening(WebSocketError.connectTimedOut) }
        defer { timeout.cancel() }
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            locked { opening = continuation }
            // Armed only once the continuation is stored, so it cannot fire into nothing.
            DispatchQueue.global().asyncAfter(deadline: .now() + options.connectTimeout, execute: timeout)
            task.resume()
        }
    }

    func didOpen(protocol chosen: String?) {
        let continuation: CheckedContinuation<Void, Error>? = locked {
            guard state == .connecting else { return nil }
            state = .open
            chosenProtocol = (chosen ?? "").isEmpty ? nil : chosen
            defer { opening = nil }
            return opening
        }
        guard let continuation else { return }
        receiveNext()
        startPinging()
        continuation.resume()
    }

    private func failOpening(_ error: Error) {
        let continuation: CheckedContinuation<Void, Error>? = locked {
            guard state == .connecting else { return nil }
            state = .closed
            defer { opening = nil }
            return opening
        }
        guard let continuation else { return }
        task.cancel()
        incoming.finish(throwing: error)
        continuation.resume(throwing: error)
    }

    // MARK: Delegate events

    func didClose(code: Int, reason: Data?) {
        finish(code: code, reason: reason.map { String(decoding: $0, as: UTF8.self) } ?? "", error: nil)
    }

    func didComplete(error: Error?) {
        if locked({ state == .connecting }) {
            let status = (task.response as? HTTPURLResponse)?.statusCode ?? 0
            let message = error?.localizedDescription ?? "connection closed"
            // A 101 that then failed is not an HTTP refusal.
            failOpening(WebSocketError.handshakeFailed(status: status == 101 ? 0 : status, message: message))
            return
        }
        finish(code: nil, reason: nil, error: error)
    }

    /// Ends `messages` once: normally after a close frame from either side,
    /// with an error when the connection simply broke.
    private func finish(code: Int?, reason: String?, error: Error?) {
        let (first, sawClose): (Bool, Bool) = locked {
            if let code, self.code == nil {
                self.code = code
                self.reason = reason ?? ""
            }
            guard state == .open else { return (false, false) }
            state = .closed
            return (true, self.code != nil)
        }
        guard first else { return }
        pinger?.cancel()
        if let error, !sawClose {
            incoming.finish(throwing: WebSocketError.transport(error.localizedDescription))
        } else {
            incoming.finish()
        }
    }

    // MARK: Receiving

    private func receiveNext() {
        task.receive { [weak self] result in
            guard let self else { return }
            switch result {
            case .success(let message):
                switch message {
                case .string(let text): self.incoming.yield(.text(text))
                case .data(let data): self.incoming.yield(.data(data))
                @unknown default: break
                }
                if self.locked({ self.state == .open }) { self.receiveNext() }
            case .failure(let error):
                // The close frame, when there was one, is reported through the
                // delegate; its code is on the task by now.
                let raw = self.task.closeCode.rawValue
                if raw != URLSessionWebSocketTask.CloseCode.invalid.rawValue {
                    self.didClose(code: raw, reason: self.task.closeReason)
                } else {
                    self.finish(code: nil, reason: nil, error: error)
                }
            }
        }
    }

    // MARK: WebSocketConnection

    func send(_ message: WebSocketMessage) async throws {
        guard locked({ state == .open }) else { throw WebSocketError.notConnected }
        do {
            switch message {
            case .text(let text): try await task.send(.string(text))
            case .data(let data): try await task.send(.data(data))
            }
        } catch {
            throw locked({ state == .open })
                ? WebSocketError.transport(error.localizedDescription) : WebSocketError.notConnected
        }
    }

    func ping() async throws {
        guard locked({ state == .open }) else { throw WebSocketError.notConnected }
        // The handler has been seen to run twice when a task is cancelled
        // mid-ping; a continuation must resume once.
        let once = Once()
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            task.sendPing { error in
                guard once.claim() else { return }
                if let error {
                    continuation.resume(throwing: WebSocketError.transport(error.localizedDescription))
                } else {
                    continuation.resume()
                }
            }
        }
    }

    func close(code: Int, reason: String) async {
        guard locked({ state == .open }) else { return }
        let closeCode = URLSessionWebSocketTask.CloseCode(rawValue: code) ?? .normalClosure
        task.cancel(with: closeCode, reason: reason.isEmpty ? nil : Data(reason.utf8))
        finish(code: code, reason: reason, error: nil)
    }

    // MARK: Keep-alive

    /// Pings every `pingInterval`: a socket the OS silently dropped
    /// would otherwise look connected until the next send.
    private func startPinging() {
        guard let interval = options.pingInterval, interval > 0 else { return }
        pinger = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: UInt64(interval * 1_000_000_000))
                guard let self, !Task.isCancelled, self.locked({ self.state == .open }) else { return }
                let missed: Bool = self.locked {
                    let missed = self.awaitingPong
                    self.awaitingPong = true
                    return missed
                }
                if missed {
                    await self.close(code: WebSocketStatus.goingAway, reason: "")
                    return
                }
                self.task.sendPing { [weak self] error in
                    if error == nil { self?.locked { self?.awaitingPong = false } }
                }
            }
        }
    }

    private func locked<T>(_ body: () throws -> T) rethrows -> T {
        lock.lock()
        defer { lock.unlock() }
        return try body()
    }
}

private final class Once: @unchecked Sendable {
    private let lock = NSLock()
    private var claimed = false

    func claim() -> Bool {
        lock.lock()
        defer { lock.unlock() }
        if claimed { return false }
        claimed = true
        return true
    }
}

private final class SocketRouter: NSObject, URLSessionWebSocketDelegate, @unchecked Sendable {
    private let lock = NSLock()
    private var connections: [Int: URLSessionWebSocketConnection] = [:]

    func register(_ connection: URLSessionWebSocketConnection, for task: URLSessionTask) {
        lock.lock()
        connections[task.taskIdentifier] = connection
        lock.unlock()
    }

    private func connection(for task: URLSessionTask, remove: Bool = false) -> URLSessionWebSocketConnection? {
        lock.lock()
        defer { lock.unlock() }
        return remove ? connections.removeValue(forKey: task.taskIdentifier) : connections[task.taskIdentifier]
    }

    func urlSession(
        _ session: URLSession, webSocketTask: URLSessionWebSocketTask, didOpenWithProtocol protocol: String?
    ) {
        connection(for: webSocketTask)?.didOpen(protocol: `protocol`)
    }

    func urlSession(
        _ session: URLSession, webSocketTask: URLSessionWebSocketTask,
        didCloseWith closeCode: URLSessionWebSocketTask.CloseCode, reason: Data?
    ) {
        connection(for: webSocketTask)?.didClose(code: closeCode.rawValue, reason: reason)
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        connection(for: task, remove: true)?.didComplete(error: error)
    }
}
#endif
