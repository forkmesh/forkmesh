import Foundation

public enum SessionStatus: Codable, Hashable, Sendable {
    case running
    case completed
    case cancelled
    case failed(String)

    public var title: String {
        switch self {
        case .running: return "Running"
        case .completed: return "Completed"
        case .cancelled: return "Cancelled"
        case .failed: return "Failed"
        }
    }

    public var isFinished: Bool { self != .running }
}

public struct ResolvedHost: Codable, Hashable, Sendable {
    public var name: String
    public var addresses: [IPAddr]
    public var error: String?

    public init(name: String, addresses: [IPAddr], error: String? = nil) {
        self.name = name
        self.addresses = addresses
        self.error = error
    }
}

public struct ScanOptions: Codable, Hashable, Sendable {
    public var timeoutSeconds: Double
    public var maximumConcurrency: Int
    public var inspectServices: Bool
    public var inspectionTimeoutSeconds: Double
    public var retryTimeouts: Bool
    public var recordClosedPorts: Bool
    public var preferIPv4: Bool

    public init(
        timeoutSeconds: Double = 1.5,
        maximumConcurrency: Int = 48,
        inspectServices: Bool = true,
        inspectionTimeoutSeconds: Double = 3,
        retryTimeouts: Bool = false,
        recordClosedPorts: Bool = false,
        preferIPv4: Bool = true
    ) {
        self.timeoutSeconds = timeoutSeconds
        self.maximumConcurrency = maximumConcurrency
        self.inspectServices = inspectServices
        self.inspectionTimeoutSeconds = inspectionTimeoutSeconds
        self.retryTimeouts = retryTimeouts
        self.recordClosedPorts = recordClosedPorts
        self.preferIPv4 = preferIPv4
    }

    public static let `default` = ScanOptions()

    public var clamped: ScanOptions {
        var copy = self
        copy.timeoutSeconds = min(max(timeoutSeconds, 0.2), 30)
        copy.maximumConcurrency = min(max(maximumConcurrency, 1), 128)
        copy.inspectionTimeoutSeconds = min(max(inspectionTimeoutSeconds, 0.5), 30)
        return copy
    }

    public var timeout: Duration { .milliseconds(Int64(timeoutSeconds * 1000)) }
    public var inspectionTimeout: Duration { .milliseconds(Int64(inspectionTimeoutSeconds * 1000)) }
}

public enum ScanEngineKind: Codable, Hashable, Sendable {
    case native
    case remote(agent: String, profile: String)

    public var title: String {
        switch self {
        case .native: return "On-device TCP connect"
        case .remote(_, let profile): return "Remote agent · \(profile)"
        }
    }
}

public struct PortStateCounts: Codable, Hashable, Sendable {
    public var open = 0
    public var closed = 0
    public var timedOut = 0
    public var unreachable = 0
    public var indeterminate = 0

    public init() {}

    public subscript(state: PortState) -> Int {
        get {
            switch state {
            case .open: return open
            case .closed: return closed
            case .timedOut: return timedOut
            case .unreachable: return unreachable
            case .indeterminate: return indeterminate
            }
        }
        set {
            switch state {
            case .open: open = newValue
            case .closed: closed = newValue
            case .timedOut: timedOut = newValue
            case .unreachable: unreachable = newValue
            case .indeterminate: indeterminate = newValue
            }
        }
    }

    public var total: Int { open + closed + timedOut + unreachable + indeterminate }
}

public struct ScanSession: Codable, Hashable, Sendable, Identifiable {
    public var id: UUID
    public var target: String
    public var resolvedHosts: [ResolvedHost]
    public var ports: PortList
    public var options: ScanOptions
    public var engine: ScanEngineKind
    public var startedAt: Date
    public var finishedAt: Date?
    public var status: SessionStatus
    public var results: [PortResult]
    public var counts: PortStateCounts
    public var attestation: AuthorizationAttestation?
    public var notes: [String]

