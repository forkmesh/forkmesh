import Foundation

public enum ShellPaneLayout: Equatable, Sendable {
    case stack
    case split(list: Route?, detail: [Route])
    case full
}

public struct ShellPaneMetrics: Equatable, Sendable {
    public let rail: Bool
    public let wide: Bool
    public let list: Double

    public init(rail: Bool, wide: Bool, list: Double) {
        self.rail = rail
        self.wide = wide
        self.list = list
    }

    public static let phone = ShellPaneMetrics(rail: false, wide: false, list: 0)
}

public enum ShellPanes {
    public static let wideWidth: Double = 600
    public static let wideHeight: Double = 480
    public static let railWidth: Double = 800
    public static let railColumn: Double = 76
    public static let listMin: Double = 280
    public static let listMax: Double = 380
    public static let listShare: Double = 0.4

    public static let listKinds: Set<Route.Kind> = [
        .agents, .allAgents, .teammates, .shortcuts, .inbox, .repos, .localRepos, .people, .docs, .notes, .tasks,
        .calendar, .timers, .desktops, .hosts, .runners, .market, .crm, .marketing, .invoices, .coves, .workflows,
        .settings, .orgs, .projects, .rssReader, .logs,
    ]

    public static func roomy(width: Double, height: Double) -> Bool {
        min(width, height) >= wideWidth
    }

    public static func metrics(width: Double, height: Double, pad: Bool) -> ShellPaneMetrics {
        guard !pad, width >= wideWidth, height >= wideHeight else { return .phone }
        let rail: Bool = width >= railWidth
        let content: Double = rail ? width - railColumn : width
        return ShellPaneMetrics(rail: rail, wide: true, list: listWidth(content: content))
    }

    public static func listWidth(content: Double) -> Double {
        min(listMax, max(listMin, (content * listShare).rounded()))
    }

    public static func layout(path: [Route], tab: AppTab, wide: Bool) -> ShellPaneLayout {
        guard wide, tab != .chat else { return .stack }
        guard let first = path.first, first.homeTab != tab, first != tab.root else {
            return .split(list: nil, detail: path)
        }
        if listKinds.contains(first.kind) {
            return .split(list: first, detail: Array(path.dropFirst()))
        }
        return .full
    }

    public static func listPrefix(_ layout: ShellPaneLayout) -> [Route] {
        guard case let .split(list, _) = layout, let list else { return [] }
        return [list]
    }

    public static func picked(_ pushed: [Route], layout: ShellPaneLayout) -> [Route]? {
        guard !pushed.isEmpty else { return nil }
        return listPrefix(layout) + pushed
    }

    public static func detailPath(_ next: [Route], layout: ShellPaneLayout) -> [Route] {
        listPrefix(layout) + next
    }
}
