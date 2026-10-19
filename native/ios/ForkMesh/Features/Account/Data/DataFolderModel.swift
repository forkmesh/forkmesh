import FMAccount
import FMBase
import FMCrypto
import Foundation
import Observation
import UIKit

@MainActor
@Observable
final class DataFolderModel {
    static let shared: DataFolderModel = DataFolderModel()

    private(set) var folder: URL?
    private(set) var reachable: Bool = false
    private(set) var status: String = ""
    private(set) var statusIsError: Bool = false
    private(set) var working: Bool = false
    private(set) var restorable: [DataFolderProject] = []
    private(set) var needsRestart: Bool = false
    private(set) var revision: Int = 0
    var dismissedThisLaunch: Bool = false
    var showSetup: Bool = false

    @ObservationIgnored private var syncing: Bool = false
    @ObservationIgnored private var backgroundTask: UIBackgroundTaskIdentifier = .invalid

    private init() {
        restoreBookmark()
    }

    private var services: AppServices? { AppServices.current }
    private var prefs: KeyValueStore? { services?.prefs.base }
    private var scope: ProjectScope { Services.get(ProjectRuntime.self)?.scope ?? .legacy }
    private var projectName: String { Services.get(ProjectRuntime.self)?.project.label ?? "ForkMesh" }
    var slug: String { DataFolder.projectFolder(scope) }
    var publicKey: String { Services.get(DeviceIdentity.self)?.publicKeyB64url ?? "" }

    var store: DataFolderStore? {
        guard reachable, let folder else { return nil }
        return DataFolderStore(root: DataFolder.root(forPicked: folder))
    }

    var need: DataFolderNeed? {
        _ = revision
        guard let prefs else { return .chooseFolder }
        return DataFolder.need(prefs: prefs, slug: slug, publicKey: publicKey, reachable: reachable)
    }

    var showsAlert: Bool { need != nil && !dismissedThisLaunch && !showSetup && !needsRestart }

    var summary: String {
        _ = revision
        guard let folder else { return "Not set up: your data is deleted with the app" }
        if !reachable { return "Can't open “\(folder.lastPathComponent)”" }
        return "Saved in “\(folder.lastPathComponent)/\(DataFolder.folderName)”"
    }

    private func restoreBookmark() {
        guard let saved = AppServices.current?.prefs.base.string(forKey: DataFolder.locationKey),
              let data = Data(base64Encoded: saved) else { return }
        var stale: Bool = false
        guard let url = try? URL(resolvingBookmarkData: data, options: [], relativeTo: nil, bookmarkDataIsStale: &stale) else {
            return
        }
        _ = url.startAccessingSecurityScopedResource()
        var isDirectory: ObjCBool = false
        folder = url
        reachable = FileManager.default.fileExists(atPath: url.path, isDirectory: &isDirectory) && isDirectory.boolValue
        if reachable, stale,
           let fresh = try? url.bookmarkData(options: .minimalBookmark, includingResourceValuesForKeys: nil, relativeTo: nil) {
            AppServices.current?.prefs.base.set(fresh.base64EncodedString(), forKey: DataFolder.locationKey)
        }
    }

    private func report(_ text: String, error: Bool = false) {
        status = text
        statusIsError = error
        revision &+= 1
    }

    func choose(_ picked: URL) async {
        let access: Bool = picked.startAccessingSecurityScopedResource()
        if let problem = DataFolder.problem(with: picked, appContainer: URL(fileURLWithPath: NSHomeDirectory(), isDirectory: true)) {
            if access { picked.stopAccessingSecurityScopedResource() }
            report(problem, error: true)
            return
        }
        guard let prefs,
              let bookmark = try? picked.bookmarkData(options: .minimalBookmark, includingResourceValuesForKeys: nil, relativeTo: nil)
        else {
            if access { picked.stopAccessingSecurityScopedResource() }
            report("ForkMesh can't keep access to that folder. Pick another one.", error: true)
            return
        }
        if let previous = folder, previous.standardizedFileURL.path != picked.standardizedFileURL.path {
            previous.stopAccessingSecurityScopedResource()
        }
        prefs.set(bookmark.base64EncodedString(), forKey: DataFolder.locationKey)
        folder = picked
        reachable = true
        let store = DataFolderStore(root: DataFolder.root(forPicked: picked))
        let current: String = publicKey
        let mine: String = slug
        restorable = store.projects().filter { project in
            store.readKit(slug: project.slug) != nil && !(project.slug == mine && project.publicKey == current)
        }
        if restorable.isEmpty {
            report("ForkMesh keeps a copy of your data in “\(picked.lastPathComponent)/\(DataFolder.folderName)”.")
            await sync()
        } else {
            report("This folder holds ForkMesh data from an earlier install. Enter its passphrase to restore it.")
        }
    }

    func startFresh() async {
        restorable = []
        report("ForkMesh keeps a copy of this install's data here too.")
        await sync()
    }

