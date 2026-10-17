import Foundation
import NexusCore
import NexusDNS

public struct ConnectivityToolkit: Sendable {
    public let connector: any StreamConnector
    public let resolver: any HostResolving

    public init(connector: any StreamConnector = Transports.defaultStreamConnector, resolver: any HostResolving = DNSResolver()) {
        self.connector = connector
        self.resolver = resolver
    }

    public func reachability(host: String, port: UInt16, timeout: Duration = .seconds(3)) async -> ConnectivityResult {
        let endpoint = Endpoint(host: host, port: port)
        let started = ContinuousClock.now
        do {
            async let resolution = resolver.resolve(host: host, timeout: timeout)
            let stream = try await connector.connect(to: endpoint, timeout: timeout)
            stream.close()
            let resolved = try await resolution
            return ConnectivityResult(endpoint: endpoint, reachable: true, latencyMilliseconds: started.duration(to: .now).milliseconds, resolvedAddresses: resolved.addresses)
        } catch let failure as ConnectionFailure {
            return ConnectivityResult(endpoint: endpoint, reachable: false, failure: failure)
        } catch {
            return ConnectivityResult(endpoint: endpoint, reachable: false, failure: .system(code: 0, message: String(describing: error)))
        }
    }

    public func monitor(host: String, port: UInt16, interval: Duration = .seconds(10), timeout: Duration = .seconds(3)) -> AsyncStream<ConnectivityResult> {
        AsyncStream { continuation in
            let task = Task {
                while !Task.isCancelled {
                    continuation.yield(await reachability(host: host, port: port, timeout: timeout))
                    do { try await Task.sleep(for: interval) } catch { break }
                }
                continuation.finish()
            }
            continuation.onTermination = { _ in task.cancel() }
        }
    }

    public func tracerouteAvailability() -> String {
        "Traceroute requires raw packet controls that iOS does not provide to App Store apps. Use an approved remote scanner for hop-by-hop traces."
    }
}
