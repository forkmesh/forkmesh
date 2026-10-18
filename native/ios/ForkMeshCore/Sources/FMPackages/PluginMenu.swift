import FMApi
import FMBase
import Foundation

public enum PluginMenu {
    public static let buttonIdentifier = "plugin-more"

    public static let optionsLabel = "Rate, report or uninstall…"

    public static let feedbackLabel = "Send feedback or report a bug"

    public static let shareLabel = "Share…"

    public static let copyLinkLabel = "Copy link"

    public static let linkCopiedNotice = "Link copied"

    public static let shareHost = "forkmesh.com"

    public static let homeKinds: Set<Route.Kind> = [
        .teammates, .shortcuts, .notes, .tasks, .calendar, .timers, .desktops, .hosts, .network,
        .crm, .crmOrg, .marketing, .analytics, .competitors, .ledger, .vault, .coves,
        .calories, .steps, .water, .sleep, .sugarBreak, .stretch, .foodGuide, .speech, .glassesAlert, .phoneSecurity, .eyeControl, .ember, .strata, .synapse, .openWrtRouter, .fileExplorer,
        .faceChanger, .videoMixDjStudio, .appStoreStatus, .rssReader, .secureDrive, .spiderRoom, .workflows, .localAI, .activity, .logs, .diagnostics, .people, .docs, .stats, .status, .orgs,
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

    public static func name(catalog: String?, route: Route) -> String {
        if let catalog, !catalog.dartTrimmed.isEmpty { return catalog.dartTrimmed }
        if case .crmOrg = route { return Route.crm(pipeline: nil).title }
        return route.title
    }

    public static func rateLabel(_ name: String, rated: Bool) -> String {
        rated ? "Edit your rating of \(name)" : "Rate \(name)"
    }

    public static func shareURL(packageId: String) -> URL? {
        let id = packageId.dartTrimmed.lowercased()
        guard RouteParser.isPackageId(id) else { return nil }
        return URL(string: "https://\(shareHost)/market/\(id)")
    }

    public static func shareSubject(_ name: String) -> String {
        "\(name) on ForkMesh"
    }

    public static func shareMessage(_ name: String) -> String {
        "Get \(name) from the ForkMesh Market."
    }
}
