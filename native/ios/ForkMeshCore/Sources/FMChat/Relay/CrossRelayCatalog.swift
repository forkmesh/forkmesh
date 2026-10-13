// Every known relay's repository catalog, side by side.
//
// The one place the app deliberately talks to more than one relay, and read-only for exactly that
// reason: it answers "what is on my other network?" without moving the app there. Each relay is
// fetched independently with its own session token, in parallel; one relay being down, slow, or
// signed out never blanks the others.

import FMAccount
import FMApi
import FMBase
import FMNet
import Foundation
import Observation

/// One relay's slice of the cross-relay repository list.
public struct RelayCatalogEntry: Equatable, Sendable, Identifiable {
    public let relay: RelayProfile
    public let active: Bool
    /// Whether this phone holds a session on the relay. A signed-out relay is listed (with an
    /// invitation to sign in) rather than hidden.
    public let signedIn: Bool
    public let repositories: [Repository]
    public let loading: Bool
    public let error: String

    public init(
        relay: RelayProfile, active: Bool, signedIn: Bool, repositories: [Repository] = [], loading: Bool = false,
        error: String = ""
    ) {
        self.relay = relay
        self.active = active
        self.signedIn = signedIn
        self.repositories = repositories
        self.loading = loading
        self.error = error
    }

    public var id: String { relay.id }

    public func copyWith(repositories: [Repository]? = nil, loading: Bool? = nil, error: String? = nil) -> RelayCatalogEntry {
        RelayCatalogEntry(
            relay: relay, active: active, signedIn: signedIn, repositories: repositories ?? self.repositories,
            loading: loading ?? self.loading, error: error ?? self.error)
    }
}

@MainActor
@Observable
public final class CrossRelayCatalog {
    @ObservationIgnored let settings: SettingsService
    @ObservationIgnored let auth: AuthService
    @ObservationIgnored let http: HTTPClient
    @ObservationIgnored let timeout: TimeInterval

    public private(set) var entries: [RelayCatalogEntry] = []
    public private(set) var loading = false
    @ObservationIgnored private var generation = 0

    public init(settings: SettingsService, auth: AuthService, http: HTTPClient, timeout: TimeInterval = 10) {
        self.settings = settings
        self.auth = auth
        self.http = http
        self.timeout = timeout
    }

    /// Repositories across every relay that answered.
    public var totalRepositories: Int { entries.reduce(0) { $0 + $1.repositories.count } }

    public func refresh() async {
        generation += 1
        let current = generation
        let activeId = settings.relays.activeId
        entries = settings.relays.relays.filter(\.hasInstance).map { relay in
            let signedIn = auth.sessionForRelay(relay.id) != nil
            return RelayCatalogEntry(relay: relay, active: relay.id == activeId, signedIn: signedIn, loading: signedIn)
        }
        loading = true

        // In parallel: a relay that takes its full timeout must not delay every relay behind it.
        let jobs: [(Int, RelayProfile, String)] = entries.enumerated().compactMap { index, entry in
            guard entry.signedIn else { return nil }
            return (index, entry.relay, auth.sessionForRelay(entry.relay.id)?.sessionToken ?? "")
        }
        await withTaskGroup(of: Void.self) { group in
            for (index, relay, token) in jobs {
                group.addTask { [http, timeout] in
                    let outcome: Result<[Repository], Error>
                    do {
                        outcome = .success(try await CrossRelayCatalog.fetch(http, relay, token, timeout))
                    } catch {
                        outcome = .failure(error)
                    }
                    await self.apply(current, index, outcome)
                }
            }
        }
        if current != generation { return }
        loading = false
    }

    private func apply(_ current: Int, _ index: Int, _ outcome: Result<[Repository], Error>) {
        if current != generation || index >= entries.count { return }
        switch outcome {
        case .success(let repos):
            entries[index] = entries[index].copyWith(repositories: repos, loading: false)
        case .failure(let error):
            entries[index] = entries[index].copyWith(loading: false, error: CrossRelayCatalog.humanize(error))
        }
    }

    nonisolated static func fetch(_ http: HTTPClient, _ relay: RelayProfile, _ token: String, _ timeout: TimeInterval) async throws
        -> [Repository]
    {
        guard let url = URL(string: "\(relay.httpOrigin)/api/repositories") else {
            throw CatalogError("\(relay.label) answered 0.")
        }
        var headers: HTTPHeaders = ["Accept": "application/json"]
        if !token.isEmpty { headers["Authorization"] = "Bearer \(token)" }
        let response = try await Deadline.send(
            http, HTTPRequest(method: "GET", url: url, headers: headers, timeout: timeout), seconds: timeout)
        if response.status == 401 {
            // Deliberately does not sign the user out: this read happens in the background for a
            // relay they are not even on.
            throw CatalogError("Your session on \(relay.label) has expired.")
        }
        if response.status < 200 || response.status >= 300 {
            throw CatalogError("\(relay.label) answered \(response.status).")
        }
        let decoded = try JSON.parse(response.body)
        let list: [JSONValue]
        if let array = decoded.arrayValue {
            list = array
        } else if let array = decoded["repositories"]?.arrayValue {
            list = array
        } else {
            list = []
        }
        let repos = list.compactMap(\.objectValue).map { Repository(json: $0) }.filter {
            !$0.owner.isEmpty && !$0.name.isEmpty
        }
        return collapseRepositoryMirrors(repos)
    }

    nonisolated static func humanize(_ error: Error) -> String {
        if let http = error as? HTTPError, http == .timedOut { return "That relay did not answer in time." }
        var text: String
        if let catalog = error as? CatalogError {
            text = catalog.message
        } else {
            text = dartErrorText(error)
            if let range = text.range(of: "Exception: ") { text.removeSubrange(range) }
        }
        text = text.dartTrimmed
        if text.isEmpty { return "Could not read that relay." }
        return text.hasSuffix(".") ? text : "\(text)."
    }
}

struct CatalogError: Error {
    let message: String
    init(_ message: String) { self.message = message }
}
