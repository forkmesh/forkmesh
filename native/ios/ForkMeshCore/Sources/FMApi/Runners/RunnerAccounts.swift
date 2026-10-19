import FMBase
import Foundation

public struct RunnerUsageWindow: Equatable, Hashable, Sendable, Identifiable {
    public var id: String
    public var label: String
    public var usedPercent: Double
    public var resetsAtMs: Int64
    public var windowMinutes: Int

    public init(id: String, label: String = "", usedPercent: Double = 0, resetsAtMs: Int64 = 0, windowMinutes: Int = 0) {
        self.id = id
        self.label = label.isEmpty ? id : label
        self.usedPercent = min(100, max(0, usedPercent))
        self.resetsAtMs = resetsAtMs
        self.windowMinutes = windowMinutes
    }

    public func text(nowMs: Int64) -> String { RunnerAccounts.usageWindowText(self, nowMs: nowMs) }
}

public struct RunnerAccount: Equatable, Hashable, Sendable, Identifiable {
    public var id: String
    public var label: String
    public var email: String
    public var plan: String
    public var active: Bool
    public var usageRead: Bool
    public var usageError: String
    public var usageFetchedAtMs: Int64
    public var usage: [RunnerUsageWindow]

    public init(
        id: String, label: String = "", email: String = "", plan: String = "", active: Bool = false,
        usageRead: Bool = false, usageError: String = "", usageFetchedAtMs: Int64 = 0, usage: [RunnerUsageWindow] = []
    ) {
        self.id = id
        self.label = label
        self.email = email
        self.plan = plan
        self.active = active
        self.usageRead = usageRead
        self.usageError = usageError
        self.usageFetchedAtMs = usageFetchedAtMs
        self.usage = usage
    }

    public var displayName: String {
        if !label.isEmpty { return label }
        if !email.isEmpty { return email }
        return "Account"
    }

    public func usageText(nowMs: Int64) -> String { RunnerAccounts.usageText(self, nowMs: nowMs) }
}

public struct RunnerAccountRequest: Equatable, Hashable, Sendable, Identifiable {
    public var id: String
    public var provider: String
    public var action: String
    public var account: String
    public var state: String
    public var message: String
    public var createdAtMs: Int64

    public init(
        id: String, provider: String, action: String, account: String, state: String, message: String = "",
        createdAtMs: Int64 = 0
    ) {
        self.id = id
        self.provider = provider
        self.action = action
        self.account = account
        self.state = state
        self.message = message
        self.createdAtMs = createdAtMs
    }

    public var pending: Bool { state == "pending" }
}

public struct RunnerProviderAccounts: Equatable, Hashable, Sendable, Identifiable {
    public var provider: String
    public var reported: Bool
    public var binaryFound: Bool
    public var loginState: String
    public var accounts: [RunnerAccount]

    public init(
        provider: String, reported: Bool = false, binaryFound: Bool = false, loginState: String = "",
        accounts: [RunnerAccount] = []
    ) {
        self.provider = provider
        self.reported = reported
        self.binaryFound = binaryFound
        self.loginState = loginState
        self.accounts = accounts
    }

    public var id: String { provider }

    public var label: String { RunnerSignIn.providerLong(provider) }

    public var activeAccount: RunnerAccount? { accounts.first { $0.active } }

    public var canSignIn: Bool { binaryFound }

    public var listed: Bool { binaryFound || !accounts.isEmpty }

    public var signInLabel: String {
        accounts.isEmpty ? "Sign in \(label)…" : "Add \(label) account…"
    }
}

public struct RunnerAccountsRunner: Equatable, Hashable, Sendable, Identifiable {
    public var name: String
    public var scope: String
    public var status: String
    public var lastSeenAtMs: Int64
    public var providers: [RunnerProviderAccounts]
    public var openLogins: [String: String]
    public var openTerminals: [String: String]
    public var features: [String]
    public var requests: [RunnerAccountRequest]

    public init(
        name: String, scope: String = "", status: String = "", lastSeenAtMs: Int64 = 0,
        providers: [RunnerProviderAccounts] = [], openLogins: [String: String] = [:],
        openTerminals: [String: String] = [:], features: [String] = [], requests: [RunnerAccountRequest] = []
    ) {
        self.name = name
        self.scope = scope
        self.status = status
        self.lastSeenAtMs = lastSeenAtMs
        self.providers = providers
        self.openLogins = openLogins
        self.openTerminals = openTerminals
        self.features = features
        self.requests = requests
    }

    public var id: String { name }

    public var active: Bool { status == "active" }

