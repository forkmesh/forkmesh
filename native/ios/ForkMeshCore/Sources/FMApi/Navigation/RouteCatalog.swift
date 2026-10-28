import Foundation

extension AppTab {
    public var title: String {
        switch self {
        case .agents: return "Agents"
        case .inbox: return "Inbox"
        case .code: return "Code"
        case .chat: return "Chat"
        }
    }

    public var symbol: String {
        switch self {
        case .agents: return "sparkles"
        case .inbox: return "tray"
        case .code: return "chevron.left.forwardslash.chevron.right"
        case .chat: return "bubble.left.and.bubble.right"
        }
    }

    public var root: Route {
        switch self {
        case .agents: return .agents(.all)
        case .inbox: return .inbox(.all)
        case .code: return .repos(.all)
        case .chat: return .chat
        }
    }

    public static let barOrder: [AppTab] = [.agents, .inbox, .code, .chat]
}

extension Route {
    public var repoRef: RepoRef? {
        switch self {
        case let .repo(repo, _), let .tree(repo, _, _), let .file(repo, _, _), let .fileHistory(repo, _, _),
            let .codeMap(repo, _), let .commits(repo, _), let .commit(repo, _), let .range(repo, _, _),
            let .compare(repo, _, _), let .conflicts(repo, _), let .issue(repo, _), let .newIssue(repo),
            let .board(repo), let .pull(repo, _, _), let .review(repo, _), let .discussion(repo, _),
            let .run(repo, _), let .release(repo, _), let .project(repo, _), let .milestones(repo),
            let .repoSettings(repo, _), let .launchedAgent(repo, _):
            return repo
        case let .coves(repo), let .shortcuts(repo), let .ember(repo), let .strata(repo), let .synapse(repo):
            return repo
        case let .compose(target):
            return target.repo
        default:
            return nil
        }
    }

    public var title: String {
        switch self {
        case let .agents(filter):
            switch filter {
            case .needsYou: return "Needs you"
            case .running: return "Running"
            case .queued: return "Queued"
            case .recent: return "Recent agents"
            case .desktop: return "Desktop agents"
            case .cloud: return "Cloud agents"
            case .all: return "Agents"
            }
        case let .inbox(filter):
            return filter == .all ? "Inbox" : filter.rawValue.capitalized
        case let .repo(repo, section):
            guard let section = section else { return repo.slug }
            return "\(RouteCatalog.sectionTitle(section)) · \(repo.slug)"
        case let .issue(repo, number): return "\(repo.slug) #\(number)"
        case let .pull(repo, number, _): return "\(repo.slug) #\(number)"
        case let .review(repo, number): return "Review \(repo.slug) #\(number)"
        case let .discussion(repo, number): return "\(repo.slug) #\(number)"
        case let .newIssue(repo): return "New issue · \(repo.slug)"
        case let .commit(_, sha): return "Commit \(String(sha.prefix(7)))"
        case let .file(_, _, path), let .tree(_, _, path):
            let name = path.split(separator: "/").last.map(String.init) ?? ""
            return name.isEmpty ? "Code" : name
        case let .profile(name): return "@\(name)"
        case let .search(query, _): return query.isEmpty ? "Search" : "Search “\(query)”"
        case let .channel(id): return id
        case let .dm(user): return "@\(user)"
        case let .org(name): return name
        default: return RouteCatalog.title(kind)
        }
    }
}

