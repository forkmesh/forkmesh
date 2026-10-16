import FMBase
import Foundation

public enum PingCelebration {
    public static let freshMillis = 5 * 60 * 1000

    public static func isCelebration(_ ping: ForkNotification) -> Bool {
        !ping.meta.str("celebrate").trimmingCharacters(in: .whitespaces).isEmpty
    }

    public static func badgeLine(_ ping: ForkNotification) -> String? {
        guard ping.meta.str("celebrate").trimmingCharacters(in: .whitespaces) == "badge" else { return nil }
        let name = ping.meta.str("badgeName").trimmingCharacters(in: .whitespaces)
        guard !name.isEmpty else { return "You earned a new badge!" }
        let tier = ping.meta.str("tier").trimmingCharacters(in: .whitespaces).lowercased()
        if ["bronze", "silver", "gold"].contains(tier) {
            return "You reached \(tier.prefix(1).uppercased() + tier.dropFirst()) \(name)!"
        }
        return "You earned \(name)!"
    }

    public static func fresh(_ pings: [ForkNotification], seen: Set<String>, now: Int) -> [ForkNotification] {
        pings.filter { ping in
            !ping.id.isEmpty && !seen.contains(ping.id) && ping.isUnread && isCelebration(ping)
                && abs(now - ping.ts) <= freshMillis
        }
    }
}
