// WS7 (Hosts, live host logs, agents over the relay): this workstream's startup hook, after the
// section 3.0 pattern in docs/native/PARITY.md. Only WS7 edits this file.
//
// Gate 2 added AppServices.installHosts(), called after installWatchBridge(), so the services are
// built at startup; HostsFeature still builds them on first use when the installer did not run
// (previews). The Hosts and All agents screens are registered as routes here.

import SwiftUI

@MainActor
struct HostsInstaller: AppServicesInstaller {
    func install(into services: AppServices) {
        HostsFeature.register()
        services.routes.register(.hosts, by: "WS7") { _ in AnyView(HostsDestination()) }
        services.routes.register(.host, by: "WS7") { route in
            guard case .host(let name, _) = route else { return AnyView(HostsDestination()) }
            return AnyView(HostDestination(name: name))
        }
        services.routes.register(.allAgents, by: "WS7") { _ in AnyView(AllAgentsDestination()) }
    }
}
