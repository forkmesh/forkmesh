#if canImport(Network)
import Foundation
import Network

public struct NetworkFrameworkConnector: StreamConnector {
    public init() {}

    public func connect(to endpoint: Endpoint, timeout: Duration) async throws -> any ByteStream {
        guard let port = NWEndpoint.Port(rawValue: endpoint.port) else { throw ConnectionFailure.system(code: 0, message: "Invalid TCP port") }
        let connection = NWConnection(host: NWEndpoint.Host(endpoint.host), port: port, using: .tcp)
        let stream = NetworkByteStream(connection: connection)
        try await stream.start(timeout: timeout)
        return stream
    }
}

public struct NetworkFrameworkDatagramConnector: DatagramConnector {
    public init() {}

    public func open(to endpoint: Endpoint, timeout: Duration) async throws -> any DatagramSocket {
        guard let port = NWEndpoint.Port(rawValue: endpoint.port) else { throw ConnectionFailure.system(code: 0, message: "Invalid UDP port") }
        let connection = NWConnection(host: NWEndpoint.Host(endpoint.host), port: port, using: .udp)
        let socket = NetworkDatagramSocket(connection: connection)
        try await socket.start(timeout: timeout)
        return socket
    }
}

func startConnection(_ connection: NWConnection, timeout: Duration) async throws {
    try await withTimeout(timeout) {
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
                connection.stateUpdateHandler = { state in
                    switch state {
                    case .ready:
                        connection.stateUpdateHandler = nil
                        continuation.resume()
                    case .failed(let error):
                        connection.stateUpdateHandler = nil
                        continuation.resume(throwing: NetworkByteStream.failure(error))
                    case .cancelled:
                        connection.stateUpdateHandler = nil
                        continuation.resume(throwing: ConnectionFailure.cancelled)
                    default: break
                    }
                }
                connection.start(queue: .global(qos: .userInitiated))
            }
        } onCancel: {
            connection.cancel()
        }
    }
}

final class NetworkByteStream: ByteStream, @unchecked Sendable {
    private let connection: NWConnection
    private let started = OnceFlag()

    init(connection: NWConnection) { self.connection = connection }

    var remoteAddress: String? { connection.endpoint.debugDescription }

    func start(timeout: Duration) async throws {
        guard started.claim() else { return }
        try await startConnection(connection, timeout: timeout)
    }

    func send(_ data: Data, timeout: Duration) async throws {
        try await withTimeout(timeout) {
            try await withTaskCancellationHandler {
                try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
                    self.connection.send(content: data, completion: .contentProcessed { error in
                        if let error { continuation.resume(throwing: Self.failure(error)) } else { continuation.resume() }
                    })
                }
            } onCancel: {
                self.connection.cancel()
            }
        }
    }

    func receive(maximumLength: Int, timeout: Duration) async throws -> Data {
        try await withTimeout(timeout) {
            try await withTaskCancellationHandler {
                try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Data, Error>) in
                    self.connection.receive(minimumIncompleteLength: 1, maximumLength: maximumLength) { data, _, complete, error in
                        if let error { continuation.resume(throwing: Self.failure(error)); return }
                        continuation.resume(returning: data ?? Data())
                        if complete { self.connection.cancel() }
                    }
                }
            } onCancel: {
                self.connection.cancel()
            }
        }
    }

    func close() { connection.cancel() }

    static func failure(_ error: NWError) -> ConnectionFailure {
        switch error {
        case .posix(let code):
            switch code {
            case .ECONNREFUSED: return .refused
            case .ETIMEDOUT: return .timedOut
            case .EHOSTUNREACH: return .hostUnreachable
            case .ENETUNREACH: return .networkUnreachable
            case .ECONNRESET: return .resetByPeer
            case .ECANCELED: return .cancelled
            default: return .system(code: code.rawValue, message: error.debugDescription)
            }
        case .dns: return .nameResolution(error.debugDescription)
        case .tls: return .tls(error.debugDescription)
        @unknown default: return .system(code: 0, message: error.debugDescription)
        }
    }
}

final class NetworkDatagramSocket: DatagramSocket, @unchecked Sendable {
    private let connection: NWConnection
    private let started = OnceFlag()

    init(connection: NWConnection) { self.connection = connection }

    func start(timeout: Duration) async throws {
        guard started.claim() else { return }
        try await startConnection(connection, timeout: timeout)
    }

    func send(_ data: Data) async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            connection.send(content: data, completion: .contentProcessed { error in
                if let error { continuation.resume(throwing: NetworkByteStream.failure(error)) } else { continuation.resume() }
            })
        }
    }

    func receive(timeout: Duration) async throws -> Data {
        try await withTimeout(timeout) {
            try await withTaskCancellationHandler {
                try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Data, Error>) in
                    self.connection.receiveMessage { data, _, _, error in
                        if let error { continuation.resume(throwing: NetworkByteStream.failure(error)) } else { continuation.resume(returning: data ?? Data()) }
                    }
                }
            } onCancel: {
                self.connection.cancel()
            }
        }
    }

    func close() { connection.cancel() }
}
#endif
