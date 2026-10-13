// The helpers of the Calorie Counter screen (days, numbers, meals,
// usernames) and the geometry of its three charts (ring, day chart,
// weight chart), so the SwiftUI Canvas code only strokes what these compute.

import FMApi
import FMBase
import Foundation

public enum CaloriesText {
    public static let kgPerLb = 0.45359237
    public static let cmPerIn = 2.54
    public static let meals = ["breakfast", "lunch", "dinner", "snack"]
    /// `_kindLabels`, in the order the challenge sheet offers them.
    public static let kinds: [(value: String, label: String)] = [
        ("weight_loss_percent", "Lose the most percent of body weight"),
        ("weight_loss_kg", "Lose the most weight"),
        ("calorie_streak", "Most days within the calorie goal"),
        ("log_streak", "Most days logged"),
    ]

    public static func kindLabel(_ kind: String) -> String { kinds.first { $0.value == kind }?.label ?? kind }

    static func calendar(_ timeZone: TimeZone) -> Calendar {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = timeZone
        return calendar
    }

    /// `_isoDay`: the local calendar day, "2026-09-14".
    public static func isoDay(_ date: Date, timeZone: TimeZone = .current) -> String {
        let c = calendar(timeZone).dateComponents([.year, .month, .day], from: date)
        let year = c.year ?? 1970, month = c.month ?? 1, day = c.day ?? 1
        let y = String(year)
        return "\(String(repeating: "0", count: max(0, 4 - y.count)))\(y)-\(month < 10 ? "0" : "")\(month)-\(day < 10 ? "0" : "")\(day)"
    }

    /// `DateTime.tryParse('${day}T00:00:00')`: local midnight of a "yyyy-MM-dd" day, nil when
    /// the text is not one.
    public static func tryParseDay(_ day: String, timeZone: TimeZone = .current) -> Date? {
        let parts = day.split(separator: "-", omittingEmptySubsequences: false)
        guard parts.count == 3, parts[0].count == 4, parts[1].count == 2, parts[2].count == 2,
              let y = Int(parts[0]), let m = Int(parts[1]), let d = Int(parts[2]) else { return nil }
        var components = DateComponents()
        components.year = y
        components.month = m
        components.day = d
        return calendar(timeZone).date(from: components)
    }

    /// `_parseDay`: today when the text is not a day.
    public static func parseDay(_ day: String, timeZone: TimeZone = .current, now: Date = Date()) -> Date {
        tryParseDay(day, timeZone: timeZone) ?? now
    }

    /// "Mon Sep 14".
    public static func dayLabel(_ day: String, timeZone: TimeZone = .current, now: Date = Date()) -> String {
        let months = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]
        let weekdays = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"]
        let c = calendar(timeZone).dateComponents([.weekday, .month, .day], from: parseDay(day, timeZone: timeZone, now: now))
        return "\(weekdays[(c.weekday ?? 1) - 1]) \(months[(c.month ?? 1) - 1]) \(c.day ?? 1)"
    }

    /// The day `days` after `day` ("yyyy-MM-dd"), for the challenge sheet's default end.
    public static func addDays(_ day: String, _ days: Int, timeZone: TimeZone = .current, now: Date = Date()) -> String {
        let start = parseDay(day, timeZone: timeZone, now: now)
        let moved = calendar(timeZone).date(byAdding: .day, value: days, to: start) ?? start
        return isoDay(moved, timeZone: timeZone)
    }

    /// `_num`: `value is num ? value.toDouble() : double.tryParse('$value') ?? 0`.
    public static func num(_ value: JSONValue?) -> Double {
        if let value, case .int(let i) = value { return Double(i) }
        if let value, case .double(let d) = value { return d }
        return DartCore.doubleTryParse(DartCore.toString(value)) ?? 0
    }

    /// `_num` of typed text.
    public static func num(_ text: String) -> Double { DartCore.doubleTryParse(text) ?? 0 }

    /// `_int`: `_num(value).round()`.
    public static func int(_ value: JSONValue?) -> Int { TelemetryMath.round(num(value)) }
    public static func int(_ text: String) -> Int { TelemetryMath.round(num(text)) }

    public static func kcal(_ value: JSONValue?) -> String { String(int(value)) }
    public static func kcal(_ value: Double) -> String { String(TelemetryMath.round(value)) }

    /// `_grams`: whole numbers bare, the rest to one decimal.
    public static func grams(_ value: JSONValue?) -> String { grams(num(value)) }

    public static func grams(_ number: Double) -> String {
        number == number.rounded() ? String(TelemetryMath.round(number)) : DartCore.toStringAsFixed(number, 1)
    }

    /// `_guessMeal` for the hour of the day.
    public static func guessMeal(hour: Int) -> String {
        if hour < 11 { return "breakfast" }
        if hour < 15 { return "lunch" }
        if hour < 21 { return "dinner" }
        return "snack"
    }

    public static func guessMeal(now: Date = Date(), timeZone: TimeZone = .current) -> String {
        guessMeal(hour: calendar(timeZone).component(.hour, from: now))
    }

    public static func cap(_ value: String) -> String { ToolsText.cap(value) }

    /// Typed ForkMesh usernames: comma or space separated, leading @ dropped, lower-cased, deduped.
    public static func usernames(_ text: String) -> [String] {
        var out = [String]()
        let separators = CharacterSet.whitespacesAndNewlines.union(CharacterSet(charactersIn: ","))
        for piece in text.components(separatedBy: separators) {
            let name = ToolsText.dropLeadingAts(piece).lowercased()
            if !name.isEmpty && !out.contains(name) { out.append(name) }
        }
        return out
    }

    /// The notice after inviting: who was invited and which names were skipped.
    public static func inviteNote(_ lead: String, _ result: JSONObject) -> String {
        func names(_ value: JSONValue?) -> [String] { (value?.arrayValue ?? []).map { DartCore.toString($0) } }
        let invited = names(result["invited"])
        let skipped = names(result["skipped"])
        var parts = [lead]
        if !invited.isEmpty { parts.append("Invited \(invited.joined(separator: ", ")).") }
        if !skipped.isEmpty { parts.append("Skipped \(skipped.joined(separator: ", ")).") }
        return parts.joined(separator: " ")
    }

    /// JavaScript's getTimezoneOffset convention, which the Worker's reminder sweep expects:
    /// minutes to add to local time to reach UTC.
    public static func tzOffsetMinutes(timeZone: TimeZone = .current, now: Date = Date()) -> Int {
        -(timeZone.secondsFromGMT(for: now) / 60)
    }

    /// "HH:mm".
    public static func clock(hour: Int, minute: Int) -> String {
        "\(hour < 10 ? "0" : "")\(hour):\(minute < 10 ? "0" : "")\(minute)"
    }

    /// A reminder's "HH:mm" as (hour, minute), 12:00 where it does not parse.
    public static func parseClock(_ text: String) -> (hour: Int, minute: Int) {
        let parts = text.split(separator: ":", omittingEmptySubsequences: false).map(String.init)
        return (DartCore.intTryParse(parts.first ?? "") ?? 12, DartCore.intTryParse(parts.last ?? "") ?? 0)
    }
}

