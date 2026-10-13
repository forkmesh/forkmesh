import Foundation

public enum PaletteCommand: Equatable, Sendable {
    case compose(ComposeTarget)
    case open(Route)
    case selectTab(AppTab)
    case switchProject
    case searchEverywhere(String)
}

public struct PaletteEntry: Identifiable, Equatable, Sendable {
    public let id: String
    public let title: String
    public let subtitle: String
    public let symbol: String
    public let command: PaletteCommand

    public init(id: String, title: String, subtitle: String = "", symbol: String, command: PaletteCommand) {
        self.id = id
        self.title = title
        self.subtitle = subtitle
        self.symbol = symbol
        self.command = command
    }
}

public struct PaletteSection: Identifiable, Equatable, Sendable {
    public let id: String
    public let title: String
    public let entries: [PaletteEntry]
}

public struct PaletteContext: Equatable, Sendable {
    public var target: ComposeTarget
    public var tab: AppTab
    public var pendingApprovals: Int
    public var recents: [Route]
    public var guest: Bool

    public init(target: ComposeTarget = ComposeTarget(), tab: AppTab = .agents, pendingApprovals: Int = 0,
                recents: [Route] = [], guest: Bool = false) {
        self.target = target
        self.tab = tab
        self.pendingApprovals = pendingApprovals
        self.recents = recents
        self.guest = guest
    }
}

public enum CommandPalette {
    public static let maxDestinations = 8

    public static func suggestions(_ context: PaletteContext) -> [PaletteEntry] {
        var entries: [PaletteEntry] = []
        let target = context.target
        if let channel = target.channel {
            entries.append(PaletteEntry(
                id: "compose", title: "Message \(channel)", symbol: "text.bubble", command: .compose(target)))
        } else if let repo = target.repo {
            entries.append(PaletteEntry(
                id: "compose", title: "New agent here", subtitle: repo.slug, symbol: "sparkles", command: .compose(target)))
        } else {
            entries.append(PaletteEntry(id: "compose", title: "New agent", symbol: "sparkles", command: .compose(target)))
        }
        if let repo = target.repo {
            entries.append(PaletteEntry(
                id: "newIssue", title: "New issue", subtitle: repo.slug, symbol: RouteCatalog.symbol(.newIssue),
                command: .open(.newIssue(repo))))
        }
        if context.pendingApprovals > 0 {
            let count = context.pendingApprovals
            entries.append(PaletteEntry(
                id: "approvals", title: "Review \(count) pending \(count == 1 ? "approval" : "approvals")",
                symbol: RouteCatalog.symbol(.approval), command: .open(.inbox(.approvals))))
        }
        entries.append(PaletteEntry(
            id: "running", title: "Running agents", symbol: "play.circle", command: .open(.agents(.running))))
        if !context.guest {
            entries.append(PaletteEntry(id: "newNote", title: "New note", symbol: "square.and.pencil", command: .open(.note(id: ""))))
        }
        entries.append(PaletteEntry(id: "timer", title: "Start a timer", symbol: "timer", command: .open(.timers)))
        entries.append(PaletteEntry(id: "switchProject", title: "Switch project", symbol: "square.stack.3d.up", command: .switchProject))
        return entries
    }

    public static var tabEntries: [PaletteEntry] {
        AppTab.barOrder.filter { PackageGate.allows(PackageGate.packageId(for: $0)) }.map { tab in
            PaletteEntry(id: "tab:\(tab.rawValue)", title: "Go to \(tab.title)", symbol: tab.symbol, command: .selectTab(tab))
        }
    }

    public static func destinationEntries() -> [PaletteEntry] {
        DestinationsSearchSource.defaultRoutes.filter { route in !AppTab.barOrder.contains { $0.root == route } }.map(entry)
    }

    public static func entry(_ route: Route) -> PaletteEntry {
        PaletteEntry(
            id: "route:\(route.kind.rawValue):\(route.title)", title: route.title,
            symbol: RouteCatalog.symbol(route.kind), command: .open(route))
    }

    public static func directEntries(_ query: String) -> [PaletteEntry] {
        let trimmed = query.trimmingCharacters(in: .whitespaces)
        var entries: [PaletteEntry] = []
        let parts = trimmed.split(separator: "#", maxSplits: 1, omittingEmptySubsequences: false).map(String.init)
        if let repo = RepoRef(slug: parts[0]) {
            if parts.count == 2, let number = Int(parts[1]), number > 0 {
                entries.append(entry(.issue(repo, number: number)))
                let pull = Route.pull(repo, number: number, tab: .conversation)
                entries.append(PaletteEntry(
                    id: "route:pull:\(repo.slug)#\(number)", title: "Pull request \(repo.slug) #\(number)",
                    symbol: RouteCatalog.symbol(.pull), command: .open(pull)))
            } else if parts.count == 1 {
                entries.append(entry(.repo(repo, section: nil)))
            }
        }
        if trimmed.hasPrefix("@"), trimmed.count > 1 {
            entries.append(entry(.profile(name: String(trimmed.dropFirst()))))
        }
        return entries
    }

