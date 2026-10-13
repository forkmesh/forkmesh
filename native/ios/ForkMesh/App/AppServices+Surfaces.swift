// WS12 (widgets, Live Activities, App Intents, Spotlight, the share extension): this workstream's startup hook, called by
// AppServices.installSurfaces() (docs/native/DESKTOP_PARITY.md section 19). Only WS12
// edits this file. Gate 2 creates it empty.
//
// Register destinations here, e.g.
//     services.routes.register(.calendar, by: "WS12") { route in AnyView(CalendarScreen(route: route)) }

import FMBase
import SwiftUI

@MainActor
struct SurfacesInstaller: AppServicesInstaller {
    func install(into services: AppServices) {
        if Services.get(SurfacesCoordinator.self) != nil { return }
        let coordinator: SurfacesCoordinator = SurfacesCoordinator(services: services)
        Services.put(coordinator)
        coordinator.start()
        ForkMeshShortcuts.updateAppShortcutParameters()
    }
}
