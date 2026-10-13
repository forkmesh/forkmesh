// The in-app log (AppLog), the per-second metric
// rings (MetricHistory) and the sampler behind the telemetry strip and the Diagnostics screen
// (AppTelemetry), plus the HTTP decorator that counts and logs every request.
//
// Inputs: FMBase's `Log.sink` (captureProcessLogs wraps it), frame timings through `recordFrame`,
// the lifecycle through `appLifecycleChanged`, and `TelemetryHTTPClient` wrapping FMNet's
// `HTTPClient`.

import FMAccount
import FMApi
import FMBase
import FMNet
import Foundation
import Observation
#if canImport(Darwin)
import Darwin
#endif

/// The families the in-app log sorts its lines into: the mobile counterpart of the desktop
/// footer's log lights. Each is an icon and a live count in the telemetry strip under the top
/// bar, and a filter in the Logs view.
public enum LogCategory: Int, CaseIterable, Sendable {
    case stall, error, net, relay, sync, auth, push, perf, info

    public var label: String {
        switch self {
        case .stall: return "STALL"
        case .error: return "ERROR"
        case .net: return "NET"
        case .relay: return "RELAY"
        case .sync: return "SYNC"
        case .auth: return "AUTH"
        case .push: return "PUSH"
        case .perf: return "PERF"
        case .info: return "INFO"
        }
    }

    public var description: String {
        switch self {
        case .stall: return "UI stalls: frames, event-loop blocks, named slow sections"
        case .error: return "Failures: HTTP errors, uncaught exceptions, failed calls"
        case .net: return "HTTP requests to ForkMesh services"
        case .relay: return "Relay socket: connects, peers, encryption notices"
        case .sync: return "Node sync drains and applies"
        case .auth: return "Session, identity and device-unlock events"
        case .push: return "Push notification registration"
        case .perf: return "Slow awaited work (the UI stayed responsive)"
        case .info: return "Everything else the app printed"
        }
    }

    /// The category's position in declaration order.
    public var index: Int { rawValue }

    /// Material icon name, for FmIcon.
    public var icon: String {
        switch self {
        case .stall: return "hourglass_bottom_rounded"
        case .error: return "error_outline_rounded"
        case .net: return "cloud_outlined"
        case .relay: return "cell_tower_rounded"
        case .sync: return "sync_rounded"
        case .auth: return "key_outlined"
        case .push: return "notifications_none_rounded"
        case .perf: return "speed_rounded"
        case .info: return "info_outline_rounded"
        }
    }

    /// Sort a free-form platform log line into a family. Order matters: the monitor's own
    /// prefixes win over words that appear in their details.
    public static func classify(_ line: String) -> LogCategory {
        let lower = line.lowercased()
        if lower.hasPrefix("[stall]") { return .stall }
        if lower.hasPrefix("[perf]") { return .perf }
        if lower.contains("exception") || lower.contains("failed") || lower.contains("error") {
            return .error
        }
        if lower.hasPrefix("[push]") || lower.contains("apns") { return .push }
        if lower.contains("node sync") { return .sync }
        if lower.contains("auth") || lower.contains("session") || lower.contains("identity")
            || lower.contains("userpresence") {
            return .auth
        }
        return .info
    }
}

/// Status ink of a category's count: failures and
/// stalls are the two families worth noticing at a glance; everything else stays in text ink.
public enum LogCategoryInk: Equatable, Sendable {
    case tertiary, danger, warning, primary

    public static func of(_ category: LogCategory, count: Int) -> LogCategoryInk {
        if count == 0 { return .tertiary }
        switch category {
        case .error: return .danger
        case .stall: return .warning
        default: return .primary
        }
    }
}

public struct AppLogEntry: Equatable, Sendable, CustomStringConvertible {
    public let timestamp: Date
    public let category: LogCategory
    public let message: String

    public init(timestamp: Date, category: LogCategory, message: String) {
        self.timestamp = timestamp
        self.category = category
        self.message = message
    }

    /// `'${timestamp.toIso8601String()} ${category.label} $message'`.
    public var description: String { text() }

    public func text(timeZone: TimeZone = .current) -> String {
        "\(DartDateFormat.iso8601(timestamp, timeZone: timeZone)) \(category.label) \(message)"
    }

