// WS16 (the desktop and the fleet from the phone: host operations, files, logs and health): this workstream's startup hook, called by
// AppServices.installDesktopRemote() (docs/native/DESKTOP_PARITY.md section 19). Only WS16
// edits this file. Gate 2 creates it empty.
//
// Register destinations here, e.g.
//     services.routes.register(.calendar, by: "WS16") { route in AnyView(CalendarScreen(route: route)) }

import FMApi
import SwiftUI

@MainActor
struct DesktopRemoteInstaller: AppServicesInstaller {
    func install(into services: AppServices) {
        DesktopRemoteFeature.register()
        services.routes.register(.desktops, by: "WS16") { _ in AnyView(DesktopsDestination()) }
        services.routes.register(.desktop, by: "WS16") { route in
            guard case .desktop(let id, let section) = route else { return AnyView(DesktopsDestination()) }
            return AnyView(DesktopDestination(id: id, section: section))
        }
        services.routes.register(.provision, by: "WS16") { route in
            guard case .provision(let id) = route else { return AnyView(FleetOperationsDestination()) }
            return AnyView(ProvisionDestination(id: id))
        }
        services.routes.register(.node, by: "WS16") { route in
            guard case .node(let name) = route else { return AnyView(FleetOperationsDestination()) }
            return AnyView(NodeDestination(name: name))
        }
    }
}
