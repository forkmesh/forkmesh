import FMApi
import FMBase
import Foundation

@MainActor
enum SurfacesFeature {
    static func opened(_ route: Route, title: String = "", detail: String = "") {
        SurfacesCoordinator.shared?.spotlight.opened(route, title: title, detail: detail)
    }

    static func openURL(_ url: URL) -> Bool {
        SurfaceRouter.openURL(url)
    }

    static func continueActivity(_ activity: NSUserActivity) -> Bool {
        SurfaceRouter.continueActivity(activity)
    }

    static func refresh() {
        SurfacesCoordinator.shared?.schedulePublish()
    }
}
