import FMAccount
import FMApi
import FMPackages
import Foundation
import SwiftUI
import UIKit

@MainActor
enum StoreScreenshotLaunch {
    nonisolated static let onKey = "FMStoreShots"
    nonisolated static let routeKey = "FMStoreRoute"
    nonisolated static let tabKey = "FMStoreTab"
    nonisolated static let phoneLayoutKey = "FMStorePhoneLayout"
    nonisolated static let canvasKey = "FMStoreCanvas"
    static let canvasTop: CGFloat = 24
    static let canvasBottom: CGFloat = 20

    nonisolated static var on: Bool {
        #if DEBUG
        return UserDefaults.standard.bool(forKey: onKey)
        #else
        return false
        #endif
    }

    static var canvas: CGSize? {
        guard on, let text = UserDefaults.standard.string(forKey: canvasKey) else { return nil }
        let parts: [Double] = text.lowercased().split(separator: "x").compactMap { Double($0) }
        guard parts.count == 2, parts[0] > 0, parts[1] > 0 else { return nil }
        return CGSize(width: parts[0], height: parts[1])
    }

    static func turnForCanvas() {
        guard let size = canvas else { return }
        let turn: UIInterfaceOrientationMask = size.width > size.height ? .landscapeRight : .portrait
        for case let scene as UIWindowScene in UIApplication.shared.connectedScenes {
            scene.requestGeometryUpdate(.iOS(interfaceOrientations: turn)) { (_: Error) in }
        }
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

struct StoreCanvas: ViewModifier {
    func body(content: Content) -> some View {
        if let size = StoreScreenshotLaunch.canvas {
            content
                .safeAreaPadding(.top, StoreScreenshotLaunch.canvasTop)
                .safeAreaPadding(.bottom, StoreScreenshotLaunch.canvasBottom)
                .frame(width: size.width, height: size.height)
                .clipped()
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
                .ignoresSafeArea()
                .statusBarHidden(true)
                .onAppear { StoreScreenshotLaunch.turnForCanvas() }
        } else {
            content
        }
    }
}