    public var terminals: Bool { features.contains(RunnerAccounts.terminalFeature) }

    public func provider(_ provider: String) -> RunnerProviderAccounts {
        providers.first { $0.provider == provider } ?? RunnerProviderAccounts(provider: provider)
    }

    public func pending(provider: String, account: String) -> Bool {
        requests.contains { $0.provider == provider && $0.account == account && $0.pending }
    }

    public var anyPending: Bool { requests.contains { $0.pending } }

    public func request(provider: String, account: String) -> RunnerAccountRequest? {
        var found: RunnerAccountRequest?
        for request in requests where request.provider == provider && request.account == account {
            if found == nil || request.createdAtMs > (found?.createdAtMs ?? 0) { found = request }
        }
        return found
    }

    public func signInRequest(_ provider: String) -> RunnerSignInRequest {
        RunnerSignInRequest(runner: name, provider: provider, openLoginId: openLogins[provider] ?? "")
    }
}

public enum RunnerAccounts {
    public static let activateAction: String = "activate"
    public static let removeAction: String = "remove"
    public static let terminalFeature: String = "terminal"
    public static let pendingPollSeconds: TimeInterval = 2
    public static let maxPendingReads: Int = 30
    public static let separator: String = " · "

    public static func accountsPath(_ runner: String) -> String {
        "/api/runners/\(runner.dartTrimmed.lowercased())/accounts"
    }

    public static func requestBody(provider: String, account: String, action: String) -> JSONValue {
        .object([
            "provider": .string(provider.dartTrimmed),
            "account": .string(account.dartTrimmed),
            "action": .string(action.dartTrimmed),
        ])
    }

    public static func activateBody(provider: String, account: String) -> JSONValue {
        requestBody(provider: provider, account: account, action: activateAction)
    }

    public static func removeBody(provider: String, account: String) -> JSONValue {
        requestBody(provider: provider, account: account, action: removeAction)
    }

    public static func usageErrorText(_ error: String) -> String {
        switch error {
        case "token_expired": return "usage refreshes after its next run"
        case "unauthorized": return "the provider refused this login; sign it in again"
        case "no_oauth_login": return "usage limits need a subscription sign-in"
        case "missing_login": return "saved login missing; sign it in again"
        case "unreachable": return "usage service unreachable"
        default: return "usage unavailable (\(error))"
        }
    }

    public static func durationText(_ ms: Int64) -> String {
        let minutes: Int64 = max(1, (ms + 30000) / 60000)
        if minutes < 60 { return "\(minutes) min" }
        let hours: Int64 = minutes / 60
        if hours < 48 { return "\(hours) h \(minutes % 60) min" }
        return "\((hours + 12) / 24) days"
    }

    public static func usageWindowText(_ window: RunnerUsageWindow, nowMs: Int64) -> String {
        var text: String = "\(window.label) \(Int(window.usedPercent.rounded()))%"
        if window.resetsAtMs > nowMs { text += " (resets in \(durationText(window.resetsAtMs - nowMs)))" }
        return text
    }

    public static func usageText(_ account: RunnerAccount, nowMs: Int64) -> String {
        if !account.usageRead { return "usage not read yet" }
        var parts: [String] = account.usage.map { usageWindowText($0, nowMs: nowMs) }
        if !account.usageError.isEmpty {
            parts.append(usageErrorText(account.usageError))
        } else if parts.isEmpty {
            parts.append("no usage limits reported")
        }
        return parts.joined(separator: separator)
    }

    public static func usageNote(_ account: RunnerAccount, nowMs: Int64) -> String {
        if !account.usageRead { return "Usage not read yet" }
        var parts: [String] = []
        if !account.usageError.isEmpty {
            parts.append(usageErrorText(account.usageError))
        } else if account.usage.isEmpty {
            parts.append("no usage limits reported")
        }
        if account.usageFetchedAtMs > 0 && nowMs > account.usageFetchedAtMs {
            parts.append("read \(durationText(nowMs - account.usageFetchedAtMs)) ago")
        }
        let text: String = parts.joined(separator: separator)
        return text.prefix(1).uppercased() + text.dropFirst()
    }

