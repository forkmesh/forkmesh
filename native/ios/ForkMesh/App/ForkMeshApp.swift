// The app root: the theme from the appearance settings, the relay-switch scope
// above every route, and the home: the auth flow (with "Explore as guest")
// until the app has access, then the shell. The home is rebuilt from scratch
// whenever the project changes (`ProjectRuntime.generation`), so no screen
// carries one project's state into another.

import FMAccount
import FMApi
import FMBase
import CoreSpotlight
import SwiftUI

@main
struct ForkMeshApp: App {
    @UIApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @Environment(\.scenePhase) var scenePhase
    @State var boot: AppBoot = AppBoot.shared

    var body: some Scene {
        WindowGroup {
            AppRootView(boot: boot, pushBridge: appDelegate.pushBridge)
                .onOpenURL { (url: URL) in
                    AppServices.current?.openURL(url)
                }
                .onContinueUserActivity(CSSearchableItemActionType) { (activity: NSUserActivity) in
                    _ = SurfacesFeature.continueActivity(activity)
                }
                .onContinueUserActivity(VaultCredentialExchange.activityType) { (activity: NSUserActivity) in
                    VaultCredentialExchange.receive(activity)
                }
        }
        .onChange(of: scenePhase) { (_: ScenePhase, phase: ScenePhase) in
            AppLifecycle.shared.scenePhaseChanged(phase)
        }
    }
}

/// The launch background until the services exist, then the themed app.
struct AppRootView: View {
    let boot: AppBoot
    let pushBridge: ApplePushBridge

    var body: some View {
        ZStack {
            Color("LaunchBackground")
                .ignoresSafeArea()
            if let services = boot.services {
                ThemedAppView(services: services)
            }
        }
        .fmTracksUserActivity()
        .fmDismissesKeyboardOnTap()
        .task {
            await boot.start(pushBridge: pushBridge)
        }
    }
}

/// Only the appearance choices rebuild the theme; watching the whole settings
/// service would redraw every route on each keystroke in a Settings field.
struct ThemedAppView: View {
    @Environment(\.scenePhase) var scenePhase
    let services: AppServices

    private var forcedDark: Bool? {
        switch services.settings.themeMode {
        case .system: return nil
        case .light: return false
        case .dark: return true
        }
    }

    private var instanceThemeOrigin: String? {
        let settings: SettingsService = services.settings
        guard settings.matchesInstanceTheme, scenePhase == .active else { return nil }
        return settings.relays.active.httpOrigin
    }

    private func refreshInstanceTheme(_ origin: String?) async {
        guard let origin else { return }
        let settings: SettingsService = services.settings
        if origin.isEmpty {
            settings.setInstanceThemeColors("")
            return
        }
        guard let colors = await InstanceTheme.fetch(http: services.http, origin: origin) else { return }
        if settings.matchesInstanceTheme, settings.relays.active.httpOrigin == origin {
            settings.setInstanceThemeColors(colors)
        }
    }

    var body: some View {
        let settings: SettingsService = services.settings
        ForkMeshTheme(theme: settings.effectiveTheme, wallpaper: !settings.backgroundImage.isEmpty, dark: forcedDark) {
            AppHomeView(auth: services.auth)
                .signInPresenter(services.signIn)
                .background { FmWallpaperBackdrop(name: settings.backgroundImage, dim: settings.backgroundDim) }
                .background { CaptureHoldAnchor().accessibilityHidden(true) }
        }
        .preferredColorScheme(forcedDark.map { (dark: Bool) -> ColorScheme in dark ? .dark : .light })
        .task(id: instanceThemeOrigin) { await refreshInstanceTheme(instanceThemeOrigin) }
    }
}

/// The home: guest mode opens the same shell as a session, so
/// signing in later swaps the session in underneath without rebuilding it.
struct AppHomeView: View {
    @Environment(\.fmPalette) var fm
    @Environment(\.scenePhase) var scenePhase
    let auth: AuthService
    @State private var projects: ProjectRuntime? = Services.get(ProjectRuntime.self)
    @State private var settings: SettingsService? = Services.get(SettingsService.self)
    @State private var updateCheck: UpdateRequiredCheck = UpdateRequiredCheck.shared

    var body: some View {
        Group {
            if updateCheck.blocksUse {
                UpdateRequiredView(requirement: updateCheck.requirement, available: updateCheck.available)
            } else if let settings, settings.needsStarterPack, auth.hasAppAccess || settings.project.hasInstance == false {
                StarterPackView(settings: settings)
            } else if auth.hasAppAccess || settings?.project.hasInstance == false {
                HomeShell()
                    .relaySwitchScope()
                    .overlay { DevPerformanceOverlay() }
                    .overlay { PingCelebrationOverlay() }
                    .directCallsRoot()
                    .groupCallsRoot()
            } else {
                AuthFlowView()
            }
        }
        // A project switch replaces every view and view model underneath.
        .id(projects?.generation ?? 0)
        .dataFolderAlert()
        .background(fm.canvas.ignoresSafeArea())
        .task {
            await updateCheck.check()
            await updateCheck.checkStore()
        }
        .onChange(of: scenePhase) { (_: ScenePhase, phase: ScenePhase) in
            if phase == .active {
                Task { await updateCheck.checkStore() }
            }
        }
    }
}