    /// The Logs view's "HH:mm:ss" stamp, in local time.
    public func stamp(timeZone: TimeZone = .current) -> String {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = timeZone
        let c = calendar.dateComponents([.hour, .minute, .second], from: timestamp)
        func two(_ v: Int?) -> String { let v = v ?? 0; return v < 10 ? "0\(v)" : "\(v)" }
        return "\(two(c.hour)):\(two(c.minute)):\(two(c.second))"
    }
}

/// Bounded in-memory log the app can show itself.
///
/// Appending never notifies: lines arrive from any thread (the `Log` sink, socket callbacks), and
/// `AppTelemetry` publishes the counts on its once a second tick instead. Thread-safe.
public final class AppLog: @unchecked Sendable {
    public let capacity: Int
    private let now: @Sendable () -> Date
    private let lock = NSLock()
    private var ring: [AppLogEntry] = []
    private var head = 0
    private var totals = [Int](repeating: 0, count: LogCategory.allCases.count)
    private var lines = 0

    public init(capacity: Int = 2000, now: (@Sendable () -> Date)? = nil) {
        self.capacity = capacity
        self.now = now ?? { Date() }
    }

    /// Lines appended since launch, including ones the ring has dropped.
    public var totalLines: Int {
        lock.lock(); defer { lock.unlock() }
        return lines
    }

    public func totalFor(_ category: LogCategory) -> Int {
        lock.lock(); defer { lock.unlock() }
        return totals[category.index]
    }

    public func add(_ category: LogCategory, _ message: String) {
        let entry = AppLogEntry(timestamp: now(), category: category, message: message)
        lock.lock()
        if ring.count < capacity {
            ring.append(entry)
        } else {
            ring[head] = entry
            head = (head + 1) % capacity
        }
        totals[category.index] += 1
        lines += 1
        lock.unlock()
    }

    /// Classify and append a raw platform log line.
    public func addLine(_ line: String) {
        let trimmed = AppLog.trimRight(line)
        if trimmed.isEmpty { return }
        add(LogCategory.classify(trimmed), trimmed)
    }

    /// Route everything the app logs through FMBase's `Log` into this log. Returns a callback
    /// that restores the previous sink.
    @discardableResult
    public func captureProcessLogs() -> () -> Void {
        let previous = Log.sink
        Log.sink = { [weak self] level, tag, message in
            self?.addLine(message)
            previous(level, tag, message)
        }
        return { Log.sink = previous }
    }

    /// Retained entries, newest first, optionally limited to `categories` and to entries at or
    /// after `since`.
    public func entries(categories: Set<LogCategory>? = nil, since: Date? = nil) -> [AppLogEntry] {
        lock.lock()
        let snapshot = ring
        let start = head
        lock.unlock()
        var out = [AppLogEntry]()
        let n = snapshot.count
        var i = 0
        while i < n {
            let entry = snapshot[(start + n - 1 - i) % n]
            i += 1
            if let since, entry.timestamp < since { break }
            if let categories, !categories.contains(entry.category) { continue }
            out.append(entry)
        }
        return out
    }

    /// Retained entries per category at or after `since`, indexed by `LogCategory.index`.
    public func countsSince(_ since: Date) -> [Int] {
        var counts = [Int](repeating: 0, count: LogCategory.allCases.count)
        for entry in entries(since: since) { counts[entry.category.index] += 1 }
        return counts
    }

    /// Trims whitespace and line breaks at the end.
    static func trimRight(_ text: String) -> String {
        var scalars = Substring(text).unicodeScalars
        while let last = scalars.last, last.properties.isWhitespace || last == "\u{FEFF}" {
            scalars.removeLast()
        }
        return String(scalars)
    }
}

/// What the strip and the stats view chart. Every metric is one number per second; `peak` metrics
/// roll minutes up by their worst second, the rest by their average (rates) so a 24 h chart still
/// reads in the same unit.
public enum TelemetryHealth: Sendable, Equatable {
    case good, warning, bad
}

public enum TelemetryMetric: Int, CaseIterable, Sendable {
    case fps, frame, loop, mem, net, relay, log, stall

    public var label: String {
        switch self {
        case .fps: return "FPS"
        case .frame: return "FRAME"
        case .loop: return "LOOP"
        case .mem: return "MEM"
        case .net: return "NET"
        case .relay: return "RELAY"
        case .log: return "LOG"
        case .stall: return "STALL"
        }
    }