    public init(
        id: UUID = UUID(),
        target: String,
        resolvedHosts: [ResolvedHost] = [],
        ports: PortList,
        options: ScanOptions = .default,
        engine: ScanEngineKind = .native,
        startedAt: Date = Date(),
        finishedAt: Date? = nil,
        status: SessionStatus = .running,
        results: [PortResult] = [],
        counts: PortStateCounts = PortStateCounts(),
        attestation: AuthorizationAttestation? = nil,
        notes: [String] = []
    ) {
        self.id = id
        self.target = target
        self.resolvedHosts = resolvedHosts
        self.ports = ports
        self.options = options
        self.engine = engine
        self.startedAt = startedAt
        self.finishedAt = finishedAt
        self.status = status
        self.results = results
        self.counts = counts
        self.attestation = attestation
        self.notes = notes
    }

    public var openResults: [PortResult] {
        results.filter { $0.state == .open }.sorted { ($0.hostKey, $0.port) < ($1.hostKey, $1.port) }
    }

    public var hostKeys: [String] {
        Array(Set(results.map(\.hostKey))).sorted()
    }

    public var duration: TimeInterval? {
        finishedAt.map { $0.timeIntervalSince(startedAt) }
    }

    public var scannedAddresses: [IPAddr] {
        resolvedHosts.flatMap(\.addresses)
    }
}

public enum DiscoverySource: String, Codable, Sendable, CaseIterable, Comparable {
    case thisDevice
    case bonjour
    case tcpSweep
    case portScan
    case reverseDNS
    case remoteAgent
    case manual

    public var title: String {
        switch self {
        case .thisDevice: return "This device"
        case .bonjour: return "Bonjour"
        case .tcpSweep: return "TCP sweep"
        case .portScan: return "Port scan"
        case .reverseDNS: return "Reverse DNS"
        case .remoteAgent: return "Remote agent"
        case .manual: return "Added manually"
        }
    }

    public var symbolName: String {
        switch self {
        case .thisDevice: return "iphone"
        case .bonjour: return "bonjour"
        case .tcpSweep: return "dot.radiowaves.left.and.right"
        case .portScan: return "scope"
        case .reverseDNS: return "character.book.closed"
        case .remoteAgent: return "server.rack"
        case .manual: return "hand.point.up"
        }
    }

    public static func < (lhs: DiscoverySource, rhs: DiscoverySource) -> Bool {
        allCases.firstIndex(of: lhs)! < allCases.firstIndex(of: rhs)!
    }
}

public struct BonjourService: Codable, Hashable, Sendable, Identifiable {
    public var name: String
    public var type: String
    public var domain: String
    public var port: UInt16?
    public var hostName: String?
    public var addresses: [IPAddr]
    public var txt: [String: String]
    public var interfaceName: String?
    public var discoveredAt: Date

    public init(
        name: String,
        type: String,
        domain: String = "local.",
        port: UInt16? = nil,
        hostName: String? = nil,
        addresses: [IPAddr] = [],
        txt: [String: String] = [:],
        interfaceName: String? = nil,
        discoveredAt: Date = Date()
    ) {
        self.name = name
        self.type = type
        self.domain = domain
        self.port = port
        self.hostName = hostName
        self.addresses = addresses
        self.txt = txt
        self.interfaceName = interfaceName
        self.discoveredAt = discoveredAt
    }

    public var id: String { "\(name).\(type).\(domain)" }
}

public enum DeviceKind: String, Codable, Sendable, CaseIterable {
    case router
    case computer
    case phone
    case tablet
    case printer
    case tv
    case speaker
    case camera
    case storage
    case smartHome
    case gameConsole
    case server
    case unknown

