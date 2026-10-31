import FMBase
import FMCrypto
import Foundation

public enum DataFolderNeed: Equatable, Sendable {
    case chooseFolder
    case reconnectFolder
}

public struct DataFolderProject: Equatable, Sendable {
    public let slug: String
    public let projectId: String
    public let namespace: String
    public let name: String
    public let publicKey: String

    public init(slug: String, projectId: String, namespace: String, name: String, publicKey: String) {
        self.slug = slug
        self.projectId = projectId
        self.namespace = namespace
        self.name = name
        self.publicKey = publicKey
    }
}

public struct DataFolderTree: Sendable {
    public let name: String
    public let local: URL
    public let skippedTopLevel: Set<String>

    public init(name: String, local: URL, skippedTopLevel: Set<String> = []) {
        self.name = name
        self.local = local
        self.skippedTopLevel = skippedTopLevel
    }
}

public struct DataFolderIndexEntry: Equatable, Sendable {
    public let local: String
    public let signature: String
}

public struct DataFolderCopyReport: Equatable, Sendable {
    public var copied: Int = 0
    public var unchanged: Int = 0
    public var failed: [String] = []

    public init() {}
}

public enum DataFolder {
    public static let defaultFolderName = "ForkMesh projects"
    public static let homeProject = "forkmesh"
    public static let markerFile = "forkmesh-data.json"
    public static let markerType = "forkmesh.data-folder"
    public static let projectFile = "project.json"
    public static let settingsPath = "data/core/config/ForkMesh/settings.json"
    public static let kitPath = "data/core/identity/recovery-kit.json"
    public static let indexPath = "data/core/state/sync-index.json"
    public static let roots = ["data/core", "data/core/config", "data/core/cache", "data/core/state",
                               "data/core/logs", "data/core/ssh"]
    public static let pluginEntries: [String: String] = [
        "chat-history": "chat/history",
        "local_mesh_history.json": "chat/local-mesh/local_mesh_history.json",
        "chat-people-pictures": "chat/people-pictures",
        "diagnostics": "core/logs/diagnostics",
        "NextTask": "agents/next-task",
        "secure-drive": "secure-drive",
        "rss-reader": "rss-reader",
    ]
    public static let partialSuffix = ".fm-partial"

    public static let locationKey = "dataFolder/location"
    public static let syncedAtKey = "dataFolder/syncedAt"
    public static let kitKeyPrefix = "dataFolder/kitKey/"
    public static let devicePrefixes = ["dataFolder/", "install/"]
    public static let skippedNames: Set<String> = ["LocalModels", "CodeInsight", ".Trash", "tmp"]

    public static let alertTitle = "Keep your ForkMesh data safe"
    public static let alertMessage =
        "ForkMesh keeps your projects in its own folder, and the phone deletes that folder with the app. Choose a folder in Files, for example On My iPhone or iCloud Drive: each project keeps its data there, data/core for settings and the identity key and one folder per plugin, so you can reinstall and pick the same folder to get everything back."
    public static let reconnectMessage =
        "The folder holding your ForkMesh projects can't be opened. Choose it again so ForkMesh can keep your data safe."
    public static let folderNameHint =
        "Suggested name: “\(defaultFolderName)”. In the picker, make a new folder with that name and choose it."
    public static let insideApp =
        "That folder is inside ForkMesh's own storage, which is deleted with the app. Pick a folder outside it, for example in On My iPhone or iCloud Drive."

    public static func projectFolder(_ scope: ProjectScope) -> String {
        scope.isLegacy ? homeProject : scope.slug
    }

    public static func folderPath(tree: String, relative: String) -> String {
        let first = String(relative.split(separator: "/", maxSplits: 1).first ?? "")
        if let mapped = pluginEntries[first] {
            return "data/" + mapped + relative.dropFirst(first.count)
        }
        return "data/core/\(tree)/\(relative)"
    }

    public static func problem(with picked: URL, appContainer: URL) -> String? {
        let folder = normalized(picked)
        let container = normalized(appContainer)
        if folder == container || folder.hasPrefix(container + "/") { return insideApp }
        return nil
    }