    public var title: String {
        switch self {
        case .fps: return "Frames drawn"
        case .frame: return "Worst frame"
        case .loop: return "Event loop lag"
        case .mem: return "Memory"
        case .net: return "HTTP requests"
        case .relay: return "Relay frames"
        case .log: return "Log lines"
        case .stall: return "Stalls"
        }
    }

    public var description: String {
        switch self {
        case .fps: return "frames drawn per second"
        case .frame: return "slowest frame build + raster in the second"
        case .loop: return "worst UI-isolate event-loop delay in the second"
        case .mem: return "resident set size of the app process"
        case .net: return "API requests finished per second"
        case .relay: return "relay socket frames received"
        case .log: return "lines written to the in-app log"
        case .stall: return "UI stalls recorded (200 ms or longer)"
        }
    }

    public var unit: String {
        switch self {
        case .frame, .loop: return "ms"
        case .mem: return "MB"
        default: return ""
        }
    }

    /// Smallest top of the y-scale, so a quiet metric draws as a low line instead of full-height
    /// noise.
    public var floor: Double {
        switch self {
        case .fps: return 60
        case .frame: return 32
        case .loop: return 50
        default: return 1
        }
    }

    public var peak: Bool { self == .frame || self == .loop }

    /// Events per second, so a range also has a meaningful total.
    public var count: Bool { self == .net || self == .relay || self == .log || self == .stall }

    public var lowerIsWorse: Bool { self == .fps }

    public var limits: (warning: Double, bad: Double) {
        switch self {
        case .fps: return (45, 24)
        case .frame: return (50, 100)
        case .loop: return (50, 200)
        case .mem: return (500, 1000)
        case .net: return (10, 30)
        case .relay: return (50, 200)
        case .log: return (20, 100)
        case .stall: return (1, 1)
        }
    }

    public func health(_ values: [Double]) -> TelemetryHealth? {
        let sampled: [Double] = values.filter { !$0.isNaN }
        guard let worst = lowerIsWorse ? sampled.min() : sampled.max() else { return nil }
        let (warning, bad) = limits
        if lowerIsWorse {
            return worst < bad ? .bad : worst < warning ? .warning : .good
        }
        return worst >= bad ? .bad : worst >= warning ? .warning : .good
    }

    /// Three letters for the live strip's squares, where `label` does not fit.
    public var short: String {
        switch self {
        case .frame: return "FRM"
        case .loop: return "LAG"
        case .relay: return "RLY"
        case .stall: return "STL"
        default: return label
        }
    }

    /// `format` without the unit: a strip square has room for the digits only, and its three
    /// letters already say what they measure.
    public func formatBare(_ value: Double) -> String {
        if value.isNaN { return "–" }
        if !unit.isEmpty { return String(TelemetryMath.round(value)) }
        return format(value)
    }

    public func format(_ value: Double) -> String {
        if value.isNaN { return "–" }
        if self == .mem { return "\(TelemetryMath.round(value))MB" }
        if unit == "ms" { return "\(TelemetryMath.round(value))ms" }
        if value >= 1000 { return "\(TelemetryMath.fixed1(value / 1000))k" }
        if value != value.rounded() { return TelemetryMath.fixed1(value) }
        return String(TelemetryMath.round(value))
    }
}

/// Number formatting the telemetry uses.
enum TelemetryMath {
    /// `double.round()` (half away from zero); infinities clamp.
    static func round(_ value: Double) -> Int {
        if !value.isFinite { return value > 0 ? Int.max : Int.min }
        let r = value.rounded(.toNearestOrAwayFromZero)
        if r >= Double(Int.max) { return Int.max }
        if r <= Double(Int.min) { return Int.min }
        return Int(r)
    }

    /// `toStringAsFixed(1)`.
    static func fixed1(_ value: Double) -> String { DartCore.toStringAsFixed(value, 1) }
}

/// The time windows the stats view offers, like the desktop Stats page.
public enum TelemetryRange: Int, CaseIterable, Sendable {
    case m1, m5, m15, h1, h6, h24

    public var label: String {
        switch self {
        case .m1: return "1m"
        case .m5: return "5m"
        case .m15: return "15m"
        case .h1: return "1h"
        case .h6: return "6h"
        case .h24: return "24h"
        }
    }

