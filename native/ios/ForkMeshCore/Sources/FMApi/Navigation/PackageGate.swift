import Foundation

public enum PackageGate {
    public static let legacyCutoff = "2026-10-11"

    public static let organizationPackages: Set<String> = ["crm", "marketing", "analytics", "competitor"]

    public static let devicePackages: Set<String> = ["sign-in"]

    public static let desktopReliantPackages: Set<String> = [
        "agents", "desktops", "special-agents", "shortcuts", "hosts", "usage",
    ]

    public static let starterPackages: Set<String> = ["agents", "chat", "inbox", "code", "tour"]

    public static func isStarter(_ packageId: String, pack: StarterPack = .fallback) -> Bool {
        pack.packages.contains(packageId)
    }

    public static let items: [String: String] = [
        "Agents": "agents", "Inbox": "inbox", "Chat": "chat", "currentRepo": "code", "reportBug": "bug-reports",
        "repo-issues": "issues", "repo-commits": "commits", "repo-branches": "branches", "repo-pulls": "pull-requests",
        "repo-releases": "releases", "repo-actions": "actions", "repo-discussions": "discussions",
        "desktops": "desktops", "teammates": "special-agents", "shortcuts": "shortcuts", "hosts": "hosts",
        "runners": "runners", "network": "network", "coves": "coves", "localAI": "on-device-ai", "activity": "activity", "logs": "logs", "diagnostics": "diagnostics", "stats": "stats",
        "orgs": "organizations", "world": "world", "people": "people", "docs": "docs",
        "notes": "notes", "tasks": "tasks", "calendar": "calendar", "timers": "timers", "ledger": "ledger",
        "tour": "tour", "designSystem": "design-system", "status": "status-page", "speech": "speech",
        "glassesAlert": "glasses-alert", "phoneSecurity": "phone-security", "eyeControl": "eye-control", "ember": "ember", "strata": "strata", "synapse": "synapse", "openWrtRouter": "openwrt", "fileExplorer": "files", "faceChanger": "face-changer", "videoMixDjStudio": "video-mix-dj-studio", "appStoreStatus": "app-store-status", "usage": "usage", "rssReader": "rss-reader", "instanceFeed": "instance-feed", "secureDrive": "secure-drive", "spiderRoom": "spider-room", "vault": "vault", "calories": "calories", "steps": "steps", "water": "water",
        "sleep": "sleep", "sugarBreak": "sugar-break", "stretch": "stretch", "foodGuide": "food-guide",
        "signIn": "sign-in", "signOut": "sign-in", "workflows": "workflows",
        "crm": "crm", "marketing": "marketing", "analytics": "analytics", "competitor": "competitor",
        "competitors": "competitor",
    ]

    public static let kinds: [Route.Kind: String] = [
        .agents: "agents", .agent: "agents", .allAgents: "agents", .launchedAgent: "agents", .agentWatch: "agents",
        .teammates: "special-agents", .teammate: "special-agents", .teammateEditor: "special-agents",
        .shortcuts: "shortcuts", .inbox: "inbox",
        .tree: "code", .file: "code", .fileHistory: "code", .codeMap: "code",
        .commits: "commits", .commit: "commits", .range: "commits", .compare: "commits",
        .issue: "issues", .newIssue: "issues", .board: "issues",
        .pull: "pull-requests", .review: "pull-requests", .discussion: "discussions", .run: "actions",
        .release: "releases",
        .chat: "chat", .channel: "chat", .thread: "chat", .dm: "chat",
        .notes: "notes", .note: "notes", .tasks: "tasks", .task: "tasks", .calendar: "calendar", .event: "calendar",
        .timers: "timers", .desktops: "desktops", .desktop: "desktops", .hosts: "hosts", .host: "hosts",
        .runners: "runners",
        .provision: "hosts", .network: "network", .node: "network",
        .crm: "crm", .contact: "crm", .crmOrg: "crm", .marketing: "marketing", .analytics: "analytics",
        .competitors: "competitor", .ledger: "ledger",
        .vault: "vault", .coves: "coves", .cove: "coves", .calories: "calories", .steps: "steps", .water: "water",
        .sleep: "sleep", .sugarBreak: "sugar-break", .stretch: "stretch", .foodGuide: "food-guide", .speech: "speech",
        .glassesAlert: "glasses-alert", .phoneSecurity: "phone-security", .eyeControl: "eye-control", .ember: "ember", .strata: "strata", .synapse: "synapse", .openWrtRouter: "openwrt", .fileExplorer: "files", .faceChanger: "face-changer", .videoMixDjStudio: "video-mix-dj-studio", .appStoreStatus: "app-store-status", .usage: "usage", .rssReader: "rss-reader", .instanceFeed: "instance-feed", .secureDrive: "secure-drive", .spiderRoom: "spider-room", .workflows: "workflows", .localAI: "on-device-ai", .activity: "activity", .logs: "logs", .diagnostics: "diagnostics", .people: "people", .docs: "docs",
        .stats: "stats", .status: "status-page", .orgs: "organizations", .org: "organizations", .tour: "tour",
    ]

