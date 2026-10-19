import FMBase
import FMNet
import Foundation
import Observation

@MainActor
@Observable
public final class RunnersModel {
    public private(set) var runners: [RunnerListEntry] = []
    public private(set) var loaded: Bool = false
    public private(set) var loading: Bool = false
    public private(set) var refused: Bool = false
    public private(set) var error: String = ""
    public private(set) var message: String = ""
    public private(set) var messageIsError: Bool = false
    public private(set) var busy: Set<String> = []

    @ObservationIgnored private let api: ApiService

    public init(api: ApiService) {
        self.api = api
    }

    public var hasSession: Bool { api.hasSession }

    public var signedOut: Bool { !api.hasSession || refused }

    public var showsSpinner: Bool { !loaded && error.isEmpty && !signedOut }

    public var showsError: Bool { !loaded && !error.isEmpty && !signedOut }

    public var showsEmpty: Bool { loaded && runners.isEmpty && !signedOut }

    public var addRunnerURL: URL? { api.webLink(RunnerList.dashboardPath) }

    public func isBusy(_ runner: RunnerListEntry) -> Bool { busy.contains(runner.name) }

    public func load() async {
        if loading { return }
        guard api.hasSession else {
            clear()
            return
        }
        loading = true
        defer { loading = false }
        do {
            let response: HTTPResponse = try await api.rawRequest(
                "GET", api.url(RunnerList.path()), headers: ["Accept": "application/json"])
            if response.status == 401 || response.status == 403 {
                clear()
                refused = true
                return
            }
            let value: JSONValue? = response.ok ? try? JSON.parse(response.body) : nil
            guard let value, value.objectValue?["ok"] != .bool(false) else {
                fail("Could not load runners: \(Self.reason(response))")
                return
            }
            runners = RunnerList.parse(value)
            refused = false
            loaded = true
            error = ""
        } catch {
            fail("Could not load runners: \(dartErrorText(error))")
        }
    }

    public func pause(_ runner: RunnerListEntry) async { await perform(.pause, runner) }

    public func resume(_ runner: RunnerListEntry) async { await perform(.resume, runner) }

    public func revoke(_ runner: RunnerListEntry) async { await perform(.revoke, runner) }

    public func perform(_ action: RunnerAction, _ runner: RunnerListEntry) async {
        let name: String = runner.name
        guard RunnerSignIn.isRunnerName(name), !busy.contains(name) else { return }
        guard api.hasSession else {
            show("Sign in to your ForkMesh account on this phone to manage runners.", error: true)
            return
        }
        busy.insert(name)
        defer { busy.remove(name) }
        do {
            let response: HTTPResponse = try await api.rawRequest(
                "POST", api.url(action.path),
                headers: ["Accept": "application/json", "Content-Type": "application/json"],
                body: JSON.encodeData(RunnerList.body(name: name)))
            guard response.ok else {
                show("\(action.failure): \(Self.reason(response))", error: true)
                if response.status == 409 { await load() }
                return
            }
            let status: String = response.jsonObject.str("status")
            apply(name, status.isEmpty ? action.after : RunnerStatus(raw: status))
            show(action.done(name), error: false)
        } catch {
            show("\(action.failure): \(dartErrorText(error))", error: true)
            return
        }
        await load()
    }

    public func dismissMessage() {
        message = ""
        messageIsError = false
    }

    private func apply(_ name: String, _ status: RunnerStatus) {
        runners = RunnerList.ordered(runners.map { (entry: RunnerListEntry) -> RunnerListEntry in
            guard entry.name == name else { return entry }
            var next: RunnerListEntry = entry
            next.status = status
            next.accounts.status = status.rawValue
            return next
        })
    }

    private func show(_ text: String, error: Bool) {
        message = text
        messageIsError = error
    }

    private func fail(_ text: String) {
        if loaded {
            show(text, error: true)
        } else {
            error = text
        }
    }

    private func clear() {
        runners = []
        loaded = false
        refused = false
        error = ""
        message = ""
        messageIsError = false
    }

    private static func reason(_ response: HTTPResponse) -> String {
        let text: String = response.jsonObject.str("message").dartTrimmed
        if !text.isEmpty { return text }
        if response.status == 404 { return "That runner is not one of yours." }
        return "HTTP \(response.status)"
    }
}