    /// The window in seconds.
    public var seconds: Int {
        switch self {
        case .m1: return 60
        case .m5: return 300
        case .m15: return 900
        case .h1: return 3600
        case .h6: return 6 * 3600
        case .h24: return 24 * 3600
        }
    }

    public var minutes: Int { seconds / 60 }

    /// Ranges up to an hour read per-second samples; longer ones read minutes.
    public var usesMinutes: Bool { seconds > 3600 }
}

public struct MetricSummary: Equatable, Sendable {
    public let latest: Double
    public let min: Double
    public let avg: Double
    public let max: Double
    /// Sum of per-second values across the range (events, for count metrics).
    public let total: Double
    public let samples: Int
}

/// A second ring (one hour) and a minute ring (one day) for one metric. Seconds the app was not
/// sampling (backgrounded) are NaN, and so are minutes with no sampled second, so charts show the
/// gap instead of a lie. Not thread-safe: AppTelemetry drives it from its lock.
public final class MetricHistory {
    public let secondCapacity: Int
    public let minuteCapacity: Int

    private var seconds: [Double]
    private var secondHead = 0
    private var secondCount = 0

    private var minuteAvg: [Double]
    private var minuteMax: [Double]
    private var minuteHead = 0
    private var minuteCount = 0

    private var pendingSeconds = 0
    private var pendingValid = 0
    private var pendingSum = 0.0
    private var pendingMax = -Double.infinity

    public init(secondCapacity: Int = 3600, minuteCapacity: Int = 1440) {
        self.secondCapacity = secondCapacity
        self.minuteCapacity = minuteCapacity
        seconds = [Double](repeating: 0, count: secondCapacity)
        minuteAvg = [Double](repeating: 0, count: minuteCapacity)
        minuteMax = [Double](repeating: 0, count: minuteCapacity)
    }

    public var latest: Double {
        if secondCount == 0 { return .nan }
        return seconds[(secondHead - 1 + secondCapacity) % secondCapacity]
    }

    public func add(_ value: Double) {
        seconds[secondHead] = value
        secondHead = (secondHead + 1) % secondCapacity
        if secondCount < secondCapacity { secondCount += 1 }

        pendingSeconds += 1
        if !value.isNaN {
            pendingValid += 1
            pendingSum += value
            pendingMax = Swift.max(pendingMax, value)
        }
        if pendingSeconds == 60 {
            minuteAvg[minuteHead] = pendingValid == 0 ? .nan : pendingSum / Double(pendingValid)
            minuteMax[minuteHead] = pendingValid == 0 ? .nan : pendingMax
            minuteHead = (minuteHead + 1) % minuteCapacity
            if minuteCount < minuteCapacity { minuteCount += 1 }
            pendingSeconds = 0
            pendingValid = 0
            pendingSum = 0
            pendingMax = -Double.infinity
        }
    }

    /// Record `count` unsampled seconds. Whole minutes past the first partial one go straight into
    /// the minute ring, so resuming after a day in the background costs a few thousand writes,
    /// not 86,400 per metric.
    public func addGap(_ count: Int) {
        if count <= 0 { return }
        var remaining = count
        let align = Swift.min(remaining, (60 - pendingSeconds) % 60)
        for _ in 0..<align { add(.nan) }
        remaining -= align
        let minutes = remaining / 60
        if minutes > 0 {
            for _ in 0..<Swift.min(minutes, minuteCapacity) {
                minuteAvg[minuteHead] = .nan
                minuteMax[minuteHead] = .nan
                minuteHead = (minuteHead + 1) % minuteCapacity
                if minuteCount < minuteCapacity { minuteCount += 1 }
            }
            let secondsFill = Swift.min(minutes * 60, secondCapacity)
            for _ in 0..<secondsFill {
                seconds[secondHead] = .nan
                secondHead = (secondHead + 1) % secondCapacity
                if secondCount < secondCapacity { secondCount += 1 }
            }
            remaining -= minutes * 60
        }
        for _ in 0..<Swift.max(0, remaining) { add(.nan) }
    }

    /// The newest `n` per-second samples, oldest first.
    public func lastSeconds(_ n: Int) -> [Double] {
        MetricHistory.tail(seconds, secondHead, secondCount, secondCapacity, n)
    }

