import FMBase
import Foundation

public enum CrmSection: String, CaseIterable, Sendable, Identifiable {
    case home
    case contacts
    case pipeline
    case activity
    case more

    public var id: String { rawValue }

    public var title: String {
        switch self {
        case .home: return "Home"
        case .contacts: return "Contacts"
        case .pipeline: return "Pipeline"
        case .activity: return "Activity"
        case .more: return "More"
        }
    }

    public var icon: String {
        switch self {
        case .home: return "house"
        case .contacts: return "person.2"
        case .pipeline: return "rectangle.split.3x1"
        case .activity: return "clock.arrow.circlepath"
        case .more: return "ellipsis.circle"
        }
    }

    public static func of(_ tab: CrmTab) -> CrmSection {
        tab.isContacts ? .contacts : .activity
    }
}

public enum CrmDealState: Sendable {
    case open
    case won
    case ended

    public static func of(_ stage: String) -> CrmDealState {
        if CrmNextContact.wonStages.contains(stage) { return .won }
        if CrmNextContact.endedStages.contains(stage) { return .ended }
        return .open
    }
}

public struct CrmDashboard: Equatable, Sendable {
    public var contacts = 0
    public var newThisWeek = 0
    public var openDeals = 0
    public var openValue: Int64 = 0
    public var wonDeals = 0
    public var wonValue: Int64 = 0
    public var neverContacted = 0
    public var quiet = 0

    private static let dayMs: Int64 = 86_400_000

    public init(contacts rows: [CrmContact], now: Int64) {
        contacts = rows.count
        for contact in rows {
            if contact.createdAt > 0 && now - contact.createdAt <= 7 * Self.dayMs { newThisWeek += 1 }
            let state = CrmDealState.of(contact.stage)
            switch state {
            case .won:
                wonDeals += 1
                wonValue += contact.value
            case .open:
                if contact.value > 0 {
                    openDeals += 1
                    openValue += contact.value
                }
                if contact.lastContactedAt <= 0 {
                    neverContacted += 1
                } else if now - contact.lastContactedAt >= CrmNextContact.followUpDays * Self.dayMs {
                    quiet += 1
                }
            case .ended:
                break
            }
        }
    }

    public static func greeting(hour: Int) -> String {
        if hour < 5 || hour >= 18 { return "Good evening" }
        if hour < 12 { return "Good morning" }
        return "Good afternoon"
    }

    public static func pipelineFlow(_ data: CrmOrganizationData, active: CrmFlow?) -> CrmFlow? {
        if let active { return active }
        let counts = Dictionary(grouping: data.contacts, by: \.flow).mapValues(\.count)
        return data.flows.max { (counts[$0.id] ?? 0) < (counts[$1.id] ?? 0) }
    }
}
