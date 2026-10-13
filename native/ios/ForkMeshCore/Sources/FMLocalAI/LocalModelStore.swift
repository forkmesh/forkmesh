// Where downloaded models live on disk, and how the app tells a complete
// download from a partial one.

import FMBase
import Foundation

/// Layout under the root (Application Support/LocalModels in the app):
///
///     <root>/<org>/<name>/                  the snapshot; what MLX loads
///     <root>/<org>/<name>/.partial/<file>   a file still downloading
///     <root>/<org>/<name>/.forkmesh-snapshot.json   written last
///
/// A model counts as downloaded only when the manifest exists and every file
/// it lists is present at its recorded size, so a crash mid-download can never
/// hand MLX a truncated checkpoint.
public final class LocalModelStore: @unchecked Sendable {
    public static let manifestName = ".forkmesh-snapshot.json"
    public static let partialDirectoryName = ".partial"

    public let root: URL
    private let fileManager: FileManager

    public init(root: URL, fileManager: FileManager = .default) {
        self.root = root
        self.fileManager = fileManager
    }

    /// Application Support/LocalModels, created and kept out of backups: a
    /// multi-gigabyte model is re-downloadable and has no place in iCloud.
    public static func defaultRoot(fileManager: FileManager = .default) throws -> URL {
        let support = try fileManager.url(
            for: .applicationSupportDirectory, in: .userDomainMask, appropriateFor: nil, create: true
        )
        let root = support.appendingPathComponent("LocalModels", isDirectory: true)
        try fileManager.createDirectory(at: root, withIntermediateDirectories: true)
        excludeFromBackup(root)
        return root
    }

    static func excludeFromBackup(_ url: URL) {
        #if canImport(Darwin)
        var values = URLResourceValues()
        values.isExcludedFromBackup = true
        var target = url
        try? target.setResourceValues(values)
        #endif
    }

    public func directory(for model: LocalAIModel) -> URL {
        var url = root
        for component in model.repo.split(separator: "/") where !component.isEmpty && component != ".." && component != "." {
            url.appendPathComponent(String(component), isDirectory: true)
        }
        return url
    }

    public func partialDirectory(for model: LocalAIModel) -> URL {
        directory(for: model).appendingPathComponent(LocalModelStore.partialDirectoryName, isDirectory: true)
    }

    public func manifestURL(for model: LocalAIModel) -> URL {
        directory(for: model).appendingPathComponent(LocalModelStore.manifestName)
    }

    // MARK: - Manifest

    public func readManifest(for model: LocalAIModel) -> SnapshotManifest? {
        guard let data = try? Data(contentsOf: manifestURL(for: model)),
              let value = try? JSON.parse(data),
              let object = value.objectValue else { return nil }
        return SnapshotManifest(json: object)
    }

    func writeManifest(_ manifest: SnapshotManifest, for model: LocalAIModel) throws {
        let data = Utf8.encode(JSON.encodePretty(.object(manifest.json)))
        try data.write(to: manifestURL(for: model), options: .atomic)
    }

    /// Manifest present, for this revision, and every file on disk at its size.
    public func isComplete(_ model: LocalAIModel) -> Bool {
        guard let manifest = readManifest(for: model),
              manifest.repo == model.repo, manifest.revision == model.revision,
              !manifest.files.isEmpty else { return false }
        let directory = directory(for: model)
        for file in manifest.files {
            guard let size = fileSize(directory.appendingPathComponent(file.path)) else { return false }
            if let expected = file.size, size != expected { return false }
        }
        return true
    }

    // MARK: - Sizes

    public func fileSize(_ url: URL) -> Int64? {
        guard let attributes = try? fileManager.attributesOfItem(atPath: url.path) else { return nil }
        if let type = attributes[.type] as? FileAttributeType, type == .typeDirectory { return nil }
        if let number = attributes[.size] as? NSNumber { return number.int64Value }
        if let int = attributes[.size] as? Int { return Int64(int) }
        if let uint = attributes[.size] as? UInt64 { return Int64(clamping: uint) }
        return nil
    }

