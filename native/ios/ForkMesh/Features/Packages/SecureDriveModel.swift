import FMAccount
import FMBase
import FMPackages
import Foundation
import Observation
import SwiftUI
import UIKit
import UniformTypeIdentifiers

final class SecureDriveAppStorage: SecureDriveStorage, SecureDriveBookmarkHolder {
    private weak var state: SecureDriveLocalState?
    private let privateRoots: [String]
    private var held: [String: [UInt8]] = [:]

    init(state: SecureDriveLocalState, privateRoots: [URL]) {
        self.state = state
        self.privateRoots = privateRoots.map { $0.standardizedFileURL.path }
    }

    func hold(_ container: String, _ bookmark: [UInt8]) { held[canonical(container)] = bookmark }

    func release(_ container: String) { held[canonical(container)] = nil }

    private func isPrivate(_ path: String) -> Bool {
        privateRoots.contains { path.hasPrefix($0 + "/") }
    }

    private func resolve(_ container: String) -> URL {
        let path = canonical(container)
        let plain = URL(fileURLWithPath: path)
        if isPrivate(path) { return plain }
        var bookmark = held[path]
        var owner: String?
        if bookmark == nil, let state {
            for m in state.drives where canonical(m.container) == path {
                if let saved = state.bookmarks[m.id] {
                    bookmark = saved
                    owner = m.id
                    break
                }
            }
        }
        guard let bookmark else { return plain }
        var stale = false
        guard let url = try? URL(resolvingBookmarkData: Data(bookmark), options: [], relativeTo: nil, bookmarkDataIsStale: &stale) else {
            return plain
        }
        if stale, let owner, let state {
            let access = url.startAccessingSecurityScopedResource()
            if let fresh = try? url.bookmarkData(options: .minimalBookmark, includingResourceValuesForKeys: nil, relativeTo: nil) {
                state.bookmarks[owner] = Array(fresh)
                try? state.save()
            }
            if access { url.stopAccessingSecurityScopedResource() }
        }
        return url
    }

    func withFile<T>(_ container: String, writable: Bool, _ body: (SecureDriveFile) throws -> T) throws -> T {
        let url = resolve(container)
        let access = url.startAccessingSecurityScopedResource()
        defer { if access { url.stopAccessingSecurityScopedResource() } }
        if isPrivate(url.standardizedFileURL.path) { return try Self.open(url, writable: writable, body) }
        var coordinationError: NSError?
        var outcome: Result<T, Error>?
        let coordinator = NSFileCoordinator(filePresenter: nil)
        if writable {
            coordinator.coordinate(writingItemAt: url, options: .forMerging, error: &coordinationError) { (actual: URL) in
                outcome = Result { try Self.open(actual, writable: true, body) }
            }
        } else {
            coordinator.coordinate(readingItemAt: url, options: .withoutChanges, error: &coordinationError) { (actual: URL) in
                outcome = Result { try Self.open(actual, writable: false, body) }
            }
        }
        if let outcome { return try outcome.get() }
        throw SecureDriveError("container_unavailable")
    }

    private static func open<T>(_ url: URL, writable: Bool, _ body: (SecureDriveFile) throws -> T) throws -> T {
        let handle: FileHandle
        do {
            handle = writable ? try FileHandle(forUpdating: url) : try FileHandle(forReadingFrom: url)
        } catch {
            throw SecureDriveError("container_unavailable")
        }
        defer { try? handle.close() }
        return try body(SecureDriveHandleFile(handle: handle))
    }

    func createEmpty(_ container: String) throws {
        let path = canonical(container)
        guard isPrivate(path) else { throw SecureDriveError("folder_unavailable") }
        try SecureDriveLocalStorage().createEmpty(path)
    }

    func exists(_ container: String) -> Bool {
        let url = resolve(container)
        let access = url.startAccessingSecurityScopedResource()
        defer { if access { url.stopAccessingSecurityScopedResource() } }
        return FileManager.default.fileExists(atPath: url.path)
    }

    func removeCreated(_ container: String) {
        let path = canonical(container)
        if isPrivate(path) { SecureDriveScratchFiles.shred(URL(fileURLWithPath: path)) }
    }

    func canonical(_ container: String) -> String { URL(fileURLWithPath: container).standardizedFileURL.path }

    func displayName(_ container: String) -> String { URL(fileURLWithPath: container).deletingPathExtension().lastPathComponent }
}

