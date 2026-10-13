// The state and actions of the local repositories section (with the local
// mesh identities) and of the clone-to-device flow.

import FMAccount
import FMApi
import FMBase
import Foundation
import Observation

/// "On this device" block of the Repos tab: create / import entry points and
/// the busy card over a running store operation.
@MainActor
@Observable
public final class LocalReposSectionModel {
    public let store: LocalRepoStore
    @ObservationIgnored let session: @MainActor () -> AuthSession?
    @ObservationIgnored let prefs: KeyValueStore?
    @ObservationIgnored let publisher: PublishService?
    @ObservationIgnored private var syncStopped = false

    public private(set) var loadError: String?
    /// Label of the cancellable busy card while a create/import runs.
    public private(set) var busyLabel: String?
    public var notice: LocalNotice?

    public init(
        store: LocalRepoStore,
        session: @escaping @MainActor () -> AuthSession?,
        prefs: KeyValueStore? = nil,
        publisher: PublishService? = nil
    ) {
        self.store = store
        self.session = session
        self.prefs = prefs
        self.publisher = publisher
    }

    public var records: [LocalRepoRecord] { store.records }
    public var isLoaded: Bool { store.isLoaded }
    public var isBusy: Bool { store.isBusy || busyLabel != nil }
    public var author: GitAuthor { LocalRepoText.author(session(), prefs: prefs) }
    public var defaultBranch: String { LocalRepoText.defaultBranch(prefs) ?? SettingsService.fallbackBranch }
    public var createHintText: String {
        "Creates README.md and an initial commit on \(defaultBranch). Stays on this device until you publish it."
    }

    public static let emptyMessage =
        "No local repositories yet. Create one or import a folder from "
        + "Files — everything stays on this phone until you publish."
    public static let createHint =
        "Creates README.md and an initial commit on main. Stays on this device until you publish it."

    public func load() async {
        if store.isLoaded { return }
        await store.ensureLoaded()
        loadError = store.loadError
    }

    public func create(name: String, description: String) async -> LocalRepoRecord? {
        let author = self.author
        let branch: String? = LocalRepoText.defaultBranch(prefs)
        return await runBusy("Creating \(name)") {
            try await self.store.create(name: name, author: author, description: description, branch: branch)
        }
    }

    public func importFolder(name: String, source: URL) async -> LocalRepoRecord? {
        let author = self.author
        let branch: String? = LocalRepoText.defaultBranch(prefs)
        return await runBusy("Importing \(name)") {
            try await self.store.importDirectory(name: name, source: source, author: author, branch: branch)
        }
    }

    public func importZip(name: String, zipPath: String) async -> LocalRepoRecord? {
        let author = self.author
        let branch: String? = LocalRepoText.defaultBranch(prefs)
        return await runBusy("Importing \(name)") {
            try await self.store.importZip(name: name, zipPath: zipPath, author: author, branch: branch)
        }
    }

    public func cancel() {
        syncStopped = true
        store.cancelActive()
    }

    public var syncableRecords: [LocalRepoRecord] { records.filter(\.isPublished) }
    public var canSync: Bool { publisher != nil && !syncableRecords.isEmpty }
    public var cloneCount: Int { records.filter(\.isCloned).count }

    public func sync(_ record: LocalRepoRecord) async {
        await sync([record])
    }

    public func syncAll() async {
        await sync(syncableRecords)
    }

    private func sync(_ targets: [LocalRepoRecord]) async {
        guard let publisher, !targets.isEmpty, busyLabel == nil else { return }
        syncStopped = false
        var results = [(record: LocalRepoRecord, result: LocalPullResult)]()
        var failures = [(record: LocalRepoRecord, message: String)]()
        defer { busyLabel = nil }
        for (index, record) in targets.enumerated() where !syncStopped {
            let step = targets.count > 1 ? " (\(index + 1) of \(targets.count))" : ""
            let title = "Syncing \(record.name)\(step)"
            busyLabel = title
            do {
                let result = try await store.sync(record, publisher: publisher) { [weak self] progress in
                    Task { @MainActor in
                        guard let self, self.busyLabel != nil, !self.syncStopped else { return }
                        self.busyLabel = "\(title) · \(progress.label)"
                    }
                }
                results.append((record, result))
            } catch {
                if syncStopped { break }
                failures.append((record, LocalRepoScreenModel.message(error)))
            }
        }
        if syncStopped {
            notice = LocalNotice("Sync cancelled.")
            return
        }
        notice = LocalNotice(LocalReposSectionModel.syncSummary(results: results, failures: failures), isError: !failures.isEmpty)
    }