    static func normalized(_ url: URL) -> String {
        let path = url.resolvingSymlinksInPath().standardizedFileURL.path
        if path.hasPrefix("/private/var/") { return String(path.dropFirst("/private".count)) }
        return path
    }

    public static func kitKey(_ slug: String) -> String { kitKeyPrefix + slug }

    public static func need(reachable: Bool, insideApp: Bool) -> DataFolderNeed? {
        if !reachable { return .reconnectFolder }
        if insideApp { return .chooseFolder }
        return nil
    }

    public static func identitySaved(prefs: KeyValueStore, slug: String, publicKey: String) -> Bool {
        publicKey.isEmpty || prefs.string(forKey: kitKey(slug)) == publicKey
    }

    public static func message(for need: DataFolderNeed) -> String {
        switch need {
        case .chooseFolder: return alertMessage
        case .reconnectFolder: return reconnectMessage
        }
    }

    public static func isDeviceKey(_ key: String) -> Bool {
        devicePrefixes.contains { key.hasPrefix($0) }
    }

    public static func exportSettings(_ store: KeyValueStore) -> JSONObject {
        var values: JSONObject = [:]
        for key in store.keys.sorted() where !isDeviceKey(key) {
            if let list = store.stringList(forKey: key) {
                values[key] = .array(list.map { .string($0) })
            } else if let text = store.string(forKey: key) {
                values[key] = .string(text)
            } else if let flag = store.bool(forKey: key) {
                values[key] = .bool(flag)
            } else if let number = store.int(forKey: key) {
                values[key] = .int(Int64(number))
            } else if let number = store.double(forKey: key) {
                values[key] = .double(number)
            }
        }
        return ["type": .string("forkmesh.mobile-settings"), "schemaVersion": .int(1), "values": .object(values)]
    }

    @discardableResult
    public static func applySettings(_ document: JSONObject, to store: KeyValueStore) -> Int {
        guard document.str("type") == "forkmesh.mobile-settings",
              let values = document["values"]?.objectValue else { return 0 }
        var applied = 0
        for (key, value) in values where !isDeviceKey(key) {
            switch value {
            case .string(let text): store.set(text, forKey: key)
            case .bool(let flag): store.set(flag, forKey: key)
            case .int(let number): store.set(Int(truncatingIfNeeded: number), forKey: key)
            case .double(let number): store.set(number, forKey: key)
            case .array(let items): store.set(items.compactMap(\.stringValue), forKey: key)
            default: continue
            }
            applied += 1
        }
        return applied
    }

    public static func signature(size: Int64, modified: Date) -> String {
        "\(size):\(Int64((modified.timeIntervalSince1970 * 1_000).rounded()))"
    }
}

public final class DataFolderStore: @unchecked Sendable {
    public let root: URL
    private let files: FileManager

    public init(root: URL, fileManager: FileManager = .default) {
        self.root = root
        self.files = fileManager
    }

    public var hasData: Bool { files.fileExists(atPath: root.appendingPathComponent(DataFolder.markerFile).path) }

    public func projectDir(_ slug: String) -> URL { root.appendingPathComponent(slug, isDirectory: true) }

    public func prepare(platform: String, nowMillis: Int64) throws {
        try files.createDirectory(at: root, withIntermediateDirectories: true)
        let marker = root.appendingPathComponent(DataFolder.markerFile)
        var object = (try? Data(contentsOf: marker)).map { JSON.parseObject($0) } ?? [:]
        if object.str("type") != DataFolder.markerType {
            object = ["type": .string(DataFolder.markerType), "schemaVersion": .int(1),
                      "createdAt": .int(nowMillis)]
        }
        object["updatedAt"] = .int(nowMillis)
        object["updatedBy"] = .string(platform)
        try write(Data(JSON.encodePretty(.object(object)).utf8), to: marker)
    }

    public func prepareProject(_ project: DataFolderProject) throws {
        let dir = projectDir(project.slug)
        for name in DataFolder.roots {
            try files.createDirectory(at: dir.appendingPathComponent(name, isDirectory: true), withIntermediateDirectories: true)
        }
        let object: JSONObject = [
            "slug": .string(project.slug), "projectId": .string(project.projectId),
            "namespace": .string(project.namespace), "name": .string(project.name),
            "publicKey": .string(project.publicKey),
        ]
        try write(Data(JSON.encodePretty(.object(object)).utf8), to: dir.appendingPathComponent(DataFolder.projectFile))
    }

