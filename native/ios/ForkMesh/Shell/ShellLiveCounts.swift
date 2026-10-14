import FMApi
import FMBase
import FMInsights
import Foundation

struct ShellLiveCounts: Equatable {
    var onlineCount: Int = 0
    var chatUnread: Int = 0
    var repos: Int?
    var hubCounts: [String: Int] = [:]
    var hubBadges: [String: Int] = [:]
    var hubActive: Set<String> = []

    static let interval: TimeInterval = 0.25

    @MainActor
    static func snapshot(
        chat: ShellChatBridge?, fleet: FleetLightsModel?, attention: RunnerAttentionModel?, guest: @escaping @MainActor () -> Bool
    ) -> ThrottledSnapshot<ShellLiveCounts> {
        ThrottledSnapshot<ShellLiveCounts>(interval: interval) {
            read(chat: chat, fleet: fleet, attention: attention, guest: guest())
        }
    }

    @MainActor
    static func read(chat: ShellChatBridge?, fleet: FleetLightsModel?, attention: RunnerAttentionModel?, guest: Bool) -> ShellLiveCounts {
        let navigator: ShellNavigatorCounts = ShellNavigatorCounts.shared
        var out: ShellLiveCounts = ShellLiveCounts()
        out.onlineCount = chat?.onlineCount ?? 0
        out.chatUnread = chat?.badgeUnread ?? 0
        out.repos = navigator.count(.repos)
        for group in HubGroups.current(guest: guest) {
            for item in group.items {
                guard case let .route(route) = item.action else { continue }
                let count: Int?
                switch route.kind {
                case .network: count = out.onlineCount
                case .hosts: count = fleet?.runners.count
                case .status: count = fleet?.watchedLights.count
                default: count = navigator.count(route.kind)
                }
                if let count { out.hubCounts[item.id] = count }
                let badge: Int
                switch route.kind {
                case .timers: badge = ShellCounts.shared.timers
                case .hosts: badge = attention?.items.count ?? 0
                case .status: badge = fleet?.watchedLights.filter { !$0.healthy }.count ?? 0
                default: badge = 0
                }
                if badge != 0 { out.hubBadges[item.id] = badge }
                if navigator.active(route.kind) { out.hubActive.insert(item.id) }
            }
        }
        return out
    }
}