    /// The newest `n` minute roll-ups, oldest first.
    public func lastMinutes(_ n: Int, peak: Bool) -> [Double] {
        MetricHistory.tail(peak ? minuteMax : minuteAvg, minuteHead, minuteCount, minuteCapacity, n)
    }

    private static func tail(_ ring: [Double], _ head: Int, _ count: Int, _ capacity: Int, _ n: Int) -> [Double] {
        let take = Swift.max(0, Swift.min(n, count))
        return (0..<take).map { i in ring[(head - take + i + capacity) % capacity] }
    }
}

/// Live app health for the telemetry strip and the stats view.
///
/// Samples once a second while the app is visible: frame rate and worst frame (`recordFrame`),
/// main-thread lag (the stall monitor's heartbeat), process memory, HTTP requests, relay frames,
/// log lines and stalls. Observers see a change only on that tick (`revision`), so a view reading
/// it re-renders at 1 Hz no matter how busy the socket or the log is.
///
/// `recordFrame`, `recordLoopLag` and the HTTP counters may be called from any thread; sampling,
/// `start`/`stop` and the observable reads belong to the main actor.
@MainActor
@Observable
public final class AppTelemetry {
    @ObservationIgnored public let log: AppLog
    @ObservationIgnored public let monitor: PerformanceMonitorService?
    /// Total relay frames received so far; read as a delta each tick. Set by whoever owns the
    /// relay (the Chat feature) once it exists.
    @ObservationIgnored public var relayFrames: (@MainActor () -> Int)?
    /// Seconds between samples.
    @ObservationIgnored public let sampleInterval: Double
    @ObservationIgnored private let now: () -> Date
    @ObservationIgnored private let rssMegabytes: () -> Double
    @ObservationIgnored public let startedAt: Date
    @ObservationIgnored private let counters = TelemetryCounters()
    @ObservationIgnored private var histories: [MetricHistory] = TelemetryMetric.allCases.map { _ in MetricHistory() }
    @ObservationIgnored private var timer: Task<Void, Never>?
    @ObservationIgnored private var lastSampleAt: Date?
    @ObservationIgnored private var lastRequests = 0
    @ObservationIgnored private var lastRelayFrames = 0
    @ObservationIgnored private var lastLogLines = 0
    @ObservationIgnored private var lastStalls = 0
    @ObservationIgnored private var started = false

    /// Bumped by every sample; everything below reads it so observers re-render on the tick.
    public private(set) var revision = 0

    public init(
        log: AppLog,
        monitor: PerformanceMonitorService? = nil,
        relayFrames: (@MainActor () -> Int)? = nil,
        sampleInterval: Double = 1,
        now: (() -> Date)? = nil,
        rssMegabytes: (() -> Double)? = nil
    ) {
        self.log = log
        self.monitor = monitor
        self.relayFrames = relayFrames
        self.sampleInterval = sampleInterval
        self.now = now ?? { Date() }
        self.rssMegabytes = rssMegabytes ?? { AppTelemetry.processRssMegabytes() }
        startedAt = self.now()
    }

    public var uptime: TimeInterval {
        _ = revision
        return now().timeIntervalSince(startedAt)
    }

    public var requestsInFlight: Int { counters.inFlight }
    public var running: Bool { timer != nil }

    public func history(_ metric: TelemetryMetric) -> MetricHistory {
        _ = revision
        return histories[metric.rawValue]
    }

    public func latest(_ metric: TelemetryMetric) -> Double {
        _ = revision
        return histories[metric.rawValue].latest
    }

    // MARK: Running

    /// Begin sampling: heartbeat lag and the 1 Hz tick. Idempotent. Feed lifecycle changes to
    /// `appLifecycleChanged`; frames to `recordFrame`.
    public func start(lifecycleState: AppLifecycleState? = nil) {
        if started { return }
        started = true
        let counters = self.counters
        monitor?.onHeartbeatLag = { lag in counters.recordLoopLag(lag) }
        if lifecycleState == nil || lifecycleState == .resumed || lifecycleState == .inactive {
            startTimer()
        }
    }

    /// The `AppLifecycleListener`: sample only while the app is visible.
    public func appLifecycleChanged(_ state: AppLifecycleState) {
        guard started else { return }
        if state == .resumed || state == .inactive {
            startTimer()
        } else {
            timer?.cancel()
            timer = nil
        }
    }