    public static func sections(query: String, context: PaletteContext) -> [PaletteSection] {
        let installed = PackageGate.installed
        return unfilteredSections(query: query, context: context).compactMap { (section: PaletteSection) -> PaletteSection? in
            let entries = section.entries.filter { PackageGate.shows($0, installed: installed) }
            return entries.isEmpty ? nil : PaletteSection(id: section.id, title: section.title, entries: entries)
        }
    }

    static func unfilteredSections(query: String, context: PaletteContext) -> [PaletteSection] {
        let trimmed = query.trimmingCharacters(in: .whitespaces)
        if trimmed.isEmpty {
            var sections = [PaletteSection(id: "suggested", title: "Suggested", entries: suggestions(context))]
            if !context.recents.isEmpty {
                sections.append(PaletteSection(id: "recent", title: "Recent", entries: context.recents.map(entry)))
            }
            sections.append(PaletteSection(id: "tabs", title: "Tabs", entries: tabEntries))
            return sections
        }
        var sections: [PaletteSection] = []
        let direct = directEntries(trimmed)
        let actions = SearchMatch.rank(suggestions(context) + tabEntries, trimmed) { [$0.title, $0.subtitle] }
        if !(direct + actions).isEmpty {
            sections.append(PaletteSection(id: "actions", title: "Actions", entries: direct + actions))
        }
        let destinations = SearchMatch.rank(destinationEntries(), trimmed) { [$0.title] }
        if !destinations.isEmpty {
            sections.append(PaletteSection(
                id: "destinations", title: "Destinations", entries: Array(destinations.prefix(maxDestinations))))
        }
        sections.append(PaletteSection(id: "everywhere", title: "Search", entries: [
            PaletteEntry(
                id: "everywhere", title: "Search everywhere for “\(trimmed)”", symbol: "magnifyingglass",
                command: .searchEverywhere(trimmed)),
        ]))
        return sections
    }
}

public enum ShellQuickAction: String, CaseIterable, Sendable {
    case newAgent = "com.forkmesh.quick.new-agent"
    case inbox = "com.forkmesh.quick.inbox"
    case search = "com.forkmesh.quick.search"
    case newNote = "com.forkmesh.quick.new-note"
    case recentRepo = "com.forkmesh.quick.recent-repo"
    case recentAgent = "com.forkmesh.quick.recent-agent"

    public static let staticActions: [ShellQuickAction] = [.newAgent, .inbox, .search, .newNote]

    public static var availableActions: [ShellQuickAction] {
        staticActions.filter { $0.route().map { PackageGate.shows($0) } ?? true }
    }

    public var title: String {
        switch self {
        case .newAgent: return "New agent"
        case .inbox: return "Inbox"
        case .search: return "Search"
        case .newNote: return "New note"
        case .recentRepo: return "Repository"
        case .recentAgent: return "Agent"
        }
    }

    public var symbol: String {
        switch self {
        case .newAgent: return "sparkles"
        case .inbox: return "tray"
        case .search: return "magnifyingglass"
        case .newNote: return "square.and.pencil"
        case .recentRepo: return "folder"
        case .recentAgent: return "play.circle"
        }
    }

    public func route(payload: String = "") -> Route? {
        switch self {
        case .newAgent: return .compose(ComposeTarget())
        case .inbox: return .inbox(.all)
        case .search: return .search(query: "", scope: .everything)
        case .newNote: return .note(id: "")
        case .recentRepo: return RepoRef(slug: payload).map { .repo($0, section: nil) }
        case .recentAgent: return payload.isEmpty ? nil : .agent(id: payload)
        }
    }

    public static func dynamicItems(recents: [Route]) -> [(action: ShellQuickAction, title: String, payload: String)] {
        var items: [(action: ShellQuickAction, title: String, payload: String)] = []
        if let repo = recents.lazy.compactMap({ $0.repoRef }).first {
            items.append((.recentRepo, repo.slug, repo.slug))
        }
        if let agent = recents.lazy.compactMap({ route -> String? in
            if case let .agent(id) = route { return id }
            return nil
        }).first {
            items.append((.recentAgent, "Agent \(agent)", agent))
        }
        return items.filter { $0.action.route(payload: $0.payload).map { PackageGate.shows($0) } ?? true }
    }
}
