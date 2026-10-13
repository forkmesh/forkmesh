import FMBase
import FMNet
import Foundation
import Observation

@MainActor
@Observable
public final class RunnerAttentionModel {
    nonisolated public static let defaultPollFloor: TimeInterval = 300
    nonisolated public static let forcedFloor: TimeInterval = 5
    nonisolated public static let attentionRetry: TimeInterval = 30 * 60

    public private(set) var items: [RunnerAttentionItem] = []
    public var request: RunnerSignInRequest?
    public var fallbackURL: URL?
    public private(set) var pollFloor: TimeInterval = RunnerAttentionModel.defaultPollFloor

    @ObservationIgnored private let api: ApiService
    @ObservationIgnored private let clock: Clock
    @ObservationIgnored private var lastFetch: Date?
    @ObservationIgnored private var attentionMissingUntil: Date?
    @ObservationIgnored private var loading = false
    @ObservationIgnored private var seenAlerts: Set<String> = []

    public init(api: ApiService, clock: Clock = SystemClock()) {
        self.api = api
        self.clock = clock
    }

    public var visible: Bool { !items.isEmpty }

    public var anyExpired: Bool { RunnerSignIn.anyExpired(items) }

    public var title: String { RunnerSignIn.summaryTitle(items) }

    public var usesAttentionEndpoint: Bool {
        guard let until = attentionMissingUntil else { return true }
        return clock.now >= until
    }

    public func refresh(force: Bool = false) async {
        if loading { return }
        guard api.hasSession else {
            items = []
            return
        }
        let now: Date = clock.now
        let floor: TimeInterval = force ? RunnerAttentionModel.forcedFloor : pollFloor
        if let last = lastFetch, now.timeIntervalSince(last) < floor { return }
        loading = true
        lastFetch = now
        defer { loading = false }
        do {
            if usesAttentionEndpoint {
                let response: HTTPResponse = try await get(RunnerSignIn.attentionPath())
                if response.status == 404 || response.status == 405 {
                    attentionMissingUntil = now.addingTimeInterval(RunnerAttentionModel.attentionRetry)
                } else if let parsed = parse(response, RunnerSignIn.parseAttention) {
                    items = parsed
                    notePollMs(response)
                    return
                } else if response.ok {
                    attentionMissingUntil = now.addingTimeInterval(RunnerAttentionModel.attentionRetry)
                } else {
                    return
                }
            }
            let response: HTTPResponse = try await get(RunnerSignIn.runnersPath())
            if let parsed = parse(response, RunnerSignIn.derive(runners:)) { items = parsed }
        } catch {
            return
        }
    }

    private func notePollMs(_ response: HTTPResponse) {
        let ms: Int64 = response.jsonObject.int64("pollMs")
        if ms > 0 { pollFloor = min(3600, max(30, TimeInterval(ms) / 1000)) }
    }

    private func get(_ path: String) async throws -> HTTPResponse {
        try await api.rawRequest("GET", api.url(path), headers: ["Accept": "application/json"])
    }

    private func parse(_ response: HTTPResponse, _ read: (JSONValue) -> [RunnerAttentionItem]) -> [RunnerAttentionItem]? {
        if response.status == 401 || response.status == 403 { return [] }
        guard response.ok, let value = try? JSON.parse(response.body) else { return nil }
        if value.objectValue?["ok"] == .bool(false) { return nil }
        return read(value)
    }

    public func noteNotifications(_ notifications: [ForkNotification]) async {
        var fresh: Bool = false
        for notification in notifications where notification.isUnread && RunnerSignIn.isRunnerAlert(notification) {
            let key: String = notification.id.isEmpty ? "\(notification.ts):\(notification.href)" : notification.id
            if seenAlerts.insert(key).inserted { fresh = true }
        }
        if fresh { await refresh(force: true) }
    }

    public func open(_ item: RunnerAttentionItem) {
        if item.canConnect {
            open(item.request)
        } else {
            fallbackURL = dashboardURL(item.request)
        }
    }

    public func open(_ next: RunnerSignInRequest) {
        if api.hasSession {
            request = next
        } else {
            fallbackURL = dashboardURL(next)
        }
    }

    public func open(runner: String, provider: String) {
        let request = RunnerSignInRequest(runner: runner.dartTrimmed.lowercased(), provider: provider.dartTrimmed.lowercased())
        if let known = items.first(where: { $0.runner == request.runner && $0.provider == request.provider }) {
            open(known)
        } else {
            open(request)
        }
    }

    @discardableResult
    public func openHref(_ href: String) -> Bool {
        guard let parsed = RunnerSignIn.request(fromHref: href) else { return false }
        if let known = items.first(where: { $0.runner == parsed.runner && $0.provider == parsed.provider }) {
            open(known)
        } else {
            open(parsed)
        }
        return true
    }

    func settle(delays: [TimeInterval] = [6, 14]) async {
        await refresh(force: true)
        for delay in delays {
            try? await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000))
            await refresh(force: true)
        }
    }

    public func dashboardURL(_ request: RunnerSignInRequest) -> URL? {
        api.webLink(request.connectPath)
    }

    public func loginModel(for request: RunnerSignInRequest) -> RunnerLoginModel {
        RunnerLoginModel(api: api, request: request, onEnded: { [weak self] in
            Task { await self?.settle() }
        })
    }
}

