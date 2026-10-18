import FMBase
import FMCodeInsight
import Foundation
import Observation

public struct CodeInsightResult: Equatable, Sendable {
    public let target: CodeInsightTarget
    public let history: CodeInsightHistory
    public let snapshot: InsightSnapshot
    public let prepared: CodeInsightPrepared
    public let finishedAt: Date

    public init(target: CodeInsightTarget, history: CodeInsightHistory, snapshot: InsightSnapshot, prepared: CodeInsightPrepared, finishedAt: Date) {
        self.target = target
        self.history = history
        self.snapshot = snapshot
        self.prepared = prepared
        self.finishedAt = finishedAt
    }

    public var sourceLabel: String { prepared.sourceLabel }
    public var branch: String { snapshot.branch }
    public var fileRef: String {
        let branch = snapshot.branch
        if branch.isEmpty || branch == "HEAD" { return snapshot.head }
        return branch
    }
}

public struct CodeInsightProgress: Equatable, Sendable {
    public let text: String
    public let fraction: Double

    public init(text: String, fraction: Double) {
        self.text = text
        self.fraction = fraction
    }
}

public final class CodeInsightCancelFlag: @unchecked Sendable {
    private let lock = NSLock()
    private var value = false

    public init() {}

    public var isCancelled: Bool {
        lock.lock()
        defer { lock.unlock() }
        return value
    }

    public func cancel() {
        lock.lock()
        value = true
        lock.unlock()
    }
}

final class CodeInsightProgressRelay: @unchecked Sendable {
    private let lock = NSLock()
    private var lastText = ""
    private var lastFraction = -1.0
    private let deliver: @Sendable (CodeInsightProgress) -> Void

    init(_ deliver: @escaping @Sendable (CodeInsightProgress) -> Void) {
        self.deliver = deliver
    }

    func report(_ text: String, _ fraction: Double) {
        lock.lock()
        let changed = text != lastText || fraction - lastFraction >= 0.01 || fraction >= 1
        if changed {
            lastText = text
            lastFraction = fraction
        }
        lock.unlock()
        if changed { deliver(CodeInsightProgress(text: text, fraction: fraction)) }
    }
}

public typealias CodeInsightPrepare = @MainActor (CodeInsightTarget, CodeInsightHistory, @escaping @MainActor (String) -> Void) async throws -> CodeInsightPrepared

@MainActor
@Observable
public final class CodeInsightSession {
    public static let freshSeconds: TimeInterval = 10 * 60
    nonisolated public static let trendSamples = 16

    public private(set) var results: [String: CodeInsightResult] = [:]
    public private(set) var progress: [String: CodeInsightProgress] = [:]
    public private(set) var errors: [String: String] = [:]
    public private(set) var history: CodeInsightHistory
    public var preferredTarget: CodeInsightTarget?

    @ObservationIgnored let prefs: KeyValueStore?
    @ObservationIgnored let prepare: CodeInsightPrepare
    @ObservationIgnored let cancelPrepare: @MainActor () -> Void
    @ObservationIgnored let now: () -> Date
    @ObservationIgnored private var tasks: [String: Task<Void, Never>] = [:]
    @ObservationIgnored private var flags: [String: CodeInsightCancelFlag] = [:]
    @ObservationIgnored private var viewers: [String: Int] = [:]
    @ObservationIgnored private var trends: [String: [InsightTrendPoint]] = [:]

    public init(
        prefs: KeyValueStore?,
        now: @escaping () -> Date = { Date() },
        cancelPrepare: @escaping @MainActor () -> Void = {},
        prepare: @escaping CodeInsightPrepare
    ) {
        self.prefs = prefs
        self.now = now
        self.cancelPrepare = cancelPrepare
        self.prepare = prepare
        history = CodeInsightHistory.load(prefs)
    }

    public convenience init(prefs: KeyValueStore?, fetcher: CodeInsightFetcher) {
        self.init(prefs: prefs, cancelPrepare: { fetcher.cancel() }) { target, history, progress in
            try await fetcher.prepare(target, history: history, progress: progress)
        }
    }

    public var lastTarget: CodeInsightTarget? {
        CodeInsightTarget.parse(prefs?.string(forKey: CodeInsightTarget.lastKey))
    }

    public func takePreferredTarget() -> CodeInsightTarget? {
        defer { preferredTarget = nil }
        return preferredTarget
    }

    public func result(for target: CodeInsightTarget) -> CodeInsightResult? { results[target.key] }

    public func progress(for target: CodeInsightTarget) -> CodeInsightProgress? { progress[target.key] }

    public func error(for target: CodeInsightTarget) -> String? { errors[target.key] }

    public func isRunning(_ target: CodeInsightTarget) -> Bool { tasks[target.key] != nil }

