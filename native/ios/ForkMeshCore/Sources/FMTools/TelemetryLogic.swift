// The logic of the telemetry strip (compactCount, formatUptime, the chart geometry), the
// Diagnostics screen (downsample, the screen's state, each metric card's numbers) and the
// developer performance overlay (the copied log, the event card text, the indicator's clamp),
// so the SwiftUI views only draw.

import FMAccount
import FMBase
import Foundation
import Observation

public enum TelemetryFormat {
    /// "999", "1.2k", "12k".
    public static func compactCount(_ count: Int) -> String {
        if count < 1000 { return "\(count)" }
        if count < 10000 { return "\(TelemetryMath.fixed1(Double(count) / 1000))k" }
        return "\(count / 1000)k"
    }

    /// "3d4h", "2h5m", "4m10s", "12s".
    public static func uptime(_ seconds: TimeInterval) -> String {
        let s = Int(seconds.rounded(.towardZero))
        let days = s / 86400, hours = s / 3600, minutes = s / 60
        if days > 0 { return "\(days)d\(hours % 24)h" }
        if hours > 0 { return "\(hours)h\(minutes % 60)m" }
        if minutes > 0 { return "\(minutes)m\(s % 60)s" }
        return "\(s)s"
    }

    /// The metric card's "when" for a scrubbed point: "now", "12s ago", "4m ago", "1h5m ago".
    public static func ago(_ seconds: Int) -> String {
        if seconds == 0 { return "now" }
        if seconds / 60 == 0 { return "\(seconds)s ago" }
        if seconds / 3600 == 0 { return "\(seconds / 60)m ago" }
        return "\(seconds / 3600)h\((seconds / 60) % 60)m ago"
    }
}

/// Fold `values` into buckets of `size` counted back from the newest point, so the latest bucket
/// is always complete. Peak metrics keep each bucket's worst value; the rest keep its average.
/// All-NaN buckets stay NaN.
public func telemetryDownsample(_ values: [Double], _ size: Int, peak: Bool) -> [Double] {
    if size <= 1 { return values }
    var out = [Double]()
    var end = values.count
    while end > 0 {
        let start = max(0, end - size)
        var sum = 0.0
        var high = -Double.infinity
        var n = 0
        for i in start..<end where !values[i].isNaN {
            sum += values[i]
            high = max(high, values[i])
            n += 1
        }
        out.append(n == 0 ? .nan : (peak ? high : sum / Double(n)))
        end -= size
    }
    return out.reversed()
}

/// TelemetryChartPainter's geometry: one series as a line over a faint area, anchored to a zero
/// baseline and right-aligned into `slots` so a young history grows in from the right. NaN points
/// break the line.
public struct TelemetryChartGeometry: Sendable {
    public let values: [Double]
    public let slots: Int
    public let floor: Double
    public let width: Double
    public let height: Double
    public let strokeWidth: Double

    public init(values: [Double], slots: Int, floor: Double, width: Double, height: Double, strokeWidth: Double = 2) {
        self.values = values
        self.slots = slots
        self.floor = floor
        self.width = width
        self.height = height
        self.strokeWidth = strokeWidth
    }

    /// Top of the y-scale: the values' max with 10% headroom, never below `floor`.
    public static func scaleTop(_ values: [Double], floor: Double) -> Double {
        var top = floor
        for v in values where !v.isNaN { top = max(top, v * 1.1) }
        return top
    }

    /// Which point a horizontal position lands on, or nil off the data.
    public static func indexAt(x: Double, width: Double, count: Int, slots: Int) -> Int? {
        if count == 0 || width <= 0 { return nil }
        let span = max(slots, count) - 1
        if span == 0 { return 0 }
        let dx = width / Double(span)
        let fromRight = TelemetryMath.round((width - x) / dx)
        let index = count - 1 - fromRight
        return min(max(index, 0), count - 1)
    }

    private var top: Double { TelemetryChartGeometry.scaleTop(values, floor: floor) }

    private var dx: Double {
        let span = max(slots, values.count) - 1
        return span == 0 ? 0 : width / Double(span)
    }

    public func x(_ i: Int) -> Double { width - Double(values.count - 1 - i) * dx }

    public func y(_ v: Double) -> Double {
        let inset = strokeWidth / 2
        let ratio = min(max(max(0, v) / top, 0), 1)
        return height - inset - ratio * (height - inset * 2)
    }

