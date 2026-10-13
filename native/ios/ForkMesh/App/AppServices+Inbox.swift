import FMApi
import SwiftUI

@MainActor
struct InboxInstaller: AppServicesInstaller {
    func install(into services: AppServices) {
        NotificationActions.register()
        _ = InboxLocalPings.log
        services.routes.register(.inbox, by: "WS9") { route in
            guard case let .inbox(filter) = route else {
                return AnyView(RoutePlaceholderView(route: route))
            }
            return AnyView(InboxScreen(filter: filter))
        }
    }
}
