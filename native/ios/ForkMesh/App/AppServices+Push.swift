// WS2 (Push, pings and code-push): this file is that workstream's startup
// hook, called by AppServices.installPush() (docs/native/PARITY.md, section
// 3.0). Only that workstream edits it. Runs after wire(), so it may extend or
// replace the push callbacks set there.
//
// Per-project push (PARITY row #52): one APNs token serves the phone, and
// every project (ForkMesh, ForkMesh Next, a self-hosted instance) keeps its
// own registration of it and its own shared-keychain session, keyed by host.
// The project id is the relay directory's id, which is the normalized origin
// (RelayProfile.normalizeOrigin): the same id WS1's `ProjectScope.id`
// carries. Until WS1's `ProjectScope` lands on main this reads it straight
// from `settings.relays` (the local shim); once it does, `activeProjectId`
// below becomes the scope's id and nothing else changes.

import FMAccount
import FMApi
import FMBase
import FMChat
import Foundation

struct PushInstaller: AppServicesInstaller {
    func install(into services: AppServices) {
        let push: PushNotificationService = services.push
        let settings: SettingsService = services.settings
        let api: ApiService = services.api

        // The project the service registers with now.
        push.attach(api: api, projectId: PushInstaller.activeProjectId(settings))
        push.activeProjectProvider = { PushInstaller.activeProjectId(settings) }
        push.knowsProject = { (projectId: String) in settings.relays.contains(projectId) }

        // Taps keep wire()'s onOpened (the inbox is re-read). A code push
        // (topic "code-push") lands on the commits of the pushed branch
        // through its href (/owner/repo/commits/<branch>, parsed by
        // NotificationDeepLink), which HomeShell routes like any other
        // repository link.

        guard let switcher = Services.get(RelaySwitchController.self) else {
            return
        }

        // A tapped push from another project's instance switches there
        // first; the tap is then routed by that project's shell.
        push.onForeignProjectOpen = { (open: PushOpen) in
            let result: RelaySwitchResult = await switcher.switchTo(open.projectId)
            if !result.ok {
                Log.warning("push", "could not follow a push to its project: \(result.error)")
            }
        }

        // The switch stops the per-account machinery and starts it again.
        // Its push.stop() must not unregister the project being left (that
        // instance should keep reaching this phone), and the service must
        // follow the switch to the new project before it rebinds.
        let suspend: (@MainActor () async throws -> Void)? = switcher.onSuspend
        switcher.onSuspend = {
            push.beginProjectSwitch()
            try await suspend?()
        }
        let resume: (@MainActor () async throws -> Void)? = switcher.onResume
        switcher.onResume = {
            push.finishProjectSwitch(api: api, projectId: PushInstaller.activeProjectId(settings))
            try await resume?()
        }
    }

    /// The active project's id. Local shim for WS1's `ProjectScope.id`.
    @MainActor
    static func activeProjectId(_ settings: SettingsService) -> String {
        return settings.relays.activeId
    }
}