public enum RouteCatalog {
    public static func title(_ kind: Route.Kind) -> String {
        switch kind {
        case .agents: return "Agents"
        case .agent, .launchedAgent: return "Agent"
        case .approval: return "Approval"
        case .compose: return "New agent"
        case .agentLogin: return "Provider sign-in"
        case .agentWatch: return "Watch"
        case .allAgents: return "All agents"
        case .teammates: return "Special Agents"
        case .teammate: return "Special Agent"
        case .teammateEditor: return "Edit special agent"
        case .shortcuts: return "Shortcuts"
        case .inbox: return "Inbox"
        case .pingSettings: return "Ping settings"
        case .repos: return "Repositories"
        case .repo: return "Repository"
        case .tree: return "Code"
        case .file: return "File"
        case .fileHistory: return "File history"
        case .codeMap: return "Code map"
        case .commits: return "Commits"
        case .commit: return "Commit"
        case .range: return "Range"
        case .compare: return "Compare"
        case .conflicts: return "Conflicts"
        case .issue: return "Issue"
        case .newIssue: return "New issue"
        case .board: return "Board"
        case .pull: return "Pull request"
        case .review: return "Review"
        case .discussion: return "Discussion"
        case .run: return "Run"
        case .release: return "Release"
        case .project: return "Project"
        case .milestones: return "Milestones"
        case .repoSettings: return "Repository settings"
        case .localRepos: return "On this device"
        case .localRepo: return "Local repository"
        case .quickEdit: return "Quick edit"
        case .chat: return "Chat"
        case .channel: return "Channel"
        case .thread: return "Thread"
        case .dm: return "Direct message"
        case .discord: return "Discord"
        case .profile: return "Profile"
        case .people: return "People"
        case .docs: return "Docs"
        case .search: return "Search"
        case .notes: return "Notes"
        case .note: return "Note"
        case .tasks: return "Tasks"
        case .task: return "Task"
        case .calendar: return "Calendar"
        case .event: return "Event"
        case .timers: return "Timers"
        case .desktops: return "Desktops"
        case .desktop: return "Desktop"
        case .hosts: return "Hosts & fleet"
        case .host: return "Host"
        case .runners: return "Runners"
        case .provision: return "Provisioning"
        case .network: return "Network & nodes"
        case .node: return "Node"
        case .market: return "Market"
        case .package: return "Package"
        case .crm: return "CRM"
        case .contact: return "Contact"
        case .crmOrg: return "CRM organization"
        case .marketing: return "Marketing"
        case .analytics: return "Analytics"
        case .competitors: return "Competitors"
        case .invoices: return "Invoices"
        case .ads: return "Ad review"
        case .ledger: return "Ledger"
        case .vault: return "Vault"
        case .coves: return "Coves"
        case .cove: return "Cove"
        case .calories: return "Calories"
        case .steps: return "Steps"
        case .water: return "Water"
        case .sleep: return "Sleep"
        case .sugarBreak: return "Sugar Break"
        case .stretch: return "Stretch Break"
        case .foodGuide: return "Food Guide"
        case .speech: return "Speech"
        case .glassesAlert: return "Glasses Alert"
        case .phoneSecurity: return "Phone Security"
        case .eyeControl: return "Eye Control"
        case .ember: return "Ember"
        case .strata: return "Strata"
        case .synapse: return "Synapse"
        case .openWrtRouter: return "Router"
        case .fileExplorer: return "Files"
        case .nexus: return "NEXUS"
        case .faceChanger: return "Face Changer"
        case .videoMixDjStudio: return "Video Mix DJ Studio"
        case .poker: return "Poker"
        case .appStoreStatus: return "App Store Status"
        case .usage: return "Usage"
        case .rssReader: return "RSS Reader"
        case .instanceFeed: return "Feed"
        case .secureDrive: return "Secure Drive"
        case .spiderRoom: return "Spider Room"
        case .workflows: return "Workflows"
        case .localAI: return "On-device AI"
        case .activity: return "Activity"
        case .logs: return "Logs"
        case .diagnostics: return "Diagnostics"
        case .stats: return "Stats"
        case .status: return "Status"
        case .admin: return "Admin"
        case .settings: return "Settings"
        case .orgs: return "Organizations"
        case .org: return "Organization"
        case .projects: return "Projects"
        case .projectAccess: return "Agent access"
        case .tour: return "Tour"
        case .updateRequired: return "Update required"
        }
    }

