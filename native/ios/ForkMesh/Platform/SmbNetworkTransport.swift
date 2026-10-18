import FMFiles
import Foundation
import Network

final class SmbNetworkTransport: SmbTransport, @unchecked Sendable {
    private let connection: NWConnection
    private let queue = DispatchQueue(label: "com.forkmesh.files.smb")
    private let host: String
    private var pending: [UInt8] = []

    static let connectTimeout: TimeInterval = 10
    static let readTimeout: TimeInterval = 60

    private init(connection: NWConnection, host: String) {
        self.connection = connection
        self.host = host
    }

    static func connector() -> SmbConnector {
        { host, port in try await open(host: host, port: port) }
    }

    static func open(host: String, port: UInt16) async throws -> SmbNetworkTransport {
        let addresses = await Task.detached { OpenWrtHTTPClient.resolve(host) }.value
        guard !addresses.isEmpty else { throw SmbError(message: "\(host) could not be found on your network.") }
        guard SmbAddress.allLocal(addresses) else { throw SmbError.notLocal }
        let chosen = addresses.first { !$0.contains(":") } ?? addresses[0]
        guard let endpointPort = NWEndpoint.Port(rawValue: port) else { throw SmbError.unreachable(host) }
        let tcp = NWProtocolTCP.Options()
        tcp.connectionTimeout = Int(connectTimeout)
        tcp.noDelay = true
        let connection = NWConnection(host: NWEndpoint.Host(chosen), port: endpointPort, using: NWParameters(tls: nil, tcp: tcp))
        let transport = SmbNetworkTransport(connection: connection, host: host)
        try await transport.start()
        return transport
    }

    private func start() async throws {
        let once = Once()
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            connection.stateUpdateHandler = { [host] state in
                switch state {
                case .ready:
                    if once.claim() { continuation.resume() }
                case .failed, .cancelled, .waiting:
                    if once.claim() { continuation.resume(throwing: SmbError.unreachable(host)) }
                default:
                    break
                }
            }
            connection.start(queue: queue)
            queue.asyncAfter(deadline: .now() + Self.connectTimeout + 1) { [connection, host] in
                if once.claim() {
                    connection.cancel()
                    continuation.resume(throwing: SmbError.unreachable(host))
                }
            }
        }
        connection.stateUpdateHandler = nil
    }

    func write(_ bytes: [UInt8]) async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            connection.send(content: Data(bytes), completion: .contentProcessed { error in
                if error != nil {
                    continuation.resume(throwing: SmbTransportError("send failed"))
                } else {
                    continuation.resume()
                }
            })
        }
    }

    func read(_ count: Int) async throws -> [UInt8] {
        while pending.count < count {
            pending += try await receive()
        }
        let chunk = Array(pending.prefix(count))
        pending.removeFirst(count)
        return chunk
    }

    private func receive() async throws -> [UInt8] {
        let once = Once()
        return try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<[UInt8], Error>) in
            connection.receive(minimumIncompleteLength: 1, maximumLength: 1 << 17) { data, _, complete, error in
                guard once.claim() else { return }
                if let data, !data.isEmpty {
                    continuation.resume(returning: [UInt8](data))
                } else if error != nil || complete {
                    continuation.resume(throwing: SmbTransportError("closed"))
                } else {
                    continuation.resume(returning: [])
                }
            }
            queue.asyncAfter(deadline: .now() + Self.readTimeout) { [connection] in
                if once.claim() {
                    connection.cancel()
                    continuation.resume(throwing: SmbTransportError("timed out"))
                }
            }
        }
    }

    func close() {
        connection.cancel()
    }
}

private final class Once: @unchecked Sendable {
    private let lock = NSLock()
    private var done = false

    func claim() -> Bool {
        lock.lock()
        defer { lock.unlock() }
        if done { return false }
        done = true
        return true
    }
}
