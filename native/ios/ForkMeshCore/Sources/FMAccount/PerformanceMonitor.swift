// Frame timings and lifecycle changes are inputs the app target feeds (`FrameTimingSource`,
// `appLifecycleChanged`); this module does the stall and latency accounting.

import FMBase
import Foundation
#if canImport(Darwin)
import Darwin
#endif
import Observation

public enum PerformanceEventKind: String, Sendable {
    /// An awaited operation (network call, background job) that took long. The UI stays
    /// responsive while it runs; it is recorded as context, not as a stall.
    case operation
    /// A rendered frame whose build-to-present span blew the stall budget.
    case frame
    /// The main thread's run loop was blocked: the heartbeat fired late. This catches synchronous
    /// work between frames, which frame timings miss.
    case eventLoop
    /// A named synchronous section (`timeSync`) that blocked the main thread itself.
    case sync
}

public struct PerformanceEvent: Sendable, Identifiable {
    public let id: Int
    public let kind: PerformanceEventKind
    public let name: String
    public let duration: Duration
    public let timestamp: Date
    public let details: JSONObject
    public let stackTrace: String

    public var durationMs: Int { Int(PerformanceMonitorService.micros(duration) / 1_000) }

    /// True for events that froze the UI, as opposed to slow awaited work.
    public var isStall: Bool { kind != .operation }
}

/// One rendered frame, as the app target measures it.
public struct FrameTiming: Sendable {
    public var buildDuration: Duration
    public var rasterDuration: Duration
    public var vsyncOverhead: Duration
    public var totalSpan: Duration?

    public init(buildDuration: Duration, rasterDuration: Duration, vsyncOverhead: Duration = .zero, totalSpan: Duration? = nil) {
        self.buildDuration = buildDuration
        self.rasterDuration = rasterDuration
        self.vsyncOverhead = vsyncOverhead
        self.totalSpan = totalSpan
    }

    public static func lateCallback(interval: Duration, period: Duration) -> FrameTiming? {
        guard period > .zero, interval > period * 2 else { return nil }
        return FrameTiming(buildDuration: interval - period, rasterDuration: .zero, vsyncOverhead: period)
    }
}

/// What feeds frame timings to the monitor (`addTimingsCallback`). The app target implements it
/// over a CADisplayLink or MetricKit; the core only consumes the batches.
public protocol FrameTimingSource: AnyObject {
    func startReporting(_ handler: @escaping ([FrameTiming]) -> Void)
    func stopReporting()
}

/// The app's lifecycle state, which the app target maps from SwiftUI's `ScenePhase`
/// (active -> resumed, inactive -> inactive, background -> paused).
public enum AppLifecycleState: String, Sendable {
    case resumed, inactive, hidden, paused, detached
}

public struct AppReturnGate: Sendable {
    private var away = false

    public init() {}

    public mutating func isReturn(_ state: AppLifecycleState) -> Bool {
        switch state {
        case .hidden, .paused, .detached:
            away = true
            return false
        case .inactive:
            return false
        case .resumed:
            defer { away = false }
            return away
        }
    }
}

/// Watches for anything that makes the app feel laggy and records it.
///
/// Runs in every build mode. Each record is written to `logSink` (default: `Log`, tag `perf`,
/// prefixed `[stall]` or `[perf]`) and, when a `stallLog` is attached, appended to
/// `diagnostics/stalls.log` on the device. The in-app HUD that lists the same events is debug-only.
///
/// Thread-safe: `track` may be awaited from any context. The heartbeat and `timeSync` measure the
/// main thread. Observation sees `revision`, bumped on the main thread after every change.
@Observable
public final class PerformanceMonitorService: @unchecked Sendable {
    public let enabled: Bool
    /// Awaited operations at or over this are recorded as slow.
    public let threshold: Duration
    /// Frames, blocked run-loop turns and sync sections at or over this are recorded as stalls.
    /// 200 ms is where a hitch reads as the app hanging.
    public let stallThreshold: Duration
    /// How often the main-thread heartbeat checks in while the app is visible.
    public let heartbeatInterval: Duration
    public let maxEvents: Int
    @ObservationIgnored public let logSink: (@Sendable (String) -> Void)?
    @ObservationIgnored public let stallLog: StallLogFile?
    @ObservationIgnored private let now: @Sendable () -> Date
    @ObservationIgnored private let elapsedForTest: (@Sendable () -> Duration)?
    @ObservationIgnored private let heartbeatQueue: DispatchQueue
    @ObservationIgnored private let watchQueue: DispatchQueue