    public func isFresh(_ target: CodeInsightTarget) -> Bool {
        guard let result = results[target.key] else { return false }
        return result.history == history && now().timeIntervalSince(result.finishedAt) < CodeInsightSession.freshSeconds
    }

    public func setHistory(_ value: CodeInsightHistory) {
        if value == history { return }
        history = value
        value.save(prefs)
    }

    public func attach(_ target: CodeInsightTarget) {
        viewers[target.key, default: 0] += 1
        if !isFresh(target) && !isRunning(target) { analyze(target) }
    }

    public func detach(_ target: CodeInsightTarget) {
        let count = (viewers[target.key] ?? 1) - 1
        viewers[target.key] = max(0, count)
        if count <= 0 { cancel(target) }
    }

    public func cancel(_ target: CodeInsightTarget) {
        let key = target.key
        guard let task = tasks[key] else { return }
        flags[key]?.cancel()
        task.cancel()
        cancelPrepare()
    }

    public func reanalyze(_ target: CodeInsightTarget) {
        cancel(target)
        tasks[target.key] = nil
        analyze(target)
    }

    public func analyze(_ target: CodeInsightTarget) {
        let key = target.key
        if tasks[key] != nil { return }
        let flag = CodeInsightCancelFlag()
        let history = self.history
        flags[key] = flag
        errors[key] = nil
        progress[key] = CodeInsightProgress(text: "Preparing \(target.title)…", fraction: 0)
        let prepare = self.prepare
        let relay = CodeInsightProgressRelay { [weak self] update in
            Task { @MainActor in self?.show(update, key: key, flag: flag) }
        }
        tasks[key] = Task { [weak self] in
            do {
                let prepared = try await prepare(target, history) { text in
                    self?.show(CodeInsightProgress(text: text, fraction: 0), key: key, flag: flag)
                }
                if flag.isCancelled { throw CancellationError() }
                let snapshot = try await CodeInsightSession.run(prepared, history: history, relay: relay, flag: flag)
                guard let self else { return }
                if flag.isCancelled { throw CancellationError() }
                self.finish(key, target: target, result: CodeInsightResult(
                    target: target, history: history, snapshot: snapshot, prepared: prepared, finishedAt: self.now()))
            } catch {
                self?.fail(key, flag: flag, error: error)
            }
        }
    }

    nonisolated static func run(
        _ prepared: CodeInsightPrepared, history: CodeInsightHistory, relay: CodeInsightProgressRelay, flag: CodeInsightCancelFlag
    ) async throws -> InsightSnapshot {
        try await Task.detached(priority: .userInitiated) {
            let source = try GitInsightSource.open(
                worktree: prepared.worktree, name: prepared.name, description: prepared.description,
                tipRef: prepared.tipRef, readsWorkingFiles: prepared.readsWorkingFiles)
            return try Insight.analyze(
                source: source, options: history.options,
                progress: { text, fraction in relay.report(text, fraction) },
                isCancelled: { flag.isCancelled })
        }.value
    }

    private func show(_ update: CodeInsightProgress, key: String, flag: CodeInsightCancelFlag) {
        guard !flag.isCancelled, flags[key] === flag else { return }
        progress[key] = update
    }

    private func finish(_ key: String, target: CodeInsightTarget, result: CodeInsightResult) {
        results[key] = result
        trends = trends.filter { !$0.key.hasPrefix(key + "\n") }
        progress[key] = nil
        tasks[key] = nil
        flags[key] = nil
        errors[key] = nil
        prefs?.set(key, forKey: CodeInsightTarget.lastKey)
    }

    private func fail(_ key: String, flag: CodeInsightCancelFlag, error: Error) {
        guard flags[key] === flag else { return }
        progress[key] = nil
        tasks[key] = nil
        flags[key] = nil
        if flag.isCancelled || error is CancellationError { return }
        if let error = error as? GitWorkspaceException, error.code == .cancelled { return }
        errors[key] = CodeInsightSession.message(error)
    }

    public static func message(_ error: Error) -> String {
        if let error = error as? CodeInsightCopyFailure { return error.message }
        if let error = error as? CloneFailure { return error.message }
        return LocalRepoScreenModel.message(error)
    }

    public func trend(_ target: CodeInsightTarget, path: String) async -> [InsightTrendPoint] {
        let key = target.key + "\n" + path
        if let cached = trends[key] { return cached }
        guard let result = results[target.key] else { return [] }
        let prepared = result.prepared
        let points: [InsightTrendPoint] = await Task.detached(priority: .userInitiated) {
            do {
                let source = try GitInsightSource.open(
                    worktree: prepared.worktree, name: prepared.name, description: prepared.description,
                    tipRef: prepared.tipRef, readsWorkingFiles: false)
                return try source.trend(path, samples: CodeInsightSession.trendSamples)
            } catch {
                return []
            }
        }.value
        if results[target.key]?.finishedAt == result.finishedAt { trends[key] = points }
        return points
    }
}