struct SecureDriveExportDocument: FileDocument {
    static var readableContentTypes: [UTType] { [.data] }
    var data: Data

    init(data: Data) {
        self.data = data
    }

    init(configuration: ReadConfiguration) throws {
        data = configuration.file.regularFileContents ?? Data()
    }

    func fileWrapper(configuration: WriteConfiguration) throws -> FileWrapper {
        FileWrapper(regularFileWithContents: data)
    }
}

struct SecureDriveShareSheet: UIViewControllerRepresentable {
    let items: [Any]

    func makeUIViewController(context: Context) -> UIActivityViewController {
        UIActivityViewController(activityItems: items, applicationActivities: nil)
    }

    func updateUIViewController(_ controller: UIActivityViewController, context: Context) {}
}

struct SecureDriveEditorTarget: Identifiable, Equatable {
    let id = UUID()
    let fileId: String
    let name: String
    let kind: String
    let version: Int
    let base: String
    let readOnly: Bool
    let bytes: [UInt8]
}

struct SecureDriveShareTarget: Identifiable {
    let id = UUID()
    let url: URL
}

enum SecureDrivePhase: Equatable {
    case loading, setup, locked, unlocked
}

@MainActor
enum SecureDriveFeature {
    private static var cached: (path: String, model: SecureDriveModel)?

    static func directory() -> URL {
        let manager = FileManager.default
        if let support = manager.urls(for: .applicationSupportDirectory, in: .userDomainMask).first {
            return AppProjectScope.router.scope.directory(support).appendingPathComponent("secure-drive", isDirectory: true)
        }
        return manager.temporaryDirectory.appendingPathComponent("secure-drive", isDirectory: true)
    }

    static func model() -> SecureDriveModel {
        let folder = directory()
        if let cached, cached.path == folder.path { return cached.model }
        cached?.model.lock()
        let model = SecureDriveModel(directory: folder)
        cached = (folder.path, model)
        return model
    }
}

@MainActor
@Observable
final class SecureDriveModel {
    static let idleLimit: TimeInterval = 15 * 60
    static let backgroundLimit: TimeInterval = 60

    private(set) var phase: SecureDrivePhase = .loading
    private(set) var drives: [SecureDriveSummary] = []
    private(set) var selected: String?
    private(set) var snapshot = SecureDriveSnapshot()
    private(set) var busy = false
    private(set) var status = ""
    private(set) var statusIsError = false
    private(set) var openCopies = 0
    var editor: SecureDriveEditorTarget?
    var share: SecureDriveShareTarget?
    var editorDirty = false

    @ObservationIgnored let service: SecureDriveService
    @ObservationIgnored let openFolder: URL
    @ObservationIgnored let newFolder: URL
    @ObservationIgnored private var lastActivity = Date()
    @ObservationIgnored private var backgroundSince: Date?
    @ObservationIgnored private var backgroundLock: Task<Void, Never>?
    @ObservationIgnored private var copies: [URL] = []
    @ObservationIgnored private var backgroundTask: UIBackgroundTaskIdentifier = .invalid
    @ObservationIgnored private var pendingCreate: (id: String, folder: URL)?

    init(directory: URL) {
        let caches = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first ?? FileManager.default.temporaryDirectory
        let open = caches.appendingPathComponent("secure-drive-open", isDirectory: true)
        let fresh = directory.appendingPathComponent("new", isDirectory: true)
        let roots = [directory, open, FileManager.default.temporaryDirectory]
        openFolder = open
        newFolder = fresh
        service = SecureDriveService(directory: directory) { (state: SecureDriveLocalState) -> SecureDriveStorage in
            SecureDriveAppStorage(state: state, privateRoots: roots)
        }
        SecureDriveScratchFiles.clearAll(open)
        SecureDriveScratchFiles.clearAll(fresh)
        Task { await start() }
    }

    var current: SecureDriveSummary? { drives.first { $0.id == selected } }

    private func start() async {
        let exists = (try? await service.run { $0.exists }) ?? false
        let unlocked = (try? await service.run { $0.isUnlocked }) ?? false
        phase = unlocked ? .unlocked : (exists ? .locked : .setup)
    }

    func touch() { lastActivity = Date() }

    func say(_ text: String, error: Bool = false) {
        status = text
        statusIsError = error
    }