    public var title: String {
        switch self {
        case .router: return "Router or gateway"
        case .computer: return "Computer"
        case .phone: return "Phone"
        case .tablet: return "Tablet"
        case .printer: return "Printer or scanner"
        case .tv: return "TV or streaming device"
        case .speaker: return "Speaker"
        case .camera: return "Camera"
        case .storage: return "Network storage"
        case .smartHome: return "Smart home device"
        case .gameConsole: return "Game console"
        case .server: return "Server"
        case .unknown: return "Unknown device"
        }
    }

    public var symbolName: String {
        switch self {
        case .router: return "wifi.router"
        case .computer: return "desktopcomputer"
        case .phone: return "iphone"
        case .tablet: return "ipad"
        case .printer: return "printer"
        case .tv: return "tv"
        case .speaker: return "hifispeaker"
        case .camera: return "web.camera"
        case .storage: return "externaldrive.connected.to.line.below"
        case .smartHome: return "homekit"
        case .gameConsole: return "gamecontroller"
        case .server: return "server.rack"
        case .unknown: return "questionmark.square.dashed"
        }
    }
}

public struct DeviceIdentification: Codable, Hashable, Sendable {
    public var kind: DeviceKind
    public var model: String?
    public var manufacturer: String?
    public var operatingSystem: String?
    public var evidence: EvidenceLevel
    public var basis: String

    public init(kind: DeviceKind, model: String? = nil, manufacturer: String? = nil, operatingSystem: String? = nil, evidence: EvidenceLevel, basis: String) {
        self.kind = kind
        self.model = model
        self.manufacturer = manufacturer
        self.operatingSystem = operatingSystem
        self.evidence = evidence
        self.basis = basis
    }
}

public struct HardwareAddress: Codable, Hashable, Sendable {
    public var value: String
    public var vendor: String?
    public var source: String

    public init(value: String, vendor: String? = nil, source: String) {
        self.value = value
        self.vendor = vendor
        self.source = source
    }
}

public struct DeviceAttribute: Codable, Hashable, Sendable {
    public var label: String
    public var value: String
    public var source: String

    public init(label: String, value: String, source: String) {
        self.label = label
        self.value = value
        self.source = source
    }
}

public struct Device: Codable, Hashable, Sendable, Identifiable {
    public var id: String
    public var displayName: String
    public var addresses: [IPAddr]
    public var hostnames: [String]
    public var services: [BonjourService]
    public var openPorts: [PortResult]
    public var identification: DeviceIdentification?
    public var hardwareAddress: HardwareAddress?
    public var attributes: [DeviceAttribute]
    public var sources: [DiscoverySource]
    public var firstSeen: Date
    public var lastSeen: Date
    public var isCurrentDevice: Bool
    public var userLabel: String?

    public init(
        id: String,
        displayName: String,
        addresses: [IPAddr] = [],
        hostnames: [String] = [],
        services: [BonjourService] = [],
        openPorts: [PortResult] = [],
        identification: DeviceIdentification? = nil,
        hardwareAddress: HardwareAddress? = nil,
        attributes: [DeviceAttribute] = [],
        sources: [DiscoverySource] = [],
        firstSeen: Date = Date(),
        lastSeen: Date = Date(),
        isCurrentDevice: Bool = false,
        userLabel: String? = nil
    ) {
        self.id = id
        self.displayName = displayName
        self.addresses = addresses
        self.hostnames = hostnames
        self.services = services
        self.openPorts = openPorts
        self.identification = identification
        self.hardwareAddress = hardwareAddress
        self.attributes = attributes
        self.sources = sources
        self.firstSeen = firstSeen
        self.lastSeen = lastSeen
        self.isCurrentDevice = isCurrentDevice
        self.userLabel = userLabel
    }

    public var title: String { userLabel ?? displayName }

    public var primaryAddress: IPAddr? {
        addresses.first { $0.family == .ipv4 } ?? addresses.first
    }

    public var kind: DeviceKind { identification?.kind ?? .unknown }

    public var serviceTypes: [String] { Array(Set(services.map(\.type))).sorted() }