    /// Bytes this model takes on disk, partial files included.
    public func diskUsage(for model: LocalAIModel) -> Int64 {
        totalSize(of: directory(for: model))
    }

    /// Bytes of an unfinished download, which the next download resumes from.
    public func partialBytes(for model: LocalAIModel) -> Int64 {
        totalSize(of: partialDirectory(for: model))
    }

    /// Every model under the root (catalog or not), for "space used" totals.
    public func totalDiskUsage() -> Int64 {
        totalSize(of: root)
    }

    private func totalSize(of directory: URL) -> Int64 {
        guard let enumerator = fileManager.enumerator(atPath: directory.path) else { return 0 }
        var total: Int64 = 0
        while let relative = enumerator.nextObject() as? String {
            if let size = fileSize(directory.appendingPathComponent(relative)) { total += size }
        }
        return total
    }

    /// Free space for large files. On iOS this is the "important usage"
    /// figure, which counts space the system would purge for the app.
    public func availableCapacity() -> Int64? {
        try? fileManager.createDirectory(at: root, withIntermediateDirectories: true)
        #if canImport(Darwin)
        if let values = try? root.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]),
           let important = values.volumeAvailableCapacityForImportantUsage, important > 0 {
            return important
        }
        #endif
        guard let attributes = try? fileManager.attributesOfFileSystem(forPath: root.path) else { return nil }
        if let number = attributes[.systemFreeSize] as? NSNumber { return number.int64Value }
        return nil
    }

    // MARK: - Removal

    /// Deletes the model's directory, partial files and manifest included.
    public func delete(_ model: LocalAIModel) throws {
        let directory = directory(for: model)
        if fileManager.fileExists(atPath: directory.path) {
            try fileManager.removeItem(at: directory)
        }
    }

    func ensureDirectories(for model: LocalAIModel) throws {
        try fileManager.createDirectory(at: partialDirectory(for: model), withIntermediateDirectories: true)
        LocalModelStore.excludeFromBackup(root)
    }

    /// Moves a finished partial file into place, replacing any older copy.
    func promote(_ partial: URL, to destination: URL) throws {
        let parent = destination.deletingLastPathComponent()
        try fileManager.createDirectory(at: parent, withIntermediateDirectories: true)
        if fileManager.fileExists(atPath: destination.path) {
            try fileManager.removeItem(at: destination)
        }
        try fileManager.moveItem(at: partial, to: destination)
    }

    func removePartialDirectory(for model: LocalAIModel) {
        try? fileManager.removeItem(at: partialDirectory(for: model))
    }

    func removeManifest(for model: LocalAIModel) {
        try? fileManager.removeItem(at: manifestURL(for: model))
    }
}

/// What a finished download recorded.
public struct SnapshotManifest: Sendable, Equatable {
    public var repo: String
    public var revision: String
    public var files: [HubFile]
    public var completedAtMs: Int64

    public init(repo: String, revision: String, files: [HubFile], completedAtMs: Int64) {
        self.repo = repo
        self.revision = revision
        self.files = files
        self.completedAtMs = completedAtMs
    }

    public var totalBytes: Int64 { files.reduce(0) { $0 + ($1.size ?? 0) } }

    public var json: JSONObject {
        var object = JSONObject()
        object["repo"] = .string(repo)
        object["revision"] = .string(revision)
        object["completedAt"] = .int(completedAtMs)
        object["files"] = .array(files.map { file in
            var entry = JSONObject()
            entry["path"] = .string(file.path)
            entry["size"] = file.size.map { JSONValue.int($0) } ?? .null
            return .object(entry)
        })
        return object
    }

    public init?(json: JSONObject) {
        guard let repo = json.optStr("repo"), let revision = json.optStr("revision") else { return nil }
        self.repo = repo
        self.revision = revision
        completedAtMs = json.int64("completedAt", 0)
        files = json.maps("files").compactMap { entry in
            guard let path = entry.optStr("path"), !path.isEmpty else { return nil }
            return HubFile(path: path, size: entry["size"]?.int64Value)
        }
    }
}
