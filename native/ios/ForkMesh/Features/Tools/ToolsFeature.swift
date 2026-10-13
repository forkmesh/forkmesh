// Wiring for the Tools area (AppLog / AppTelemetry setup): the in-app log, the telemetry sampler,
// the hooks other features fill in (the relay log, node sync), and the vault's clipboard. Created
// once and registered in `Services`; see docs/FMTools.md.

import FMAccount
import FMBase
import FMNet
import FMTools
import FMVault
import Foundation
import SwiftUI

enum ToolsFeature {
    /// Whether the frame probe (FPS and worst frame in the telemetry strip) runs while the app is
    /// visible. It asks for a 60 Hz display link, which keeps the display from dropping to its
    /// idle rate; set false before `register()` to keep FPS/FRAME empty instead.
    static var frameProbeEnabled: Bool = true

    private static var lifecycleToken: UUID?
    private static var frameProbe: TelemetryFrameProbe?

    /// The log and the sampler, created (and registered) on first use. AppServices may call this
    /// before building its HTTP client to count requests: `telemetry.wrap(URLSessionHTTPClient())`.
    @MainActor
    @discardableResult
    static func bootstrapTelemetry() -> AppTelemetry {
        if let existing = Services.get(AppTelemetry.self) {
            return existing
        }
        // The in-app log starts first so every line logged during launch shows up behind the
        // telemetry strip's log icons.
        let log: AppLog = AppLog()
        log.captureProcessLogs()
        let telemetry: AppTelemetry = AppTelemetry(log: log, monitor: Services.get(PerformanceMonitorService.self))
        Services.put(log)
        Services.put(telemetry)
        return telemetry
    }

    /// Registers `AppLog`, `AppTelemetry`, `ToolsHooks` and the vault's `VaultClipboard`, and
    /// starts sampling. AppServices calls this last (Account -> Api -> ... -> Tools). The
    /// VaultService itself is AppServices'.
    @MainActor
    static func register() {
        let telemetry: AppTelemetry = bootstrapTelemetry()
        let hooks: ToolsHooks = Services.get(ToolsHooks.self) ?? Services.put(ToolsHooks())
        // The relay's frame counter arrives through the hook once the Chat feature fills it in.
        telemetry.relayFrames = { hooks.relayFrames?() ?? 0 }
        if Services.get(VaultClipboard.self) == nil {
            Services.put(PasteboardVaultClipboard(), as: VaultClipboard.self)
        }

        HealthFeature.register()

        let lifecycle: AppLifecycle = AppLifecycle.shared
        telemetry.start(lifecycleState: lifecycle.state)
        if frameProbeEnabled && frameProbe == nil {
            let probe: TelemetryFrameProbe = TelemetryFrameProbe(telemetry: telemetry)
            frameProbe = probe
            if lifecycle.state == .resumed || lifecycle.state == .inactive {
                probe.start()
            }
        }
        if lifecycleToken == nil {
            lifecycleToken = lifecycle.observe { (state: AppLifecycleState) in
                telemetry.appLifecycleChanged(state)
                if state == .resumed || state == .inactive {
                    frameProbe?.start()
                } else {
                    frameProbe?.stop()
                }
            }
        }
    }

    /// The hooks object, for the Chat feature (or AppServices) to fill in.
    @MainActor
    static var hooks: ToolsHooks {
        if let existing = Services.get(ToolsHooks.self) {
            return existing
        }
        return Services.put(ToolsHooks())
    }
}