    static func syncSummary(
        results: [(record: LocalRepoRecord, result: LocalPullResult)],
        failures: [(record: LocalRepoRecord, message: String)]
    ) -> String {
        if results.count + failures.count == 1 {
            if let only = results.first { return "\(only.record.name): \(only.result.message)" }
            if let only = failures.first { return "\(only.record.name): \(only.message)" }
        }
        let updated = results.filter { $0.result.changedWorktree }.count
        var parts = ["Synced \(results.count) of \(results.count + failures.count) repositories"]
        if updated > 0 { parts.append("\(updated) updated") }
        let attention = results.filter { $0.result.needsAttention }.map(\.record.name)
        if !attention.isEmpty { parts.append("check \(attention.joined(separator: ", "))") }
        if !failures.isEmpty { parts.append("failed: \(failures.map(\.record.name).joined(separator: ", "))") }
        return parts.joined(separator: " · ")
    }

    /// Run a store operation behind the cancellable busy card.
    func runBusy<T>(_ label: String, _ body: () async throws -> T) async -> T? {
        busyLabel = label
        defer { busyLabel = nil }
        do {
            return try await body()
        } catch {
            notice = LocalNotice(LocalRepoScreenModel.message(error), isError: true)
            return nil
        }
    }

    /// `_slugify`: a picked folder or zip name as a valid repository name.
    public static func slugify(_ raw: String) -> String {
        var out = ""
        var pendingDash = false
        for scalar in raw.unicodeScalars {
            let v = scalar.value
            let keep = (0x41...0x5a).contains(v) || (0x61...0x7a).contains(v) || (0x30...0x39).contains(v)
                || v == 0x2e || v == 0x5f || v == 0x2d
            if keep {
                if pendingDash { out += "-" }
                pendingDash = false
                out.unicodeScalars.append(scalar)
            } else {
                pendingDash = true
            }
        }
        if pendingDash { out += "-" }
        // Collapse runs of dashes, then trim leading/trailing dashes and dots.
        var collapsed = ""
        for character in out {
            if character == "-" && collapsed.last == "-" { continue }
            collapsed.append(character)
        }
        var scalars = Substring(collapsed)
        while let first = scalars.first, first == "-" || first == "." { scalars = scalars.dropFirst() }
        while let last = scalars.last, last == "-" || last == "." { scalars = scalars.dropLast() }
        let cleaned = String(scalars)
        if cleaned.isEmpty { return "imported" }
        return cleaned.count > 100 ? String(cleaned.prefix(100)) : cleaned
    }

    /// The suggested name for a picked zip file name ("proj.zip" -> "proj").
    public static func zipBaseName(_ fileName: String) -> String {
        fileName.lowercased().hasSuffix(".zip") ? String(fileName.dropLast(4)) : fileName
    }
}

extension LocalRepoStore {
    /// `localMeshIdentities`: every `owner/name` (lower-cased) that already has
    /// a row in "On this device". The network catalog drops these duplicates;
    /// the local row carries the mesh identity and sync state instead.
    public var meshIdentities: Set<String> {
        Set(records.filter(\.isPublished).compactMap { $0.publishedAs?.lowercased() })
    }
}

/// `showCloneToDevice`: clone `owner/name` from the mesh into a new on-device
/// repository behind a cancellable progress card.
///
/// Anonymous when signed out (public repositories clone without a signature);
/// signed with the device key otherwise, so a private or org-scoped read
/// authorizes with the same key. A guest is asked to sign in first (the view
/// runs the gate).
@MainActor
@Observable
public final class CloneToDeviceModel {
    @ObservationIgnored let store: LocalRepoStore
    @ObservationIgnored let publisher: PublishService
    @ObservationIgnored let session: @MainActor () -> AuthSession?
    @ObservationIgnored let mirrorLookup: @MainActor (String, String) async throws -> [RepoMirror]

    /// The progress card's label while the clone runs.
    public private(set) var busyLabel: String?
    public private(set) var source: CloneEndpoint?
    public private(set) var wasCancelled = false
    public var notice: LocalNotice?

    public init(
        store: LocalRepoStore,
        publisher: PublishService,
        session: @escaping @MainActor () -> AuthSession?,
        mirrors: (@MainActor (String, String) async throws -> [RepoMirror])? = nil
    ) {
        self.store = store
        self.publisher = publisher
        self.session = session
        let api = publisher.api
        self.mirrorLookup = mirrors ?? { owner, name in try await api.freshMirrorSet(owner, name).mirrors }
    }

    public var isBusy: Bool { busyLabel != nil }

    public func existing(_ repo: Repository) -> LocalRepoRecord? { store.record(for: repo) }

    public func sync(_ record: LocalRepoRecord) async -> LocalPullResult? {
        wasCancelled = false
        busyLabel = "Syncing \(record.publishedAs ?? record.name)"
        defer { busyLabel = nil }
        do {
            let result = try await store.sync(record, publisher: publisher) { [weak self] progress in
                Task { @MainActor in
                    guard let self, self.busyLabel != nil else { return }
                    self.busyLabel = progress.label
                }
            }
            notice = LocalNotice(result.message, isError: result.needsAttention)
            return result
        } catch {
            notice = LocalNotice(LocalRepoScreenModel.message(error), isError: true)
            return nil
        }
    }

