import FMAccount
import FMBase
import Foundation

@MainActor
public enum PerformanceReport {
    public static let title = "ForkMesh iOS performance report"
    public static let maxChars = 4000
    public static let minChars = 1000
    public static let stallLogBytes = 2 * 1024
    public static let stallLogChars = 400
    public static let logLines = 10
    public static let lineChars = 120
    public static let keyChars = 90
    public static let recentMinutes = 10
    public static let recentEvents = 4
    public static let topRows = 6
    public static let busyWindow: TimeInterval = 300
    static let ranges: [TelemetryRange] = [.m5, .h1]
    static let httpMethods: Set<String> = ["GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"]

    public static func text(
        telemetry: AppTelemetry,
        context: [(String, String)] = [],
        metricKit: [String] = [],
        stallLog: String = "",
        now: Date = Date(),
        timeZone: TimeZone = .current,
        limit: Int = maxChars
    ) -> String {
        let monitor = telemetry.monitor
        let events = monitor?.events ?? []
        let minutes = Int(busyWindow / 60)
        let parts: [Part] = [
            Part("", header(telemetry, monitor, events, context, now, timeZone), rank: headerRank),
            Part("Metrics (5m | 1h)", metricTable(telemetry), rank: 10),
            Part("Stalls by name", eventGroups(events.filter(\.isStall), lastSeen: timeZone), rank: 9),
            Part("Stalls by screen", eventGroups(events.filter(\.isStall), lastSeen: timeZone, key: stallScreen), rank: 8),
            Part("Slow operations by name", eventGroups(events.filter { !$0.isStall }), rank: 8),
            Part("Main-actor work during stalls", stallSections(events), rank: 8),
            Part("In flight or finished during stalls", stallCulprits(events), rank: 7),
            Part("HTTP endpoints, last \(minutes) min", endpoints(telemetry.log, now), rank: 6),
            Part("MetricKit", metricKitLines(metricKit), rank: 8),
            Part("Newest slow events", recentEventLines(events, timeZone), rank: 5),
            Part("Busiest log lines, last \(minutes) min", busiestLines(telemetry.log, now), rank: 4),
            Part("Last \(recentMinutes) min per minute, oldest first (frame, loop: worst second)", minuteSeries(telemetry), rank: 1),
            Part("stalls.log tail", tailLines(stallLog, stallLogChars), rank: 3, keepsNewest: true),
            Part("App log, last \(logLines) lines", recentLog(telemetry.log, timeZone), rank: 2, keepsNewest: true),
            Part("Log volume", logVolume(telemetry.log), rank: 0),
        ]
        let budget = min(maxChars, max(minChars, limit))
        return fit(shrink(parts, budget), budget)
    }

    static let headerRank = Int.max

    struct Part {
        let title: String
        var lines: [String]
        let rank: Int
        let keepsNewest: Bool
        var trimmed = 0

        init(_ title: String, _ body: String, rank: Int, keepsNewest: Bool = false) {
            self.title = title
            self.lines = body.isEmpty ? [] : body.components(separatedBy: "\n")
            self.rank = rank
            self.keepsNewest = keepsNewest
        }

        mutating func dropOne() {
            if keepsNewest { lines.removeFirst() } else { lines.removeLast() }
            trimmed += 1
        }

        var text: String {
            var body = lines
            if trimmed > 0 {
                let note = "... \(trimmed) \(keepsNewest ? "older " : "")\(trimmed == 1 ? "line" : "lines") trimmed to fit"
                if keepsNewest { body.insert(note, at: 0) } else { body.append(note) }
            }
            let joined = body.joined(separator: "\n")
            return title.isEmpty ? joined : "## \(title)\n\(joined.isEmpty ? "none" : joined)"
        }
    }

    static func render(_ parts: [Part]) -> String {
        parts.map(\.text).joined(separator: "\n\n") + "\n"
    }

