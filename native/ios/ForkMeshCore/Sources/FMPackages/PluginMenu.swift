import FMApi
import FMBase
import Foundation

public enum PluginMenu {
    public enum Item: String, CaseIterable, Sendable {
        case rate
        case feedback
        case uninstall
    }

    public static let buttonIdentifier = "plugin-more"

    public static let feedbackLabel = "Send feedback or report a bug"

    public static let homeKinds: Set<Route.Kind> = [
        .teammates, .shortcuts, .notes, .tasks, .calendar, .timers, .desktops, .hosts, .network,
        .crm, .crmOrg, .marketing, .analytics, .competitors, .ledger, .vault, .coves,
        .calories, .steps, .water, .sleep, .sugarBreak, .stretch, .foodGuide, .speech, .glassesAlert,
        .faceChanger, .appStoreStatus, .rssReader, .localAI, .activity, .logs, .diagnostics, .people, .docs, .stats, .status, .orgs,
    ]

    public static func packageId(for route: Route) -> String? {
        guard homeKinds.contains(route.kind) else { return nil }
        return PackageGate.packageId(for: route)
    }

    public static func screenOwnsMenu(_ route: Route) -> Bool {
        switch route {
        case .crm, .crmOrg, .teammates: return true
        case let .coves(repo): return repo != nil
        default: return false
        }
    }

    public static func addsButton(for route: Route) -> Bool {
        packageId(for: route) != nil && !screenOwnsMenu(route)
    }

    public static func items(canUninstall: Bool) -> [Item] {
        canUninstall ? [.rate, .feedback, .uninstall] : [.rate, .feedback]
    }

    public static func name(catalog: String?, route: Route) -> String {
        if let catalog, !catalog.dartTrimmed.isEmpty { return catalog.dartTrimmed }
        if case .crmOrg = route { return Route.crm(pipeline: nil).title }
        return route.title
    }

    public static func rateLabel(_ name: String, rated: Bool) -> String {
        rated ? "Edit your rating of \(name)" : "Rate \(name)"
    }
}