    public func projects() -> [DataFolderProject] {
        guard let names = try? files.contentsOfDirectory(atPath: root.path) else { return [] }
        return names.sorted().compactMap { name in
            let file = projectDir(name).appendingPathComponent(DataFolder.projectFile)
            guard let data = try? Data(contentsOf: file) else { return nil }
            let object = JSON.parseObject(data)
            guard object.str("slug") == name else { return nil }
            return DataFolderProject(
                slug: name, projectId: object.str("projectId"), namespace: object.str("namespace"),
                name: object.str("name"), publicKey: object.str("publicKey"))
        }
    }

    public func writeSettings(_ document: JSONObject, slug: String) throws {
        try write(Data(JSON.encodePretty(.object(document)).utf8),
                  to: projectDir(slug).appendingPathComponent(DataFolder.settingsPath))
    }

    public func readSettings(slug: String) -> JSONObject? {
        guard let data = try? Data(contentsOf: projectDir(slug).appendingPathComponent(DataFolder.settingsPath)) else { return nil }
        let object = JSON.parseObject(data)
        return object.isEmpty ? nil : object
    }

    public func writeKit(_ text: String, slug: String) throws {
        try write(Data(text.utf8), to: projectDir(slug).appendingPathComponent(DataFolder.kitPath))
    }

    public func readKit(slug: String) -> String? {
        guard let data = try? Data(contentsOf: projectDir(slug).appendingPathComponent(DataFolder.kitPath)) else { return nil }
        return String(data: data, encoding: .utf8)
    }

    public func backup(slug: String, trees: [DataFolderTree]) -> DataFolderCopyReport {
        var report = DataFolderCopyReport()
        let dir = projectDir(slug)
        var index = readIndex(slug: slug)
        for tree in trees {
            for (relative, url, signature) in walk(tree.local, skippedTopLevel: tree.skippedTopLevel) {
                let key = DataFolder.folderPath(tree: tree.name, relative: relative)
                let local = tree.name + "/" + relative
                let target = dir.appendingPathComponent(key)
                if index[key]?.signature == signature && index[key]?.local == local &&
                    files.fileExists(atPath: target.path) {
                    report.unchanged += 1
                    continue
                }
                do {
                    try copy(url, to: target)
                    index[key] = DataFolderIndexEntry(local: local, signature: signature)
                    report.copied += 1
                } catch {
                    report.failed.append(key)
                }
            }
        }
        writeIndex(index, slug: slug)
        return report
    }

    public func restore(slug: String, trees: [DataFolderTree]) -> DataFolderCopyReport {
        var report = DataFolderCopyReport()
        let dir = projectDir(slug)
        let byName = Dictionary(trees.map { ($0.name, $0) }, uniquingKeysWith: { first, _ in first })
        for (key, entry) in readIndex(slug: slug).sorted(by: { $0.key < $1.key }) {
            let parts = entry.local.split(separator: "/", omittingEmptySubsequences: false).map(String.init)
            guard parts.count >= 2, let tree = byName[parts[0]],
                  !parts.contains(where: { $0 == ".." || $0 == "." || $0.isEmpty }),
                  !key.split(separator: "/").contains(where: { $0 == ".." })
            else { continue }
            let relative = parts.dropFirst().joined(separator: "/")
            let target = tree.local.appendingPathComponent(relative)
            if files.fileExists(atPath: target.path) {
                report.unchanged += 1
                continue
            }
            do {
                try copy(dir.appendingPathComponent(key), to: target)
                report.copied += 1
            } catch {
                report.failed.append(key)
            }
        }
        return report
    }