    public static let repoSections: [RepoSection: String] = [
        .code: "code", .issues: "issues", .pulls: "pull-requests", .discussions: "discussions", .actions: "actions",
        .agents: "agents", .releases: "releases", .branches: "branches", .commits: "commits",
    ]

    public static let tabs: [AppTab: String] = [.agents: "agents", .inbox: "inbox", .chat: "chat"]

    public static let searchBuckets: [SearchBucketKind: String] = [
        .commits: "commits", .issues: "issues", .pulls: "pull-requests", .code: "code", .chat: "chat",
    ]

    public static let packages: Set<String> = Set(items.values).union(kinds.values).union(repoSections.values)

    public static func isGated(_ packageId: String) -> Bool { packages.contains(packageId) }

    public static func packageId(forItem id: String) -> String? { items[id] }

    public static func reliesOnDesktop(item id: String) -> Bool {
        packageId(forItem: id).map(desktopReliantPackages.contains) ?? false
    }

    public static func packageId(for route: Route) -> String? {
        switch route {
        case let .repo(_, section):
            return section.flatMap { repoSections[$0] }
        case let .compose(target):
            return target.channel == nil ? "agents" : "chat"
        default:
            return kinds[route.kind]
        }
    }

    public static func packageId(for tab: AppTab) -> String? { tabs[tab] }

    public static func packageId(for command: PaletteCommand) -> String? {
        switch command {
        case let .open(route): return packageId(for: route)
        case let .compose(target): return target.channel == nil ? "agents" : "chat"
        case let .selectTab(tab): return packageId(for: tab)
        case .switchProject, .searchEverywhere: return nil
        }
    }

    private static let box = PackageGateBox()

    public static var installed: Set<String>? { box.value }

    public static func publish(_ installed: Set<String>?) { box.value = installed }

    public static func allows(_ packageId: String?, installed: Set<String>? = PackageGate.installed) -> Bool {
        guard let packageId, let installed else { return true }
        return installed.contains(packageId)
    }

    public static func shows(item id: String, route: Route? = nil, installed: Set<String>? = PackageGate.installed) -> Bool {
        allows(packageId(forItem: id), installed: installed) && allows(route.flatMap(packageId(for:)), installed: installed)
    }

    public static func shows(_ route: Route, installed: Set<String>? = PackageGate.installed) -> Bool {
        allows(packageId(for: route), installed: installed)
    }

    public static func shows(_ entry: PaletteEntry, installed: Set<String>? = PackageGate.installed) -> Bool {
        allows(packageId(for: entry.command), installed: installed)
    }

    public static func shows(_ item: HubItem, installed: Set<String>? = PackageGate.installed) -> Bool {
        shows(item: item.id, route: item.route, installed: installed)
    }

    public static func filter(_ sources: [GlobalSearchSource], installed: Set<String>? = PackageGate.installed) -> [GlobalSearchSource] {
        sources.filter { allows(searchBuckets[$0.kind], installed: installed) }
    }

    public static func filter(_ groups: [HubGroup], installed: Set<String>? = PackageGate.installed) -> [HubGroup] {
        guard installed != nil else { return groups }
        return groups.compactMap { (group: HubGroup) -> HubGroup? in
            let items = group.items.filter { shows($0, installed: installed) }
            return items.isEmpty ? nil : HubGroup(id: group.id, title: group.title, items: items)
        }
    }
}

final class PackageGateBox: @unchecked Sendable {
    private let lock = NSLock()
    private var stored: Set<String>?

    var value: Set<String>? {
        get {
            lock.lock()
            defer { lock.unlock() }
            return stored
        }
        set {
            lock.lock()
            stored = newValue
            lock.unlock()
        }
    }
}

public enum StarterPack: String, CaseIterable, Sendable {
    case minimal
    case standard
    case everything

    public static let fallback: StarterPack = .standard

    public static func fromName(_ name: String?) -> StarterPack {
        name.flatMap(StarterPack.init(rawValue:)) ?? fallback
    }

    public var packages: Set<String> {
        switch self {
        case .minimal: return ["code"]
        case .standard: return PackageGate.starterPackages
        case .everything: return PackageGate.packages.subtracting(PackageGate.organizationPackages)
        }
    }

    public var label: String {
        switch self {
        case .minimal: return "Minimal"
        case .standard: return "Standard"
        case .everything: return "Everything"
        }
    }

    public var summary: String {
        switch self {
        case .minimal: return "Just your code. No agents, chat or inbox; add more from the Market later."
        case .standard: return "Agents, Chat, Inbox, Code and the tour. A good place to start."
        case .everything: return "Every package on the phone, for people who want it all at once."
        }
    }
}
