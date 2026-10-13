// WS4 (Agent Watch and Listen on the iPhone): this file is that workstream's
// startup hook, called by AppServices.installAgentWatch()
// (docs/native/PARITY.md, section 3.0). Only that workstream edits it. Runs
// after wire(), installPush() and installComposer().

import FMAgentWatch
import FMBase

/// Registers what the Watch screen reads from Services: where its sound and
/// touch go (AVAudioEngine and Core Haptics, Platform/AgentWatchOutput.swift).
/// The screen's doorbell is the AgentListSignal wire() already rings on the
/// account's `agents` topic, so nothing else needs hooking up here.
struct AgentWatchInstaller: AppServicesInstaller {
    func install(into services: AppServices) {
        Services.put(AgentWatchEnvironment(makeOutput: { PlatformAgentWatchOutput() }))
    }
}
