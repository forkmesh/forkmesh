import Foundation

public enum PhoneSecurityStatus: String, CaseIterable, Sendable {
    case pass
    case review
    case warning
    case critical
    case unavailable

    public var label: String {
        switch self {
        case .pass: return "Pass"
        case .review: return "Review"
        case .warning: return "Warning"
        case .critical: return "Critical"
        case .unavailable: return "Unavailable"
        }
    }

    public var rank: Int {
        switch self {
        case .critical: return 0
        case .warning: return 1
        case .review: return 2
        case .unavailable: return 3
        case .pass: return 4
        }
    }

    public var needsAttention: Bool { self == .critical || self == .warning || self == .review }
}

public enum PhoneSecurityFilter: String, CaseIterable, Sendable {
    case all
    case attention
    case critical
    case warning
    case review
    case fixable
    case pass
    case unavailable

    public var label: String {
        switch self {
        case .all: return "All"
        case .attention: return "Needs attention"
        case .critical: return "Critical"
        case .warning: return "Warnings"
        case .review: return "For review"
        case .fixable: return "Can fix"
        case .pass: return "Passed"
        case .unavailable: return "Unavailable"
        }
    }
}

public enum PhoneSecurityCategory: String, CaseIterable, Sendable {
    case lockScreen
    case deviceIntegrity
    case system
    case appProtection
    case network
    case privacy

    public var label: String {
        switch self {
        case .lockScreen: return "Lock screen"
        case .deviceIntegrity: return "Device integrity"
        case .system: return "System"
        case .appProtection: return "App protection"
        case .network: return "Network"
        case .privacy: return "Privacy"
        }
    }
}

public enum PhoneSettingsPage: String, Equatable, Sendable {
    case app
    case appNotifications
}

public enum PhoneSecurityFixAction: Equatable, Sendable {
    case open(PhoneSettingsPage)
    case steps(String)

    public var opensSettings: Bool {
        if case .open = self { return true }
        return false
    }
}

public struct PhoneSecurityFix: Equatable, Sendable {
    public let label: String
    public let description: String
    public let action: PhoneSecurityFixAction

    public init(label: String, description: String, action: PhoneSecurityFixAction) {
        self.label = label
        self.description = description
        self.action = action
    }
}

public struct PhoneSecurityCheck: Equatable, Sendable, Identifiable {
    public let id: String
    public let title: String
    public let category: PhoneSecurityCategory

    public init(id: String, title: String, category: PhoneSecurityCategory) {
        self.id = id
        self.title = title
        self.category = category
    }

    public var explanation: String { PhoneSecurityExplanations.text(for: id) ?? "" }
}

public struct PhoneSecurityFinding: Equatable, Sendable, Identifiable {
    public let check: PhoneSecurityCheck
    public let status: PhoneSecurityStatus
    public let detail: String
    public let usage: String?
    public let fix: PhoneSecurityFix?

    public init(check: PhoneSecurityCheck, status: PhoneSecurityStatus, detail: String, usage: String? = nil,
                fix: PhoneSecurityFix? = nil) {
        self.check = check
        self.status = status
        self.detail = detail
        self.usage = usage
        self.fix = status.needsAttention ? fix : nil
    }

    public var id: String { check.id }
    public var canFix: Bool { fix?.action.opensSettings ?? false }
}

public enum PhoneBiometry: String, Equatable, Sendable {
    case none
    case touchID
    case faceID
    case opticID

    public var label: String {
        switch self {
        case .none: return "No biometric sensor"
        case .touchID: return "Touch ID"
        case .faceID: return "Face ID"
        case .opticID: return "Optic ID"
        }
    }
}

public enum PhoneNotificationPreviews: String, Equatable, Sendable {
    case always
    case whenAuthenticated
    case never
    case notificationsOff
}

public enum PhoneInstallSource: String, Equatable, Sendable {
    case appStore
    case testFlight
    case development
}

public enum PhoneFileProtection: String, Equatable, Sendable {
    case complete
    case completeUnlessOpen
    case completeUntilFirstUserAuthentication
    case none
}

public enum PhonePermission: String, CaseIterable, Sendable {
    case camera
    case microphone
    case photos
    case speech
    case bluetooth
    case motion
}

public enum PhonePermissionState: String, Equatable, Sendable {
    case allowed
    case limited
    case denied
    case restricted
    case notAsked
}

public struct PhoneTransportSecurity: Equatable, Sendable {
    public let allowsArbitraryLoads: Bool
    public let insecureDomains: [String]

    public init(allowsArbitraryLoads: Bool = false, insecureDomains: [String] = []) {
        self.allowsArbitraryLoads = allowsArbitraryLoads
        self.insecureDomains = insecureDomains
    }
}

public struct PhoneProxySettings: Equatable, Sendable {
    public let httpHost: String?
    public let httpPort: Int?
    public let httpsHost: String?
    public let httpsPort: Int?
    public let autoConfigURL: String?

    public init(httpHost: String? = nil, httpPort: Int? = nil, httpsHost: String? = nil, httpsPort: Int? = nil,
                autoConfigURL: String? = nil) {
        self.httpHost = httpHost
        self.httpPort = httpPort
        self.httpsHost = httpsHost
        self.httpsPort = httpsPort
        self.autoConfigURL = autoConfigURL
    }

