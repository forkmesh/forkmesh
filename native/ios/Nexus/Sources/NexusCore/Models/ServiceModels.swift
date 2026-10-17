import Foundation

public enum EvidenceLevel: String, Codable, Sendable, CaseIterable {
    case confirmed
    case inferred

    public var title: String {
        switch self {
        case .confirmed: return "Confirmed"
        case .inferred: return "Inferred"
        }
    }
}

public struct HTTPHeader: Codable, Hashable, Sendable {
    public var name: String
    public var value: String

    public init(name: String, value: String) {
        self.name = name
        self.value = value
    }
}

public struct HTTPProbeSummary: Codable, Hashable, Sendable {
    public var statusCode: Int
    public var reason: String
    public var httpVersion: String
    public var headers: [HTTPHeader]
    public var title: String?

    public init(statusCode: Int, reason: String, httpVersion: String, headers: [HTTPHeader], title: String? = nil) {
        self.statusCode = statusCode
        self.reason = reason
        self.httpVersion = httpVersion
        self.headers = headers
        self.title = title
    }

    public func header(_ name: String) -> String? {
        headers.first { $0.name.caseInsensitiveCompare(name) == .orderedSame }?.value
    }
}

public struct ServiceIdentity: Codable, Hashable, Sendable {
    public var name: String
    public var title: String
    public var evidence: EvidenceLevel
    public var basis: String
    public var product: String?
    public var version: String?
    public var detail: String?
    public var banner: String?
    public var http: HTTPProbeSummary?
    public var tls: TLSObservation?

    public init(
        name: String,
        title: String,
        evidence: EvidenceLevel,
        basis: String,
        product: String? = nil,
        version: String? = nil,
        detail: String? = nil,
        banner: String? = nil,
        http: HTTPProbeSummary? = nil,
        tls: TLSObservation? = nil
    ) {
        self.name = name
        self.title = title
        self.evidence = evidence
        self.basis = basis
        self.product = product
        self.version = version
        self.detail = detail
        self.banner = banner
        self.http = http
        self.tls = tls
    }

    public static func inferred(port: UInt16) -> ServiceIdentity? {
        guard let known = ServiceCatalog.service(forPort: port) else { return nil }
        return ServiceIdentity(
            name: known.name,
            title: known.title,
            evidence: .inferred,
            basis: "Port \(port) is commonly used by \(known.title); the service itself did not identify."
        )
    }

    public var productDescription: String? {
        switch (product, version) {
        case let (product?, version?): return "\(product) \(version)"
        case let (product?, nil): return product
        case let (nil, version?): return version
        case (nil, nil): return nil
        }
    }
}

public struct PortResult: Codable, Hashable, Sendable, Identifiable {
    public var host: String
    public var address: String?
    public var port: UInt16
    public var state: PortState
    public var latencyMilliseconds: Double?
    public var failure: ConnectionFailure?
    public var service: ServiceIdentity?
    public var observedAt: Date

    public init(
        host: String,
        address: String? = nil,
        port: UInt16,
        state: PortState,
        latencyMilliseconds: Double? = nil,
        failure: ConnectionFailure? = nil,
        service: ServiceIdentity? = nil,
        observedAt: Date = Date()
    ) {
        self.host = host
        self.address = address
        self.port = port
        self.state = state
        self.latencyMilliseconds = latencyMilliseconds
        self.failure = failure
        self.service = service
        self.observedAt = observedAt
    }

    public var id: String { "\(address ?? host)#\(port)" }
    public var hostKey: String { address ?? host }
    public var serviceName: String { service?.title ?? ServiceCatalog.service(forPort: port)?.title ?? "Unknown" }
}

public struct CertificateSummary: Codable, Hashable, Sendable {
    public var subject: String
    public var subjectCommonName: String?
    public var issuer: String
    public var issuerCommonName: String?
    public var serialNumber: String
    public var version: Int
    public var notBefore: Date
    public var notAfter: Date
    public var subjectAlternativeNames: [String]
    public var publicKeyAlgorithm: String
    public var publicKeyBits: Int?
    public var signatureAlgorithm: String
    public var isCertificateAuthority: Bool
    public var isSelfIssued: Bool
    public var sha256Fingerprint: String

    public init(
        subject: String,
        subjectCommonName: String?,
        issuer: String,
        issuerCommonName: String?,
        serialNumber: String,
        version: Int,
        notBefore: Date,
        notAfter: Date,
        subjectAlternativeNames: [String],
        publicKeyAlgorithm: String,
        publicKeyBits: Int?,
        signatureAlgorithm: String,
        isCertificateAuthority: Bool,
        isSelfIssued: Bool,
        sha256Fingerprint: String
    ) {
        self.subject = subject
        self.subjectCommonName = subjectCommonName
        self.issuer = issuer
        self.issuerCommonName = issuerCommonName
        self.serialNumber = serialNumber
        self.version = version
        self.notBefore = notBefore
        self.notAfter = notAfter
        self.subjectAlternativeNames = subjectAlternativeNames
        self.publicKeyAlgorithm = publicKeyAlgorithm
        self.publicKeyBits = publicKeyBits
        self.signatureAlgorithm = signatureAlgorithm
        self.isCertificateAuthority = isCertificateAuthority
        self.isSelfIssued = isSelfIssued
        self.sha256Fingerprint = sha256Fingerprint
    }

    public var displayName: String { subjectCommonName ?? subjectAlternativeNames.first ?? subject }

    public func daysUntilExpiry(from date: Date = Date()) -> Int {
        Int((notAfter.timeIntervalSince(date) / 86_400).rounded(.down))
    }

    public var validityDays: Int {
        Int((notAfter.timeIntervalSince(notBefore) / 86_400).rounded())
    }
}

public enum TrustOutcome: Codable, Hashable, Sendable {
    case trusted
    case untrusted(String)
    case notEvaluated(String)

    public var isTrusted: Bool { self == .trusted }

    public var summary: String {
        switch self {
        case .trusted: return "Trusted by the system trust store"
        case .untrusted(let reason): return "Not trusted: \(reason)"
        case .notEvaluated(let reason): return "Not evaluated: \(reason)"
        }
    }
}

public struct TLSObservation: Codable, Hashable, Sendable {
    public var host: String
    public var port: UInt16
    public var serverName: String?
    public var protocolVersion: String?
    public var cipherSuite: String?
    public var negotiatedALPN: String?
    public var chain: [CertificateSummary]
    public var trust: TrustOutcome
    public var handshakeMilliseconds: Double?
    public var observedAt: Date

    public init(
        host: String,
        port: UInt16,
        serverName: String?,
        protocolVersion: String? = nil,
        cipherSuite: String? = nil,
        negotiatedALPN: String? = nil,
        chain: [CertificateSummary],
        trust: TrustOutcome,
        handshakeMilliseconds: Double? = nil,
        observedAt: Date = Date()
    ) {
        self.host = host
        self.port = port
        self.serverName = serverName
        self.protocolVersion = protocolVersion
        self.cipherSuite = cipherSuite
        self.negotiatedALPN = negotiatedALPN
        self.chain = chain
        self.trust = trust
        self.handshakeMilliseconds = handshakeMilliseconds
        self.observedAt = observedAt
    }

    public var leaf: CertificateSummary? { chain.first }
}

public protocol TLSProbing: Sendable {
    func inspectTLS(host: String, port: UInt16, serverName: String?, timeout: Duration) async throws -> TLSObservation
}