    public static func symbol(_ kind: Route.Kind) -> String {
        switch kind {
        case .agents, .agent, .launchedAgent, .allAgents, .compose: return "sparkles"
        case .approval: return "checkmark.shield"
        case .agentLogin: return "terminal"
        case .agentWatch: return "waveform"
        case .teammates, .teammate, .teammateEditor: return "person.2"
        case .shortcuts: return "command"
        case .inbox: return "tray"
        case .pingSettings: return "bell.badge"
        case .repos, .repo, .localRepos, .localRepo: return "folder"
        case .tree, .file, .fileHistory, .codeMap, .quickEdit: return "doc.text"
        case .commits, .commit, .range: return "clock.arrow.circlepath"
        case .compare, .conflicts: return "arrow.triangle.branch"
        case .issue, .newIssue: return "smallcircle.filled.circle"
        case .board: return "rectangle.split.3x1"
        case .pull, .review: return "arrow.triangle.pull"
        case .discussion: return "text.bubble"
        case .run: return "play.circle"
        case .release: return "shippingbox"
        case .project, .milestones: return "flag"
        case .repoSettings, .settings: return "gearshape"
        case .chat, .channel, .thread: return "bubble.left.and.bubble.right"
        case .dm: return "envelope"
        case .discord: return "gamecontroller"
        case .profile: return "person.crop.circle"
        case .people: return "person.3"
        case .docs: return "book"
        case .search: return "magnifyingglass"
        case .notes, .note: return "note.text"
        case .tasks, .task: return "checkmark.circle"
        case .calendar, .event: return "calendar"
        case .timers: return "timer"
        case .desktops, .desktop: return "desktopcomputer"
        case .hosts, .host, .provision: return "server.rack"
        case .runners: return "externaldrive.connected.to.line.below"
        case .network, .node: return "network"
        case .market, .package: return "bag"
        case .crm, .contact, .crmOrg: return "person.crop.rectangle.stack"
        case .marketing: return "megaphone"
        case .analytics: return "chart.xyaxis.line"
        case .competitors: return "binoculars"
        case .invoices: return "receipt"
        case .ads: return "rectangle.and.text.magnifyingglass"
        case .ledger: return "creditcard"
        case .vault: return "key"
        case .coves, .cove: return "lock.shield"
        case .calories: return "fork.knife"
        case .steps: return "figure.walk"
        case .water: return "drop"
        case .sleep: return "moon.zzz"
        case .sugarBreak: return "birthday.cake"
        case .stretch: return "figure.flexibility"
        case .foodGuide: return "carrot"
        case .speech: return "speaker.wave.2"
        case .glassesAlert: return "eyeglasses"
        case .phoneSecurity: return "checkmark.shield"
        case .eyeControl: return "eye"
        case .ember: return "flame"
        case .strata: return "square.3.layers.3d"
        case .synapse: return "brain"
        case .openWrtRouter: return "wifi.router"
        case .fileExplorer: return "folder"
        case .nexus: return "network"
        case .faceChanger: return "theatermasks"
        case .videoMixDjStudio: return "music.note.list"
        case .poker: return "suit.spade.fill"
        case .appStoreStatus: return "app.badge"
        case .usage: return "gauge.with.dots.needle.33percent"
        case .rssReader: return "dot.radiowaves.up.forward"
        case .instanceFeed: return "bubble.left.and.bubble.right"
        case .secureDrive: return "lock.shield"
        case .spiderRoom: return "arkit"
        case .workflows: return "point.3.connected.trianglepath.dotted"
        case .localAI: return "cpu"
        case .activity: return "bolt"
        case .logs: return "text.alignleft"
        case .diagnostics: return "stethoscope"
        case .stats: return "chart.bar"
        case .status: return "waveform.path.ecg"
        case .admin: return "wrench.and.screwdriver"
        case .orgs, .org: return "building.2"
        case .projects, .projectAccess: return "square.stack.3d.up"
        case .tour: return "sparkles.rectangle.stack"
        case .updateRequired: return "arrow.down.app"
        }
    }

    public static func sectionTitle(_ section: RepoSection) -> String {
        switch section {
        case .pulls: return "Pull requests"
        case .quality: return "Quality"
        default: return section.rawValue.capitalized
        }
    }
}

public struct HubItem: Identifiable, Equatable, Sendable {
    public static let workflowsPath = "/dashboard/workflows"

    public enum Action: Equatable, Sendable {
        case route(Route)
        case world
        case designSystem
        case reportBug
        case setStatus
        case signIn
        case signOut
    }

    public let id: String
    public let title: String
    public let symbol: String
    public let action: Action
    public let guestAction: String?
    public let needsRegistration: Bool
    public let adminOnly: Bool

    public init(
        id: String, title: String, symbol: String, action: Action, guestAction: String? = nil,
        needsRegistration: Bool = false, adminOnly: Bool = false
    ) {
        self.id = id
        self.title = title
        self.symbol = symbol
        self.action = action
        self.guestAction = guestAction
        self.needsRegistration = needsRegistration
        self.adminOnly = adminOnly
    }

    public var route: Route? {
        if case let .route(route) = action { return route }
        return nil
    }

    static func destination(
        _ route: Route, title: String? = nil, guestAction: String? = nil, needsRegistration: Bool = false,
        adminOnly: Bool = false
    ) -> HubItem {
        HubItem(
            id: route.kind.rawValue, title: title ?? RouteCatalog.title(route.kind),
            symbol: RouteCatalog.symbol(route.kind), action: .route(route), guestAction: guestAction,
            needsRegistration: needsRegistration, adminOnly: adminOnly)
    }
}

public struct HubGroup: Identifiable, Equatable, Sendable {
    public let id: String
    public let title: String
    public let items: [HubItem]

    public init(id: String, title: String, items: [HubItem]) {
        self.id = id
        self.title = title
        self.items = items
    }
}

