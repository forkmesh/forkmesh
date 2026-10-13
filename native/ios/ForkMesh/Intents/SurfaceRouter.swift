import CoreSpotlight
import FMApi
import FMSurfaces
import Foundation

@MainActor
enum SurfaceRouter {
    static func open(_ route: Route) {
        if case let .compose(target) = route, let compose = NotificationActions.onCompose {
            compose(target)
            return
        }
        NotificationActions.open(route: route, href: "")
    }

    static func openURL(_ url: URL) -> Bool {
        guard SurfaceLink.isSurfaceLink(url) else { return false }
        if let route = SurfaceLink.route(from: url) {
            open(route)
        }
        return true
    }

    static func continueActivity(_ activity: NSUserActivity) -> Bool {
        guard activity.activityType == CSSearchableItemActionType,
              let identifier = activity.userInfo?[CSSearchableItemActivityIdentifier] as? String,
              let route = SurfaceEntityRef.fromSpotlightIdentifier(identifier)?.route
        else { return false }
        open(route)
        return true
    }
}