    public static func parse(_ dictionary: [String: Any]) -> PhoneProxySettings {
        func enabled(_ key: String) -> Bool {
            if let number = dictionary[key] as? NSNumber { return number.boolValue }
            if let flag = dictionary[key] as? Bool { return flag }
            if let value = dictionary[key] as? Int { return value != 0 }
            return false
        }
        func text(_ key: String) -> String? {
            guard let value = dictionary[key] as? String else { return nil }
            let trimmed: String = value.trimmingCharacters(in: .whitespacesAndNewlines)
            return trimmed.isEmpty ? nil : trimmed
        }
        func port(_ key: String) -> Int? {
            if let number = dictionary[key] as? NSNumber { return number.intValue }
            return dictionary[key] as? Int
        }
        let http: Bool = enabled("HTTPEnable")
        let https: Bool = enabled("HTTPSEnable")
        let pac: Bool = enabled("ProxyAutoConfigEnable")
        return PhoneProxySettings(
            httpHost: http ? text("HTTPProxy") : nil, httpPort: http ? port("HTTPPort") : nil,
            httpsHost: https ? text("HTTPSProxy") : nil, httpsPort: https ? port("HTTPSPort") : nil,
            autoConfigURL: pac ? text("ProxyAutoConfigURLString") : nil)
    }

    public static func vpnInterfaces(scoped: [String]) -> [String] {
        let prefixes: [String] = ["utun", "ipsec", "ppp", "tap", "tun"]
        return scoped.filter { name in prefixes.contains { name.lowercased().hasPrefix($0) } }.sorted()
    }

    public var isEmpty: Bool { httpHost == nil && httpsHost == nil && autoConfigURL == nil }

    public var summary: String {
        var parts: [String] = []
        if let httpHost { parts.append("HTTP \(Self.endpoint(httpHost, httpPort))") }
        if let httpsHost { parts.append("HTTPS \(Self.endpoint(httpsHost, httpsPort))") }
        if let autoConfigURL { parts.append("automatic from \(autoConfigURL)") }
        return parts.joined(separator: ", ")
    }

    private static func endpoint(_ host: String, _ port: Int?) -> String {
        guard let port, port > 0 else { return host }
        return "\(host):\(port)"
    }
}

public struct PhoneSecurityFacts: Equatable, Sendable {
    public var passcodeSet: Bool?
    public var biometry: PhoneBiometry?
    public var biometryEnrolled: Bool?
    public var notificationPreviews: PhoneNotificationPreviews?
    public var existingPaths: Set<String>?
    public var symbolicLinks: Set<String>?
    public var loadedImages: [String]?
    public var environment: [String: String]?
    public var canWriteOutsideSandbox: Bool?
    public var debuggerAttached: Bool?
    public var simulator: Bool?
    public var osVersion: String?
    public var installSource: PhoneInstallSource?
    public var debugBuild: Bool?
    public var fileProtection: PhoneFileProtection?
    public var transportSecurity: PhoneTransportSecurity?
    public var relayURL: String?
    public var screenCaptured: Bool?
    public var proxy: PhoneProxySettings?
    public var vpnInterfaces: [String]?
    public var permissions: [PhonePermission: PhonePermissionState]

    public init(
        passcodeSet: Bool? = nil, biometry: PhoneBiometry? = nil, biometryEnrolled: Bool? = nil,
        notificationPreviews: PhoneNotificationPreviews? = nil, existingPaths: Set<String>? = nil,
        symbolicLinks: Set<String>? = nil, loadedImages: [String]? = nil, environment: [String: String]? = nil,
        canWriteOutsideSandbox: Bool? = nil, debuggerAttached: Bool? = nil, simulator: Bool? = nil,
        osVersion: String? = nil, installSource: PhoneInstallSource? = nil, debugBuild: Bool? = nil,
        fileProtection: PhoneFileProtection? = nil, transportSecurity: PhoneTransportSecurity? = nil,
        relayURL: String? = nil, screenCaptured: Bool? = nil, proxy: PhoneProxySettings? = nil,
        vpnInterfaces: [String]? = nil, permissions: [PhonePermission: PhonePermissionState] = [:]
    ) {
        self.passcodeSet = passcodeSet
        self.biometry = biometry
        self.biometryEnrolled = biometryEnrolled
        self.notificationPreviews = notificationPreviews
        self.existingPaths = existingPaths
        self.symbolicLinks = symbolicLinks
        self.loadedImages = loadedImages
        self.environment = environment
        self.canWriteOutsideSandbox = canWriteOutsideSandbox
        self.debuggerAttached = debuggerAttached
        self.simulator = simulator
        self.osVersion = osVersion
        self.installSource = installSource
        self.debugBuild = debugBuild
        self.fileProtection = fileProtection
        self.transportSecurity = transportSecurity
        self.relayURL = relayURL
        self.screenCaptured = screenCaptured
        self.proxy = proxy
        self.vpnInterfaces = vpnInterfaces
        self.permissions = permissions
    }

    public var developing: Bool { (debugBuild ?? false) || (simulator ?? false) }
}
