// The LocalRepos area's wiring, plus the shared pieces every local surface
// uses: LocalGitBusyOverlay, LocalGitBusyCard and the user-presence
// confirmation the delete asks for.

import FMAccount
import FMApi
import FMBase
import FMChat
import FMCode
import FMCrypto
import Foundation
import LocalAuthentication
import SwiftUI

@MainActor
enum LocalReposFeature {
    static func register() {
        let manager: FileManager = FileManager.default
        let documents: URL = manager.urls(for: .documentDirectory, in: .userDomainMask).first
            ?? manager.temporaryDirectory
        let scope: LocalRepoScope = LocalRepoServices.scope
        let folder: LocalRepoFolderSetting = LocalRepoFolderSetting(
            prefs: Services.get(KeyValueStore.self), scope: scope,
            appRoot: LocalRepoStore.root(for: scope, documents: documents))
        Services.put(folder)
        let store: LocalRepoStore = LocalRepoStore(
            root: folder.root,
            performanceMonitor: Services.get(PerformanceMonitorService.self))
        Services.put(store)
        if let api = Services.get(ApiService.self),
           let identity = Services.get(DeviceIdentity.self),
           let prefs = Services.get(KeyValueStore.self) {
            Services.put(PublishService(api: api, identity: identity, prefs: prefs, scope: scope))
        }
        installNodeSyncApplier(store: store, scope: scope)
    }

    /// Installs the LocalNodeSyncApplier on node sync. Without it the node
    /// acknowledges nothing and every inbox row waits for a desktop.
    private static func installNodeSyncApplier(store: LocalRepoStore, scope: LocalRepoScope) {
        guard let nodeSync = Services.get(NodeSyncService.self),
              let prefs = Services.get(KeyValueStore.self) else { return }
        Services.get(LocalNodeSyncApplier.self)?.dispose()
        let applier: LocalNodeSyncApplier = LocalNodeSyncApplier.forLocalRepos(store, prefs: prefs, scope: scope) {
            let session: AuthSession? = Services.get(AuthService.self)?.session
            return (nodeName: session?.nodeName ?? "", email: session?.email ?? "")
        }
        applier.installOn(nodeSync)
        Services.put(applier)
    }
}

/// Where the local repository views find their dependencies.
@MainActor
enum LocalRepoServices {
    static var store: LocalRepoStore? { Services.get(LocalRepoStore.self) }
    static var publisher: PublishService? { Services.get(PublishService.self) }
    static var folder: LocalRepoFolderSetting? { Services.get(LocalRepoFolderSetting.self) }
    static var session: AuthSession? { Services.get(AuthService.self)?.session }
    /// The node-sync applier, when the phone serves as a node.
    static var applier: LocalNodeSyncApplier? { Services.get(LocalNodeSyncApplier.self) }

    /// The running project's scope. WS1 shim (docs/native/PARITY.md, WS5):
    /// until FMAccount's `ProjectScope` exists, a project installer registers
    /// its scope with `Services.put(scope, as: (any LocalRepoScope).self)`
    /// (after `extension ProjectScope: LocalRepoScope {}`); with nothing
    /// registered this is the legacy scope, the pre-projects layout.
    static var scope: LocalRepoScope {
        Services.get((any LocalRepoScope).self) ?? LegacyLocalRepoScope()
    }

    static func sessionProvider() -> AuthSession? { session }

    /// `canCloneToDevice`: the on-device store and the publisher are wired in.
    static var canClone: Bool { store != nil && publisher != nil }

    /// Pull requests from this device: push through the publisher, submit
    /// through the relay inbox. Nil when either is missing.
    static var pullRequests: PullRequestService? {
        guard let publisher = publisher, let inbox = Services.get(InboxService.self) else { return nil }
        return PullRequestService.live(publisher: publisher, inbox: inbox)
    }
}

/// The device owner confirms a destructive action with Face ID, Touch ID or the passcode. A
/// device without any of them set up falls back to the confirmation the view
/// already showed.
@MainActor
enum LocalUserPresence {
    static func confirm(reason: String) async -> Bool {
        let context: LAContext = LAContext()
        var error: NSError?
        guard context.canEvaluatePolicy(.deviceOwnerAuthentication, error: &error) else {
            return true
        }
        do {
            return try await context.evaluatePolicy(.deviceOwnerAuthentication, localizedReason: reason)
        } catch {
            return false
        }
    }
}

/// Blocking overlay with the engine operation name and a Cancel button.
struct LocalGitBusyOverlay: View {
    @Environment(\.fmPalette) private var fm
    let label: String
    let onCancel: (() -> Void)?

    var body: some View {
        ZStack {
            // Swallow taps on the page underneath while the engine works.
            fm.bgBase.opacity(0.55)
                .ignoresSafeArea()
                .contentShape(Rectangle())
                .onTapGesture {}
            LocalGitBusyCard(label: label, onCancel: onCancel)
        }
    }
}

/// Progress card used by the overlay and the create/import/clone flows.
struct LocalGitBusyCard: View {
    @Environment(\.fmPalette) private var fm
    let label: String
    let onCancel: (() -> Void)?

    var body: some View {
        FmCard(radius: FmRadius.lg) {
            VStack(spacing: FmSpace.x3) {
                ProgressView()
                    .progressViewStyle(.circular)
                Text("\(label)…")
                    .fmText(.labelMedium, color: fm.textPrimary)
                    .multilineTextAlignment(.center)
                if let onCancel = onCancel {
                    FmToneButton(label: "Cancel", tone: .danger, onPressed: onCancel)
                }
            }
            .frame(minWidth: 180, maxWidth: .infinity)
        }
        .padding(FmSpace.x5)
    }
}