    /// Every heartbeat's lag behind schedule, stall or not. The telemetry strip charts the worst of
    /// each second from this. Called on the heartbeat queue.
    public var onHeartbeatLag: ((Duration) -> Void)? {
        get { locked { heartbeatLagHandler } }
        set { locked { heartbeatLagHandler = newValue } }
    }

    public var screenName: (@MainActor () -> String)? {
        get { locked { screenSource } }
        set { locked { screenSource = newValue } }
    }

    /// Bumped (on the main thread) whenever `events` or `stallCount` changes.
    public private(set) var revision = 0
    @ObservationIgnored public let listeners = Listeners()

    @ObservationIgnored private let lock = NSLock()
    @ObservationIgnored private var eventStore: [PerformanceEvent] = []
    @ObservationIgnored private var stallTotal = 0
    @ObservationIgnored private var nextEventId = 0
    @ObservationIgnored private var heartbeatLagHandler: ((Duration) -> Void)?
    @ObservationIgnored private var screenSource: (@MainActor () -> String)?
    // Monotonic clock; wall-clock time can jump and would fake (or hide) a stall.
    @ObservationIgnored private let origin = DispatchTime.now().uptimeNanoseconds
    @ObservationIgnored private var heartbeatGeneration = 0
    @ObservationIgnored private var heartbeatRunning = false
    @ObservationIgnored private var lastBeatUs: Int64 = 0
    @ObservationIgnored private var heartbeatStartedUs: Int64 = 0
    @ObservationIgnored private var lastAttributedStallUs: Int64 = -1
    // Awaited operations currently running and the ones that just finished. A blocked run loop
    // cannot say what blocked it, but the continuation of an operation that finished inside the
    // stalled window (decoding its response, rebuilding for it) is almost always the culprit.
    @ObservationIgnored private var inFlight: [(Int, String)] = []
    @ObservationIgnored private var nextOperationId = 0
    @ObservationIgnored private var recentlyFinished: [(String, Int64)] = []
    private static let recentLimit = 16
    public let sectionFloor: Duration
    @ObservationIgnored private var recentSections: [(name: String, startUs: Int64, endUs: Int64)] = []
    @ObservationIgnored private var mainSectionDepth = 0
    @ObservationIgnored private var sectionWatcher: (@Sendable (String, Double, Double) -> Void)?
    public static let watchedFloor: Duration = .milliseconds(1)
    public static let sectionLimit = 256
    @ObservationIgnored private weak var frameSource: FrameTimingSource?
    @ObservationIgnored private var installed = false
    @ObservationIgnored private var watchTimer: DispatchSourceTimer?
    @ObservationIgnored private var lastWatchUs: Int64 = 0
    @ObservationIgnored private var recentPauses: [(startUs: Int64, endUs: Int64)] = []
    @ObservationIgnored private var pauseTotal = 0
    @ObservationIgnored private var longestPauseUs: Int64 = 0
    @ObservationIgnored private var hiddenSinceUs: Int64?
    @ObservationIgnored private var recentHidden: [(startUs: Int64, endUs: Int64)] = []
    @ObservationIgnored private var showCount = 0
    public static let shownWindowMs = 10_000

    public init(
        enabled: Bool = true,
        threshold: Duration = .milliseconds(500),
        stallThreshold: Duration = .milliseconds(200),
        heartbeatInterval: Duration = .milliseconds(100),
        sectionFloor: Duration = .milliseconds(8),
        maxEvents: Int = 100,
        logSink: (@Sendable (String) -> Void)? = nil,
        stallLog: StallLogFile? = nil,
        now: (@Sendable () -> Date)? = nil,
        elapsedForTest: (@Sendable () -> Duration)? = nil,
        heartbeatQueue: DispatchQueue = .main,
        watchQueue: DispatchQueue = DispatchQueue(label: "forkmesh.perf.watch", qos: .userInitiated)
    ) {
        self.enabled = enabled
        self.threshold = threshold
        self.stallThreshold = stallThreshold
        self.heartbeatInterval = heartbeatInterval
        self.sectionFloor = sectionFloor
        self.maxEvents = maxEvents
        self.logSink = logSink
        self.stallLog = stallLog
        self.now = now ?? { Date() }
        self.elapsedForTest = elapsedForTest
        self.heartbeatQueue = heartbeatQueue
        self.watchQueue = watchQueue
    }

