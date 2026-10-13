import Foundation

public struct MarketingChannelAttention: Equatable, Sendable {
    public static let order = ["attention", "stale", "setup"]

    public var organization: String
    public var channel: MarketingChannelStats
    public var name: String

    public init(organization: String, channel: MarketingChannelStats, name: String) {
        self.organization = organization
        self.channel = channel
        self.name = name
    }

    public var title: String { channel.healthTitle }

    public var caption: String { "\(name): \(title.lowercased())" }

    public static func next(_ overviews: [MarketingOverview]) -> MarketingChannelAttention? {
        let pool = overviews.flatMap { overview in
            overview.channels
                .filter { order.contains($0.health) && (overview.setting($0.id)?.enabled ?? true) }
                .map { MarketingChannelAttention(organization: overview.organization, channel: $0, name: overview.channelName($0.id)) }
        }
        return pool.min { ranksAhead($0.channel, of: $1.channel) }
    }

    public static func ranksAhead(_ a: MarketingChannelStats, of b: MarketingChannelStats) -> Bool {
        let aRank = order.firstIndex(of: a.health) ?? order.count
        let bRank = order.firstIndex(of: b.health) ?? order.count
        if aRank != bRank { return aRank < bRank }
        if a.observedAt != b.observedAt { return a.observedAt < b.observedAt }
        return a.id < b.id
    }
}
