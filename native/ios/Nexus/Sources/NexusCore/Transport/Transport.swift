import Foundation

public struct Endpoint: Hashable, Sendable, Codable, CustomStringConvertible {
    public var host: String
    public var port: UInt16

    public init(host: String, port: UInt16) {
        self.host = host
        self.port = port
    }

    public var description: String {
        host.contains(":") && !host.hasPrefix("[") ? "[\(host)]:\(port)" : "\(host):\(port)"
    }
}

public protocol ByteStream: AnyObject, Sendable {
    var remoteAddress: String? { get }
    func send(_ data: Data, timeout: Duration) async throws
    func receive(maximumLength: Int, timeout: Duration) async throws -> Data
    func close()
}

public protocol StreamConnector: Sendable {
    func connect(to endpoint: Endpoint, timeout: Duration) async throws -> any ByteStream
}

public protocol DatagramSocket: AnyObject, Sendable {
    func send(_ data: Data) async throws
    func receive(timeout: Duration) async throws -> Data
    func close()
}

public protocol DatagramConnector: Sendable {
    func open(to endpoint: Endpoint, timeout: Duration) async throws -> any DatagramSocket
}

public protocol HostResolving: Sendable {
    func resolve(host: String, timeout: Duration) async throws -> ResolvedHost
}

public enum Transports {
    public static var defaultStreamConnector: any StreamConnector {
        #if canImport(Network)
        return NetworkFrameworkConnector()
        #else
        return UnsupportedStreamConnector()
        #endif
    }

    public static var defaultDatagramConnector: any DatagramConnector {
        #if canImport(Network)
        return NetworkFrameworkDatagramConnector()
        #else
        return UnsupportedDatagramConnector()
        #endif
    }
}

public struct UnsupportedStreamConnector: StreamConnector {
    public init() {}

    public func connect(to endpoint: Endpoint, timeout: Duration) async throws -> any ByteStream {
        throw ConnectionFailure.unsupported("TCP connections require Network.framework on this platform.")
    }
}

public struct UnsupportedDatagramConnector: DatagramConnector {
    public init() {}

    public func open(to endpoint: Endpoint, timeout: Duration) async throws -> any DatagramSocket {
        throw ConnectionFailure.unsupported("UDP connections require Network.framework on this platform.")
    }
}

extension ByteStream {
    public func receive(until predicate: (Data) -> Bool, maximumLength: Int, timeout: Duration) async throws -> Data {
        let clock = ContinuousClock()
        let deadline = clock.now.advanced(by: timeout)
        var buffer = Data()
        while buffer.count < maximumLength && !predicate(buffer) {
            let remaining = clock.now.duration(to: deadline)
            guard remaining > .zero else { break }
            let chunk: Data
            do {
                chunk = try await receive(maximumLength: maximumLength - buffer.count, timeout: remaining)
            } catch ConnectionFailure.timedOut where !buffer.isEmpty {
                break
            }
            if chunk.isEmpty { break }
            buffer.append(chunk)
        }
        return buffer
    }
}

extension Duration {
    public var timeInterval: TimeInterval {
        let parts = components
        return TimeInterval(parts.seconds) + TimeInterval(parts.attoseconds) / 1e18
    }

    public var milliseconds: Double { timeInterval * 1000 }

    public static func interval(_ seconds: TimeInterval) -> Duration {
        .nanoseconds(Int64(max(seconds, 0) * 1e9))
    }
}