    // MARK: Reading

    /// Newest first, at most `maxEvents`.
    public var events: [PerformanceEvent] {
        if Thread.isMainThread { _ = revision }
        return locked { eventStore }
    }

    public var hasEvents: Bool { !events.isEmpty }
    public var latestEvent: PerformanceEvent? { events.first }

    /// Stalls recorded since launch; unlike `events` this is never trimmed.
    public var stallCount: Int {
        if Thread.isMainThread { _ = revision }
        return locked { stallTotal }
    }

    public var pauseCount: Int {
        if Thread.isMainThread { _ = revision }
        return locked { pauseTotal }
    }

    public var longestPause: Duration {
        if Thread.isMainThread { _ = revision }
        return .microseconds(locked { longestPauseUs })
    }

    /// Where stalls are being written on this device, once known.
    public var stallLogPath: String? { stallLog?.path }

    @discardableResult
    public func addListener(_ listener: @escaping @Sendable () -> Void) -> ListenerToken { listeners.add(listener) }

    public func removeListener(_ token: ListenerToken) { listeners.remove(token) }

    // MARK: Measuring

    /// Run `action` and record it as a slow operation when it takes `threshold` or longer. A
    /// thrown error is recorded in the details (`error`) and rethrown.
    public func track<T>(
        _ name: String,
        details: JSONObject = [:],
        _ action: () async throws -> T
    ) async rethrows -> T {
        guard enabled else { return try await action() }
        let callStack = PerformanceMonitorService.captureReturnAddresses()
        let startUs = elapsedMicros()
        let id: Int = locked {
            let id = nextOperationId
            nextOperationId += 1
            inFlight.append((id, name))
            return id
        }
        do {
            let result = try await action()
            finishOperation(id: id, name: name, startUs: startUs, details: details, error: nil, stack: callStack)
            return result
        } catch {
            finishOperation(id: id, name: name, startUs: startUs, details: details, error: error, stack: callStack)
            throw error
        }
    }

    private func finishOperation(id: Int, name: String, startUs: Int64, details: JSONObject, error: Error?, stack: [UInt]) {
        let endUs = elapsedMicros()
        let awayUs: Int64 = locked {
            inFlight.removeAll { $0.0 == id }
            recentlyFinished.append((name, endUs))
            if recentlyFinished.count > PerformanceMonitorService.recentLimit { recentlyFinished.removeFirst() }
            return hiddenMicros(from: startUs, to: endUs) + pausedMicros(from: startUs, to: endUs)
        }
        let span = elapsedForTest?() ?? .microseconds(endUs - startUs)
        let elapsed = max(.zero, span - .microseconds(awayUs))
        guard elapsed >= threshold else { return }
        var merged = details
        if awayUs >= 1_000 { merged["awayMs"] = .int(awayUs / 1_000) }
        if let error { merged["error"] = .string(String(describing: error)) }
        recordSlowEvent(kind: .operation, name: name, duration: elapsed, details: merged, stackTrace: PerformanceMonitorService.symbolicate(stack))
    }

    /// Run synchronous `action` and record a named stall when it blocks for `stallThreshold` or
    /// longer. Wrap work known to be heavy (big decodes, crypto, list rebuilds) so the log names
    /// it instead of reporting an anonymous blocked run loop.
    public func timeSync<T>(_ name: String, details: JSONObject = [:], _ action: () throws -> T) rethrows -> T {
        guard enabled else { return try action() }
        let onMain = Thread.isMainThread
        let nested: Bool = onMain ? locked {
            mainSectionDepth += 1
            return mainSectionDepth > 1
        } : false
        let startUs = elapsedMicros()
        defer {
            let endUs = elapsedMicros()
            if onMain { locked { mainSectionDepth -= 1 } }
            let elapsed = Duration.microseconds(endUs - startUs)
            let inner = elapsed >= stallThreshold && !nested ? sectionsText(from: startUs, to: endUs) : nil
            if elapsed >= (onMain ? min(sectionFloor, PerformanceMonitorService.watchedFloor) : sectionFloor) {
                noteSection(name, startUs: startUs, endUs: endUs)
            }
            if onMain, elapsed >= PerformanceMonitorService.watchedFloor, let watcher = locked({ sectionWatcher }) {
                watcher(name, uptimeSeconds(startUs), uptimeSeconds(endUs))
            }
            if elapsed >= stallThreshold && !nested {
                locked { lastAttributedStallUs = endUs }
                var merged = details
                if let inner { merged["sections"] = .string(inner) }
                recordSlowEvent(
                    kind: .sync, name: name, duration: elapsed, details: merged,
                    stackTrace: PerformanceMonitorService.captureStack())
            }
        }
        return try action()
    }

