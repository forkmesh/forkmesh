import Foundation

public enum TargetScope: String, Codable, Sendable, Comparable {
    case loopback
    case localNetwork
    case authorizedNetwork
    case publicInternet

    public static func classify(_ address: IPAddr, authorizedNetworks: [IPNetwork] = []) -> TargetScope {
        if address.isLoopback { return .loopback }
        if address.isLocalScope { return .localNetwork }
        if authorizedNetworks.contains(where: { $0.contains(address) }) { return .authorizedNetwork }
        return .publicInternet
    }

    var rank: Int {
        switch self {
        case .loopback: return 0
        case .localNetwork: return 1
        case .authorizedNetwork: return 2
        case .publicInternet: return 3
        }
    }

    public static func < (lhs: TargetScope, rhs: TargetScope) -> Bool { lhs.rank < rhs.rank }
}

public struct AuthorizationAttestation: Codable, Hashable, Sendable {
    public var statement: String
    public var confirmedAt: Date
    public var reference: String?

    public init(statement: String = AuthorizationAttestation.defaultStatement, confirmedAt: Date = Date(), reference: String? = nil) {
        self.statement = statement
        self.confirmedAt = confirmedAt
        self.reference = reference
    }

    public static let defaultStatement = "I own these targets or have written permission to test them."
}

public enum AuthorizationDecision: Equatable, Sendable {
    case allowed
    case requiresAttestation(String)
    case denied(String)

    public var isAllowed: Bool { self == .allowed }
}

public struct ScanAuthorizationPolicy: Codable, Hashable, Sendable {
    public var allowPublicTargets: Bool
    public var authorizedNetworks: [IPNetwork]
    public var maximumHostsPerScan: Int
    public var maximumPublicHostsPerScan: Int
    public var maximumProbesPerScan: Int

    public init(
        allowPublicTargets: Bool = false,
        authorizedNetworks: [IPNetwork] = [],
        maximumHostsPerScan: Int = 256,
        maximumPublicHostsPerScan: Int = 4,
        maximumProbesPerScan: Int = 70_000
    ) {
        self.allowPublicTargets = allowPublicTargets
        self.authorizedNetworks = authorizedNetworks
        self.maximumHostsPerScan = maximumHostsPerScan
        self.maximumPublicHostsPerScan = maximumPublicHostsPerScan
        self.maximumProbesPerScan = maximumProbesPerScan
    }

    public static let `default` = ScanAuthorizationPolicy()

    public func evaluate(
        addresses: [IPAddr],
        portCount: Int,
        isRange: Bool,
        attestation: AuthorizationAttestation?
    ) -> AuthorizationDecision {
        guard !addresses.isEmpty else { return .denied("No addresses to scan.") }
        if let bad = addresses.first(where: { $0.isMulticastOrBroadcast || $0.isUnspecified }) {
            return .denied("\(bad) is a multicast, broadcast or unspecified address and cannot be scanned.")
        }
        guard addresses.count <= maximumHostsPerScan else {
            return .denied("\(addresses.count) hosts exceeds the limit of \(maximumHostsPerScan) per scan.")
        }
        let probes = addresses.count * portCount
        guard probes <= maximumProbesPerScan else {
            return .denied("\(probes) connection attempts exceeds the limit of \(maximumProbesPerScan) per scan.")
        }
        let scopes = addresses.map { TargetScope.classify($0, authorizedNetworks: authorizedNetworks) }
        let publicCount = scopes.filter { $0 == .publicInternet }.count
        if publicCount == 0 { return .allowed }
        guard allowPublicTargets else {
            return .denied("Internet addresses are off. Turn on “Allow internet targets” in Settings, or add the network to Authorized networks.")
        }
        guard !isRange else {
            return .denied("Ranges of internet addresses can only be scanned when the network is listed under Authorized networks.")
        }
        guard publicCount <= maximumPublicHostsPerScan else {
            return .denied("\(publicCount) internet hosts exceeds the limit of \(maximumPublicHostsPerScan) per scan.")
        }
        guard attestation != nil else {
            return .requiresAttestation("Confirm you own or are authorized to test the internet hosts in this scan.")
        }
        return .allowed
    }
}