    private func startTimer() {
        if timer != nil { return }
        let nanos = UInt64(Swift.max(sampleInterval, 0.01) * 1_000_000_000)
        timer = Task { @MainActor [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: nanos)
                if Task.isCancelled { return }
                self?.sampleNow()
            }
        }
    }

    public func stop() {
        timer?.cancel()
        timer = nil
        if started { monitor?.onHeartbeatLag = nil }
        started = false
    }

    public func dispose() { stop() }

    // MARK: Inputs (any thread)

    /// Count one rendered frame.
    public nonisolated func recordFrame(_ totalSpan: Duration) { counters.recordFrame(totalSpan) }

    /// A batch of frame timings (the `addTimingsCallback` shape).
    public nonisolated func recordFrames(_ timings: [FrameTiming]) {
        for timing in timings {
            counters.recordFrame(timing.totalSpan ?? (timing.buildDuration + timing.rasterDuration + timing.vsyncOverhead))
        }
    }

    /// Fold one heartbeat's lag into this second.
    public nonisolated func recordLoopLag(_ lag: Duration) { counters.recordLoopLag(lag) }

    // MARK: Sampling

    /// Take one sample now. The timer calls this; tests call it directly.
    public func sampleNow() {
        let at = now()
        if let last = lastSampleAt {
            // A backgrounded stretch leaves a visible gap rather than a flat line.
            let gapSeconds = at.timeIntervalSince(last) * 1000 / (sampleInterval * 1000)
            let missing = Swift.min(TelemetryMath.round(gapSeconds) - 1, 24 * 3600)
            for history in histories { history.addGap(missing) }
        }
        lastSampleAt = at

        let stalls = monitor?.stallCount ?? 0
        let relay = relayFrames?() ?? 0
        let lines = log.totalLines
        let second = counters.takeSecond()
        record(.fps, Double(second.frames))
        record(.frame, second.worstFrameMs)
        record(.loop, second.worstLoopLagMs)
        record(.mem, rssMegabytes())
        record(.net, Double(second.requestsTotal - lastRequests))
        record(.relay, Double(Swift.max(0, relay - lastRelayFrames)))
        record(.log, Double(lines - lastLogLines))
        record(.stall, Double(Swift.max(0, stalls - lastStalls)))

        lastRequests = second.requestsTotal
        lastRelayFrames = relay
        lastLogLines = lines
        lastStalls = stalls
        revision += 1
    }

    private func record(_ metric: TelemetryMetric, _ value: Double) {
        histories[metric.rawValue].add(value)
    }

    /// Chart points for `metric` across `range`, oldest first.
    public func series(_ metric: TelemetryMetric, _ range: TelemetryRange) -> [Double] {
        let history = self.history(metric)
        if range.usesMinutes { return history.lastMinutes(range.minutes, peak: metric.peak) }
        return history.lastSeconds(range.seconds)
    }

    public func summary(_ metric: TelemetryMetric, _ range: TelemetryRange) -> MetricSummary {
        let points = series(metric, range)
        var low = Double.infinity
        var high = -Double.infinity
        var sum = 0.0
        var n = 0
        for value in points where !value.isNaN {
            low = Swift.min(low, value)
            high = Swift.max(high, value)
            sum += value
            n += 1
        }
        // Minute points of rate metrics are per-second averages; a total counts every second in them.
        let perPoint = range.usesMinutes ? 60.0 : 1.0
        return MetricSummary(
            latest: latest(metric),
            min: n == 0 ? .nan : low,
            avg: n == 0 ? .nan : sum / Double(n),
            max: n == 0 ? .nan : high,
            total: metric.peak ? .nan : sum * perPoint,
            samples: n)
    }

    // MARK: HTTP

    /// Wrap the app's HTTP client so every request is counted and logged.
    public nonisolated func wrap(_ inner: HTTPClient) -> HTTPClient {
        TelemetryHTTPClient(inner: inner, log: log, counters: counters)
    }

    // MARK: Memory

    /// Resident set size of this process in MB, NaN when the platform will not say.
    public nonisolated static func processRssMegabytes() -> Double {
        #if canImport(Darwin)
        var info = mach_task_basic_info()
        var count = mach_msg_type_number_t(MemoryLayout<mach_task_basic_info>.size / MemoryLayout<natural_t>.size)
        let result: kern_return_t = withUnsafeMutablePointer(to: &info) { pointer in
            pointer.withMemoryRebound(to: integer_t.self, capacity: Int(count)) { raw in
                task_info(mach_task_self_, task_flavor_t(MACH_TASK_BASIC_INFO), raw, &count)
            }
        }
        if result != KERN_SUCCESS { return .nan }
        return Double(info.resident_size) / (1024 * 1024)
        #else
        // Linux: the second field of /proc/self/statm is resident pages.
        guard let text = try? String(contentsOfFile: "/proc/self/statm", encoding: .utf8) else { return .nan }
        let fields = text.split(separator: " ")
        guard fields.count > 1, let pages = Double(fields[1]) else { return .nan }
        return pages * 4096 / (1024 * 1024)
        #endif
    }
}