/// A photo for `POST /api/calories/analyze`: small JPEG, PNG and WebP files travel as they are;
/// anything larger (or of another type) is re-encoded by the app at 800, 560 then 400 px wide as
/// PNG until it fits.
public enum CaloriesPhoto {
    public static let limit = 1_100_000
    public static let widths = [800, 560, 400]

    /// The data URL of an original file that may travel as it is, or nil when it must be resized.
    public static func dataURL(bytes: Data, fileExtension: String) -> String? {
        let kind: String
        switch fileExtension.lowercased() {
        case "jpg", "jpeg": kind = "jpeg"
        case "png": kind = "png"
        case "webp": kind = "webp"
        default: kind = ""
        }
        guard !kind.isEmpty, bytes.count <= limit else { return nil }
        return "data:image/\(kind);base64,\(B64.encode(bytes))"
    }

    /// The data URL of a re-encoded PNG, or nil when it is still too large.
    public static func pngDataURL(_ png: Data) -> String? {
        png.count <= limit ? "data:image/png;base64,\(B64.encode(png))" : nil
    }

    public static let receiptLimit = 1_600_000
    public static let receiptSides = [1600, 1280, 1024, 800]

    public static func receiptDataURL(jpeg: Data) -> String? {
        let url = "data:image/jpeg;base64,\(B64.encode(jpeg))"
        return url.utf8.count <= receiptLimit ? url : nil
    }
}

// MARK: - Chart geometry

/// `_RingPainter`: the share of the day's goal eaten.
public struct CaloriesRing: Equatable, Sendable {
    public let ratio: Double
    public let over: Bool

    public init(calories: Double, goal: Int) {
        ratio = goal > 0 ? min(max(calories / Double(goal), 0), 1) : 0
        over = goal > 0 && calories > Double(goal)
    }
}

/// `_DayChartPainter`: 30 bars against the goal line.
public struct CaloriesDayChart: Sendable {
    public struct Bar: Equatable, Sendable {
        public let x: Double
        public let y: Double
        public let width: Double
        public let height: Double
        public let over: Bool
    }

    public struct Label: Equatable, Sendable {
        public let text: String
        /// The label's box: left edge, top, width; `alignRight` right-aligns the text in it.
        public let x: Double
        public let y: Double
        public let width: Double
        public let alignRight: Bool
    }

    public static let left = 36.0, top = 8.0, bottom = 22.0, right = 6.0

    public let plot: (x: Double, y: Double, width: Double, height: Double)
    public let peak: Double
    public let gridLines: [Double]
    public let labels: [Label]
    public let bars: [Bar]
    /// The dashed goal line's y, nil without a goal.
    public let goalY: Double?

