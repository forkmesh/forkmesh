// WS8 (the navigation shell: tab root, iPad split view, You hub, search, command palette, deep links (RouteParser)): this workstream's startup hook, called by
// AppServices.installNavigation() (docs/native/DESKTOP_PARITY.md section 19). Only WS8
// edits this file. Gate 2 creates it empty.
//
// Register destinations here, e.g.
//     services.routes.register(.calendar, by: "WS8") { route in AnyView(CalendarScreen(route: route)) }

import FMApi
import SwiftUI

@MainActor
struct NavigationInstaller: AppServicesInstaller {
    func install(into services: AppServices) {
        NavigationRoutes.register(into: services)
        _ = AppNavigator.install()
        ShellTour.configureTips()
    }
}
