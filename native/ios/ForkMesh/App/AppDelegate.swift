// The UIKit half the SwiftUI lifecycle cannot do: APNs registration callbacks.
// The notification-center delegate is installed at launch, before any tap that
// cold-launched the app is delivered.

import FMApi
import Foundation
import UIKit

@MainActor
final class AppDelegate: NSObject, UIApplicationDelegate {
    /// Built before `didFinishLaunching` returns so the notification-center
    /// delegate is in place for a tap that launched the app.
    let pushBridge: ApplePushBridge = ApplePushBridge()

    func application(
        _ application: UIApplication,
        didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil
    ) -> Bool {
        HiddenUnicodeInputGuard.install()
        return true
    }

    func application(_ application: UIApplication, didRegisterForRemoteNotificationsWithDeviceToken deviceToken: Data) {
        pushBridge.didRegister(deviceToken: deviceToken)
    }

    func application(
        _ application: UIApplication, configurationForConnecting connectingSceneSession: UISceneSession,
        options: UIScene.ConnectionOptions
    ) -> UISceneConfiguration {
        let configuration = UISceneConfiguration(name: nil, sessionRole: connectingSceneSession.role)
        configuration.delegateClass = ShellSceneDelegate.self
        return configuration
    }

    func application(_ application: UIApplication, supportedInterfaceOrientationsFor window: UIWindow?) -> UIInterfaceOrientationMask {
        FmOrientationLock.mask
    }

    func application(_ application: UIApplication, didFailToRegisterForRemoteNotificationsWithError error: Error) {
        pushBridge.didFailToRegister(error: error)
    }
}
