import FMBase
import Foundation

public enum RunnerStatus: String, Sendable, Hashable {
    case active
    case paused
    case revoked

    public init(raw: String) {
        switch raw.dartTrimmed.lowercased() {
        case "active": self = .active
        case "paused": self = .paused
        default: self = .revoked
        }
    }

    public var label: String? {
        switch self {
        case .active: return nil
        case .paused: return "Paused"
        case .revoked: return "Revoked"
        }
    }
}

public enum RunnerProviderSignIn: String, Sendable, Hashable {
    case signedIn
    case expiring
    case expired
    case signedOut
    case notInstalled
    case unknown

    public var label: String {
        switch self {
        case .signedIn: return "Signed in"
        case .expiring: return "Expires soon"
        case .expired: return "Sign-in expired"
        case .signedOut: return "Not signed in"
        case .notInstalled: return "Not installed"
        case .unknown: return "Not reported"
        }
    }

    public var needsSignIn: Bool { self == .expired || self == .expiring || self == .signedOut }
}

public struct RunnerProviderState: Equatable, Hashable, Sendable, Identifiable {
    public var provider: String
    public var signIn: RunnerProviderSignIn
    public var activeAccount: String
    public var accountCount: Int
    public var canSignIn: Bool

    public init(
        provider: String, signIn: RunnerProviderSignIn, activeAccount: String = "", accountCount: Int = 0,
        canSignIn: Bool = false
    ) {
        self.provider = provider
        self.signIn = signIn
        self.activeAccount = activeAccount
        self.accountCount = accountCount
        self.canSignIn = canSignIn
    }

    public var id: String { provider }

    public var label: String { RunnerSignIn.providerLong(provider) }

    public var detail: String {
        activeAccount.isEmpty ? signIn.label : "\(signIn.label) as \(activeAccount)"
    }
}

public struct RunnerListEntry: Equatable, Hashable, Sendable, Identifiable {
    public var name: String
    public var scope: String
    public var status: RunnerStatus
    public var lastSeenAtMs: Int64
    public var activeLeases: Int
    public var providers: [RunnerProviderState]
    public var accounts: RunnerAccountsRunner

    public init(
        name: String, scope: String = "", status: RunnerStatus = .active, lastSeenAtMs: Int64 = 0,
        activeLeases: Int = 0, providers: [RunnerProviderState] = [], accounts: RunnerAccountsRunner? = nil
    ) {
        self.name = name
        self.scope = scope
        self.status = status
        self.lastSeenAtMs = lastSeenAtMs
        self.activeLeases = activeLeases
        self.providers = providers
        self.accounts = accounts ?? RunnerAccountsRunner(
            name: name, scope: scope, status: status.rawValue, lastSeenAtMs: lastSeenAtMs)
    }

    public var id: String { name }

    public func online(nowMs: Int64) -> Bool {
        lastSeenAtMs > 0 && nowMs - lastSeenAtMs <= RunnerList.onlineMs
    }

    public func seenText(nowMs: Int64) -> String {
        if lastSeenAtMs <= 0 { return "Never seen" }
        if online(nowMs: nowMs) { return "Online" }
        return "Last seen \(RunnerAccounts.durationText(nowMs - lastSeenAtMs)) ago"
    }

    public var scopeLabel: String {
        switch scope {
        case "pool": return "Pool"
        case "personal": return "Personal"
        case "": return ""
        default: return scope.prefix(1).uppercased() + scope.dropFirst()
        }
    }

    public var jobsText: String {
        switch activeLeases {
        case ..<1: return "No jobs"
        case 1: return "1 job"
        default: return "\(activeLeases) jobs"
        }
    }

    public var revoked: Bool { status == .revoked }

    public var canPause: Bool { status == .active }

    public var canResume: Bool { status == .paused }

    public var canRevoke: Bool { status != .revoked }

    public var signInProviders: [RunnerProviderState] {
        revoked ? [] : providers.filter(\.canSignIn)
    }

    public var hasAccounts: Bool {
        !revoked && accounts.providers.contains { !$0.accounts.isEmpty }
    }

    public func signInRequest(_ provider: String) -> RunnerSignInRequest {
        accounts.signInRequest(provider)
    }

    public func provider(_ provider: String) -> RunnerProviderState? {
        providers.first { $0.provider == provider }
    }
}

public enum RunnerAction: String, Sendable, Hashable, CaseIterable {
    case pause
    case resume
    case revoke

    public var path: String { "/api/runners/\(rawValue)" }

    public var label: String {
        switch self {
        case .pause: return "Pause"
        case .resume: return "Resume"
        case .revoke: return "Revoke"
        }
    }

    public func done(_ name: String) -> String {
        switch self {
        case .pause: return "\(name) is paused. It finishes the jobs it holds and takes no new ones."
        case .resume: return "\(name) takes jobs again."
        case .revoke: return "\(name) is revoked. Its key no longer works."
        }
    }

    public var failure: String {
        switch self {
        case .pause: return "Could not pause the runner"
        case .resume: return "Could not resume the runner"
        case .revoke: return "Could not revoke the runner"
        }
    }

    public var after: RunnerStatus {
        switch self {
        case .pause: return .paused
        case .resume: return .active
        case .revoke: return .revoked
        }
    }
}

public enum RunnerList {
    public static let onlineMs: Int64 = 10 * 60 * 1000
    public static let dashboardPath: String = "/dashboard/runners"

    public static func path() -> String { RunnerSignIn.runnersPath() }

    public static func body(name: String) -> JSONValue {
        .object(["name": .string(name.dartTrimmed.lowercased())])
    }

    public static func revokeTitle(_ name: String) -> String { "Revoke \(name)?" }

    public static let revokeMessage: String =
        "The runner stops taking jobs and its key stops working. This cannot be undone; to use the machine again, add it as a new runner."

    static func signIn(_ entry: JSONObject?) -> RunnerProviderSignIn {
        guard let entry else { return .unknown }
        if !entry.bool("binaryFound") { return .notInstalled }
        let loginState: String = entry.str("loginState").dartTrimmed.lowercased()
        let authState: String = entry.obj("auth").str("state").dartTrimmed.lowercased()
        if loginState == "expired" || authState == "expired" { return .expired }
        if authState == "expiring" { return .expiring }
        if authState == "missing" { return .signedOut }
        if loginState == "available" || authState == "ok" { return .signedIn }
        return .signedOut
    }

    public static func parse(runner row: JSONObject) -> RunnerListEntry? {
        guard let accounts = RunnerAccounts.parse(runner: row) else { return nil }
        let availability: JSONObject = row.obj("availability")
        let providers: [RunnerProviderState] = RunnerSignIn.providers.map { (provider: String) -> RunnerProviderState in
            let known: RunnerProviderAccounts = accounts.provider(provider)
            return RunnerProviderState(
                provider: provider, signIn: signIn(availability[provider]?.objectValue),
                activeAccount: known.activeAccount?.displayName ?? "", accountCount: known.accounts.count,
                canSignIn: known.binaryFound)
        }
        return RunnerListEntry(
            name: accounts.name, scope: accounts.scope, status: RunnerStatus(raw: row.str("status")),
            lastSeenAtMs: accounts.lastSeenAtMs, activeLeases: max(0, row.int("activeLeases")), providers: providers,
            accounts: accounts)
    }

    public static func parse(_ payload: JSONValue) -> [RunnerListEntry] {
        ordered(payload.maps("runners").compactMap { parse(runner: $0) })
    }

    public static func ordered(_ runners: [RunnerListEntry]) -> [RunnerListEntry] {
        runners.filter { !$0.revoked } + runners.filter(\.revoked)
    }
}
