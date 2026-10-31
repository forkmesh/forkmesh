import UIKit

@MainActor
enum FmOrientationLock {
    private static var landscape = false
    private static var roomy = false

    static var mask: UIInterfaceOrientationMask {
        if UIDevice.current.userInterfaceIdiom != .phone { return .all }
        return landscape || roomy ? .allButUpsideDown : .portrait
    }

    static func allowLandscape(_ allowed: Bool) {
        guard landscape != allowed else { return }
        landscape = allowed
        refresh(portrait: !allowed && !roomy)
    }

    static func allowRoomy(_ allowed: Bool) {
        guard roomy != allowed else { return }
        roomy = allowed
        refresh(portrait: !allowed && !landscape)
    }

    private static func refresh(portrait: Bool) {
        guard UIDevice.current.userInterfaceIdiom == .phone else { return }
        for scene in UIApplication.shared.connectedScenes.compactMap({ $0 as? UIWindowScene }) {
            for window in scene.windows {
                var controller = window.rootViewController
                while let current = controller {
                    current.setNeedsUpdateOfSupportedInterfaceOrientations()
                    controller = current.presentedViewController
                }
            }
            if portrait {
                scene.requestGeometryUpdate(.iOS(interfaceOrientations: .portrait)) { _ in }
            }
        }
    }
}