    public func watchSections(_ watcher: (@Sendable (_ name: String, _ start: Double, _ end: Double) -> Void)?) {
        locked { sectionWatcher = watcher }
    }

    public static func section<T>(_ name: String, _ action: () throws -> T) rethrows -> T {
        guard let monitor = Services.get(PerformanceMonitorService.self) else { return try action() }
        return try monitor.timeSync(name, action)
    }

    // MARK: Stall monitors

    /// Start every stall detector: frame timings from `frameSource`, the main-thread heartbeat,
    /// and lifecycle-driven parking of the heartbeat (a suspended process would otherwise look
    /// like one giant stall). Idempotent. Feed later lifecycle changes to `appLifecycleChanged`.
    public func installStallMonitors(frameSource: FrameTimingSource?, lifecycleState: AppLifecycleState? = nil) {
        let proceed: Bool = locked {
            if !enabled || installed { return false }
            installed = true
            self.frameSource = frameSource
            return true
        }
        guard proceed else { return }
        frameSource?.startReporting { [weak self] timings in self?.handleFrameTimings(timings) }
        if lifecycleState == nil || lifecycleState == .resumed || lifecycleState == .inactive {
            startHeartbeat()
        } else {
            locked { hiddenSinceUs = hiddenSinceUs ?? elapsedMicros() }
        }
    }

    public func uninstallStallMonitors() {
        let source: FrameTimingSource? = locked {
            guard installed else { return nil }
            installed = false
            let source = frameSource
            frameSource = nil
            return source
        }
        source?.stopReporting()
        stopHeartbeat()
    }

    /// The `AppLifecycleListener` hook: run the heartbeat only while the app is visible.
    public func appLifecycleChanged(_ state: AppLifecycleState) {
        guard locked({ installed }) else { return }
        if state == .resumed || state == .inactive {
            startHeartbeat()
        } else {
            stopHeartbeat()
        }
    }

    /// Begin watching the main thread's run loop. Exposed for tests; the app goes through
    /// `installStallMonitors`.
    public func startHeartbeat() {
        let generation: Int? = locked {
            if !enabled || heartbeatRunning { return nil }
            heartbeatRunning = true
            heartbeatGeneration += 1
            lastBeatUs = elapsedMicros()
            heartbeatStartedUs = lastBeatUs
            lastWatchUs = lastBeatUs
            showCount += 1
            if let since = hiddenSinceUs {
                recentHidden.append((since, lastBeatUs))
                if recentHidden.count > PerformanceMonitorService.recentLimit { recentHidden.removeFirst() }
                hiddenSinceUs = nil
            }
            return heartbeatGeneration
        }
        guard let generation else { return }
        startWatch()
        scheduleBeat(generation)
    }

    public func stopHeartbeat() {
        let timer: DispatchSourceTimer? = locked {
            if heartbeatRunning { hiddenSinceUs = elapsedMicros() }
            heartbeatRunning = false
            heartbeatGeneration += 1
            let timer = watchTimer
            watchTimer = nil
            return timer
        }
        timer?.cancel()
    }

    public func withoutPauses(_ span: Duration) -> Duration {
        let nowUs = elapsedMicros()
        let paused = locked { pausedMicros(from: nowUs - PerformanceMonitorService.micros(span), to: nowUs) }
        return paused > 0 ? max(.zero, span - .microseconds(paused)) : span
    }

