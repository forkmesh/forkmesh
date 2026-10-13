import FMBase
import Foundation

public enum PingCelebration {
    public static let freshMillis = 5 * 60 * 1000

    public static func isCelebration(_ ping: ForkNotification) -> Bool {
        !ping.meta.str("celebrate").trimmingCharacters(in: .whitespaces).isEmpty
    }

    public static func fresh(_ pings: [ForkNotification], seen: Set<String>, now: Int) -> [ForkNotification] {
        pings.filter { ping in
            !ping.id.isEmpty && !seen.contains(ping.id) && ping.isUnread && isCelebration(ping)
                && abs(now - ping.ts) <= freshMillis
        }
    }
}
