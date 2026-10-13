import FMBase
import FMChat
import FMControl
import Foundation
import Observation

extension DesktopControlFamily {
    public func prepare(_ params: JSONObject) async throws -> RemoteConfirmation {
        let reply = try await call("prepare", params, timeout: 20)
        guard let confirmation = RemoteConfirmation(family: name, json: reply, params: params) else {
            throw LocalControlException("bad_answer")
        }
        return confirmation
    }

    public func run(_ confirmation: RemoteConfirmation, typed: String = "") async throws -> RemoteStarted {
        if !confirmation.accepts(typed: typed) { throw LocalControlException("name_mismatch") }
        return RemoteStarted(json: try await call("run", confirmation.runParams(typed: typed), timeout: 45))
    }
}

@MainActor
@Observable
public final class FleetStore {
    @ObservationIgnored public let family: DesktopControlFamily
    @ObservationIgnored private let now: () -> Int
    @ObservationIgnored private var tapId: UUID?
    @ObservationIgnored private var refreshing = false
    @ObservationIgnored private var again = false

    public private(set) var summary: FleetSummary?
    public private(set) var repos: [FleetRepo] = []
    public private(set) var loading = false
    public private(set) var error = ""
    public private(set) var unsupported = false
    public private(set) var outcomes: [RemoteActionOutcome] = []
    public private(set) var running: Set<String> = []

    public init(channel: DesktopControlCalling, now: @escaping () -> Int = { Int(Date().timeIntervalSince1970 * 1000) }) {
        family = DesktopControlFamily(.fleet, channel: channel)
        self.now = now
        tapId = family.addEventTap { [weak self] suffix, data in self?.onEvent(suffix, data) }
    }

    private func onEvent(_ suffix: String, _ data: JSONObject) {
        switch suffix {
        case "changed":
            Task { await self.refresh() }
        case "action":
            let outcome = RemoteActionOutcome(json: data, atMs: now())
            running.remove("\(outcome.action):\(outcome.subject)")
            outcomes.insert(outcome, at: 0)
            if outcomes.count > 20 { outcomes.removeLast(outcomes.count - 20) }
        default:
            break
        }
    }

    public func refresh() async {
        if refreshing {
            again = true
            return
        }
        refreshing = true
        loading = true
        defer {
            refreshing = false
            loading = false
        }
        repeat {
            again = false
            do {
                summary = FleetSummary(json: try await family.call("summary"))
                error = ""
                unsupported = false
            } catch {
                unsupported = DesktopControlFamily.isUnsupported(error)
                self.error = RemoteErrorText.describe(error)
            }
        } while again
    }

    public func provision() async throws -> ProvisionStatus {
        ProvisionStatus(json: try await family.call("provision"))
    }

    public func loadRepos() async {
        do {
            repos = try await family.call("repos").maps("repos").map(FleetRepo.init(json:))
        } catch {
            self.error = RemoteErrorText.describe(error)
        }
    }

    public func diskUsage(host: String, path: String) async throws -> DiskUsage {
        DiskUsage(json: try await family.call("disk", ["host": .string(host), "path": .string(path)], timeout: 75))
    }

    public func prepare(_ action: FleetAction) async throws -> RemoteConfirmation {
        try await family.prepare(action.params)
    }

    @discardableResult
    public func run(_ confirmation: RemoteConfirmation, typed: String = "") async throws -> String {
        let started = try await family.run(confirmation, typed: typed)
        running.insert("\(confirmation.action):\(started.subject.isEmpty ? confirmation.subject : started.subject)")
        await refresh()
        return started.message
    }

    public func isRunning(action: String, subject: String) -> Bool {
        running.contains("\(action):\(subject)") || (summary?.running.contains("\(action):\(subject)") ?? false)
    }

    public func stop() {
        if let tapId { family.removeEventTap(tapId) }
        tapId = nil
    }
}

@MainActor
@Observable
public final class FilesBrowserStore {
    @ObservationIgnored public let family: DesktopControlFamily
    @ObservationIgnored private let now: () -> Int
    @ObservationIgnored private var tapId: UUID?

    public private(set) var roots: [FileRoot] = []
    public private(set) var loadingRoots = false
    public private(set) var error = ""
    public private(set) var unsupported = false
    public private(set) var listings: [String: FileListing] = [:]
    public private(set) var statuses: [String: GitWorkingStatus] = [:]
    public private(set) var outcomes: [RemoteActionOutcome] = []
    public private(set) var changedRoots: Set<String> = []

    public init(channel: DesktopControlCalling, now: @escaping () -> Int = { Int(Date().timeIntervalSince1970 * 1000) }) {
        family = DesktopControlFamily(.filesBrowse, channel: channel)
        self.now = now
        tapId = family.addEventTap { [weak self] suffix, data in self?.onEvent(suffix, data) }
    }

    private func onEvent(_ suffix: String, _ data: JSONObject) {
        switch suffix {
        case "changed":
            let root = data.str("root")
            if !root.isEmpty { changedRoots.insert(root) }
        case "action":
            outcomes.insert(RemoteActionOutcome(json: data, atMs: now()), at: 0)
            if outcomes.count > 20 { outcomes.removeLast(outcomes.count - 20) }
        default:
            break
        }
    }

    public var groups: [FileRootGroup] { FileRootGroup.group(roots) }

    public static func key(root: String, path: String) -> String { root + "|" + path }

    public func loadRoots() async {
        loadingRoots = true
        defer { loadingRoots = false }
        do {
            roots = try await family.call("roots").maps("roots").map(FileRoot.init(json:))
            error = ""
            unsupported = false
        } catch {
            unsupported = DesktopControlFamily.isUnsupported(error)
            self.error = RemoteErrorText.describe(error)
        }
    }