    /// The dialog's explanation under the name field.
    public static var sizeNote: String {
        "Downloads the latest commit of the default branch from the repository's mirrors into app "
            + "storage. Older history is fetched only when a push needs it."
    }

    /// First free on-device name derived from the mesh repository name.
    public func suggestedName(_ name: String) async -> String {
        await store.ensureLoaded()
        return CloneToDeviceModel.suggest(name, taken: Set(store.records.map(\.name)))
    }

    static func suggest(_ name: String, taken: Set<String>) -> String {
        var base = ""
        var pendingDash = false
        for scalar in name.unicodeScalars {
            let v = scalar.value
            let keep = (0x41...0x5a).contains(v) || (0x61...0x7a).contains(v) || (0x30...0x39).contains(v)
                || v == 0x2e || v == 0x5f || v == 0x2d
            if keep {
                if pendingDash { base += "-" }
                pendingDash = false
                base.unicodeScalars.append(scalar)
            } else {
                pendingDash = true
            }
        }
        if pendingDash { base += "-" }
        if !taken.contains(base) { return base }
        for index in 2..<100 where !taken.contains("\(base)-\(index)") {
            return "\(base)-\(index)"
        }
        return base
    }

    /// Clone into `localName`; returns the record, or nil when it failed or
    /// was cancelled (the notice says why).
    public func clone(repo: Repository, localName: String) async -> LocalRepoRecord? {
        await clone(owner: repo.displayOwner, name: repo.name, localName: localName, description: repo.description)
    }

    public func clone(owner: String, name: String, localName: String, description: String = "") async -> LocalRepoRecord? {
        let repository = "\(owner)/\(name)"
        wasCancelled = false
        await store.ensureLoaded()
        if let existing = store.record(meshIdentity: repository) {
            notice = LocalNotice(LocalRepoStore.alreadyOnDevice(repository, as: existing.name), isError: true)
            return nil
        }
        let current = session()
        let relay = publisher.credentials(
            owner: owner, repo: name, signed: current != nil && !(current?.nodeName ?? "").isEmpty)
        busyLabel = "Finding mirrors of \(repository)"
        source = nil
        defer {
            busyLabel = nil
            source = nil
        }
        var mirrors = [RepoMirror]()
        var lookupProblem: String?
        do {
            mirrors = try await mirrorLookup(owner, name)
        } catch {
            lookupProblem = LocalRepoScreenModel.message(error)
        }
        let endpoints = CloneEndpoints.plan(owner: owner, name: name, mirrors: mirrors, relay: relay)
        do {
            let record = try await CloneEndpoints.run(
                repository: repository,
                endpoints: endpoints,
                lookupProblem: lookupProblem,
                onAttempt: { [weak self] endpoint, index in
                    guard let self else { return }
                    self.source = endpoint
                    self.busyLabel = CloneToDeviceModel.attemptLabel(endpoint, index: index, of: endpoints.count)
                }
            ) { endpoint in
                try await self.clone(from: endpoint, relay: relay, localName: localName, description: description)
            }
            let from = source.map { " from \($0.label)" } ?? ""
            notice = LocalNotice("Cloned \(repository) to \(localName)\(from).")
            return record
        } catch let failure as CloneFailure {
            notice = LocalNotice(failure.message, isError: true)
            return nil
        } catch {
            notice = LocalNotice(LocalRepoScreenModel.message(error), isError: true)
            return nil
        }
    }

    private func clone(
        from endpoint: CloneEndpoint, relay: ForkMeshRemoteCredentials, localName: String, description: String
    ) async throws -> LocalRepoRecord {
        let label = endpoint.label
        let onProgress: @Sendable (GitTransferProgress) -> Void = { [weak self] progress in
            Task { @MainActor in
                guard let self, self.busyLabel != nil else { return }
                self.busyLabel = CloneToDeviceModel.progressLabel(progress, source: label)
            }
        }
        do {
            return try await store.cloneFromMesh(
                name: localName, credentials: endpoint.credentials(relay), description: description,
                depth: GitWorkspace.meshCloneDepth, onProgress: onProgress)
        } catch let error as GitWorkspaceException where endpoint.isRelay && error.code == .protocolUnsupported {
            return try await store.cloneFromMesh(
                name: localName, credentials: endpoint.credentials(relay), description: description,
                onProgress: onProgress)
        }
    }

    static func attemptLabel(_ endpoint: CloneEndpoint, index: Int, of count: Int) -> String {
        let base = "Connecting to \(endpoint.label)"
        return count > 1 ? "\(base) (source \(index + 1) of \(count))" : base
    }

    static func progressLabel(_ progress: GitTransferProgress, source: String) -> String {
        progress.phase == "connecting" ? "Connecting to \(source)" : "\(progress.label) · \(source)"
    }

    public func cancel() {
        wasCancelled = true
        store.cancelActive()
    }
}
