import Foundation
import NexusCore
import NexusDNS
import NexusWeb
import NexusConnectivity
import NexusScan
import NexusDiscovery
import NexusRemote

public struct NexusConfiguration: Sendable {
    public var authorization: ScanAuthorizationPolicy
    public var defaultScanOptions: ScanOptions
    public var remoteScanner: RemoteScannerConfiguration?

    public init(authorization: ScanAuthorizationPolicy = .default, defaultScanOptions: ScanOptions = .default, remoteScanner: RemoteScannerConfiguration? = nil) {
        self.authorization = authorization
        self.defaultScanOptions = defaultScanOptions
        self.remoteScanner = remoteScanner
    }
}

public actor NexusClient {
    public let configuration: NexusConfiguration
    public let discovery: DiscoveryEngine
    public let scanner: NativePortScanner
    public let dns: DNSResolver
    public let web: HTTPInspector
    public let connectivity: ConnectivityToolkit

    public init(configuration: NexusConfiguration = .init(), streamConnector: any StreamConnector = Transports.defaultStreamConnector, datagramConnector: any DatagramConnector = Transports.defaultDatagramConnector, browser: any BonjourBrowsing = BonjourBrowser()) {
        let resolver = DNSResolver(connector: datagramConnector)
        self.configuration = configuration
        self.discovery = DiscoveryEngine(browser: browser)
        self.scanner = NativePortScanner(connector: streamConnector, resolver: resolver)
        self.dns = resolver
        self.web = HTTPInspector()
        self.connectivity = ConnectivityToolkit(connector: streamConnector, resolver: resolver)
    }

    public func discover(options: DiscoveryOptions = .init()) -> AsyncThrowingStream<DiscoveryProgress, Error> {
        discovery.discover(options: options)
    }

    public func scan(target: String, ports: PortList = PortPreset.quick.ports, options: ScanOptions? = nil, attestation: AuthorizationAttestation? = nil) async throws -> AsyncThrowingStream<ScanProgress, Error> {
        let specification = try TargetSpecification.parse(target, maximumHosts: configuration.authorization.maximumHostsPerScan)
        return try await scanner.scan(target: specification, ports: ports, options: options ?? configuration.defaultScanOptions, authorization: configuration.authorization, attestation: attestation)
    }

    public func dnsLookup(_ name: String, type: DNSRecordType) async throws -> DNSLookup { try await dns.lookup(name: name, type: type) }
    public func inspectWeb(_ url: URL) async throws -> HTTPInspection { try await web.inspect(url: url) }
    public func checkConnectivity(host: String, port: UInt16) async -> ConnectivityResult { await connectivity.reachability(host: host, port: port) }
}