    @discardableResult
    public func list(root: String, path: String) async throws -> FileListing {
        let listing = FileListing(
            json: try await family.call("list", ["root": .string(root), "path": .string(path)]))
        listings[Self.key(root: root, path: listing.path)] = listing
        return listing
    }

    public func cached(root: String, path: String) -> FileListing? {
        listings[Self.key(root: root, path: path)]
    }

    public func read(root: String, path: String, offset: Double = 0, length: Int = 256 * 1024) async throws -> FileSlice {
        FileSlice(
            json: try await family.call(
                "read",
                ["root": .string(root), "path": .string(path), "offset": .double(offset), "length": .int(Int64(length))],
                timeout: 30))
    }

    @discardableResult
    public func status(root: String) async throws -> GitWorkingStatus {
        let status = GitWorkingStatus(json: try await family.call("status", ["root": .string(root)], timeout: 25))
        statuses[root] = status
        changedRoots.remove(root)
        return status
    }

    public func prepare(_ action: GitRemoteAction, root: String) async throws -> RemoteConfirmation {
        try await family.prepare(["action": .string(action.rawValue), "root": .string(root)])
    }

    @discardableResult
    public func run(_ confirmation: RemoteConfirmation) async throws -> String {
        try await family.run(confirmation).message
    }

    public func stop() {
        if let tapId { family.removeEventTap(tapId) }
        tapId = nil
    }
}

@MainActor
@Observable
public final class DiagnosticsStore {
    @ObservationIgnored public let family: DesktopControlFamily
    @ObservationIgnored private var tapId: UUID?

    public private(set) var health: DesktopHealth?
    public private(set) var stalls: StallList?
    public private(set) var processes: ProcessList?
    public private(set) var backups: DesktopBackupStatus?
    public private(set) var loading = false
    public private(set) var error = ""
    public private(set) var unsupported = false

    public init(channel: DesktopControlCalling) {
        family = DesktopControlFamily(.diagnostics, channel: channel)
        tapId = family.addEventTap { [weak self] suffix, _ in
            if suffix == "changed" { Task { await self?.refreshProcesses() } }
        }
    }

    public func refreshHealth() async {
        loading = true
        defer { loading = false }
        do {
            health = DesktopHealth(json: try await family.call("health"))
            error = ""
            unsupported = false
        } catch {
            unsupported = DesktopControlFamily.isUnsupported(error)
            self.error = RemoteErrorText.describe(error)
        }
    }

    public func refreshStalls() async {
        do {
            stalls = StallList(json: try await family.call("stalls"))
        } catch {
            self.error = RemoteErrorText.describe(error)
        }
    }

    public func stall(_ index: Int) async throws -> StallRecord {
        StallRecord(json: try await family.call("stall", ["index": .int(Int64(index))]))
    }

    public func refreshBackups() async {
        do {
            backups = DesktopBackupStatus(json: try await family.call("backups"))
            unsupported = false
        } catch {
            if DesktopControlFamily.isUnsupported(error) { unsupported = true }
            self.error = RemoteErrorText.describe(error)
        }
    }

    public func refreshProcesses() async {
        do {
            processes = ProcessList(json: try await family.call("processes", [:], timeout: 15))
        } catch {
            self.error = RemoteErrorText.describe(error)
        }
    }

    public func log(before: Int? = nil, lines: Int = 200, query: String = "", severity: LogSeverity = .all)
        async throws -> DesktopLogPage
    {
        var params: JSONObject = [
            "lines": .int(Int64(lines)), "query": .string(query), "severity": .string(severity.rawValue),
        ]
        if let before { params["before"] = .int(Int64(before)) }
        return DesktopLogPage(json: try await family.call("log", params, timeout: 20))
    }

    public func prepare(_ action: DiagnosticsAction) async throws -> RemoteConfirmation {
        try await family.prepare(action.params)
    }

    @discardableResult
    public func run(_ confirmation: RemoteConfirmation) async throws -> String {
        let started = try await family.run(confirmation)
        if confirmation.action == "stopAgent" { await refreshProcesses() }
        return started.message
    }

    public func stop() {
        if let tapId { family.removeEventTap(tapId) }
        tapId = nil
    }
}

@MainActor
@Observable
public final class DesktopLogTail {
    @ObservationIgnored private let store: DiagnosticsStore

    public private(set) var lines: [DesktopLogLine] = []
    public private(set) var more = false
    public private(set) var loading = false
    public private(set) var error = ""
    public var query = ""
    public var severity: LogSeverity = .all
    public let pageSize: Int
    public let maxLines: Int

    public init(store: DiagnosticsStore, pageSize: Int = 200, maxLines: Int = 2000) {
        self.store = store
        self.pageSize = pageSize
        self.maxLines = maxLines
    }

    public func loadLatest() async {
        loading = true
        defer { loading = false }
        do {
            let page = try await store.log(lines: pageSize, query: query, severity: severity)
            lines = page.lines
            more = page.more
            error = ""
        } catch {
            self.error = RemoteErrorText.describe(error)
        }
    }

    public func loadOlder() async {
        guard more, !loading, let first = lines.first else { return }
        loading = true
        defer { loading = false }
        do {
            let page = try await store.log(before: first.offset, lines: pageSize, query: query, severity: severity)
            let known = Set(lines.map(\.offset))
            lines = page.lines.filter { !known.contains($0.offset) } + lines
            if lines.count > maxLines {
                lines.removeLast(lines.count - maxLines)
            }
            more = page.more
        } catch {
            self.error = RemoteErrorText.describe(error)
        }
    }

    public var errorCount: Int { lines.filter { $0.severity == .error }.count }
}
