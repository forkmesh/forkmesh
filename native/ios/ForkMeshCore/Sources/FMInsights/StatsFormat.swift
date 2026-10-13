import Foundation

public enum StatsFormat {
    public static func compactNumber(_ value: Double) -> String {
        let magnitude = abs(value)
        if magnitude < 1000 {
            if abs(value - value.rounded()) < 0.05 { return grouped(Int64(value.rounded())) }
            return String(format: "%.1f", value)
        }
        if magnitude < 10_000 { return grouped(Int64(value.rounded())) }
        if magnitude < 1_000_000 {
            return String(format: magnitude < 100_000 ? "%.1fK" : "%.0fK", value / 1000)
        }
        if magnitude < 1_000_000_000 {
            return String(format: magnitude < 10_000_000 ? "%.2fM" : "%.1fM", value / 1_000_000)
        }
        return String(format: "%.1fB", value / 1_000_000_000)
    }

    public static func compactBytes(_ bytes: Int64) -> String {
        let units = ["B", "KB", "MB", "GB", "TB"]
        var value = Double(max(0, bytes))
        var unit = 0
        while value >= 1024, unit < 4 {
            value /= 1024
            unit += 1
        }
        if unit == 0 { return "\(Int64(value)) B" }
        return String(format: value < 10 ? "%.1f" : "%.0f", value) + " " + units[unit]
    }

    public static func compactUsd(_ value: Double) -> String {
        let magnitude = abs(value)
        let sign = value < 0 ? "-" : ""
        if magnitude < 100 { return sign + String(format: "$%.2f", magnitude) }
        return sign + "$" + compactNumber(magnitude)
    }

    public static func compactDuration(_ ms: Int64) -> String {
        let seconds = max(0, ms / 1000)
        if seconds < 60 { return "\(seconds)s" }
        let minutes = seconds / 60
        if minutes < 60 { return "\(minutes)m" }
        let hours = minutes / 60
        if hours < 48 { return "\(hours)h \(minutes % 60)m" }
        return "\(hours / 24)d \(hours % 24)h"
    }

    public static func trendPercent(_ values: [Double], categorical: Bool = false) -> Double? {
        let n = values.count
        if categorical || n < 4 { return nil }
        let half = n / 2
        var older = 0.0
        var newer = 0.0
        for i in 0..<half {
            older += values[i]
            newer += values[n - half + i]
        }
        older /= Double(half)
        newer /= Double(half)
        if abs(older) < 1e-9 { return nil }
        return 100 * (newer - older) / abs(older)
    }

    public static func trendText(_ percent: Double) -> String {
        let arrow = percent >= 0 ? "\u{25B2}" : "\u{25BC}"
        let magnitude = abs(percent)
        if magnitude >= 1000 { return arrow + " >999%" }
        return arrow + " " + String(format: magnitude < 10 ? "%.1f%%" : "%.0f%%", magnitude)
    }

    public static func percent(_ value: Double?, digits: Int = 1) -> String {
        guard let value else { return "–" }
        return String(format: "%.\(digits)f%%", value)
    }

    private static func grouped(_ value: Int64) -> String {
        let negative = value < 0
        let digits = String(negative ? -value : value)
        var out = ""
        for (index, char) in digits.enumerated() {
            if index > 0 && (digits.count - index) % 3 == 0 { out.append(",") }
            out.append(char)
        }
        return negative ? "-" + out : out
    }
}
