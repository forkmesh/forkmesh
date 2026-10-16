import UIKit

@MainActor
enum FmOrientationLock {
    private static var landscape = false

    static var mask: UIInterfaceOrientationMask {
        if UIDevice.current.userInterfaceIdiom != .phone { return .all }
        return landscape ? .allButUpsideDown : .portrait
    }

    static func allowLandscape(_ allowed: Bool) {
        guard landscape != allowed else { return }
        landscape = allowed
        guard UIDevice.current.userInterfaceIdiom == .phone else { return }
        for scene in UIApplication.shared.connectedScenes.compactMap({ $0 as? UIWindowScene }) {
            for window in scene.windows {
                var controller = window.rootViewController
                while let current = controller {
                    current.setNeedsUpdateOfSupportedInterfaceOrientations()
                    controller = current.presentedViewController
                }
            }
            if !allowed {
                scene.requestGeometryUpdate(.iOS(interfaceOrientations: .portrait)) { _ in }
            }
        }
    }
}
