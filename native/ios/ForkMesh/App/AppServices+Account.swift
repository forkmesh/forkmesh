// WS18 (settings, account, identity and devices): this workstream's startup hook, called by
// AppServices.installAccount() (docs/native/DESKTOP_PARITY.md section 19). Only WS18
// edits this file. Gate 2 creates it empty.
//
// Register destinations here, e.g.
//     services.routes.register(.calendar, by: "WS18") { route in AnyView(CalendarScreen(route: route)) }

import FMApi
import SwiftUI

@MainActor
struct AccountInstaller: AppServicesInstaller {
    func install(into services: AppServices) {
        AccountSyncSetup.install()
        services.routes.register(.settings, by: "WS18") { route in
            guard case let .settings(section) = route else {
                return AnyView(SettingsScreen())
            }
            return AnyView(SettingsScreen(section: section))
        }
        services.routes.register(.profile, by: "WS18") { route in
            guard case let .profile(name) = route, !name.isEmpty else {
                return AnyView(RoutePlaceholderView(route: route))
            }
            return AnyView(AccountProfileScreen(name: name))
        }
        services.routes.register(.orgs, by: "WS18") { _ in AnyView(OrgsScreen()) }
        services.routes.register(.org, by: "WS18") { route in
            guard case let .org(name) = route, !name.isEmpty else {
                return AnyView(OrgsScreen())
            }
            return AnyView(OrgDetailScreen(orgName: name))
        }
    }
}