    func explain(_ error: Error) -> String {
        if let failure = error as? SecureDriveError { return SecureDriveText.errorText(failure.code) }
        return SecureDriveText.errorText("")
    }

    private func work<T: Sendable>(_ busyText: String, commit: Bool = true,
                                   _ body: @escaping @Sendable (SecureDriveService) throws -> T) async -> T? {
        if busy {
            say("Secure Drive is still busy with the last task. Try again in a moment.", error: true)
            return nil
        }
        touch()
        busy = true
        if !busyText.isEmpty { say(busyText) }
        if commit { beginBackgroundWork() }
        defer {
            busy = false
            endBackgroundWork()
        }
        do {
            let value = try await service.run(body)
            if !busyText.isEmpty && !statusIsError && status == busyText { say("") }
            return value
        } catch {
            say(explain(error), error: true)
            return nil
        }
    }

    private func beginBackgroundWork() {
        endBackgroundWork()
        backgroundTask = UIApplication.shared.beginBackgroundTask(withName: "SecureDriveCommit") { [weak self] in
            MainActor.assumeIsolated { self?.endBackgroundWork() }
        }
    }

    private func endBackgroundWork() {
        guard backgroundTask != .invalid else { return }
        UIApplication.shared.endBackgroundTask(backgroundTask)
        backgroundTask = .invalid
    }

    func setup(name: String, passphrase: String, again: String) async {
        if passphrase != again {
            say("The two passphrases do not match.", error: true)
            return
        }
        if passphrase.utf16.count < 10 {
            say(SecureDriveText.errorText("passphrase_too_short"), error: true)
            return
        }
        guard await work("Creating this iPhone's keys\u{2026}", commit: false, { try $0.setup(name: name, passphrase: passphrase) }) != nil else { return }
        phase = .unlocked
        say("This iPhone's keys are ready. Create a drive, or join one with an invitation code.")
        await reloadDrives()
    }

    func unlock(passphrase: String) async {
        guard await work("Unlocking\u{2026}", commit: false, { try $0.unlock(passphrase: passphrase) }) != nil else { return }
        phase = .unlocked
        say("")
        await reloadDrives()
        if drives.count == 1 { await select(drives[0].id) }
    }

    func lock(reason: String = "Locked. The keys were cleared from memory.") {
        backgroundLock?.cancel()
        backgroundLock = nil
        editor = nil
        share = nil
        editorDirty = false
        clearCopies()
        selected = nil
        snapshot = SecureDriveSnapshot()
        drives = []
        let service = self.service
        Task.detached { _ = try? await service.run { $0.lock() } }
        if phase == .unlocked {
            phase = .locked
            say(reason)
        }
    }

    func changePassphrase(current: String, next: String, again: String) async -> Bool {
        if next != again {
            say("The two new passphrases do not match.", error: true)
            return false
        }
        guard await work("Changing the passphrase\u{2026}", commit: false, { try $0.changePassphrase(current: current, next: next) }) != nil else { return false }
        say("Passphrase changed.")
        return true
    }

    func reloadDrives() async {
        drives = (try? await service.run { $0.summaries() }) ?? []
        if let selected, !drives.contains(where: { $0.id == selected }) { self.selected = nil }
    }

    func select(_ id: String?) async {
        touch()
        selected = id
        snapshot = SecureDriveSnapshot()
        guard let id else { return }
        await refresh(id, force: true)
    }

    func refresh(_ id: String? = nil, force: Bool = false) async {
        guard phase == .unlocked, !busy, let id = id ?? selected else { return }
        let wasPending = snapshot.pending && snapshot.id == id
        guard let shot = await work(force ? "Reading and verifying the drive\u{2026}" : "", commit: true, { try $0.snapshot(id, scan: true, force: force) }) else { return }
        guard selected == id else { return }
        snapshot = shot
        if wasPending && !shot.pending {
            say("This iPhone was approved. The drive's files are below.")
        }
        await reloadDrives()
    }

    func poll() async {
        guard phase == .unlocked, !busy, let id = selected else { return }
        if let shot = try? await service.run({ try $0.snapshot(id, scan: true) }), selected == id {
            let approved = snapshot.pending && !shot.pending
            if shot != snapshot { snapshot = shot }
            if approved {
                say("This iPhone was approved. The drive's files are below.")
                await reloadDrives()
            }
        }
    }

