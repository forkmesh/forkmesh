import Foundation
import NexusCore
import NexusDNS

public struct ScanProgress: Sendable, Hashable {
    public var completed: Int
    public var total: Int
    public var result: PortResult?

    public init(completed: Int, total: Int, result: PortResult? = nil) {
        self.completed = completed
        self.total = total
        self.result = result
    }

    public var fraction: Double { total == 0 ? 1 : Double(completed) / Double(total) }
}
public protocol ServiceInspecting: Sendable {
    func inspect(host: String, port: UInt16, timeout: Duration) async -> ServiceIdentity?
}

public struct NativePortScanner: Sendable {
    public let connector: any StreamConnector
    public let inspector: any ServiceInspecting
    public let resolver: any HostResolving

    public init(connector: any StreamConnector = Transports.defaultStreamConnector, inspector: (any ServiceInspecting)? = nil, resolver: any HostResolving = DNSResolver()) {
        self.connector = connector
        self.inspector = inspector ?? ServiceInspector(connector: connector)
        self.resolver = resolver
    }

    public func scan(target: TargetSpecification, ports: PortList, options: ScanOptions = .default, authorization: ScanAuthorizationPolicy = .default, attestation: AuthorizationAttestation? = nil) async throws -> AsyncThrowingStream<ScanProgress, Error> {
        let hosts = try await resolve(target: target, timeout: options.timeout)
        let addresses = hosts.flatMap(\.addresses)
        switch authorization.evaluate(addresses: addresses, portCount: ports.count, isRange: target.containsRange, attestation: attestation) {
        case .allowed: break
        case .requiresAttestation(let message), .denied(let message): throw ConnectionFailure.unsupported(message)
        }
        let jobs = hosts.flatMap { resolved in resolved.addresses.map { address in (resolved.name, address) } }.flatMap { host, address in ports.ports.map { (host, address, $0) } }
        let settings = options.clamped
        return AsyncThrowingStream { continuation in
            let task = Task {
                let completed = Locked(0)
                await BoundedConcurrency.run(jobs, maximumConcurrency: settings.maximumConcurrency, operation: { job in
                    await probe(host: job.0, address: job.1, port: job.2, options: settings)
                }, onOutput: { result in
                    let count = completed.withLock { value in value += 1; return value }
                    continuation.yield(ScanProgress(completed: count, total: jobs.count, result: result))
                })
                continuation.finish()
            }
            continuation.onTermination = { _ in task.cancel() }
        }
    }

    public func run(target: TargetSpecification, ports: PortList, options: ScanOptions = .default, authorization: ScanAuthorizationPolicy = .default, attestation: AuthorizationAttestation? = nil, progress: @escaping @Sendable (ScanProgress) async -> Void) async throws -> ScanSession {
        let stream = try await scan(target: target, ports: ports, options: options, authorization: authorization, attestation: attestation)
        var session = ScanSession(target: target.raw, ports: ports, options: options.clamped, attestation: attestation)
        do {
            for try await update in stream {
                guard let result = update.result else { continue }
                session.results.append(result)
                session.counts[result.state] += 1
                await progress(update)
            }
            session.status = Task.isCancelled ? .cancelled : .completed
        } catch is CancellationError {
            session.status = .cancelled
        } catch {
            session.status = .failed(String(describing: error))
            session.finishedAt = Date()
            throw error
        }
        session.finishedAt = Date()
        return session
    }

    private func resolve(target: TargetSpecification, timeout: Duration) async throws -> [ResolvedHost] {
        var resolved: [ResolvedHost] = []
        for host in target.hosts() {
            try Task.checkCancellation()
            switch host {
            case .address(let address): resolved.append(ResolvedHost(name: address.description, addresses: [address]))
            case .hostname(let name): resolved.append(try await resolver.resolve(host: name, timeout: timeout))
            }
        }
        return resolved
    }

    private func probe(host: String, address: IPAddr, port: UInt16, options: ScanOptions) async -> PortResult {
        let endpoint = Endpoint(host: address.description, port: port)
        let started = ContinuousClock.now
        do {
            let stream = try await connector.connect(to: endpoint, timeout: options.timeout)
            stream.close()
            let latency = started.duration(to: .now).milliseconds
            let service = options.inspectServices ? await inspector.inspect(host: host, port: port, timeout: options.inspectionTimeout) : ServiceIdentity.inferred(port: port)
            return PortResult(host: host, address: address.description, port: port, state: .open, latencyMilliseconds: latency, service: service)
        } catch let failure as ConnectionFailure {
            if failure == .timedOut, options.retryTimeouts, !Task.isCancelled {
                return await probe(host: host, address: address, port: port, options: ScanOptions(timeoutSeconds: min(options.timeoutSeconds * 2, 30), maximumConcurrency: options.maximumConcurrency, inspectServices: options.inspectServices, inspectionTimeoutSeconds: options.inspectionTimeoutSeconds, retryTimeouts: false, recordClosedPorts: options.recordClosedPorts, preferIPv4: options.preferIPv4))
            }
            return PortResult(host: host, address: address.description, port: port, state: failure.portState, failure: failure)
        } catch {
            let failure = ConnectionFailure.system(code: 0, message: String(describing: error))
            return PortResult(host: host, address: address.description, port: port, state: .indeterminate, failure: failure)
        }
    }
}

public struct ServiceInspector: ServiceInspecting {
    public let connector: any StreamConnector

    public init(connector: any StreamConnector = Transports.defaultStreamConnector) { self.connector = connector }

    public func inspect(host: String, port: UInt16, timeout: Duration) async -> ServiceIdentity? {
        let endpoint = Endpoint(host: host, port: port)
        do {
            let stream = try await connector.connect(to: endpoint, timeout: timeout)
            defer { stream.close() }
            if [80, 81, 3000, 5000, 8000, 8080, 8081, 8088, 8888, 9000, 9090].contains(port) {
                try await stream.send(Data("HEAD / HTTP/1.1\r\nHost: \(host)\r\nConnection: close\r\n\r\n".utf8), timeout: timeout)
            }
            let data = try await stream.receive(until: { $0.contains(0x0A) || $0.count >= 4096 }, maximumLength: 4096, timeout: timeout)
            return identify(data: data, port: port)
        } catch {
            return ServiceIdentity.inferred(port: port)
        }
    }

    func identify(data: Data, port: UInt16) -> ServiceIdentity? {
        let text = String(decoding: data, as: UTF8.self).trimmingCharacters(in: .controlCharacters.union(.whitespacesAndNewlines))
        if text.hasPrefix("HTTP/") {
            let first = text.split(separator: "\n").first.map(String.init) ?? "HTTP response"
            return ServiceIdentity(name: "http", title: "HTTP", evidence: .confirmed, basis: "The service returned \(first).", banner: String(text.prefix(1024)))
        }
        let upper = text.uppercased()
        let name: String? = text.hasPrefix("SSH-") ? "ssh" : upper.hasPrefix("220") && upper.contains("FTP") ? "ftp" : upper.hasPrefix("220") && (upper.contains("SMTP") || upper.contains("ESMTP")) ? "smtp" : nil
        if let name {
            let title = name == "ssh" ? "SSH" : name.uppercased()
            return ServiceIdentity(name: name, title: title, evidence: .confirmed, basis: "The service returned its protocol greeting.", banner: String(text.prefix(1024)))
        }
        return ServiceIdentity.inferred(port: port)
    }
}