/// The counters fed from any thread between two samples.
final class TelemetryCounters: @unchecked Sendable {
    struct Second {
        let frames: Int
        let worstFrameMs: Double
        let worstLoopLagMs: Double
        let requestsTotal: Int
    }

    private let lock = NSLock()
    private var frames = 0
    private var worstFrameMs = 0.0
    private var worstLoopLagMs = 0.0
    private var requestsTotal = 0
    private var requestsInFlight = 0

    var inFlight: Int { lock.lock(); defer { lock.unlock() }; return requestsInFlight }

    func recordFrame(_ span: Duration) {
        let ms = TelemetryCounters.millis(span)
        lock.lock()
        frames += 1
        worstFrameMs = max(worstFrameMs, ms)
        lock.unlock()
    }

    func recordLoopLag(_ lag: Duration) {
        let ms = TelemetryCounters.millis(lag)
        lock.lock()
        worstLoopLagMs = max(worstLoopLagMs, ms)
        lock.unlock()
    }

    func requestStarted() {
        lock.lock()
        requestsInFlight += 1
        lock.unlock()
    }

    func requestFinished() {
        lock.lock()
        requestsInFlight = max(0, requestsInFlight - 1)
        requestsTotal += 1
        lock.unlock()
    }

    /// This second's values; the frame and lag accumulators start again from zero.
    func takeSecond() -> Second {
        lock.lock()
        defer { lock.unlock() }
        let second = Second(frames: frames, worstFrameMs: worstFrameMs, worstLoopLagMs: worstLoopLagMs, requestsTotal: requestsTotal)
        frames = 0
        worstFrameMs = 0
        worstLoopLagMs = 0
        return second
    }

    /// `inMicroseconds / 1000`.
    static func millis(_ duration: Duration) -> Double {
        let c = duration.components
        let micros = c.seconds * 1_000_000 + c.attoseconds / 1_000_000_000_000
        return Double(micros) / 1000
    }
}

/// Counts and logs each request, then hands it to the real client.
public struct TelemetryHTTPClient: HTTPClient {
    let inner: HTTPClient
    let log: AppLog
    let counters: TelemetryCounters

    public func send(_ request: HTTPRequest) async throws -> HTTPResponse {
        let started = DispatchTime.now().uptimeNanoseconds
        counters.requestStarted()
        do {
            let response = try await inner.send(request)
            finished(request, status: response.status, started: started, error: nil)
            return response
        } catch {
            finished(request, status: nil, started: started, error: error)
            throw error
        }
    }

    public func stream(_ request: HTTPRequest) async throws -> HTTPStreamedResponse {
        let started = DispatchTime.now().uptimeNanoseconds
        counters.requestStarted()
        do {
            let response = try await inner.stream(request)
            finished(request, status: response.status, started: started, error: nil)
            return response
        } catch {
            finished(request, status: nil, started: started, error: error)
            throw error
        }
    }

    private func finished(_ request: HTTPRequest, status: Int?, started: UInt64, error: Error?) {
        counters.requestFinished()
        let ms = Int((DispatchTime.now().uptimeNanoseconds &- started) / 1_000_000)
        // Paths only: query strings can carry tickets and tokens.
        let base = "\(request.method) \(request.url.host ?? "")\(request.url.path)"
        if let error {
            log.add(.error, "\(base) failed after \(ms)ms: \(error)")
        } else if let status, status >= 400 {
            log.add(.error, "\(base) HTTP \(status) in \(ms)ms")
        } else {
            log.add(.net, "\(base) \(status.map(String.init) ?? "null") in \(ms)ms")
        }
    }
}