    static func shrink(_ parts: [Part], _ limit: Int) -> String {
        var parts = parts
        var text = render(parts)
        let order = parts.indices.filter { parts[$0].rank != headerRank }.sorted { parts[$0].rank < parts[$1].rank }
        for floor in [3, 1, 0] {
            for index in order {
                while text.unicodeScalars.count > limit && parts[index].lines.count > floor {
                    parts[index].dropOne()
                    text = render(parts)
                }
            }
        }
        return text
    }

    static func metricKitLines(_ reports: [String]) -> String {
        if reports.isEmpty { return "none" }
        return reports.prefix(2).flatMap { (report: String) -> [String] in
            report.split(separator: "\n").map { clip(String($0), 240) }
        }.joined(separator: "\n")
    }

    static func eventKey(_ event: PerformanceEvent) -> String {
        event.name == event.kind.rawValue ? event.name : "\(event.kind.rawValue) \(event.name)"
    }

    static func fit(_ text: String, _ limit: Int) -> String {
        if text.unicodeScalars.count <= limit { return text }
        let note = "\n[cut at \(limit) chars]\n"
        return String(text.unicodeScalars.prefix(limit - note.unicodeScalars.count)) + note
    }

    static func clip(_ text: String, _ limit: Int) -> String {
        let flat = text.replacingOccurrences(of: "\n", with: " ")
        return flat.count <= limit ? flat : String(flat.prefix(limit - 1)) + "…"
    }

    static func tailLines(_ text: String, _ limit: Int) -> String {
        var kept: [String] = []
        var used = 0
        for line in text.split(separator: "\n").reversed() {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if trimmed.isEmpty { continue }
            let piece = clip(trimmed, lineChars)
            if used + piece.count + 1 > limit { break }
            kept.append(piece)
            used += piece.count + 1
        }
        return kept.isEmpty ? "empty" : kept.reversed().joined(separator: "\n")
    }

    static func header(
        _ telemetry: AppTelemetry, _ monitor: PerformanceMonitorService?, _ events: [PerformanceEvent],
        _ context: [(String, String)], _ now: Date, _ timeZone: TimeZone
    ) -> String {
        var lines = ["# \(title)", "generated: \(DartDateFormat.iso8601(now, timeZone: timeZone)) (\(timeZone.identifier))"]
        for (key, value) in context { lines.append("\(key): \(clip(value, lineChars))") }
        lines.append(
            "uptime \(TelemetryFormat.uptime(telemetry.uptime)), sampling \(telemetry.running ? "on" : "off"), "
                + "requests in flight \(telemetry.requestsInFlight)")
        if let monitor {
            lines.append(
                "stalls since launch \(monitor.stallCount) (over \(Int(TelemetryCounters.millis(monitor.stallThreshold)))ms), "
                    + "slow ops over \(Int(TelemetryCounters.millis(monitor.threshold)))ms, events kept \(events.count)/\(monitor.maxEvents)")
        } else {
            lines.append("monitor: not wired")
        }
        return lines.joined(separator: "\n")
    }

    static func metricTable(_ telemetry: AppTelemetry) -> String {
        let lines = TelemetryMetric.allCases.compactMap { (metric: TelemetryMetric) -> String? in
            let summaries = ranges.map { telemetry.summary(metric, $0) }
            if summaries.allSatisfy({ $0.samples == 0 }) { return nil }
            let cells = summaries.map { (s: MetricSummary) -> String in
                if s.samples == 0 { return "-" }
                let first = metric.count ? "total \(metric.format(s.total))" : "avg \(metric.format(s.avg))"
                return "\(first) max \(metric.format(s.max))"
            }
            return "\(pad(metric.label, 6)) \(cells.joined(separator: " | "))"
        }
        return lines.isEmpty ? "no samples yet" : lines.joined(separator: "\n")
    }

    static func minuteSeries(_ telemetry: AppTelemetry) -> String {
        let lines = TelemetryMetric.allCases.compactMap { (metric: TelemetryMetric) -> String? in
            let values = telemetry.history(metric).lastMinutes(recentMinutes, peak: metric.peak)
            if values.allSatisfy({ $0.isNaN || $0 == 0 }) { return nil }
            return "\(pad(metric.label, 6)) \(values.map { cell(metric, $0) }.joined(separator: " "))"
        }
        return lines.isEmpty ? "nothing sampled yet" : lines.joined(separator: "\n")
    }

