// WS17 (teammates): this workstream's startup hook, called by
// AppServices.installTeammates() (docs/native/DESKTOP_PARITY.md section 19). Only WS17
// edits this file. Gate 2 creates it empty.
//
// Register destinations here, e.g.
//     services.routes.register(.calendar, by: "WS17") { route in AnyView(CalendarScreen(route: route)) }

import FMApi
import SwiftUI

@MainActor
struct TeammatesInstaller: AppServicesInstaller {
    func install(into services: AppServices) {
        TeammatesFeature.register()
        services.routes.register(.teammates, by: "WS17") { _ in AnyView(TeammatesDestination()) }
        services.routes.register(.teammate, by: "WS17") { route in
            guard case .teammate(let id) = route else { return AnyView(TeammatesDestination()) }
            return AnyView(TeammateDestination(id: id))
        }
        services.routes.register(.teammateEditor, by: "WS17") { route in
            guard case .teammateEditor(let id) = route else { return AnyView(TeammatesDestination()) }
            return AnyView(TeammateEditorDestination(id: id))
        }
        services.routes.register(.shortcuts, by: "WS17") { route in
            guard case .shortcuts(let repo) = route else { return AnyView(ShortcutsDestination(repo: nil)) }
            return AnyView(ShortcutsDestination(repo: repo))
        }
    }
}
