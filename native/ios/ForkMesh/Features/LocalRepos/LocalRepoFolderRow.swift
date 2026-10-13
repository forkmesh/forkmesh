import FMBase
import FMCode
import Foundation
import Observation
import SwiftUI
import UniformTypeIdentifiers

@MainActor
@Observable
final class LocalRepoFolderSetting {
    private(set) var chosen: URL?
    private(set) var problem: String?
    @ObservationIgnored private let prefs: KeyValueStore?
    @ObservationIgnored private let scope: LocalRepoScope
    @ObservationIgnored let appRoot: URL

    init(prefs: KeyValueStore?, scope: LocalRepoScope, appRoot: URL) {
        self.prefs = prefs
        self.scope = scope
        self.appRoot = appRoot
        restore()
    }

    var root: URL { chosen.map { scope.directory($0) } ?? appRoot }

    var note: String {
        if let problem = problem { return problem }
        if let chosen = chosen { return LocalRepoFolder.chosenNote(chosen.lastPathComponent) }
        return LocalRepoFolder.appFolderNote
    }

    var canUseAppFolder: Bool { chosen != nil || problem != nil }

    private func restore() {
        guard let saved = prefs?.string(forKey: LocalRepoFolder.bookmarkKey),
              let data = Data(base64Encoded: saved) else { return }
        var stale: Bool = false
        guard let url = try? URL(resolvingBookmarkData: data, options: [], relativeTo: nil, bookmarkDataIsStale: &stale) else {
            problem = LocalRepoFolder.unreachable
            return
        }
        let access: Bool = url.startAccessingSecurityScopedResource()
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(atPath: url.path, isDirectory: &isDirectory), isDirectory.boolValue else {
            if access { url.stopAccessingSecurityScopedResource() }
            problem = LocalRepoFolder.unreachable
            return
        }
        if stale, let fresh = try? url.bookmarkData(options: .minimalBookmark, includingResourceValuesForKeys: nil, relativeTo: nil) {
            prefs?.set(fresh.base64EncodedString(), forKey: LocalRepoFolder.bookmarkKey)
        }
        chosen = url
    }

    func choose(_ picked: URL, store: LocalRepoStore) async -> LocalNotice {
        let access: Bool = picked.startAccessingSecurityScopedResource()
        let inICloud: Bool = (try? picked.resourceValues(forKeys: [.isUbiquitousItemKey]))?.isUbiquitousItem ?? false
        if let refusal = LocalRepoFolder.problem(
            with: picked, appContainer: URL(fileURLWithPath: NSHomeDirectory(), isDirectory: true), isInICloud: inICloud) {
            if access { picked.stopAccessingSecurityScopedResource() }
            return LocalNotice(refusal, isError: true)
        }
        do {
            let bookmark: Data = try picked.bookmarkData(options: .minimalBookmark, includingResourceValuesForKeys: nil, relativeTo: nil)
            let result: LocalRepoRelocation = try await store.relocate(to: scope.directory(picked))
            prefs?.set(bookmark.base64EncodedString(), forKey: LocalRepoFolder.bookmarkKey)
            if let previous = chosen, previous.standardizedFileURL.path != picked.standardizedFileURL.path {
                previous.stopAccessingSecurityScopedResource()
            }
            chosen = picked
            problem = nil
            return LocalNotice(LocalRepoFolder.movedMessage(result, to: picked.lastPathComponent), isError: !result.leftBehind.isEmpty)
        } catch {
            if access { picked.stopAccessingSecurityScopedResource() }
            return LocalNotice(LocalRepoFolder.failure(error), isError: true)
        }
    }

    func useAppFolder(store: LocalRepoStore) async -> LocalNotice {
        do {
            let result: LocalRepoRelocation = try await store.relocate(to: appRoot)
            prefs?.remove(LocalRepoFolder.bookmarkKey)
            chosen?.stopAccessingSecurityScopedResource()
            chosen = nil
            problem = nil
            if result.leftBehind.isEmpty {
                return LocalNotice(LocalRepoFolder.backInAppFolder)
            }
            return LocalNotice(LocalRepoFolder.movedMessage(result, to: "ForkMesh"), isError: true)
        } catch {
            return LocalNotice(LocalRepoFolder.failure(error), isError: true)
        }
    }
}

struct LocalRepoFolderRow: View {
    @Environment(\.fmPalette) private var fm
    let folder: LocalRepoFolderSetting
    let store: LocalRepoStore
    let isBusy: Bool
    let onNotice: (LocalNotice) -> Void
    @State private var choosing: Bool = false
    @State private var picking: Bool = false

    var body: some View {
        HStack(alignment: .center, spacing: FmSpace.x2) {
            Text(folder.note)
                .fmText(.bodySmall, color: folder.problem == nil ? fm.textSecondary : fm.danger)
                .frame(maxWidth: .infinity, alignment: .leading)
                .fmMessageTools(folder.problem == nil ? nil : folder.note)
            FmToneButton(label: "Change", icon: "folder_open_outlined", onPressed: isBusy ? nil : { choosing = true })
        }
        .padding(EdgeInsets(top: 0, leading: FmSpace.x4, bottom: FmSpace.x3, trailing: FmSpace.x4))
        .confirmationDialog("Where repositories are saved", isPresented: $choosing, titleVisibility: .visible) {
            Button("Choose a folder in Files") { picking = true }
            if folder.canUseAppFolder {
                Button("Use ForkMesh's own folder") {
                    Task { @MainActor in onNotice(await folder.useAppFolder(store: store)) }
                }
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("Make a folder in Files, for example in On My iPhone, and choose it here. Repositories saved there stay if ForkMesh is deleted; after reinstalling, choose the same folder to get them back.")
        }
        .fileImporter(isPresented: $picking, allowedContentTypes: [UTType.folder]) { (result: Result<URL, Error>) in
            if case .success(let url) = result {
                Task { @MainActor in onNotice(await folder.choose(url, store: store)) }
            }
        }
    }
}
