import FMAccount
import Foundation

public struct PerformanceAlert: Equatable, Sendable {
    public static let hangMs: Int = 250
    public static let hitchCount: Int = 5
    public static let window: TimeInterval = 600
    public static let diagnosticWindow: TimeInterval = 86_400
    public static let none: PerformanceAlert = PerformanceAlert(reason: "")

    public struct Sample: Equatable, Sendable {
        public var kind: PerformanceEventKind
        public var durationMs: Int
        public var at: Date

        public init(kind: PerformanceEventKind, durationMs: Int, at: Date) {
            self.kind = kind
            self.durationMs = durationMs
            self.at = at
        }

        public init(_ event: PerformanceEvent) {
            self.init(kind: event.kind, durationMs: event.durationMs, at: event.timestamp)
        }
    }

    public struct Diagnostic: Equatable, Sendable {
        public var summary: String
        public var at: Date

        public init(summary: String, at: Date) {
            self.summary = summary
            self.at = at
        }
    }

    public let reason: String

    public init(reason: String) {
        self.reason = reason
    }

    public var detected: Bool { !reason.isEmpty }

    public var label: String { detected ? "Slow, send report" : "Send diagnostics" }

    public static func evaluate(
        samples: [Sample], diagnostics: [Diagnostic], lastSent: Date?, now: Date = Date()
    ) -> PerformanceAlert {
        let sent: Date = lastSent ?? .distantPast
        let since: Date = max(now.addingTimeInterval(-window), sent)
        let diagnosticSince: Date = max(now.addingTimeInterval(-diagnosticWindow), sent)
        if let newest = diagnostics.filter({ (item: Diagnostic) -> Bool in item.at > diagnosticSince && item.at <= now })
            .max(by: { (a: Diagnostic, b: Diagnostic) -> Bool in a.at < b.at })
        {
            return PerformanceAlert(reason: "iOS reported: \(newest.summary)")
        }
        let recent: [Sample] = samples.filter { (sample: Sample) -> Bool in sample.at > since && sample.at <= now }
        let hangs: [Sample] = recent.filter { (sample: Sample) -> Bool in
            (sample.kind == .eventLoop || sample.kind == .sync) && sample.durationMs >= hangMs
        }
        if let worst = hangs.max(by: { (a: Sample, b: Sample) -> Bool in a.durationMs < b.durationMs }) {
            let seconds: String = String(format: "%.1f", Double(worst.durationMs) / 1000)
            let extra: String = hangs.count > 1 ? " (\(hangs.count) freezes)" : ""
            return PerformanceAlert(reason: "The app froze for \(seconds) s\(extra)")
        }
        let hitches: Int = recent.filter { (sample: Sample) -> Bool in sample.kind == .frame }.count
        if hitches >= hitchCount {
            return PerformanceAlert(reason: "\(hitches) dropped-frame hitches in \(Int(window / 60)) min")
        }
        return .none
    }
}