    private func startWatch() {
        let intervalUs = Int(PerformanceMonitorService.micros(heartbeatInterval))
        let timer = DispatchSource.makeTimerSource(queue: watchQueue)
        timer.schedule(
            deadline: .now() + .microseconds(intervalUs), repeating: .microseconds(intervalUs),
            leeway: .microseconds(max(1, intervalUs / 10)))
        timer.setEventHandler { [weak self] in self?.watchTick() }
        let previous: DispatchSourceTimer? = locked {
            let previous = watchTimer
            watchTimer = timer
            return previous
        }
        previous?.cancel()
        timer.resume()
    }

    private func watchTick() {
        let nowUs = elapsedMicros()
        let intervalUs = PerformanceMonitorService.micros(heartbeatInterval)
        let pausedUs: Int64? = locked {
            guard heartbeatRunning else { return nil }
            let startUs = lastWatchUs + intervalUs
            lastWatchUs = nowUs
            let gapUs = nowUs - startUs
            if gapUs < PerformanceMonitorService.micros(stallThreshold) { return nil }
            recentPauses.append((startUs, nowUs))
            if recentPauses.count > PerformanceMonitorService.recentLimit { recentPauses.removeFirst() }
            pauseTotal += 1
            longestPauseUs = max(longestPauseUs, gapUs)
            return gapUs
        }
        guard let pausedUs else { return }
        let line = "[pause] process paused \(pausedUs / 1_000)ms while visible"
        if let logSink { logSink(line) } else { Log.info("perf", line) }
        stallLog?.append("\(DartDateFormat.iso8601(now())) \(line)")
        changed()
    }

    private func pausedMicros(from startUs: Int64, to endUs: Int64) -> Int64 {
        let intervalUs = PerformanceMonitorService.micros(heartbeatInterval)
        var spans = recentPauses
        if heartbeatRunning, endUs - lastWatchUs - intervalUs >= PerformanceMonitorService.micros(stallThreshold) {
            spans.append((lastWatchUs + intervalUs, endUs))
        }
        return spans.reduce(0) { $0 + max(0, min($1.endUs, endUs) - max($1.startUs, startUs)) }
    }

    private func hiddenMicros(from startUs: Int64, to endUs: Int64) -> Int64 {
        var spans = recentHidden
        if let since = hiddenSinceUs { spans.append((since, endUs)) }
        return spans.reduce(0) { $0 + max(0, min($1.endUs, endUs) - max($1.startUs, startUs)) }
    }

    public func dispose() { uninstallStallMonitors() }

    private func scheduleBeat(_ generation: Int) {
        let delay = DispatchTimeInterval.microseconds(Int(PerformanceMonitorService.micros(heartbeatInterval)))
        heartbeatQueue.asyncAfter(deadline: .now() + delay) { [weak self] in self?.beat(generation) }
    }

    private func beat(_ generation: Int) {
        let nowUs = elapsedMicros()
        let intervalUs = PerformanceMonitorService.micros(heartbeatInterval)
        let snapshot: (windowStart: Int64, attributed: Int64, pausedUs: Int64, inFlight: [String], recent: [String], lagHandler: ((Duration) -> Void)?)? = locked {
            guard heartbeatRunning, generation == heartbeatGeneration else { return nil }
            let windowStart = lastBeatUs
            lastBeatUs = nowUs
            return (
                windowStart, lastAttributedStallUs, pausedMicros(from: windowStart, to: nowUs), inFlight.map(\.1),
                recentlyFinished.filter { $0.1 >= windowStart }.map(\.0), heartbeatLagHandler
            )
        }
        guard let snapshot else { return }
        scheduleBeat(generation)
        let lagUs = max(0, nowUs - snapshot.windowStart - intervalUs - snapshot.pausedUs)
        let lag = Duration.microseconds(lagUs)
        snapshot.lagHandler?(lag)
        // A timeSync section already named this stall; don't log it twice.
        if lag < stallThreshold || snapshot.attributed >= snapshot.windowStart { return }
        var details = JSONObject()
        if snapshot.pausedUs > 0 { details["pausedMs"] = .int(snapshot.pausedUs / 1_000) }
        if !snapshot.inFlight.isEmpty { details["inFlight"] = .string(PerformanceMonitorService.uniqueJoined(snapshot.inFlight)) }
        if !snapshot.recent.isEmpty { details["finishedDuring"] = .string(PerformanceMonitorService.uniqueJoined(snapshot.recent)) }
        if let sections = sectionsText(from: snapshot.windowStart, to: nowUs) { details["sections"] = .string(sections) }
        recordSlowEvent(kind: .eventLoop, name: "ui-isolate blocked", duration: lag, details: details)
    }

