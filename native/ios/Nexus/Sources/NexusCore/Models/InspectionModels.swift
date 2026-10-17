import Foundation

public enum DNSRecordType: UInt16, Codable, Hashable, Sendable, CaseIterable {
    case a = 1
    case ns = 2
    case cname = 5
    case soa = 6
    case ptr = 12
    case mx = 15
    case txt = 16
    case aaaa = 28

    public var title: String { rawValue == 1 ? "A" : rawValue == 2 ? "NS" : rawValue == 5 ? "CNAME" : rawValue == 6 ? "SOA" : rawValue == 12 ? "PTR" : rawValue == 15 ? "MX" : rawValue == 16 ? "TXT" : "AAAA" }
}

public struct DNSRecord: Codable, Hashable, Sendable, Identifiable {
    public var name: String
    public var type: DNSRecordType
    public var ttl: UInt32
    public var value: String

    public init(name: String, type: DNSRecordType, ttl: UInt32, value: String) {
        self.name = name
        self.type = type
        self.ttl = ttl
        self.value = value
    }

    public var id: String { "\(name)|\(type.rawValue)|\(value)" }
}

public struct DNSLookup: Codable, Hashable, Sendable {
    public var name: String
    public var type: DNSRecordType
    public var answers: [DNSRecord]
    public var authority: [DNSRecord]
    public var additional: [DNSRecord]
    public var responseCode: Int
    public var resolver: String
    public var latencyMilliseconds: Double
    public var error: String?

    public init(name: String, type: DNSRecordType, answers: [DNSRecord] = [], authority: [DNSRecord] = [], additional: [DNSRecord] = [], responseCode: Int = 0, resolver: String, latencyMilliseconds: Double, error: String? = nil) {
        self.name = name
        self.type = type
        self.answers = answers
        self.authority = authority
        self.additional = additional
        self.responseCode = responseCode
        self.resolver = resolver
        self.latencyMilliseconds = latencyMilliseconds
        self.error = error
    }
}

public struct HTTPInspection: Codable, Hashable, Sendable {
    public var requestedURL: URL
    public var finalURL: URL
    public var statusCode: Int
    public var headers: [HTTPHeader]
    public var redirects: [URL]
    public var elapsedMilliseconds: Double
    public var securityObservations: [SecurityObservation]
    public var tls: TLSObservation?

    public init(requestedURL: URL, finalURL: URL, statusCode: Int, headers: [HTTPHeader], redirects: [URL], elapsedMilliseconds: Double, securityObservations: [SecurityObservation], tls: TLSObservation? = nil) {
        self.requestedURL = requestedURL
        self.finalURL = finalURL
        self.statusCode = statusCode
        self.headers = headers
        self.redirects = redirects
        self.elapsedMilliseconds = elapsedMilliseconds
        self.securityObservations = securityObservations
        self.tls = tls
    }
}

public enum SecuritySeverity: String, Codable, Hashable, Sendable, Comparable {
    case informational
    case attention
    case warning

    public static func < (lhs: SecuritySeverity, rhs: SecuritySeverity) -> Bool { [Self.informational, .attention, .warning].firstIndex(of: lhs)! < [Self.informational, .attention, .warning].firstIndex(of: rhs)! }
}

public struct SecurityObservation: Codable, Hashable, Sendable, Identifiable {
    public var id: UUID
    public var severity: SecuritySeverity
    public var title: String
    public var detail: String
    public var subject: String
    public var observedAt: Date

    public init(id: UUID = UUID(), severity: SecuritySeverity, title: String, detail: String, subject: String, observedAt: Date = Date()) {
        self.id = id
        self.severity = severity
        self.title = title
        self.detail = detail
        self.subject = subject
        self.observedAt = observedAt
    }
}

public struct ConnectivityResult: Codable, Hashable, Sendable {
    public var endpoint: Endpoint
    public var reachable: Bool
    public var latencyMilliseconds: Double?
    public var resolvedAddresses: [IPAddr]
    public var failure: ConnectionFailure?
    public var observedAt: Date

    public init(endpoint: Endpoint, reachable: Bool, latencyMilliseconds: Double? = nil, resolvedAddresses: [IPAddr] = [], failure: ConnectionFailure? = nil, observedAt: Date = Date()) {
        self.endpoint = endpoint
        self.reachable = reachable
        self.latencyMilliseconds = latencyMilliseconds
        self.resolvedAddresses = resolvedAddresses
        self.failure = failure
        self.observedAt = observedAt
    }
}
