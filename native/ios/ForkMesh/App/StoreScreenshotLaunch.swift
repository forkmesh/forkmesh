import FMAccount
import FMApi
import FMPackages
import Foundation
import UIKit

@MainActor
enum StoreScreenshotLaunch {
    nonisolated static let onKey = "FMStoreShots"
    nonisolated static let routeKey = "FMStoreRoute"
    nonisolated static let tabKey = "FMStoreTab"
    nonisolated static let phoneLayoutKey = "FMStorePhoneLayout"

    nonisolated static var on: Bool {
        #if DEBUG
        return UserDefaults.standard.bool(forKey: onKey)
        #else
        return false
        #endif
    }

    static var pad: Bool {
        UIDevice.current.userInterfaceIdiom == .pad && !(on && UserDefaults.standard.bool(forKey: phoneLayoutKey))
    }

    static func prepare(auth: AuthService, settings: SettingsService?) {
        guard on else { return }
        UserDefaults.standard.set(true, forKey: ShellTour.seenKey)
        DataFolderModel.shared.dismissedThisLaunch = true
        if let settings {
            if settings.needsPermissionsIntro {
                settings.finishPermissionsIntro()
            }
            if settings.needsStarterPack {
                PhonePackages.chooseStarterPack(.standard, settings: settings)
                PackageGateSync.publish()
            }
        }
        if !auth.isAuthenticated && !auth.isGuest {
            auth.enterGuestMode()
        }
    }

    static func open(_ router: AppRouter) -> Bool {
        guard on else { return false }
        let defaults: UserDefaults = UserDefaults.standard
        if let text = defaults.string(forKey: routeKey), let url = URL(string: text),
            let link = RouteParser.parse(url)
        {
            router.show(stack: link.stack)
            return true
        }
        if let name = defaults.string(forKey: tabKey), let tab = AppTab(rawValue: name) {
            router.setPath([], for: tab)
            router.select(tab)
            return true
        }
        return false
    }
}