@MainActor
@Observable
public final class RunnerLoginModel {
    public let request: RunnerSignInRequest
    public private(set) var text: String = ""
    public private(set) var urls: [URL] = []
    public private(set) var status: String = ""
    public private(set) var state: String = ""
    public private(set) var failed: Bool = false
    public private(set) var fallbackURL: URL?
    public private(set) var sending: Bool = false
    public var code: String = ""
    public var inputError: String = ""

    @ObservationIgnored public let session: RunnerLoginSession
    @ObservationIgnored private let api: ApiService
    @ObservationIgnored private let sleeper: @Sendable (TimeInterval) async throws -> Void
    @ObservationIgnored private let onEnded: @MainActor () -> Void
    @ObservationIgnored private var started = false
    @ObservationIgnored private var ended = false

    public init(
        api: ApiService, request: RunnerSignInRequest,
        sleep: @escaping @Sendable (TimeInterval) async throws -> Void = { seconds in
            try await Task.sleep(nanoseconds: UInt64(max(0, seconds) * 1_000_000_000))
        },
        onEnded: @escaping @MainActor () -> Void = {}
    ) {
        self.api = api
        self.request = request
        self.sleeper = sleep
        self.onEnded = onEnded
        self.session = RunnerLoginSession(runner: request.runner, provider: request.provider)
    }

    public var open: Bool { session.open }

    public var canSubmit: Bool { open && !sending && !RunnerSignIn.codeSubmission(code).isEmpty }

    public func run() async {
        if started { return }
        started = true
        await start()
        await poll()
    }

    func start() async {
        guard api.hasSession else {
            fallbackURL = api.webLink(request.connectPath)
            session.markFailed("Sign in to your ForkMesh account on this phone first.")
            sync()
            return
        }
        if RunnerSignIn.isLoginId(request.openLoginId) {
            session.attach(request.openLoginId)
            sync()
            return
        }
        do {
            let body: Data = JSON.encodeData(.object(["provider": .string(request.provider)]))
            let response: HTTPResponse = try await api.rawRequest(
                "POST", api.url(RunnerSignIn.startPath(request.runner)),
                headers: ["Accept": "application/json", "Content-Type": "application/json"], body: body)
            session.applyStartReply(status: response.status, body: response.jsonObject)
            if response.status == 401 || response.status == 403 {
                fallbackURL = api.webLink(request.connectPath)
            }
        } catch {
            session.markFailed("The sign-in could not be started: \(dartErrorText(error))")
        }
        sync()
    }

    func poll() async {
        while session.open && !Task.isCancelled {
            let again: Bool = await pollOnce()
            if !session.open { break }
            if again { continue }
            do {
                try await sleeper(RunnerSignIn.pollSeconds)
            } catch {
                break
            }
        }
        finishIfEnded()
    }

    func pollOnce() async -> Bool {
        let path: String = RunnerSignIn.outputPath(session.runner, session.id)
        let url: URL = api.url(path, [("after", String(session.afterSeq))])
        do {
            let response: HTTPResponse = try await api.rawRequest("GET", url, headers: ["Accept": "application/json"])
            if [401, 403, 404].contains(response.status) {
                session.markFailed(
                    response.status == 404
                        ? "The sign-in is no longer available."
                        : "This account may not read this sign-in any more.")
                sync()
                return false
            }
            if response.ok {
                session.applyOutputReply(response.jsonObject)
                sync()
                return session.more
            }
        } catch {
            return false
        }
        return false
    }

    public func submitCode() async {
        let data: String = RunnerSignIn.codeSubmission(code)
        guard !data.isEmpty, RunnerSignIn.inputValid(data) else {
            inputError = "The code must be 1 to 4096 bytes of printable text."
            return
        }
        guard session.open else { return }
        inputError = ""
        sending = true
        defer { sending = false }
        do {
            let body: Data = JSON.encodeData(.object(["data": .string(data)]))
            let response: HTTPResponse = try await api.rawRequest(
                "POST", api.url(RunnerSignIn.inputPath(session.runner, session.id)),
                headers: ["Accept": "application/json", "Content-Type": "application/json"], body: body)
            if response.ok {
                code = ""
            } else {
                let message: String = response.jsonObject.str("message")
                inputError = message.isEmpty ? "The code was not sent (HTTP \(response.status))." : message
            }
        } catch {
            inputError = "The code was not sent: \(dartErrorText(error))"
        }
    }

    public func cancel() async {
        guard session.needsCancelOnClose else { return }
        do {
            let response: HTTPResponse = try await api.rawRequest(
                "POST", api.url(RunnerSignIn.cancelPath(session.runner, session.id)),
                headers: ["Accept": "application/json", "Content-Type": "application/json"], body: Data("{}".utf8))
            if response.ok {
                session.applyCancelReply(response.jsonObject)
            } else {
                session.applyCancelReply(["state": .string("cancelled")])
            }
        } catch {
            session.applyCancelReply(["state": .string("cancelled")])
        }
        sync()
        finishIfEnded()
    }

    public func close() async {
        await cancel()
        finishIfEnded(force: true)
    }

    private func finishIfEnded(force: Bool = false) {
        if ended { return }
        if force || !session.open {
            ended = true
            onEnded()
        }
    }

    private func sync() {
        text = session.text
        urls = session.urls
        state = session.state
        failed = session.failed
        status = session.statusText
    }
}