    /// Runs of consecutive sampled points, as index ranges; a run of one is drawn as a dot.
    public var segments: [ClosedRange<Int>] {
        var out = [ClosedRange<Int>]()
        var i = 0
        let n = values.count
        while i < n {
            while i < n && values[i].isNaN { i += 1 }
            if i >= n { break }
            let start = i
            while i < n && !values[i].isNaN { i += 1 }
            out.append(start...(i - 1))
        }
        return out
    }
}

/// The numbers of one metric card of the Diagnostics screen for a range and an optional scrub.
public struct MetricCardState: Sendable {
    public static let maxPoints = 180

    public let metric: TelemetryMetric
    /// The downsampled points, oldest first.
    public let points: [Double]
    /// How many points the full range would hold.
    public let slots: Int
    public let secondsPerPoint: Int
    public let summary: MetricSummary

    public init(metric: TelemetryMetric, range: TelemetryRange, raw: [Double], summary: MetricSummary) {
        self.metric = metric
        let expected = range.usesMinutes ? range.minutes : range.seconds
        let bucket = max(1, Int((Double(expected) / Double(MetricCardState.maxPoints)).rounded(.up)))
        points = telemetryDownsample(raw, bucket, peak: metric.peak)
        slots = Int((Double(expected) / Double(bucket)).rounded(.up))
        secondsPerPoint = bucket * (range.usesMinutes ? 60 : 1)
        self.summary = summary
    }

    @MainActor
    public init(telemetry: AppTelemetry, metric: TelemetryMetric, range: TelemetryRange) {
        self.init(metric: metric, range: range, raw: telemetry.series(metric, range),
                  summary: telemetry.summary(metric, range))
    }

    private func valid(_ scrub: Int?) -> Int? {
        guard let scrub, scrub < points.count else { return nil }
        return scrub
    }

    /// The big number: the scrubbed point, or the latest second.
    public func value(scrub: Int?) -> String {
        if let s = valid(scrub) { return metric.format(points[s]) }
        return metric.format(summary.latest)
    }

    public func when(scrub: Int?) -> String {
        guard let s = valid(scrub) else { return "now" }
        return TelemetryFormat.ago((points.count - 1 - s) * secondsPerPoint)
    }

    public var footer: String {
        if metric.count {
            return "total \(metric.format(summary.total)) · peak \(metric.format(summary.max))/s"
        }
        return "min \(metric.format(summary.min)) · avg \(metric.format(summary.avg)) · max \(metric.format(summary.max))"
    }

    public func scrubIndex(x: Double, width: Double) -> Int? {
        TelemetryChartGeometry.indexAt(x: x, width: width, count: points.count, slots: slots)
    }
}

/// The Diagnostics screen: range, focused metric,
/// log filter and the pause anchor.
@MainActor
@Observable
public final class TelemetryScreenModel {
    @ObservationIgnored public let telemetry: AppTelemetry
    @ObservationIgnored private let now: () -> Date

    public var range: TelemetryRange = .m5
    /// Drawn large at the top of Stats; tapping it again clears it.
    public var focus: TelemetryMetric?
    public private(set) var categories: Set<LogCategory> = []
    public private(set) var pausedAt: Date?
    /// 0 = Stats, 1 = Logs.
    public var tab: Int

    public init(telemetry: AppTelemetry, focusMetric: TelemetryMetric? = nil, logCategory: LogCategory? = nil,
                initialTab: Int = 0, now: (() -> Date)? = nil) {
        self.telemetry = telemetry
        self.now = now ?? { Date() }
        tab = logCategory != nil ? 1 : initialTab
        focus = focusMetric
        if let logCategory { categories = [logCategory] }
    }

    public static let title = "Diagnostics"

    /// "Up 4m10s · sampled every second while the app is open · history since launch".
    public var uptimeLine: String {
        "Up \(TelemetryFormat.uptime(telemetry.uptime)) · sampled every second while the app is open · history since launch"
    }

    public func card(_ metric: TelemetryMetric) -> MetricCardState {
        MetricCardState(telemetry: telemetry, metric: metric, range: range)
    }

    /// Stats grid columns for a content width (the list's padding already taken off).
    public static func columns(width: Double) -> Int { width >= 720 ? 4 : width >= 480 ? 3 : 2 }

    // MARK: Logs

    private var anchor: Date {
        _ = telemetry.revision
        return pausedAt ?? now()
    }

    public var visibleEntries: [AppLogEntry] {
        let entries = telemetry.log.entries(
            categories: categories.isEmpty ? nil : categories,
            since: anchor.addingTimeInterval(-Double(range.seconds)))
        guard let pausedAt else { return entries }
        return entries.filter { $0.timestamp <= pausedAt }
    }

