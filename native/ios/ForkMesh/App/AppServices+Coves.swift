// WS21 (coves, secrets and AutoFill): this workstream's startup hook, called by
// AppServices.installCoves() (docs/native/DESKTOP_PARITY.md section 19). Only WS21
// edits this file. Gate 2 creates it empty.
//
// Register destinations here, e.g.
//     services.routes.register(.calendar, by: "WS21") { route in AnyView(CalendarScreen(route: route)) }

import FMApi
import FMVault
import SwiftUI
import UIKit

@MainActor
struct CovesInstaller: AppServicesInstaller {
    func install(into services: AppServices) {
        let vault: VaultService = services.vault
        Task { @MainActor in await vault.attach(platform: AutoFillVaultPlatform()) }
        VaultCredentialExchange.handOver(to: vault)
        NotificationCenter.default.addObserver(
            forName: UIApplication.didEnterBackgroundNotification, object: nil, queue: .main
        ) { _ in
            MainActor.assumeIsolated { CovesFeature.lockEverything() }
        }
        services.routes.register(.vault, by: "WS21") { route in
            if case .vault(let section) = route, section == "secrets" {
                return AnyView(ActionsSecretsScreen())
            }
            return AnyView(VaultScreen())
        }
        services.routes.register(.coves, by: "WS21") { route in
            guard case .coves(let repo) = route else { return AnyView(CovesRouteScreen(repo: nil)) }
            return AnyView(CovesRouteScreen(repo: repo))
        }
        services.routes.register(.cove, by: "WS21") { route in
            guard case .cove(let id) = route else { return AnyView(CovesRouteScreen(repo: nil)) }
            return AnyView(CoveRouteScreen(id: id))
        }
    }
}