    static func cell(_ metric: TelemetryMetric, _ value: Double) -> String {
        value.isNaN ? "-" : metric.formatBare(value)
    }

    struct EventGroup {
        let key: String
        var count = 0
        var totalMs = 0
        var maxMs = 0
        var last = ""
    }

    static func stallScreen(_ event: PerformanceEvent) -> String {
        event.details["screen"].map(PerformanceLogText.dartText) ?? "not recorded"
    }

    static func eventGroups(
        _ events: [PerformanceEvent], lastSeen timeZone: TimeZone? = nil, key: (PerformanceEvent) -> String = eventKey
    ) -> String {
        var groups: [String: EventGroup] = [:]
        for event in events {
            let key = key(event)
            var group = groups[key] ?? EventGroup(key: key)
            if group.last.isEmpty, let timeZone {
                group.last = AppLogEntry(timestamp: event.timestamp, category: .perf, message: "").stamp(timeZone: timeZone)
            }
            group.count += 1
            group.totalMs += event.durationMs
            group.maxMs = max(group.maxMs, event.durationMs)
            groups[key] = group
        }
        let sorted = groups.values.sorted { ($0.totalMs, $1.key) > ($1.totalMs, $0.key) }
        var lines = sorted.prefix(topRows).map { (g: EventGroup) -> String in
            let when: [String] = g.last.isEmpty ? [] : ["last \(g.last)"]
            return row(["\(g.count)x", "total \(g.totalMs)ms", "max \(g.maxMs)ms"] + when + [clip(g.key, keyChars)])
        }
        if sorted.count > topRows { lines.append("... \(sorted.count - topRows) more") }
        return lines.joined(separator: "\n")
    }

    static func recentEventLines(_ events: [PerformanceEvent], _ timeZone: TimeZone) -> String {
        if events.isEmpty { return "none" }
        return events.prefix(recentEvents).map { (event: PerformanceEvent) -> String in
            let details = event.details.map { "\($0.key)=\(PerformanceLogText.dartText($0.value))" }
            let stamp = AppLogEntry(timestamp: event.timestamp, category: .perf, message: "").stamp(timeZone: timeZone)
            return clip(([stamp, "\(event.durationMs)ms", eventKey(event)] + details).joined(separator: " "), 160)
        }.joined(separator: "\n")
    }

    static func stallCulprits(_ events: [PerformanceEvent]) -> String {
        var counts: [String: Int] = [:]
        for event in events where event.isStall {
            for field in ["finishedDuring", "inFlight"] {
                guard let value = event.details[field] else { continue }
                for name in PerformanceLogText.dartText(value).components(separatedBy: ", ") where !name.isEmpty {
                    counts["\(field) \(name)", default: 0] += 1
                }
            }
        }
        return ranked(counts, limit: topRows)
    }

    static func stallSections(_ events: [PerformanceEvent]) -> String {
        var groups: [String: EventGroup] = [:]
        for event in events where event.isStall {
            guard let value = event.details["sections"] else { continue }
            for item in PerformanceLogText.dartText(value).components(separatedBy: ", ") {
                guard let space = item.lastIndex(of: " "), item.hasSuffix("ms"),
                    let ms = Int(item[item.index(after: space)...].dropLast(2))
                else { continue }
                let name = String(item[..<space])
                var group = groups[name] ?? EventGroup(key: name)
                group.count += 1
                group.totalMs += ms
                group.maxMs = max(group.maxMs, ms)
                groups[name] = group
            }
        }
        let sorted = groups.values.sorted { ($0.totalMs, $1.key) > ($1.totalMs, $0.key) }
        var lines = sorted.prefix(topRows).map { (g: EventGroup) -> String in
            row(["\(g.count)x", "total \(g.totalMs)ms", "max \(g.maxMs)ms", clip(g.key, keyChars)])
        }
        if sorted.count > topRows { lines.append("... \(sorted.count - topRows) more") }
        return lines.joined(separator: "\n")
    }

