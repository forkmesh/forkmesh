import FMBase
import FMNet
import Foundation
import Observation

@MainActor
@Observable
public final class RunnerAccountsModel {
    nonisolated public static let staleSeconds: TimeInterval = 300

    public private(set) var runners: [RunnerAccountsRunner] = []
    public private(set) var loaded: Bool = false
    public private(set) var loading: Bool = false
    public private(set) var message: String = ""
    public private(set) var messageIsError: Bool = false
    public private(set) var sending: Set<String> = []
    public private(set) var anyPending: Bool = false

    @ObservationIgnored private let api: ApiService
    @ObservationIgnored private let clock: Clock
    @ObservationIgnored private let sleeper: @Sendable (TimeInterval) async throws -> Void
    @ObservationIgnored private var lastFetch: Date?
    @ObservationIgnored private var pendingReads: Int = 0
    @ObservationIgnored private var following: Bool = false

    public init(
        api: ApiService, clock: Clock = SystemClock(),
        sleep: @escaping @Sendable (TimeInterval) async throws -> Void = { seconds in
            try await Task.sleep(nanoseconds: UInt64(max(0, seconds) * 1_000_000_000))
        }
    ) {
        self.api = api
        self.clock = clock
        self.sleeper = sleep
    }

    public var hasSession: Bool { api.hasSession }

    public var hasAccounts: Bool { RunnerAccounts.hasAccounts(runners) }

    public var visible: Bool { hasAccounts }

    public var summary: String { RunnerAccounts.summary(runners) }

    public var emptyText: String {
        if !api.hasSession { return "Sign in to your ForkMesh account to manage runner accounts." }
        if !loaded { return "Loading runners…" }
        return "No runner has Claude Code or Codex installed."
    }

    public func isPending(_ runner: RunnerAccountsRunner, provider: String, account: String) -> Bool {
        sending.contains(Self.key(runner.name, provider, account)) || runner.pending(provider: provider, account: account)
    }

    public func refresh() async {
        if let last = lastFetch, clock.now.timeIntervalSince(last) < RunnerAccountsModel.staleSeconds { return }
        await load()
    }

    public func load() async {
        if loading { return }
        guard api.hasSession else {
            clear()
            return
        }
        loading = true
        lastFetch = clock.now
        defer { loading = false }
        do {
            let response: HTTPResponse = try await api.rawRequest(
                "GET", api.url(RunnerSignIn.runnersPath()), headers: ["Accept": "application/json"])
            if response.status == 401 || response.status == 403 {
                clear()
                return
            }
            let value: JSONValue? = response.ok ? try? JSON.parse(response.body) : nil
            guard let value, value.objectValue?["ok"] != .bool(false) else {
                let text: String = response.jsonObject.str("message").dartTrimmed
                show("Could not load runners: \(text.isEmpty ? "HTTP \(response.status)" : text)", error: true)
                return
            }
            let all: [RunnerAccountsRunner] = RunnerAccounts.parse(value)
            runners = RunnerAccounts.listed(all)
            anyPending = RunnerAccounts.anyPending(all)
            loaded = true
        } catch {
            return
        }
    }

    public func activate(runner: String, provider: String, account: RunnerAccount) async {
        if account.active { return }
        await change(runner: runner, provider: provider, account: account, action: RunnerAccounts.activateAction)
    }

    public func remove(runner: String, provider: String, account: RunnerAccount) async {
        await change(runner: runner, provider: provider, account: account, action: RunnerAccounts.removeAction)
    }

    private func change(runner: String, provider: String, account: RunnerAccount, action: String) async {
        let name: String = runner.dartTrimmed.lowercased()
        let removing: Bool = action == RunnerAccounts.removeAction
        guard RunnerSignIn.isRunnerName(name), RunnerSignIn.isProvider(provider), RunnerSignIn.isLoginId(account.id)
        else { return }
        guard api.hasSession else {
            show("Sign in to your ForkMesh account on this phone to manage runner accounts.", error: true)
            return
        }
        let key: String = Self.key(name, provider, account.id)
        if sending.contains(key) { return }
        sending.insert(key)
        var accepted: Bool = false
        do {
            let body: Data = JSON.encodeData(
                RunnerAccounts.requestBody(provider: provider, account: account.id, action: action))
            let response: HTTPResponse = try await api.rawRequest(
                "POST", api.url(RunnerAccounts.accountsPath(name)),
                headers: ["Accept": "application/json", "Content-Type": "application/json"], body: body)
            accepted = response.ok
            if accepted {
                show(removing
                    ? "Logging \(account.displayName) out of \(name); the runner applies it on its next check-in."
                    : "Switching \(name) to \(account.displayName); the runner applies it on its next check-in.",
                    error: false)
            } else {
                let text: String = response.jsonObject.str("message").dartTrimmed
                show("\(Self.failure(action)): \(text.isEmpty ? "HTTP \(response.status)" : text)", error: true)
            }
        } catch {
            show("\(Self.failure(action)): \(dartErrorText(error))", error: true)
        }
        guard accepted else {
            sending.remove(key)
            return
        }
        pendingReads = RunnerAccounts.maxPendingReads
        if following {
            sending.remove(key)
            return
        }
        following = true
        await load()
        sending.remove(key)
        while anyPending && pendingReads > 0 && !Task.isCancelled {
            pendingReads -= 1
            do {
                try await sleeper(RunnerAccounts.pendingPollSeconds)
            } catch {
                break
            }
            await load()
        }
        following = false
        report(runner: name, provider: provider, account: account)
    }

    public func signInRequest(runner: RunnerAccountsRunner, provider: String) -> RunnerSignInRequest {
        runner.signInRequest(provider)
    }

    public func loginModel(for request: RunnerSignInRequest) -> RunnerLoginModel {
        RunnerLoginModel(api: api, request: request, onEnded: { [weak self] in
            Task { await self?.settle() }
        })
    }

    func settle(delays: [TimeInterval] = [6, 14]) async {
        await load()
        for delay in delays {
            do {
                try await sleeper(delay)
            } catch {
                return
            }
            await load()
        }
    }

    public func dismissMessage() {
        message = ""
        messageIsError = false
    }

    private func report(runner: String, provider: String, account: RunnerAccount) {
        guard let row = runners.first(where: { $0.name == runner }),
              let request = row.request(provider: provider, account: account.id) else { return }
        let removing: Bool = request.action == RunnerAccounts.removeAction
        switch request.state {
        case "done":
            show(removing
                ? "\(account.displayName) is logged out of \(RunnerSignIn.providerLong(provider)) on \(runner)."
                : "\(runner) now runs \(RunnerSignIn.providerLong(provider)) as \(account.displayName).",
                error: false)
        case "failed":
            let fallback: String = removing ? "the runner could not log it out" : "the runner could not switch accounts"
            show("\(Self.failure(request.action)): \(request.message.isEmpty ? fallback : request.message)", error: true)
        default:
            return
        }
    }

    private static func failure(_ action: String) -> String {
        action == RunnerAccounts.removeAction ? "Could not log the account out" : "Could not change the runner account"
    }

    private func show(_ text: String, error: Bool) {
        message = text
        messageIsError = error
    }

    private func clear() {
        runners = []
        anyPending = false
        loaded = false
        message = ""
        messageIsError = false
    }

    private static func key(_ runner: String, _ provider: String, _ account: String) -> String {
        "\(runner):\(provider):\(account)"
    }
}