    private func handleFrameTimings(_ timings: [FrameTiming]) {
        let nowUs = elapsedMicros()
        guard let visibleSinceUs: Int64 = locked({ heartbeatRunning ? heartbeatStartedUs : nil }) else { return }
        for timing in timings {
            let span = timing.totalSpan ?? timing.buildDuration + timing.rasterDuration + timing.vsyncOverhead
            let spanUs = PerformanceMonitorService.micros(span)
            if nowUs - spanUs < visibleSinceUs { continue }
            let paused = Duration.microseconds(locked { pausedMicros(from: nowUs - spanUs, to: nowUs) })
            recordFrameDelay(
                buildDuration: max(.zero, timing.buildDuration - paused),
                rasterDuration: timing.rasterDuration,
                vsyncOverhead: timing.vsyncOverhead,
                totalSpan: timing.totalSpan.map { max(.zero, $0 - paused) })
        }
    }

    public func recordFrameDelay(
        buildDuration: Duration,
        rasterDuration: Duration,
        vsyncOverhead: Duration = .zero,
        totalSpan: Duration? = nil
    ) {
        guard enabled else { return }
        let total = totalSpan ?? buildDuration + rasterDuration
        if total < stallThreshold { return }
        var details: JSONObject = [
            "buildMs": .int(PerformanceMonitorService.micros(buildDuration) / 1_000),
            "rasterMs": .int(PerformanceMonitorService.micros(rasterDuration) / 1_000),
        ]
        let vsyncMs = PerformanceMonitorService.micros(vsyncOverhead) / 1_000
        if vsyncMs > 0 { details["vsyncMs"] = .int(vsyncMs) }
        let running = locked { inFlight.map(\.1) }
        if !running.isEmpty { details["inFlight"] = .string(PerformanceMonitorService.uniqueJoined(running)) }
        let endUs = elapsedMicros()
        if let sections = sectionsText(from: endUs - PerformanceMonitorService.micros(total), to: endUs) {
            details["sections"] = .string(sections)
        }
        recordSlowEvent(kind: .frame, name: "frame", duration: total, details: details)
    }

    public func recordSlowEvent(
        kind: PerformanceEventKind,
        name: String,
        duration: Duration,
        details: JSONObject = [:],
        stackTrace: String = ""
    ) {
        let floor = kind == .operation ? threshold : stallThreshold
        guard enabled, duration >= floor else { return }
        var details = details
        if kind != .operation, details["screen"] == nil, let screen = currentScreen(), !screen.isEmpty {
            details["screen"] = .string(screen)
        }
        if kind != .operation, details["shown"] == nil, let shown = shownMoment(before: duration) {
            details["shown"] = .string(shown.by)
            details["shownMs"] = .int(shown.ms)
        }
        let event: PerformanceEvent = locked {
            let event = PerformanceEvent(
                id: nextEventId, kind: kind, name: name, duration: duration, timestamp: now(),
                details: details, stackTrace: stackTrace)
            nextEventId += 1
            if event.isStall { stallTotal += 1 }
            eventStore.insert(event, at: 0)
            if eventStore.count > maxEvents { eventStore.removeLast(eventStore.count - maxEvents) }
            return event
        }
        log(event)
        changed()
    }

    public func clear() {
        let hadEvents: Bool = locked {
            if eventStore.isEmpty { return false }
            eventStore.removeAll()
            return true
        }
        if hadEvents { changed() }
    }

    // MARK: Helpers

    private func shownMoment(before duration: Duration) -> (by: String, ms: Int64)? {
        let startUs = elapsedMicros() - PerformanceMonitorService.micros(duration)
        return locked {
            guard heartbeatRunning, showCount > 0 else { return nil }
            return (showCount == 1 ? "launch" : "resume", max(0, startUs - heartbeatStartedUs) / 1_000)
        }
    }

    private func currentScreen() -> String? {
        guard Thread.isMainThread, let source = locked({ screenSource }) else { return nil }
        return MainActor.assumeIsolated { source() }
    }

    private func changed() {
        if Thread.isMainThread {
            revision &+= 1
        } else {
            DispatchQueue.main.async { [weak self] in self?.revision &+= 1 }
        }
        listeners.notify()
    }

