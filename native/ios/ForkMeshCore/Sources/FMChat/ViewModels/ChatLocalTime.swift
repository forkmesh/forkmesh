import Foundation

public enum ChatLocalTime {
    public static func zone(profile: String, presence: String) -> TimeZone? {
        for identifier in [profile, presence] where RelayService.validTimeZone(identifier) {
            if let zone = TimeZone(identifier: identifier) { return zone }
        }
        return nil
    }

    public static func time(_ date: Date, in zone: TimeZone, locale: Locale = .current) -> String {
        let formatter = DateFormatter()
        formatter.locale = locale
        formatter.timeZone = zone
        formatter.dateStyle = .none
        formatter.timeStyle = .short
        return formatter.string(from: date)
    }

    public static func difference(_ zone: TimeZone, from local: TimeZone, at date: Date) -> String {
        let minutes = (zone.secondsFromGMT(for: date) - local.secondsFromGMT(for: date)) / 60
        if minutes == 0 { return "Same time as you" }
        let hours = abs(minutes) / 60
        let rest = abs(minutes) % 60
        var amount = hours == 0 ? "\(rest) minutes" : "\(hours)"
        if hours > 0 {
            if rest == 30 { amount += "\u{00BD}" } else if rest > 0 { amount += ":\(String(format: "%02d", rest))" }
            amount += hours == 1 && rest == 0 ? " hour" : " hours"
        }
        return "\(amount) \(minutes > 0 ? "ahead of" : "behind") you"
    }

    public static func place(_ zone: TimeZone) -> String {
        let city = zone.identifier.split(separator: "/").last.map(String.init) ?? zone.identifier
        return city.replacingOccurrences(of: "_", with: " ")
    }
}