    func readIndex(slug: String) -> [String: DataFolderIndexEntry] {
        guard let data = try? Data(contentsOf: projectDir(slug).appendingPathComponent(DataFolder.indexPath)) else { return [:] }
        var index: [String: DataFolderIndexEntry] = [:]
        for (key, value) in JSON.parseObject(data) {
            guard let object = value.objectValue else { continue }
            let local = object.str("local")
            if !local.isEmpty { index[key] = DataFolderIndexEntry(local: local, signature: object.str("sig")) }
        }
        return index
    }

    private func writeIndex(_ index: [String: DataFolderIndexEntry], slug: String) {
        var object: JSONObject = [:]
        for key in index.keys.sorted() {
            guard let entry = index[key] else { continue }
            object[key] = .object(["local": .string(entry.local), "sig": .string(entry.signature)])
        }
        try? write(Data(JSON.encode(object).utf8), to: projectDir(slug).appendingPathComponent(DataFolder.indexPath))
    }

    private func walk(_ base: URL, skippedTopLevel: Set<String>) -> [(String, URL, String)] {
        let basePath = base.standardizedFileURL.path
        guard files.fileExists(atPath: basePath),
              let enumerator = files.enumerator(
                at: base, includingPropertiesForKeys: [.isRegularFileKey, .isSymbolicLinkKey, .fileSizeKey, .contentModificationDateKey])
        else { return [] }
        var found: [(String, URL, String)] = []
        for case let url as URL in enumerator {
            let path = url.standardizedFileURL.path
            guard path.hasPrefix(basePath + "/") else { continue }
            let relative = String(path.dropFirst(basePath.count + 1))
            let components = relative.split(separator: "/").map(String.init)
            if let first = components.first, skippedTopLevel.contains(first) || DataFolder.skippedNames.contains(first) {
                enumerator.skipDescendants()
                continue
            }
            if components.contains(where: { DataFolder.skippedNames.contains($0) }) {
                enumerator.skipDescendants()
                continue
            }
            let values = try? url.resourceValues(forKeys: [.isRegularFileKey, .isSymbolicLinkKey, .fileSizeKey, .contentModificationDateKey])
            guard values?.isSymbolicLink != true, values?.isRegularFile == true,
                  !relative.hasSuffix(DataFolder.partialSuffix) else { continue }
            let signature = DataFolder.signature(
                size: Int64(values?.fileSize ?? 0), modified: values?.contentModificationDate ?? Date(timeIntervalSince1970: 0))
            found.append((relative, url, signature))
        }
        return found.sorted { $0.0 < $1.0 }
    }

    private func copy(_ source: URL, to target: URL) throws {
        try files.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
        let partial = target.deletingLastPathComponent()
            .appendingPathComponent("\(target.lastPathComponent).\(UUID().uuidString)\(DataFolder.partialSuffix)")
        try files.copyItem(at: source, to: partial)
        guard rename(partial.path, target.path) == 0 else {
            throw CocoaError(.fileWriteUnknown, userInfo: [NSFilePathErrorKey: target.path])
        }
    }

    private func write(_ data: Data, to target: URL) throws {
        try files.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
        try data.write(to: target, options: .atomic)
    }
}

public enum DataFolderIdentity {
    public static func saveKit(identity: DeviceIdentity, passphrase: String, confirm: String, slug: String,
                               store: DataFolderStore, prefs: KeyValueStore, nowMillis: Int64) throws {
        let kit = try RecoveryKit.export(identity: identity, passphrase: passphrase, confirm: confirm, createdAt: nowMillis)
        try store.writeKit(kit, slug: slug)
        prefs.set(identity.publicKeyB64url, forKey: DataFolder.kitKey(slug))
    }

    public static func restoreSeed(kit: String, passphrase: String, namespace: String, secureStore: SecureStore) throws -> String {
        var seed = try RecoveryKit.openSeed(kit: kit, passphrase: passphrase)
        defer { CryptoBytes.wipe(&seed) }
        let publicKey = B64.encodeUrlNoPadding(try Ed25519.publicKey(seed: seed))
        if ChatKeyring.storedSeed(secureStore.read(namespace + Identity.seedKey)) != seed {
            secureStore.delete(namespace + ChatKeyring.identityStoreKey)
        }
        secureStore.write(namespace + Identity.seedKey, B64.encode(seed))
        return publicKey
    }
}