    func checkIdle() {
        guard phase == .unlocked else { return }
        if editorDirty || openCopies > 0 || busy { return }
        if Date().timeIntervalSince(lastActivity) >= Self.idleLimit {
            lock(reason: "Locked after 15 idle minutes. The keys were cleared from memory.")
        }
    }

    func scenePhaseChanged(_ scene: ScenePhase) {
        switch scene {
        case .background:
            guard phase == .unlocked else { return }
            backgroundSince = Date()
            backgroundLock?.cancel()
            backgroundLock = Task { [weak self] in
                try? await Task.sleep(nanoseconds: UInt64(Self.backgroundLimit * 1_000_000_000))
                guard !Task.isCancelled else { return }
                self?.lock(reason: "Locked while ForkMesh was in the background. The keys were cleared from memory.")
            }
        case .active:
            backgroundLock?.cancel()
            backgroundLock = nil
            if let since = backgroundSince, Date().timeIntervalSince(since) >= Self.backgroundLimit {
                lock(reason: "Locked while ForkMesh was in the background. The keys were cleared from memory.")
            }
            backgroundSince = nil
            checkIdle()
        default:
            break
        }
    }

    func prepareNewDrive(label: String) async -> URL? {
        let clean = label.trimmingCharacters(in: .whitespacesAndNewlines)
        let fileName = (clean.isEmpty ? "SecureDrive" : clean.replacingOccurrences(of: "/", with: "-").replacingOccurrences(of: ":", with: "-")) + ".fmsd"
        guard let folder = SecureDriveScratchFiles.makeFolder(in: newFolder) else {
            say(SecureDriveText.errorText("folder_unavailable"), error: true)
            return nil
        }
        let path = folder.appendingPathComponent(fileName).path
        guard let id = await work("Creating the drive\u{2026}", { try $0.create(label: clean, at: path) }) else {
            SecureDriveScratchFiles.clear(folder, inside: newFolder)
            return nil
        }
        pendingCreate = (id, folder)
        say("Choose where to save the drive file, for example the router's shared folder in Files.")
        return URL(fileURLWithPath: path)
    }

    func finishNewDrive(_ result: Result<URL, Error>) async {
        guard let pending = pendingCreate else { return }
        pendingCreate = nil
        switch result {
        case .success(let url):
            let access = url.startAccessingSecurityScopedResource()
            defer { if access { url.stopAccessingSecurityScopedResource() } }
            let bookmark = (try? url.bookmarkData(options: .minimalBookmark, includingResourceValuesForKeys: nil, relativeTo: nil)).map { Array($0) }
            let path = url.standardizedFileURL.path
            guard await work("", { try $0.relocate(pending.id, to: path, bookmark: bookmark) }) != nil else { return }
            SecureDriveScratchFiles.clear(pending.folder, inside: newFolder)
            await reloadDrives()
            await select(pending.id)
            if bookmark == nil {
                say("The drive was saved, but this iPhone could not keep access to it. Forget it and use Join a drive with the file instead.", error: true)
            } else {
                say("The drive is ready. Add files, or invite another device under Devices.")
            }
        case .failure:
            _ = await work("", commit: false, { try $0.forget(pending.id) })
            SecureDriveScratchFiles.clear(pending.folder, inside: newFolder)
            await reloadDrives()
            say("The drive was not created.")
        }
    }

    func join(_ url: URL, code: String) async {
        let access = url.startAccessingSecurityScopedResource()
        defer { if access { url.stopAccessingSecurityScopedResource() } }
        let bookmark = (try? url.bookmarkData(options: .minimalBookmark, includingResourceValuesForKeys: nil, relativeTo: nil)).map { Array($0) }
        let path = url.standardizedFileURL.path
        guard let made = await work("Asking to join\u{2026}", { try $0.join(path: path, bookmark: bookmark, code: code) }) else { return }
        await reloadDrives()
        await select(made.id)
    }

    func cancelJoin() async {
        guard let id = selected else { return }
        guard await work("", { try $0.cancelJoin(id) }) != nil else { return }
        selected = nil
        snapshot = SecureDriveSnapshot()
        await reloadDrives()
    }

    func forget() async {
        guard let id = selected else { return }
        editor = nil
        clearCopies()
        guard await work("", commit: false, { try $0.forget(id) }) != nil else { return }
        selected = nil
        snapshot = SecureDriveSnapshot()
        await reloadDrives()
    }

    private func afterChange(_ text: String) async {
        guard let id = selected else { return }
        if let shot = try? await service.run({ try $0.snapshot(id, scan: false) }), selected == id { snapshot = shot }
        say(text)
    }