    static func parseAccount(_ object: JSONObject) -> RunnerAccount {
        var account: RunnerAccount = RunnerAccount(
            id: object.str("id").dartTrimmed, label: object.str("label").dartTrimmed,
            email: object.str("email").dartTrimmed, plan: object.str("plan").dartTrimmed, active: object.bool("active"))
        guard let usage = object["usage"]?.objectValue else { return account }
        account.usageRead = true
        account.usageError = usage.str("error").dartTrimmed
        account.usageFetchedAtMs = usage.int64("fetchedAt")
        for item in usage.maps("windows") {
            let id: String = item.str("id").dartTrimmed
            if id.isEmpty { continue }
            account.usage.append(RunnerUsageWindow(
                id: id, label: item.str("label").dartTrimmed, usedPercent: item.double("usedPercent"),
                resetsAtMs: item.int64("resetsAt"), windowMinutes: item.int("windowMinutes")))
        }
        return account
    }

    static func parseRequest(_ object: JSONObject) -> RunnerAccountRequest {
        RunnerAccountRequest(
            id: object.str("id").dartTrimmed, provider: object.str("provider").dartTrimmed,
            action: object.str("action").dartTrimmed, account: object.str("account").dartTrimmed,
            state: object.str("state").dartTrimmed, message: object.str("message").dartTrimmed,
            createdAtMs: object.int64("createdAt"))
    }

    public static func parse(runner row: JSONObject) -> RunnerAccountsRunner? {
        let name: String = row.str("name").dartTrimmed
        guard RunnerSignIn.isRunnerName(name) else { return nil }
        let availability: JSONObject = row.obj("availability")
        var providers: [RunnerProviderAccounts] = []
        var openLogins: [String: String] = [:]
        for provider in RunnerSignIn.providers {
            var state: RunnerProviderAccounts = RunnerProviderAccounts(provider: provider)
            if let entry = availability[provider]?.objectValue {
                state.reported = true
                state.binaryFound = entry.bool("binaryFound")
                state.loginState = entry.str("loginState")
                state.accounts = entry.maps("accounts").map(parseAccount).filter { RunnerSignIn.isLoginId($0.id) }
            }
            providers.append(state)
        }
        for login in row.maps("logins") {
            let id: String = login.str("id").dartTrimmed
            let provider: String = login.str("provider").dartTrimmed
            if RunnerSignIn.isLoginId(id), RunnerSignIn.loginStateOpen(login.str("state").dartTrimmed),
               openLogins[provider] == nil {
                openLogins[provider] = id
            }
        }
        var openTerminals: [String: String] = [:]
        for terminal in row.maps("terminals") {
            let id: String = terminal.str("id").dartTrimmed
            let provider: String = terminal.str("provider").dartTrimmed
            if RunnerSignIn.isLoginId(id), RunnerSignIn.isProvider(provider),
               RunnerSignIn.loginStateOpen(terminal.str("state").dartTrimmed), openTerminals[provider] == nil {
                openTerminals[provider] = id
            }
        }
        let features: [String] = (row["features"]?.arrayValue ?? []).compactMap { $0.stringValue }
            .filter { $0 == terminalFeature }
        let requests: [RunnerAccountRequest] = row.maps("accountRequests").map(parseRequest)
            .filter { RunnerSignIn.isLoginId($0.id) }
        return RunnerAccountsRunner(
            name: name, scope: row.str("scope").dartTrimmed, status: row.str("status").dartTrimmed,
            lastSeenAtMs: row.int64("lastSeenAt"), providers: providers, openLogins: openLogins,
            openTerminals: openTerminals, features: features, requests: requests)
    }

    public static func parse(_ payload: JSONValue) -> [RunnerAccountsRunner] {
        payload.maps("runners").compactMap { parse(runner: $0) }
    }

    public static func listed(_ runners: [RunnerAccountsRunner]) -> [RunnerAccountsRunner] {
        runners.compactMap { (runner: RunnerAccountsRunner) -> RunnerAccountsRunner? in
            guard runner.active || runner.status == RunnerStatus.paused.rawValue else { return nil }
            var shown: RunnerAccountsRunner = runner
            shown.providers = runner.providers.filter(\.listed)
            return shown.providers.isEmpty ? nil : shown
        }
    }

    public static func hasAccounts(_ runners: [RunnerAccountsRunner]) -> Bool {
        runners.contains { $0.providers.contains { !$0.accounts.isEmpty } }
    }

    public static func summary(_ runners: [RunnerAccountsRunner]) -> String {
        var parts: [String] = []
        for runner in runners {
            for provider in runner.providers {
                guard let account = provider.activeAccount else { continue }
                parts.append("\(runner.name) \(RunnerSignIn.providerShort(provider.provider)): \(account.displayName)")
            }
        }
        return parts.isEmpty ? "Choose the account each runner uses" : parts.joined(separator: separator)
    }

    public static func anyPending(_ runners: [RunnerAccountsRunner]) -> Bool {
        runners.contains { $0.anyPending }
    }
}