    func saveIdentity(passphrase: String, confirm: String) async {
        guard let store, let prefs, let identity = Services.get(DeviceIdentity.self) else {
            report(RecoveryKitError.noIdentity.description, error: true)
            return
        }
        working = true
        defer { working = false }
        let target: String = slug
        let now: Int64 = SystemClock().nowMillis
        let result: Result<Void, Error> = await Task.detached(priority: .userInitiated) {
            Result {
                try DataFolderIdentity.saveKit(
                    identity: identity, passphrase: passphrase, confirm: confirm, slug: target,
                    store: store, prefs: prefs, nowMillis: now)
            }
        }.value
        switch result {
        case .success:
            report("Your identity key is saved, encrypted, in the data folder.")
            await sync()
        case .failure(let failure):
            report((failure as? RecoveryKitError)?.description ?? "The identity key was not saved.", error: true)
        }
    }

    func restore(passphrase: String) async {
        guard let store, let services else { return }
        working = true
        defer { working = false }
        let projects: [DataFolderProject] = restorable
        let secure: SecureStore = services.secure.base
        let prefs: KeyValueStore = services.prefs.base
        let roots: [URL] = ProjectRuntime.roots()
        let outcome: (restored: [String], files: Int, failed: [String]) = await Task.detached(priority: .userInitiated) {
            var restored: [String] = []
            for project in projects {
                guard let kit = store.readKit(slug: project.slug),
                      let key = try? DataFolderIdentity.restoreSeed(
                        kit: kit, passphrase: passphrase, namespace: project.namespace, secureStore: secure)
                else { continue }
                prefs.set(key, forKey: DataFolder.kitKey(project.slug))
                restored.append(project.slug)
            }
            guard !restored.isEmpty else { return ([], 0, []) }
            if let settings = store.readSettings(slug: DataFolder.homeProject) {
                DataFolder.applySettings(settings, to: prefs)
            }
            var files: Int = 0
            var failed: [String] = []
            for project in projects where restored.contains(project.slug) {
                let scope = ProjectScope(projectId: project.projectId, namespace: project.namespace)
                let copy = store.restore(slug: project.slug, trees: DataFolderModel.trees(for: scope, roots: roots))
                files += copy.copied
                failed += copy.failed
            }
            return (restored, files, failed)
        }.value
        if outcome.restored.isEmpty {
            report("The passphrase didn't open the identity key in this folder.", error: true)
            return
        }
        restorable = []
        needsRestart = true
        var text: String = "Restored \(outcome.restored.count == 1 ? "1 project" : "\(outcome.restored.count) projects") and \(outcome.files) files."
        if !outcome.failed.isEmpty { text += " \(outcome.failed.count) files could not be copied." }
        report(text + " Close ForkMesh and open it again to finish.", error: !outcome.failed.isEmpty)
    }

    nonisolated static func trees(for scope: ProjectScope, roots: [URL]) -> [DataFolderTree] {
        let names: [String] = ["documents", "support"]
        let skipped: Set<String> = scope.isLegacy ? ["projects"] : []
        return zip(names, roots).map { name, root in
            DataFolderTree(name: name, local: scope.directory(root), skippedTopLevel: skipped)
        }
    }

    func sync() async {
        guard let store, let prefs, !syncing, !needsRestart, restorable.isEmpty else { return }
        syncing = true
        defer { syncing = false }
        let project = DataFolderProject(
            slug: slug, projectId: scope.projectId, namespace: scope.namespace, name: projectName, publicKey: publicKey)
        let trees: [DataFolderTree] = DataFolderModel.trees(for: scope, roots: ProjectRuntime.roots())
        let now: Int64 = SystemClock().nowMillis
        let copy: DataFolderCopyReport? = await Task.detached(priority: .utility) {
            do {
                try store.prepare(platform: "ios", nowMillis: now)
                try store.prepareProject(project)
                try store.writeSettings(DataFolder.exportSettings(prefs), slug: DataFolder.homeProject)
            } catch {
                return nil
            }
            return store.backup(slug: project.slug, trees: trees)
        }.value
        guard let copy else {
            reachable = false
            report(DataFolder.reconnectMessage, error: true)
            return
        }
        prefs.set(Double(now), forKey: DataFolder.syncedAtKey)
        if !copy.failed.isEmpty {
            report("\(copy.failed.count) files could not be copied to the data folder.", error: true)
        } else {
            revision &+= 1
        }
    }

    func syncInBackground() {
        guard store != nil else { return }
        endBackgroundWork()
        backgroundTask = UIApplication.shared.beginBackgroundTask(withName: "forkmesh.dataFolder") { [weak self] in
            MainActor.assumeIsolated { self?.endBackgroundWork() }
        }
        Task { @MainActor in
            await sync()
            endBackgroundWork()
        }
    }

    private func endBackgroundWork() {
        guard backgroundTask != .invalid else { return }
        UIApplication.shared.endBackgroundTask(backgroundTask)
        backgroundTask = .invalid
    }

    func closeToFinish() {
        exit(0)
    }
}