public enum HubCatalog {
    public static let maxRecents = 5

    public static let workspace: [HubItem] = [
        .destination(.notes, guestAction: "use notes"),
        .destination(.tasks(filter: nil), guestAction: "use tasks"),
        .destination(.calendar(date: nil, mode: .month)),
        .destination(.timers),
        .destination(.fileExplorer),
        .destination(.nexus),
    ]

    public static let operate: [HubItem] = [
        .destination(.desktops),
        .destination(.teammates),
        .destination(.shortcuts(nil)),
        .destination(.hosts, guestAction: "manage your hosts"),
        .destination(.runners, guestAction: "manage your runners"),
        .destination(.network(section: nil)),
        .destination(.logs),
        .destination(.diagnostics),
    ]

    public static let business: [HubItem] = [
        .destination(.market(filter: nil)),
        .destination(.crm(pipeline: nil), needsRegistration: true),
        .destination(.marketing(section: nil), needsRegistration: true),
        .destination(.analytics(section: nil), needsRegistration: true),
        .destination(.competitors, needsRegistration: true),
        .destination(.invoices(organization: nil, invoice: nil), needsRegistration: true),
        .destination(.ads, needsRegistration: true),
        .destination(.ledger(section: nil), needsRegistration: true),
    ]

    public static let personal: [HubItem] = [
        .destination(.vault(section: nil), guestAction: "open your password vault"),
        .destination(.coves(nil), needsRegistration: true),
        .destination(.calories),
        .destination(.steps),
        .destination(.water),
        .destination(.sleep),
        .destination(.sugarBreak),
        .destination(.stretch),
        .destination(.foodGuide),
        .destination(.speech),
        .destination(.glassesAlert),
        .destination(.phoneSecurity),
        .destination(.eyeControl),
        .destination(.openWrtRouter),
        .destination(.faceChanger),
        .destination(.videoMixDjStudio),
        .destination(.poker(table: nil)),
        .destination(.rssReader),
        .destination(.instanceFeed(post: nil)),
        .destination(.secureDrive),
        .destination(.spiderRoom),
        .destination(.localAI),
        .destination(.activity),
    ]

    public static let insights: [HubItem] = [
        .destination(.stats),
        .destination(.status),
        .destination(.appStoreStatus),
        .destination(.usage),
        .destination(.ember(nil)),
        .destination(.strata(nil)),
        .destination(.synapse(nil)),
        .destination(.admin(section: nil), needsRegistration: true, adminOnly: true),
    ]

    public static func app(guest: Bool) -> [HubItem] {
        var items: [HubItem] = [
            .destination(.settings(section: nil)),
            .destination(.orgs),
            .destination(.people),
            .destination(.docs),
            HubItem(id: "reportBug", title: "Report a bug", symbol: "ladybug", action: .reportBug),
            .destination(.tour),
            HubItem(id: "world", title: "World", symbol: "globe", action: .world),
            HubItem(id: "designSystem", title: "Design", symbol: "paintpalette", action: .designSystem),
            .destination(.workflows),
        ]
        if guest {
            items.append(HubItem(id: "signIn", title: "Sign in", symbol: "person.crop.circle.badge.plus", action: .signIn))
        } else {
            items.append(
                HubItem(id: "signOut", title: "Sign out", symbol: "rectangle.portrait.and.arrow.right", action: .signOut))
        }
        return items
    }

    public static func groups(guest: Bool, admin: Bool = false, isRegistered: (Route.Kind) -> Bool) -> [HubGroup] {
        let all: [HubGroup] = [
            HubGroup(id: "workspace", title: "Workspace", items: workspace),
            HubGroup(id: "operate", title: "Operate", items: operate),
            HubGroup(id: "business", title: "Business", items: business),
            HubGroup(id: "personal", title: "Personal", items: personal),
            HubGroup(id: "insights", title: "Insights", items: insights),
            HubGroup(id: "app", title: "App", items: app(guest: guest)),
        ]
        return all.compactMap { (group: HubGroup) -> HubGroup? in
            let items = group.items.filter { item in
                if item.adminOnly && !admin { return false }
                guard item.needsRegistration, let route = item.route else { return true }
                return isRegistered(route.kind)
            }
            return items.isEmpty ? nil : HubGroup(id: group.id, title: group.title, items: items)
        }
    }

    public static var routes: [Route] {
        (workspace + operate + business + personal + insights + app(guest: false)).compactMap(\.route)
    }

    public static func guestAction(for route: Route) -> String? {
        (workspace + operate + business + personal + insights).first { $0.route?.kind == route.kind }?.guestAction
    }
}