    func newFile(kind: String) async {
        guard let id = selected else { return }
        let name = SecureDriveEditorText.untitled(kind, existing: snapshot.files.map(\.name))
        let bytes: [UInt8]
        switch kind {
        case "document": bytes = SecureDriveEditorText.newDocument
        case "spreadsheet": bytes = SecureDriveEditorText.newSpreadsheet
        default: bytes = SecureDriveEditorText.newDrawing
        }
        guard let fileId = await work("Encrypting \(name)\u{2026}", { try $0.add(id, name: name, content: .bytes(bytes)) }) else { return }
        await afterChange("")
        await open(fileId, purpose: "edit")
    }

    func addFiles(_ urls: [URL]) async {
        guard let id = selected, !urls.isEmpty else { return }
        var failed = [String]()
        var added = 0
        for (index, url) in urls.enumerated() {
            let access = url.startAccessingSecurityScopedResource()
            let name = url.lastPathComponent
            let text = urls.count == 1 ? "Encrypting \(name)\u{2026}" : "Encrypting \(index + 1) of \(urls.count) onto the drive\u{2026}"
            if await work(text, { try $0.add(id, name: name, content: .file(url)) }) != nil {
                added += 1
            } else {
                failed.append(name)
            }
            if access { url.stopAccessingSecurityScopedResource() }
        }
        await afterChange(failed.isEmpty
            ? "Encrypted \(SecureDriveEditorText.plural(added, "file")) onto the drive. The originals were not changed."
            : "Some files were not added: \(failed.joined(separator: ", "))")
        if !failed.isEmpty { statusIsError = true }
    }

    func addData(_ data: Data, name: String) async {
        guard let id = selected else { return }
        let bytes = Array(data)
        guard await work("Encrypting \(name)\u{2026}", { try $0.add(id, name: name, content: .bytes(bytes)) }) != nil else { return }
        await afterChange("Encrypted \(name) onto the drive.")
    }

    func open(_ fileId: String, version: Int = -1, readOnly: Bool = false, purpose: String = "view") async {
        guard let id = selected, let entry = snapshot.file(fileId) else { return }
        let index = version < 0 ? entry.versions.count - 1 : version
        guard index >= 0, index < entry.versions.count else { return }
        let kind = entry.kind
        if !SecureDriveEditorText.editable(kind) && kind != "image" {
            await openExternally(fileId, version: index)
            return
        }
        if entry.versions[index].size > 64 * 1024 * 1024 {
            await openExternally(fileId, version: index)
            return
        }
        let editing = !readOnly && !snapshot.removed && SecureDriveEditorText.editable(kind) && index == entry.versions.count - 1
        let use = editing ? "edit" : purpose
        guard let bytes = await work("Decrypting \(entry.name)\u{2026}", { try $0.read(id, file: fileId, version: index, purpose: use) }) else { return }
        await afterChange("")
        editorDirty = false
        editor = SecureDriveEditorTarget(fileId: fileId, name: index == entry.versions.count - 1 ? entry.name : "\(entry.name) (version \(index + 1))",
                                         kind: kind, version: index, base: entry.versions[index].blob, readOnly: !editing, bytes: bytes)
    }

    func save(_ target: SecureDriveEditorTarget, bytes: [UInt8]) async -> SecureDriveEditorTarget? {
        guard let id = selected else { return nil }
        let fileId = target.fileId
        let base = target.base
        guard await work("Encrypting \(target.name)\u{2026}", { try $0.save(id, file: fileId, bytes: bytes, base: base) }) != nil else { return nil }
        await afterChange("Saved a new encrypted version to the drive.")
        editorDirty = false
        guard let entry = snapshot.file(fileId), let latest = entry.current else { return nil }
        return SecureDriveEditorTarget(fileId: fileId, name: entry.name, kind: target.kind, version: entry.versions.count - 1, base: latest.blob,
                                       readOnly: false, bytes: bytes)
    }

