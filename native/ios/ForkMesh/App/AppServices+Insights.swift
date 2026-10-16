// WS20 (stats, health, logs and the admin console): this workstream's startup hook, called by
// AppServices.installInsights() (docs/native/DESKTOP_PARITY.md section 19). Only WS20
// edits this file. Gate 2 creates it empty.
//
// Register destinations here, e.g.
//     services.routes.register(.calendar, by: "WS20") { route in AnyView(CalendarScreen(route: route)) }

import FMApi
import SwiftUI

@MainActor
struct InsightsInstaller: AppServicesInstaller {
    func install(into services: AppServices) {
        services.routes.register(.stats, by: "WS20") { _ in AnyView(StatsDestination()) }
        services.routes.register(.status, by: "WS20") { _ in AnyView(StatusDestination()) }
        services.routes.register(.network, by: "WS20") { route in
            guard case .network(let section) = route else { return AnyView(NetworkDestination(section: nil)) }
            return AnyView(NetworkDestination(section: section))
        }
        services.routes.register(.admin, by: "WS20") { route in
            guard case .admin(let section) = route else { return AnyView(AdminDestination(section: nil)) }
            return AnyView(AdminDestination(section: section))
        }
        services.routes.register(.activity, by: "WS20") { _ in AnyView(ActivityDestination()) }
        services.routes.register(.logs, by: "WS20") { _ in AnyView(TelemetryScreen(showLogs: true)) }
        services.routes.register(.diagnostics, by: "WS20") { _ in AnyView(TelemetryScreen()) }
        RepoSectionRegistry.register(.insights, by: "WS20") { repo in AnyView(RepoInsightsScreen(repo: repo)) }
        RepoSectionRegistry.register(.quality, by: "WS20") { repo in AnyView(RepoQualityScreen(repo: repo)) }
        #if canImport(MetricKit)
        MetricKitSink.shared.install(stallLog: services.monitor.stallLog)
        #endif
    }
}