    public var counts: [Int] { telemetry.log.countsSince(anchor.addingTimeInterval(-Double(range.seconds))) }

    public var allChipLabel: String { "All \(counts.reduce(0, +))" }

    public func chipLabel(_ category: LogCategory) -> String { "\(category.label) \(counts[category.index])" }

    public func isSelected(_ category: LogCategory) -> Bool { categories.contains(category) }

    public var allSelected: Bool { categories.isEmpty }

    public func selectAll() { categories.removeAll() }

    public func setCategory(_ category: LogCategory, on: Bool) {
        if on { categories.insert(category) } else { categories.remove(category) }
    }

    public var caption: String {
        let log = telemetry.log
        if categories.count == 1, let single = categories.first {
            let stallPath = telemetry.monitor?.stallLogPath
            let also = single == .stall && stallPath != nil ? " Also written to \(stallPath!)" : ""
            return "\(single.description). \(log.totalFor(single)) since launch.\(also)"
        }
        return "\(visibleEntries.count) lines in the last \(range.label) · \(log.totalLines) since launch"
    }

    public var emptyText: String { "Nothing logged in the last \(range.label)" }

    public var paused: Bool { pausedAt != nil }
    public var pauseLabel: String { pausedAt == nil ? "Pause" : "Resume" }

    public func togglePause() { pausedAt = pausedAt == nil ? now() : nil }

    /// What "Copy the logs shown" puts on the clipboard, oldest first, and its confirmation.
    public func copyText() -> (text: String, confirmation: String) {
        let entries = visibleEntries
        return (entries.reversed().map(\.description).joined(separator: "\n"), "Copied \(entries.count) log lines")
    }
}

// MARK: - Developer performance overlay

public enum PerformanceLogText {
    /// `_formatPerformanceLog`: what the overlay's Copy puts on the clipboard.
    public static func log(_ events: [PerformanceEvent], timeZone: TimeZone = .current) -> String {
        if events.isEmpty { return "No slow performance events recorded." }
        return events.map { event($0, timeZone: timeZone) }.joined(separator: "\n\n")
    }

    public static func event(_ event: PerformanceEvent, timeZone: TimeZone = .current) -> String {
        var lines = [
            "[\(event.kind.rawValue)] \(event.name)",
            "duration: \(event.durationMs)ms",
            "timestamp: \(DartDateFormat.iso8601(event.timestamp, timeZone: timeZone))",
        ]
        for (key, value) in event.details { lines.append("\(key): \(dartText(value))") }
        if !event.stackTrace.isEmpty {
            lines.append("stack:")
            lines.append(event.stackTrace)
        }
        return lines.joined(separator: "\n")
    }

    /// The card's details block: "key: value" lines.
    public static func details(_ event: PerformanceEvent) -> String {
        event.details.map { "\($0.key): \(dartText($0.value))" }.joined(separator: "\n")
    }

    /// "<kind> at HH:mm:ss".
    public static func kindLine(_ event: PerformanceEvent, timeZone: TimeZone = .current) -> String {
        let entry = AppLogEntry(timestamp: event.timestamp, category: .perf, message: "")
        return "\(event.kind.rawValue) at \(entry.stamp(timeZone: timeZone))"
    }

    /// Frames get the layers glyph, everything else the route glyph.
    public static func icon(_ event: PerformanceEvent) -> String {
        event.kind == .frame ? "layers_outlined" : "route_outlined"
    }

    public static func emptyMessage(_ monitor: PerformanceMonitorService) -> String {
        "Stalls over \(Int(TelemetryCounters.millis(monitor.stallThreshold))) ms and operations over \(Int(TelemetryCounters.millis(monitor.threshold))) ms will appear here."
    }

    /// A details value as text: strings bare, null as "null", the rest as JSON text.
    static func dartText(_ value: JSONValue) -> String {
        switch value {
        case .string(let s): return s
        case .null: return "null"
        default: return value.text
        }
    }

    /// `_clampIndicatorOffset`: keeps the dragged indicator inside the screen, below the top inset.
    public static func clampIndicator(x: Double, y: Double, boundsWidth: Double, boundsHeight: Double,
                                      width: Double, height: Double, topInset: Double, bottomInset: Double) -> (x: Double, y: Double) {
        let margin = 8.0
        let minX = margin
        let maxX = max(boundsWidth - width - margin, minX)
        let minY = topInset + margin
        let maxY = max(boundsHeight - height - bottomInset - margin, minY)
        return (min(max(x, minX), maxX), min(max(y, minY), maxY))
    }
}
