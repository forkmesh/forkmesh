import FMBase
import FMFiles
import Foundation
import Observation

struct SavedFolder: Codable, Identifiable, Hashable {
    var id: String
    var name: String
    var bookmark: Data
}

struct FileClipboard {
    var entry: FileEntry
    var source: FileSource
    var mode: FileTransferMode
}

@MainActor
@Observable
final class FilesLibrary {
    static let shared = FilesLibrary()
    static let foldersKey = "files/saved-folders"
    static let hiddenKey = "files/show-hidden"
    static let sortKey = "files/sort"

    var folders: [SavedFolder] = []
    var shares: [NetworkShare] = []
    var routerShares: [RouterShareSuggestion] = []
    var clipboard: FileClipboard?
    var showHidden: Bool {
        didSet { UserDefaults.standard.set(showHidden, forKey: Self.hiddenKey) }
    }
    var sort: FileSort {
        didSet { UserDefaults.standard.set(sort.rawValue, forKey: Self.sortKey) }
    }

    @ObservationIgnored private var sources: [String: FileSource] = [:]
    @ObservationIgnored private let store: NetworkShareStore

    private init() {
        store = NetworkShareStore(prefs: Services.get(KeyValueStore.self) ?? UserDefaultsStore(), secure: Services.get(SecureStore.self) ?? KeychainStore())
        showHidden = UserDefaults.standard.bool(forKey: Self.hiddenKey)
        sort = FileSort(rawValue: UserDefaults.standard.string(forKey: Self.sortKey) ?? "") ?? .name
    }

    func reload() {
        shares = store.all().sorted { $0.title.localizedCaseInsensitiveCompare($1.title) == .orderedAscending }
        if let data = UserDefaults.standard.data(forKey: Self.foldersKey), let saved = try? JSONDecoder().decode([SavedFolder].self, from: data) {
            folders = saved
        }
    }

    var suggestions: [RouterShareSuggestion] {
        routerShares.filter { suggestion in
            !shares.contains { $0.share.caseInsensitiveCompare(suggestion.name) == .orderedSame && $0.host.caseInsensitiveCompare(suggestion.host) == .orderedSame }
        }
    }

    func deviceSource() -> FileSource {
        if let cached = sources["device"] { return cached }
        let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        let source = LocalFileSource(id: "device", title: "On this iPhone", root: documents)
        sources["device"] = source
        return source
    }

    func folderSource(_ folder: SavedFolder) throws -> FileSource {
        if let cached = sources["folder:" + folder.id] { return cached }
        var stale = false
        guard let url = try? URL(resolvingBookmarkData: folder.bookmark, options: [], relativeTo: nil, bookmarkDataIsStale: &stale) else {
            throw FileSourceError("\(folder.name) is no longer available. Remove it and add it again.")
        }
        let granted = url.startAccessingSecurityScopedResource()
        if stale, let fresh = try? url.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil) {
            var updated = folder
            updated.bookmark = fresh
            folders = folders.map { $0.id == folder.id ? updated : $0 }
            persistFolders()
        }
        let source = LocalFileSource(id: "folder:" + folder.id, title: folder.name, root: url) {
            if granted { url.stopAccessingSecurityScopedResource() }
        }
        sources[source.id] = source
        return source
    }

    func addFolder(_ url: URL) throws {
        let granted = url.startAccessingSecurityScopedResource()
        defer { if granted { url.stopAccessingSecurityScopedResource() } }
        guard let bookmark = try? url.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil) else {
            throw FileSourceError("ForkMesh cannot keep access to that folder.")
        }
        folders.append(SavedFolder(id: UUID().uuidString, name: url.lastPathComponent, bookmark: bookmark))
        persistFolders()
    }

    func removeFolder(_ folder: SavedFolder) {
        folders.removeAll { $0.id == folder.id }
        sources["folder:" + folder.id] = nil
        persistFolders()
    }

    private func persistFolders() {
        if let data = try? JSONEncoder().encode(folders) { UserDefaults.standard.set(data, forKey: Self.foldersKey) }
    }

    func savedPassword(_ share: NetworkShare) -> String? { store.password(for: share) }

    func canConnectWithoutAsking(_ share: NetworkShare) -> Bool {
        share.guest || sources["smb:" + share.id] != nil || savedPassword(share) != nil
    }

    func client(_ share: NetworkShare, password: String, shareName: String? = nil) -> SmbClient {
        let address = share.parsed ?? SmbAddress(host: share.address)
        return SmbClient(
            host: address.host, port: address.port, share: shareName ?? share.share,
            credentials: SmbCredentials(user: share.user, password: password, guest: share.guest),
            connector: SmbNetworkTransport.connector())
    }

    func connect(_ share: NetworkShare, password: String? = nil, save: Bool) async throws -> FileSource {
        if let cached = sources["smb:" + share.id] { return cached }
        let secret = password ?? savedPassword(share) ?? ""
        let smb = client(share, password: secret)
        do {
            _ = try await smb.list("")
        } catch {
            await smb.disconnect()
            throw error
        }
        var final = share
        if save {
            final = store.save(share, password: secret)
            reload()
        }
        let source = SmbFileSource(share: final, client: smb)
        sources[source.id] = source
        return source
    }

    func findShares(_ share: NetworkShare, password: String) async throws -> [String] {
        let smb = client(share, password: password, shareName: "")
        defer { Task { await smb.disconnect() } }
        return try await smb.shares().map(\.name)
    }

    func removeShare(_ share: NetworkShare) {
        store.remove(share.id)
        sources["smb:" + share.id] = nil
        reload()
    }

    func forget(_ id: String) {
        sources[id] = nil
    }
}