    struct Endpoint {
        var count = 0
        var failures = 0
        var totalMs = 0
        var maxMs = 0
    }

    static func endpoints(_ log: AppLog, _ now: Date) -> String {
        var table: [String: Endpoint] = [:]
        for entry in log.entries(categories: [.net, .error], since: now.addingTimeInterval(-busyWindow)) {
            guard let call = parseRequest(entry.message) else { continue }
            var e = table[call.key] ?? Endpoint()
            e.count += 1
            if entry.category == .error { e.failures += 1 }
            e.totalMs += call.ms
            e.maxMs = max(e.maxMs, call.ms)
            table[call.key] = e
        }
        let sorted = table.sorted { ($0.value.count, $0.value.totalMs, $1.key) > ($1.value.count, $1.value.totalMs, $0.key) }
        var lines = sorted.prefix(topRows).map { (item: (key: String, value: Endpoint)) -> String in
            let e = item.value
            return row(["\(e.count)x", "avg \(e.totalMs / max(1, e.count))ms", "max \(e.maxMs)ms", "failed \(e.failures)", clip(item.key, keyChars)])
        }
        if sorted.count > topRows { lines.append("... \(sorted.count - topRows) more endpoints") }
        return lines.joined(separator: "\n")
    }

    static func parseRequest(_ message: String) -> (key: String, ms: Int)? {
        let parts = message.split(separator: " ")
        guard parts.count >= 3, httpMethods.contains(String(parts[0])) else { return nil }
        for token in parts.dropFirst(2) {
            var text = Substring(token)
            if text.hasSuffix(":") { text = text.dropLast() }
            guard text.hasSuffix("ms"), let ms = Int(text.dropLast(2)) else { continue }
            return ("\(parts[0]) \(pattern(String(parts[1])))", ms)
        }
        return nil
    }

    static func busiestLines(_ log: AppLog, _ now: Date) -> String {
        var counts: [String: Int] = [:]
        for entry in log.entries(since: now.addingTimeInterval(-busyWindow)) where entry.category != .net {
            counts["\(entry.category.label) \(pattern(entry.message))", default: 0] += 1
        }
        return ranked(counts, limit: topRows)
    }

    static func logVolume(_ log: AppLog) -> String {
        let parts = LogCategory.allCases.map { "\($0.label) \(log.totalFor($0))" }
        return "\(parts.joined(separator: ", ")); total \(log.totalLines), ring keeps \(log.capacity)"
    }

    static func recentLog(_ log: AppLog, _ timeZone: TimeZone) -> String {
        log.entries().prefix(logLines).reversed().map { clip($0.text(timeZone: timeZone), lineChars) }.joined(separator: "\n")
    }

    static func ranked(_ counts: [String: Int], limit: Int) -> String {
        let sorted = counts.sorted { ($0.value, $1.key) > ($1.value, $0.key) }
        var lines = sorted.prefix(limit).map { "\(pad("\($0.value)x", 6)) \(clip($0.key, keyChars))" }
        if sorted.count > limit { lines.append("... \(sorted.count - limit) more") }
        return lines.joined(separator: "\n")
    }

    static func pattern(_ text: String) -> String {
        var out = ""
        var inDigits = false
        for ch in text.prefix(160) {
            if ch.isASCII && ch.isNumber {
                if !inDigits { out.append("#") }
                inDigits = true
            } else {
                out.append(ch)
                inDigits = false
            }
        }
        return out
    }

    static func row(_ cells: [String]) -> String {
        let widths = [6, 8, 9, 9, 9, 9, 9, 8]
        return cells.indices.map { (i: Int) -> String in
            i == cells.count - 1 ? cells[i] : pad(cells[i], i < widths.count ? widths[i] : 8)
        }.joined(separator: " ")
    }

    static func pad(_ text: String, _ width: Int) -> String {
        text.count >= width ? text : text + String(repeating: " ", count: width - text.count)
    }
}