    func openExternally(_ fileId: String, version: Int = -1) async {
        guard let id = selected, let entry = snapshot.file(fileId) else { return }
        guard let folder = SecureDriveScratchFiles.makeFolder(in: openFolder) else {
            say(SecureDriveText.errorText("write_failed"), error: true)
            return
        }
        let url = folder.appendingPathComponent(entry.name)
        guard await work("Decrypting \(entry.name) for another app\u{2026}", { try $0.read(id, file: fileId, version: version, purpose: "external", to: url) }) != nil else {
            SecureDriveScratchFiles.clear(folder, inside: openFolder)
            return
        }
        copies.append(folder)
        openCopies = copies.count
        await afterChange("Opened a decrypted copy of \(entry.name). It is deleted when you lock.")
        share = SecureDriveShareTarget(url: url)
    }

    func exportBytes(_ fileId: String) async -> Data? {
        guard let id = selected else { return nil }
        guard let bytes = await work("Decrypting\u{2026}", { try $0.read(id, file: fileId, version: -1, purpose: "export") }) else { return nil }
        await afterChange("")
        return Data(bytes)
    }

    func clearCopies() {
        for folder in copies { SecureDriveScratchFiles.clear(folder, inside: openFolder) }
        copies = []
        openCopies = 0
        SecureDriveScratchFiles.clearAll(openFolder)
    }

    func rename(_ fileId: String, to name: String) async {
        guard let id = selected else { return }
        guard await work("", { try $0.rename(id, file: fileId, to: name) }) != nil else { return }
        await afterChange("")
    }

    func delete(_ fileId: String) async {
        guard let id = selected else { return }
        guard await work("Deleting\u{2026}", { try $0.delete(id, file: fileId) }) != nil else { return }
        await afterChange("Deleted. The activity log keeps a record of it.")
    }

    func restore(_ fileId: String, version: Int) async {
        guard let id = selected else { return }
        guard await work("Restoring version \(version + 1)\u{2026}", { try $0.restore(id, file: fileId, version: version) }) != nil else { return }
        await afterChange("Restored. The newer versions are still kept.")
    }

    func exportActivity() async -> URL? {
        guard let id = selected else { return nil }
        guard let csv = await work("Exporting activity\u{2026}", { try $0.exportActivity(id) }) else { return nil }
        guard let folder = SecureDriveScratchFiles.makeFolder(in: openFolder) else { return nil }
        let name = snapshot.label.isEmpty ? "activity.csv" : "\(snapshot.label.replacingOccurrences(of: "/", with: "-")) activity.csv"
        let url = folder.appendingPathComponent(name)
        do {
            try Data(csv.utf8).write(to: url, options: [.atomic, .completeFileProtection])
        } catch {
            SecureDriveScratchFiles.clear(folder, inside: openFolder)
            say(SecureDriveText.errorText("write_failed"), error: true)
            return nil
        }
        copies.append(folder)
        openCopies = copies.count
        await afterChange("The activity log is ready to share. It is not encrypted.")
        return url
    }

    func invite() async -> String? {
        guard let id = selected else { return nil }
        guard let code = await work("Creating an invitation\u{2026}", { try $0.invite(id) }) else { return nil }
        await afterChange("")
        return code
    }

    func cancelInvites() async {
        guard let id = selected else { return }
        guard await work("Cancelling invitations\u{2026}", { try $0.cancelInvites(id) }) != nil else { return }
        await afterChange("Cancelled. Those codes no longer work.")
    }

    func approve(_ request: SecureDriveJoinRequest) async {
        guard let id = selected else { return }
        let mailbox = request.mailbox
        guard await work("Sealing the drive key to \(request.name)\u{2026}", { try $0.approve(id, mailbox: mailbox) }) != nil else { return }
        await afterChange("Approved \(request.name). It picks up the drive key the next time it checks the drive.")
    }

    func deny(_ request: SecureDriveJoinRequest) async {
        guard let id = selected else { return }
        let mailbox = request.mailbox
        guard await work("Turning the request down\u{2026}", { try $0.deny(id, mailbox: mailbox) }) != nil else { return }
        await afterChange("Turned down. Its invitation code no longer works.")
    }

    func remove(_ device: SecureDriveDeviceRow) async {
        guard let id = selected else { return }
        let deviceId = device.id
        let progress: @Sendable (Int, Int) -> Void = { [weak self] done, total in
            Task { @MainActor in self?.say("Re-encrypting under the new key: \(done) of \(total)\u{2026}") }
        }
        guard await work("Removing \(device.name) and replacing the drive key\u{2026}", { try $0.remove(id, device: deviceId, progress: progress) }) != nil else { return }
        await afterChange("Removed \(device.name). Everything on the drive is now under a key it never had.")
    }
}
