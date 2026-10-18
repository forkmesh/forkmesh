import FMApi
import Foundation

public enum LedgerSection: String, CaseIterable, Sendable {
    case records
    case rewards
    case mine

    public init(route: String?) {
        self = LedgerSection(rawValue: (route ?? "").lowercased()) ?? .records
    }

    public var title: String {
        switch self {
        case .records: return "Public ledger"
        case .rewards: return "Rewards"
        case .mine: return "My records"
        }
    }
}

public enum MarketingSection: String, CaseIterable, Sendable {
    case overview
    case plan
    case campaigns
    case channels
    case ads
    case growth
    case reddit
    case performance

    public init(route: String?) {
        let value = (route ?? "").lowercased()
        switch value {
        case "dashboard": self = .overview
        case "content": self = .plan
        case "ads-manager": self = .ads
        case "reddit-desk": self = .reddit
        default: self = MarketingSection(rawValue: value) ?? .overview
        }
    }

    public var title: String {
        switch self {
        case .overview: return "Overview"
        case .plan: return "Content"
        case .campaigns: return "Campaigns"
        case .channels: return "Channels"
        case .ads: return "Ads"
        case .growth: return "Growth"
        case .reddit: return "Reddit"
        case .performance: return "Performance"
        }
    }

    public var symbol: String {
        switch self {
        case .overview: return "square.grid.2x2"
        case .plan: return "calendar"
        case .campaigns: return "flag"
        case .channels: return "dot.radiowaves.left.and.right"
        case .ads: return "megaphone"
        case .growth: return "chart.line.uptrend.xyaxis"
        case .reddit: return "bubble.left.and.bubble.right"
        case .performance: return "chart.bar"
        }
    }
}

public enum PackageRoutes {
    public static func route(for package: MarketPackage) -> Route? {
        guard case .native(let destination) = package.phoneSurface else { return nil }
        switch destination {
        case "crm": return .crm(pipeline: nil)
        case "marketing": return .marketing(section: nil)
        case "analytics": return .analytics(section: nil)
        case "competitors": return .competitors
        case "ledger": return .ledger(section: nil)
        case "vault": return .vault(section: nil)
        case "calories": return .calories
        case "steps": return .steps
        case "water": return .water
        case "sleep": return .sleep
        case "sugarBreak": return .sugarBreak
        case "stretch": return .stretch
        case "foodGuide": return .foodGuide
        case "speech": return .speech
        case "glassesAlert": return .glassesAlert
        case "phoneSecurity": return .phoneSecurity
        case "eyeControl": return .eyeControl
        case "ember": return .ember(nil)
        case "strata": return .strata(nil)
        case "synapse": return .synapse(nil)
        case "openWrtRouter": return .openWrtRouter
        case "faceChanger": return .faceChanger
        case "videoMixDjStudio": return .videoMixDjStudio
        case "appStoreStatus": return .appStoreStatus
        case "rssReader": return .rssReader
        case "secureDrive": return .secureDrive
        case "spiderRoom": return .spiderRoom
        case "workflows": return .workflows
        case "people": return .people
        case "docs": return .docs
        case "notes": return .notes
        case "tasks": return .tasks(filter: nil)
        case "calendar": return .calendar(date: nil, mode: .month)
        case "timers": return .timers
        case "status": return .status
        case "tour": return .tour
        case "Agents": return .agents(.all)
        case "Inbox": return .inbox(.all)
        case "Chat": return .chat
        case "desktops": return .desktops
        case "teammates": return .teammates
        case "shortcuts": return .shortcuts(nil)
        case "hosts": return .hosts
        case "network": return .network(section: nil)
        case "coves": return .coves(nil)
        case "localAI": return .localAI
        case "activity": return .activity
        case "logs": return .logs
        case "diagnostics": return .diagnostics
        case "stats": return .stats
        case "orgs": return .orgs
        default: return nil
        }
    }

    public static func packageId(forHubItem id: String) -> String? {
        if MarketPackage.nativeDestinations[id] != nil { return id }
        return MarketPackage.nativeDestinations.first { $0.value == id }?.key
    }

    public static let businessPackageIds = ["crm", "marketing", "analytics", "competitor", "ledger"]

    public static func hubRoutes(installed: Set<String>, adReviewer: Bool = false) -> [(id: String, title: String, route: Route)] {
        var rows: [(id: String, title: String, route: Route)] = [("market", "Market", .market(filter: nil))]
        let entries: [(String, String, Route)] = [
            ("crm", "CRM", .crm(pipeline: nil)), ("marketing", "Marketing", .marketing(section: nil)),
            ("analytics", "Analytics", .analytics(section: nil)), ("competitor", "Competitors", .competitors),
        ]
        for (id, title, route) in entries where installed.contains(id) { rows.append((id, title, route)) }
        if adReviewer { rows.append(("ads", "Ad review", .ads)) }
        rows.append(("ledger", "Ledger", .ledger(section: nil)))
        return rows
    }

    public static func organization(fromPipeline pipeline: String?) -> (organization: String?, flow: String?) {
        guard let pipeline, !pipeline.isEmpty else { return (nil, nil) }
        let parts = pipeline.split(separator: "/", maxSplits: 1).map(String.init)
        if parts.count == 2, PackagesText.isOrgName(parts[0]) { return (parts[0], parts[1].isEmpty ? nil : parts[1]) }
        return (nil, pipeline)
    }
}
