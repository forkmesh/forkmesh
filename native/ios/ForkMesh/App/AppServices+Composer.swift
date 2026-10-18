// WS3 (Composer, photos, dictation and model catalog): this file is that
// workstream's startup hook, called by AppServices.installComposer()
// (docs/native/PARITY.md, section 3.0). Only that workstream edits it. Runs
// after wire() and installPush().
//
// It builds the shell's composer once, at launch, so the prompt button opens
// something that already exists: the composer state and the quick launcher
// behind Send (QuickAgentLauncher: repository, where it runs and the model,
// remembered in the app's preferences).

import FMAccount
import FMAgents
import FMApi
import FMBase

struct ComposerInstaller: AppServicesInstaller {
    func install(into services: AppServices) {
        _ = Services.put(ComposerInstaller.makeComposer(prefs: services.prefs, api: services.api, auth: services.auth))
        Task(priority: .utility) { @MainActor in
            try? await Task.sleep(for: .seconds(5))
            _ = await PromptDictation.installModel()
        }
    }

    /// The composer and its launcher. HomeShell falls back to this when the
    /// installer did not run (previews).
    @MainActor
    static func makeComposer(
        prefs: KeyValueStore? = Services.get(KeyValueStore.self),
        api: ApiService? = Services.get(ApiService.self),
        auth: AuthService? = Services.get(AuthService.self)
    ) -> ShellComposer {
        let form: AgentStartModel = AgentStartModel(
            api: api,
            records: AgentsFeature.localRecords(),
            signedIn: { auth == nil || auth?.session != nil },
            credentialStore: AgentsFeature.credentialStore(),
            localModels: { AgentsFeature.localModels() })
        let launcher = QuickAgentLauncher(form: form, prefs: prefs, account: { auth?.session?.nodeName ?? "" })
        launcher.desktop = HubQuickLaunchDesktop { HostsFeature.hub }
        launcher.actions = ShellPromptActions(api: { api }, auth: { auth })
        return ShellComposer(launcher: launcher)
    }
}
