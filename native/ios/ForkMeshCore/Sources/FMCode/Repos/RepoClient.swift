// The ApiService calls the repository list, repository detail (its
// non-collaboration tabs) and mirror import make, behind a protocol so the
// view models can be tested with a fake, plus the snapshot the tabs load
// through.

import FMApi
import FMBase
import Foundation
import Observation

/// The repository reads and owner-signed calls of ApiService. ApiService
/// conforms.
@MainActor
public protocol RepoClient: AnyObject {
    func repositories() async throws -> [Repository]
    func tree(_ owner: String, _ name: String, path: String, ref: String) async throws -> RepoTree
    func blob(_ owner: String, _ name: String, _ path: String, ref: String) async throws -> RepoBlob
    func rawURL(_ owner: String, _ name: String, _ path: String, ref: String) -> URL
    func cloneURL(_ owner: String, _ name: String) -> URL
    func clearRepoCache(_ owner: String, _ name: String)
    func branches(_ owner: String, _ name: String) async throws -> [RepoBranch]
    func searchRepo(_ owner: String, _ name: String, _ query: String, ref: String) async throws -> RepoSearchResults
    func commits(_ owner: String, _ name: String) async throws -> [JSONObject]
    func history(_ owner: String, _ name: String, ref: String) async throws -> RepoHistoryRead
    func commitDetail(_ owner: String, _ name: String, _ hash: String, fallbackCommit: JSONObject) async throws -> RepoCommitDetail
    func mirrorSet(_ owner: String, _ name: String) async throws -> RepoMirrorSet
    func releases(_ owner: String, _ name: String) async throws -> [RepoRelease]
    func desktopCommand(
        _ owner: String, _ name: String, ownerAccount: String, command: String, target: String, ts: String, sig: String,
        payload: JSONObject
    ) async throws -> DesktopCommandResult
    func agentSessions(_ owner: String, _ name: String, ownerAccount: String) async throws -> [AgentSession]
    func agentTranscript(_ owner: String, _ name: String, _ id: Int, ownerAccount: String) async throws -> AgentTranscript
    func createRepositoryImport(sourceUrl: String, targetOwner: String, mode: String, providerToken: String) async throws -> ApiJsonResult
}

extension ApiService: RepoClient {
    public func history(_ owner: String, _ name: String, ref: String) async throws -> RepoHistoryRead {
        RepoHistoryRead(json: try await historyRead(owner, name, ref: ref))
    }
}

extension RepoClient {
    public func history(_ owner: String, _ name: String, ref: String) async throws -> RepoHistoryRead {
        RepoHistoryRead(commits: try await commits(owner, name))
    }
}

/// An error that prints as "Exception: <message>".
public struct RepoException: Error, Equatable, Sendable, CustomStringConvertible {
    public let message: String

    public init(_ message: String) {
        self.message = message
    }

    public var description: String { "Exception: \(message)" }
}

/// A load's state: a fresh load keeps the previous data and error while it
/// waits, success replaces both, failure drops the data.
public struct RepoSnapshot<Value> {
    public var loading: Bool
    public var data: Value?
    /// The error's text, or nil.
    public var error: String?

    public init(loading: Bool = false, data: Value? = nil, error: String? = nil) {
        self.loading = loading
        self.data = data
        self.error = error
    }

    public var hasData: Bool { data != nil }
    public var hasError: Bool { error != nil }
}

extension RepoSnapshot: Equatable where Value: Equatable {}

/// One FutureBuilder: the view model owns it, the view reads `snapshot`. The
/// newest `load` wins, as a fresh Future does; a load whose task was cancelled
/// (the view went away) leaves the future ready to load again on `start`.
@MainActor
@Observable
public final class RepoFuture<Value> {
    public private(set) var snapshot: RepoSnapshot<Value>
    @ObservationIgnored private var generation = 0
    @ObservationIgnored public private(set) var started = false

    /// Without data it starts out waiting, as a FutureBuilder built with its
    /// future does.
    public init(data: Value? = nil) {
        snapshot = RepoSnapshot(loading: data == nil, data: data, error: nil)
    }

    public var loading: Bool { snapshot.loading }
    public var data: Value? { snapshot.data }
    public var error: String? { snapshot.error }

    /// Runs `operation` unless a load already started (FutureBuilder's
    /// `_future ??= ...`).
    public func start(_ operation: () async throws -> Value) async {
        if started { return }
        await load(operation)
    }

    /// A fresh future.
    public func load(_ operation: () async throws -> Value) async {
        started = true
        generation += 1
        let mine = generation
        snapshot.loading = true
        do {
            let value = try await operation()
            guard mine == generation else { return }
            snapshot = RepoSnapshot(loading: false, data: value, error: nil)
        } catch {
            guard mine == generation else { return }
            if Task.isCancelled || error is CancellationError {
                snapshot.loading = false
                started = false
                return
            }
            snapshot = RepoSnapshot(loading: false, data: nil, error: dartErrorText(error))
        }
    }

    /// Sets a value without a load (tests, and screens handed their data).
    public func resolve(_ value: Value) {
        started = true
        generation += 1
        snapshot = RepoSnapshot(loading: false, data: value, error: nil)
    }
}