    public var searchText: String {
        ([title, displayName] + addresses.map(\.description) + hostnames + services.map(\.name) + services.map(\.type)
            + openPorts.map { "\($0.port) \($0.serviceName)" } + attributes.map(\.value)
            + [identification?.model, identification?.manufacturer, kind.title].compactMap { $0 })
            .joined(separator: " ").lowercased()
    }

    public static func key(for address: IPAddr) -> String { "ip:\(address)" }
}

public struct DiscoverySession: Codable, Hashable, Sendable, Identifiable {
    public var id: UUID
    public var startedAt: Date
    public var finishedAt: Date?
    public var status: SessionStatus
    public var network: String?
    public var interfaceName: String?
    public var methods: [DiscoverySource]
    public var serviceTypes: [String]
    public var hostsProbed: Int
    public var devices: [Device]
    public var notes: [String]

    public init(
        id: UUID = UUID(),
        startedAt: Date = Date(),
        finishedAt: Date? = nil,
        status: SessionStatus = .running,
        network: String? = nil,
        interfaceName: String? = nil,
        methods: [DiscoverySource] = [],
        serviceTypes: [String] = [],
        hostsProbed: Int = 0,
        devices: [Device] = [],
        notes: [String] = []
    ) {
        self.id = id
        self.startedAt = startedAt
        self.finishedAt = finishedAt
        self.status = status
        self.network = network
        self.interfaceName = interfaceName
        self.methods = methods
        self.serviceTypes = serviceTypes
        self.hostsProbed = hostsProbed
        self.devices = devices
        self.notes = notes
    }
}

public enum ChangeKind: String, Codable, Sendable, CaseIterable {
    case deviceAppeared
    case deviceDisappeared
    case addressChanged
    case hostnameChanged
    case serviceAdded
    case serviceRemoved
    case portOpened
    case portClosed
    case serviceChanged
    case certificateChanged

    public var title: String {
        switch self {
        case .deviceAppeared: return "New device"
        case .deviceDisappeared: return "Device not seen"
        case .addressChanged: return "Address changed"
        case .hostnameChanged: return "Hostname changed"
        case .serviceAdded: return "Service advertised"
        case .serviceRemoved: return "Service no longer advertised"
        case .portOpened: return "Port opened"
        case .portClosed: return "Port no longer open"
        case .serviceChanged: return "Service changed"
        case .certificateChanged: return "Certificate changed"
        }
    }

    public var symbolName: String {
        switch self {
        case .deviceAppeared: return "plus.circle"
        case .deviceDisappeared: return "minus.circle"
        case .addressChanged, .hostnameChanged: return "arrow.triangle.swap"
        case .serviceAdded: return "antenna.radiowaves.left.and.right"
        case .serviceRemoved: return "antenna.radiowaves.left.and.right.slash"
        case .portOpened: return "lock.open"
        case .portClosed: return "lock"
        case .serviceChanged: return "arrow.triangle.2.circlepath"
        case .certificateChanged: return "checkmark.seal"
        }
    }
}

public struct ObservedChange: Codable, Hashable, Sendable, Identifiable {
    public var id: UUID
    public var kind: ChangeKind
    public var deviceID: String?
    public var subject: String
    public var before: String?
    public var after: String?
    public var detectedAt: Date

    public init(id: UUID = UUID(), kind: ChangeKind, deviceID: String?, subject: String, before: String? = nil, after: String? = nil, detectedAt: Date = Date()) {
        self.id = id
        self.kind = kind
        self.deviceID = deviceID
        self.subject = subject
        self.before = before
        self.after = after
        self.detectedAt = detectedAt
    }

    public var summary: String {
        switch (before, after) {
        case let (before?, after?): return "\(subject): \(before) → \(after)"
        case let (nil, after?): return "\(subject): \(after)"
        case let (before?, nil): return "\(subject): was \(before)"
        case (nil, nil): return subject
        }
    }
}