    public init(days: [JSONObject], goal: Int, width: Double, height: Double) {
        let plotX = CaloriesDayChart.left, plotY = CaloriesDayChart.top
        let plotW = width - CaloriesDayChart.right - plotX
        let plotH = height - CaloriesDayChart.bottom - plotY
        plot = (plotX, plotY, plotW, plotH)
        var peak = max(500.0, Double(goal) * 1.15)
        for day in days { peak = max(peak, CaloriesText.num(day["calories"])) }
        self.peak = peak
        let bottomY = plotY + plotH
        var grid = [Double]()
        var labels = [Label]()
        for step in 0...4 {
            let y = bottomY - plotH * Double(step) / 4
            grid.append(y)
            labels.append(Label(text: CaloriesText.kcal(peak * Double(step) / 4), x: 0, y: y - 6, width: CaloriesDayChart.left - 4, alignRight: true))
        }
        gridLines = grid
        var bars = [Bar]()
        if !days.isEmpty {
            let slot = plotW / Double(days.count)
            let barWidth = min(14.0, max(2.0, slot - 3))
            for (index, day) in days.enumerated() where CaloriesText.int(day["entries"]) != 0 {
                let calories = CaloriesText.num(day["calories"])
                let h = max(2.0, plotH * calories / peak)
                let x = plotX + Double(index) * slot + (slot - barWidth) / 2
                bars.append(Bar(x: x, y: bottomY - h, width: barWidth, height: h, over: goal > 0 && calories > Double(goal)))
            }
            let first = ToolsJSON.show(days.first?["day"])
            let last = ToolsJSON.show(days.last?["day"])
            labels.append(Label(text: String(first.dropFirst(5)), x: plotX, y: bottomY + 4, width: 60, alignRight: false))
            labels.append(Label(text: String(last.dropFirst(5)), x: plotX + plotW - 60, y: bottomY + 4, width: 60, alignRight: true))
        }
        self.bars = bars
        self.labels = labels
        goalY = goal > 0 && !days.isEmpty ? bottomY - plotH * Double(goal) / peak : nil
    }

    /// Dash segments of a horizontal dashed line across the plot: (fromX, toX).
    public var dashes: [(Double, Double)] {
        var out = [(Double, Double)]()
        var x = plot.x
        while x < plot.x + plot.width {
            out.append((x, min(x + 4, plot.x + plot.width)))
            x += 8
        }
        return out
    }
}

/// `_WeightChartPainter`: weigh-ins over time against the target.
public struct CaloriesWeightChart: Sendable {
    public static let left = 40.0, top = 8.0, bottom = 22.0, right = 8.0

    public let plot: (x: Double, y: Double, width: Double, height: Double)
    public let low: Double
    public let high: Double
    /// Grid rows: (y, label).
    public let grid: [(y: Double, label: String)]
    public let points: [(x: Double, y: Double)]
    public let targetY: Double?

    public init(points: [JSONObject], target: Double, imperial: Bool, width: Double, height: Double, timeZone: TimeZone = .current) {
        let plotX = CaloriesWeightChart.left, plotY = CaloriesWeightChart.top
        let plotW = width - CaloriesWeightChart.right - plotX
        let plotH = height - CaloriesWeightChart.bottom - plotY
        plot = (plotX, plotY, plotW, plotH)
        func display(_ kg: Double) -> Double { imperial ? kg / CaloriesText.kgPerLb : kg }
        guard !points.isEmpty else {
            low = 0
            high = 0
            grid = []
            self.points = []
            targetY = nil
            return
        }
        var low = Double.infinity, high = -Double.infinity
        for point in points {
            let value = display(CaloriesText.num(point["weightKg"]))
            low = min(low, value)
            high = max(high, value)
        }
        if target > 0 {
            low = min(low, display(target))
            high = max(high, display(target))
        }
        low = (low - 1).rounded(.down)
        high = (high + 1).rounded(.up)
        self.low = low
        self.high = high
        func dayMillis(_ point: JSONObject) -> Double {
            CaloriesText.parseDay(ToolsJSON.show(point["day"]), timeZone: timeZone).timeIntervalSince1970 * 1000
        }
        let first = dayMillis(points[0])
        let last = dayMillis(points[points.count - 1])
        let span = max(1, last - first)
        let bottomY = plotY + plotH
        func py(_ value: Double) -> Double { bottomY - plotH * (value - low) / (high - low) }
        grid = (0...3).map { step in
            let value = low + (high - low) * Double(step) / 3
            return (py(value), CaloriesText.grams(value))
        }
        self.points = points.map { point in
            (plotX + plotW * (dayMillis(point) - first) / span, py(display(CaloriesText.num(point["weightKg"]))))
        }
        targetY = target > 0 ? py(display(target)) : nil
    }

    public var dashes: [(Double, Double)] {
        var out = [(Double, Double)]()
        var x = plot.x
        while x < plot.x + plot.width {
            out.append((x, min(x + 4, plot.x + plot.width)))
            x += 8
        }
        return out
    }
}
