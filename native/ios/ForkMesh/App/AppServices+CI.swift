import FMApi
import SwiftUI

@MainActor
struct CIInstaller: AppServicesInstaller {
    func install(into services: AppServices) {
        services.routes.register(.run, by: "WS14") { route in
            guard case let .run(repo, id) = route, let runId = Int(id), runId > 0 else {
                return AnyView(RoutePlaceholderView(route: route))
            }
            return AnyView(ActionRunDestination(repo: repo, runId: runId))
        }
        services.routes.register(.release, by: "WS14") { route in
            guard case let .release(repo, tag) = route, !tag.isEmpty else {
                return AnyView(RoutePlaceholderView(route: route))
            }
            return AnyView(ReleaseDestination(repo: repo, tag: tag))
        }
    }
}
