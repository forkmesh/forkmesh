// WS1 (Projects and multi-instance isolation): this file is that workstream's
// startup hook, called by AppServices.installProjects()
// (docs/native/PARITY.md, section 3.0). Only that workstream edits it. Runs
// before the feature areas register, so a project scope set here is what every
// service reads.
//
// * The app's Keychain and UserDefaults stores address the active project
//   (FMAccount's `ProjectScopeRouter`), resolved from the persisted project
//   list before any service reads them, so the node key, owner key, vault
//   copy, secrets and per-project preferences AppServices loads at launch are
//   the active project's.
// * A switch is a rebuild of everything per project: the RelaySwitchController
//   takes the old project's sockets down and clears its caches, then calls
//   `activate` here, which moves both stores to the new project's namespace,
//   reloads the node key in place (every holder signs with it at once),
//   replaces the owner-encryption keys, re-reads the secrets and locks the
//   vault; the controller then adopts the new project's session and brings
//   its sockets up. `generation` changes, and ForkMeshApp re-creates the whole
//   view tree from it, so no screen or view model carries state across.
// * Removing a project erases only its own namespace (`ProjectStorage`).

import FMAccount
import FMAgents
import FMApi
import FMBase
import FMChat
import FMControl
import FMCrypto
import FMPackages
import FMVault
import Foundation
import Observation

/// Installs the project runtime: registers it (as itself and as the
/// `ProjectActivator` the relay switch controller calls) and reconciles the
/// stores with the project list.
struct ProjectsInstaller: AppServicesInstaller {
    func install(into services: AppServices) {
        let runtime: ProjectRuntime = ProjectRuntime(services: services)
        Services.put(runtime)
        Services.put(runtime, as: ProjectActivator.self)
        runtime.start()
    }
}

/// The active project and the per-project half of the service graph.
@MainActor
@Observable
final class ProjectRuntime: ProjectActivator {
    /// Changes on every switch. The app root re-creates its view tree from it.
    private(set) var generation: Int = 0
    /// The project the app is on.
    private(set) var project: RelayProfile

    @ObservationIgnored private let services: AppServices

    init(services: AppServices) {
        self.services = services
        self.project = services.settings.relays.active
    }

    /// Where the active project's data lives.
    var scope: ProjectScope { project.scope }

    /// The router both app stores share.
    private var router: ProjectScopeRouter { services.secure.router }

    /// Launch: the stores resolved the scope from the persisted list before the
    /// directory loaded. Loading can repair the list (a missing active row falls
    /// back to the first), so make sure both agree before anything else runs.
    func start() {
        let active: RelayProfile = services.settings.relays.active
        project = active
        LegalLinks.activeHomepage = active.homepageURL ?? LegalLinks.productionHomepage
        if services.prefs.router !== services.secure.router {
            Log.error("projects", "the preference store and the Keychain address different routers")
        }
        if router.scope != active.scope {
            Log.warning("projects", "stores opened \(router.scope), the directory says \(active.scope); moving")
            apply(active)
        }
    }

    // MARK: ProjectActivator

    func activate(_ project: RelayProfile) {
        apply(project)
        forgetPreviousProject()
        generation &+= 1
    }

    private func forgetPreviousProject() {
        Services.get(LocalControlService.self)?.resetForProjectSwitch()
        Services.get(DesktopControlHub.self)?.resetForProjectSwitch()
        Services.get(AgentControlCenter.self)?.resetForProjectSwitch()
        Services.get(AgentSessionService.self)?.load()
        HubPackageState.shared.reset()
        Services.remove(AccountPackagesStore.self)
        ShellNavigatorCounts.shared.reset()
        ShellCounts.shared.agents = 0
        ShellCounts.shared.agentsTotal = 0
        ShellCounts.shared.runnerAgents = [:]
        ShellCounts.shared.swimmers = []
    }

    func erase(_ project: RelayProfile) {
        if project.id == self.project.id {
            // The controller switches away first; erasing the project in use would
            // pull the node key out from under every running service.
            Log.error("projects", "refusing to erase the active project \(project.id)")
            return
        }
        ProjectLogoStore.delete(project.logo)
        ProjectStorage.erase(
            project, secureStore: services.secure.base, prefs: services.prefs.base, roots: ProjectRuntime.roots())
    }

    // MARK: Switching

    private func apply(_ next: RelayProfile) {
        project = next
        LegalLinks.activeHomepage = next.homepageURL ?? LegalLinks.productionHomepage
        // 1. Storage: both stores share this router.
        router.activate(next.scope)
        // 2. The node key. Loaded once at launch and held by every service that
        //    signs (auth, inbox, chat, node sync, publishing); reloaded in place
        //    so all of them move together.
        if let identity = services.security.identity as? Identity {
            do {
                try identity.reload()
            } catch {
                Log.error("projects", "node key for \(next.id) unavailable: \(error)")
            }
        }
        // 3. The owner-encryption keys: the private halves this project's
        //    desktops seal agent transcripts to. Enrolment follows the session
        //    change the controller makes next.
        let owner: OwnerCrypto = OwnerCrypto.loadOrCreate(secureStore: services.secure)
        services.api.ownerCrypto = owner
        Services.put(owner, as: OwnerKeys.self)
        // 4. Secrets read once into memory (room passphrase, import tokens).
        services.settings.reloadProjectSecrets()
        // 5. The vault holds the unlocked key of the project being left. Lock
        //    it; its device copy stays where it is, in that project's namespace.
        services.vault.lock()
    }

    /// The folders a project may keep files under (`<root>/projects/<slug>`).
    static func roots() -> [URL] {
        let manager: FileManager = FileManager.default
        var roots: [URL] = []
        if let documents = manager.urls(for: .documentDirectory, in: .userDomainMask).first {
            roots.append(documents)
        }
        if let support = manager.urls(for: .applicationSupportDirectory, in: .userDomainMask).first {
            roots.append(support)
        }
        return roots
    }
}