    private func log(_ event: PerformanceEvent) {
        let details = event.details.map { "\($0.key)=\(PerformanceMonitorService.quote($0.value.text))" }.joined(separator: " ")
        let detailSuffix = details.isEmpty ? "" : " \(details)"
        let line = "\(event.isStall ? "[stall]" : "[perf]") \(event.kind.rawValue) \(event.name) \(event.durationMs)ms\(detailSuffix)"
        if let logSink {
            logSink(line)
        } else {
            Log.info("perf", line)
        }
        guard let stallLog else { return }
        let stack = event.stackTrace.isEmpty
            ? ""
            : "\n" + event.stackTrace.split(separator: "\n", omittingEmptySubsequences: false).map { "    \($0)" }.joined(separator: "\n")
        stallLog.append("\(DartDateFormat.iso8601(event.timestamp)) \(line)\(stack)")
    }

    private static func quote(_ text: String) -> String { text.contains(" ") ? "\"\(text)\"" : text }

    /// `values.toSet().join(', ')`: first occurrence order.
    private func noteSection(_ name: String, startUs: Int64, endUs: Int64) {
        locked {
            recentSections.append((name, startUs, endUs))
            if recentSections.count > PerformanceMonitorService.sectionLimit { recentSections.removeFirst() }
        }
    }

    private func sectionsText(from startUs: Int64, to endUs: Int64) -> String? {
        let spans: [(String, Int64)] = locked {
            recentSections.filter { $0.endUs > startUs && $0.startUs < endUs }
                .map { ($0.name, min($0.endUs, endUs) - max($0.startUs, startUs)) }
        }
        if spans.isEmpty { return nil }
        var order: [String] = []
        var totals: [String: Int64] = [:]
        for (name, us) in spans {
            if totals[name] == nil { order.append(name) }
            totals[name, default: 0] += us
        }
        return order.sorted { (totals[$0] ?? 0) > (totals[$1] ?? 0) }
            .map { "\($0) \((totals[$0] ?? 0) / 1_000)ms" }
            .joined(separator: ", ")
    }

    private static func uniqueJoined(_ values: [String]) -> String {
        var seen = Set<String>()
        return values.filter { seen.insert($0).inserted }.joined(separator: ", ")
    }

    private static func captureStack() -> String {
        Thread.callStackSymbols.dropFirst(2).prefix(8).joined(separator: "\n")
    }

    static func captureReturnAddresses() -> [UInt] {
        Thread.callStackReturnAddresses.dropFirst(2).prefix(8).map(\.uintValue)
    }

    static func symbolicate(_ addresses: [UInt]) -> String {
        addresses.enumerated().map { (index: Int, address: UInt) -> String in
            let hex = "0x" + String(address, radix: 16)
            #if canImport(Darwin)
            var info = Dl_info()
            guard let pointer = UnsafeRawPointer(bitPattern: address), dladdr(pointer, &info) != 0 else {
                return "\(index) ??? \(hex)"
            }
            let image = info.dli_fname.map { URL(fileURLWithPath: String(cString: $0)).lastPathComponent } ?? "???"
            let offset = info.dli_fbase.map { " +0x" + String(address &- UInt(bitPattern: $0), radix: 16) } ?? ""
            guard let name = info.dli_sname, let start = info.dli_saddr else { return "\(index) \(image) \(hex)\(offset)" }
            return "\(index) \(image) \(hex)\(offset) \(String(cString: name)) + \(address &- UInt(bitPattern: start))"
            #else
            return "\(index) ??? \(hex)"
            #endif
        }.joined(separator: "\n")
    }

    private func uptimeSeconds(_ micros: Int64) -> Double {
        Double(origin) / 1_000_000_000 + Double(micros) / 1_000_000
    }

    private func elapsedMicros() -> Int64 {
        Int64((DispatchTime.now().uptimeNanoseconds - origin) / 1_000)
    }

    static func micros(_ duration: Duration) -> Int64 {
        let parts = duration.components
        return parts.seconds * 1_000_000 + parts.attoseconds / 1_000_000_000_000
    }

    private func locked<R>(_ body: () throws -> R) rethrows -> R {
        lock.lock()
        defer { lock.unlock() }
        return try body()
    }
}
