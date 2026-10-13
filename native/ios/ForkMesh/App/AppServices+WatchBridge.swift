// The watch app's iPhone bridge: this file is that workstream's startup hook,
// called by AppServices.installWatchBridge() (docs/native/PARITY.md, section
// 3.0). Only that workstream edits it. Runs last, after installAgentWatch().

import FMBase
import FMChat

/// Starts the WatchConnectivity bridge (ForkMesh/Platform/WatchBridge.swift)
/// and registers it. It reads nothing until a paired watch has the app.
struct WatchBridgeInstaller: AppServicesInstaller {
    func install(into services: AppServices) {
        let bridge = WatchBridge(
            api: services.api,
            auth: services.auth,
            signal: services.agentList,
            control: Services.get(LocalControlService.self))
        Services.put(bridge)
        bridge.start()
    }
}
