import Foundation

public enum TargetHost: Hashable, Sendable, Codable, CustomStringConvertible, Comparable {
    case address(IPAddr)
    case hostname(String)

    public var description: String {
        switch self {
        case .address(let address): return address.description
        case .hostname(let name): return name
        }
    }

    public var address: IPAddr? {
        if case .address(let address) = self { return address }
        return nil
    }

    public static func < (lhs: TargetHost, rhs: TargetHost) -> Bool {
        switch (lhs, rhs) {
        case (.address(let a), .address(let b)): return a < b
        case (.hostname(let a), .hostname(let b)): return a < b
        case (.address, .hostname): return true
        case (.hostname, .address): return false
        }
    }
}

public enum TargetParseError: Error, Equatable, Sendable, CustomStringConvertible {
    case empty
    case invalid(String)
    case reversedRange(String)
    case tooManyHosts(count: UInt64, limit: Int)
    case unboundedIPv6(String)

    public var description: String {
        switch self {
        case .empty: return "Enter an IP address, hostname, range or CIDR network."
        case .invalid(let token): return "“\(token)” is not an IP address, hostname, range or CIDR network."
        case .reversedRange(let token): return "The range “\(token)” ends before it starts."
        case .tooManyHosts(let count, let limit): return "The targets cover \(count) hosts; the limit is \(limit)."
        case .unboundedIPv6(let token): return "“\(token)” is too large; IPv6 networks must be /120 or smaller."
        }
    }
}

public struct TargetSpecification: Hashable, Sendable, Codable, CustomStringConvertible {
    public enum Component: Hashable, Sendable, Codable {
        case address(IPAddr)
        case hostname(String)
        case network(IPv4Network)
        case network6(IPv6Network)
        case range(IPv4Addr, IPv4Addr)

        public var hostCount: UInt64 {
            switch self {
            case .address, .hostname: return 1
            case .network(let network): return network.hostCount
            case .network6(let network): return network.addressCount ?? .max
            case .range(let start, let end): return UInt64(end.value - start.value) + 1
            }
        }

        public var isRange: Bool {
            switch self {
            case .address, .hostname: return false
            case .network, .network6, .range: return true
            }
        }
    }

    public let raw: String
    public let components: [Component]

    public init(components: [Component], raw: String? = nil) {
        self.components = components
        self.raw = raw ?? components.map(TargetSpecification.render).joined(separator: ", ")
    }

    public static func parse(_ text: String, maximumHosts: Int = 1024) throws -> TargetSpecification {
        let tokens = text.split { $0 == "," || $0.isWhitespace }.map(String.init)
        guard !tokens.isEmpty else { throw TargetParseError.empty }
        var components: [Component] = []
        for token in tokens {
            let component = try parseToken(token)
            if !components.contains(component) {
                components.append(component)
            }
        }
        let spec = TargetSpecification(components: components, raw: tokens.joined(separator: ", "))
        let count = spec.hostCount
        guard count <= UInt64(maximumHosts) else {
            throw TargetParseError.tooManyHosts(count: count, limit: maximumHosts)
        }
        return spec
    }

    static func parseToken(_ token: String) throws -> Component {
        if let address = IPAddr(token) {
            return .address(address)
        }
        if token.contains("/") {
            if let network = IPv4Network(token) {
                return network.prefixLength == 32 ? .address(.v4(network.network)) : .network(network)
            }
            if let network = IPv6Network(token) {
                guard network.prefixLength >= 120 else { throw TargetParseError.unboundedIPv6(token) }
                return network.prefixLength == 128 ? .address(.v6(network.network)) : .network6(network)
            }
            throw TargetParseError.invalid(token)
        }
        if let dash = token.firstIndex(of: "-"), let start = IPv4Addr(token[..<dash]) {
            let tail = token[token.index(after: dash)...]
            let end: IPv4Addr
            if let full = IPv4Addr(tail) {
                end = full
            } else if tail.allSatisfy(\.isASCIIDigit), !tail.isEmpty, tail.count <= 3, let last = UInt8(tail) {
                end = IPv4Addr(start.value & 0xFFFF_FF00 | UInt32(last))
            } else {
                throw TargetParseError.invalid(token)
            }
            guard end >= start else { throw TargetParseError.reversedRange(token) }
            return start == end ? .address(.v4(start)) : .range(start, end)
        }
        if let name = Hostname.normalized(token) {
            return .hostname(name)
        }
        throw TargetParseError.invalid(token)
    }

    public var hostCount: UInt64 {
        components.reduce(UInt64(0)) { total, component in
            let (sum, overflow) = total.addingReportingOverflow(component.hostCount)
            return overflow ? .max : sum
        }
    }

    public var containsRange: Bool { components.contains { $0.isRange } }

    public func hosts() -> [TargetHost] {
        var seen = Set<TargetHost>()
        var result: [TargetHost] = []
        func add(_ host: TargetHost) {
            if seen.insert(host).inserted { result.append(host) }
        }
        for component in components {
            switch component {
            case .address(let address): add(.address(address))
            case .hostname(let name): add(.hostname(name))
            case .network(let network): network.hosts().forEach { add(.address(.v4($0))) }
            case .network6(let network): network.addresses(limit: 256).forEach { add(.address(.v6($0))) }
            case .range(let start, let end):
                for value in start.value...end.value { add(.address(.v4(IPv4Addr(value)))) }
            }
        }
        return result
    }

    public var description: String { raw }

    static func render(_ component: Component) -> String {
        switch component {
        case .address(let address): return address.description
        case .hostname(let name): return name
        case .network(let network): return network.description
        case .network6(let network): return network.description
        case .range(let start, let end): return "\(start)-\(end)"
        }
    }
}

public enum Hostname {
    public static func normalized(_ text: some StringProtocol) -> String? {
        var name = String(text).lowercased()
        if name.hasSuffix(".") { name.removeLast() }
        guard !name.isEmpty, name.utf8.count <= 253 else { return nil }
        let labels = name.split(separator: ".", omittingEmptySubsequences: false)
        for label in labels {
            guard !label.isEmpty, label.utf8.count <= 63 else { return nil }
            guard label.first != "-", label.last != "-" else { return nil }
            guard label.unicodeScalars.allSatisfy({ scalar in
                (scalar >= "a" && scalar <= "z") || (scalar >= "0" && scalar <= "9") || scalar == "-" || scalar == "_"
            }) else { return nil }
        }
        if labels.count > 1, let last = labels.last, last.allSatisfy(\.isASCIIDigit) { return nil }
        if labels.allSatisfy({ $0.allSatisfy(\.isASCIIDigit) }) { return nil }
        return name
    }

    public static func isLocal(_ name: String) -> Bool {
        let lower = name.lowercased()
        return lower == "localhost" || lower.hasSuffix(".local") || lower.hasSuffix(".localhost")
            || lower.hasSuffix(".home.arpa") || lower.hasSuffix(".lan") || lower.hasSuffix(".internal") || !lower.contains(".")
    }
}
