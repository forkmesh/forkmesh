import Foundation

public protocol FileSource: AnyObject, Sendable {
    var id: String { get }
    var title: String { get }
    var isNetwork: Bool { get }
    func list(_ path: String) async throws -> [FileEntry]
    func read(_ path: String, limit: Int64, progress: FileProgress?) async throws -> Data
    func write(_ path: String, data: Data) async throws
    func download(_ path: String, to url: URL, progress: FileProgress?) async throws
    func upload(_ url: URL, to path: String, progress: FileProgress?) async throws
    func makeFolder(_ path: String) async throws
    func makeFile(_ path: String) async throws
    func delete(_ path: String, isFolder: Bool) async throws
    func rename(_ path: String, to target: String) async throws
    func describe(_ path: String) -> String
}

public struct FileSourceError: Error, Equatable, LocalizedError, Sendable {
    public var message: String
    public init(_ message: String) { self.message = message }
    public var errorDescription: String? { message }
}

public final class LocalFileSource: FileSource, @unchecked Sendable {
    public let id: String
    public let title: String
    public let root: URL
    public var isNetwork: Bool { false }
    private let onRelease: (@Sendable () -> Void)?

    public init(id: String, title: String, root: URL, onRelease: (@Sendable () -> Void)? = nil) {
        self.id = id
        self.title = title
        self.root = root.standardizedFileURL
        self.onRelease = onRelease
    }

    deinit { onRelease?() }

    public func url(_ path: String) throws -> URL {
        let clean = FileBrowsing.clean(path)
        if clean.split(separator: "/").contains("..") { throw FileSourceError("That path leaves the folder.") }
        return clean.isEmpty ? root : root.appendingPathComponent(clean)
    }

    public func list(_ path: String) async throws -> [FileEntry] {
        let folder = try url(path)
        let keys: [URLResourceKey] = [.isDirectoryKey, .fileSizeKey, .contentModificationDateKey, .isHiddenKey]
        let urls: [URL]
        do {
            urls = try FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: keys, options: [])
        } catch {
            throw FileSourceError(FileManager.default.fileExists(atPath: folder.path) ? "This folder cannot be read." : "That file or folder no longer exists.")
        }
        return urls.map { item in
            let values = try? item.resourceValues(forKeys: Set(keys))
            let name = item.lastPathComponent
            return FileEntry(
                name: name, path: FileBrowsing.join(path, name), isFolder: values?.isDirectory ?? false,
                size: Int64(values?.fileSize ?? 0), modified: values?.contentModificationDate,
                hidden: name.hasPrefix(".") || (values?.isHidden ?? false))
        }
    }

    public func read(_ path: String, limit: Int64, progress: FileProgress?) async throws -> Data {
        let file = try url(path)
        let size = (try? FileManager.default.attributesOfItem(atPath: file.path)[.size] as? NSNumber)?.int64Value ?? 0
        if size > limit { throw SmbError.tooLarge }
        guard let data = FileManager.default.contents(atPath: file.path) else { throw FileSourceError("That file cannot be read.") }
        progress?(Int64(data.count), Int64(data.count))
        return data
    }

    public func write(_ path: String, data: Data) async throws {
        do {
            try data.write(to: try url(path), options: .atomic)
        } catch let error as FileSourceError {
            throw error
        } catch {
            throw FileSourceError("Could not save \(FileBrowsing.name(path)).")
        }
    }

    public func download(_ path: String, to target: URL, progress: FileProgress?) async throws {
        try await copyFile(from: try url(path), to: target, progress: progress)
    }

    public func upload(_ source: URL, to path: String, progress: FileProgress?) async throws {
        try await copyFile(from: source, to: try url(path), progress: progress)
    }

    private func copyFile(from source: URL, to target: URL, progress: FileProgress?) async throws {
        guard let input = try? FileHandle(forReadingFrom: source) else { throw FileSourceError("Could not read \(source.lastPathComponent).") }
        defer { try? input.close() }
        guard FileManager.default.createFile(atPath: target.path, contents: nil), let output = try? FileHandle(forWritingTo: target) else {
            throw FileSourceError("Could not write \(target.lastPathComponent).")
        }
        defer { try? output.close() }
        let total = (try? FileManager.default.attributesOfItem(atPath: source.path)[.size] as? NSNumber)?.int64Value ?? 0
        var done: Int64 = 0
        while true {
            try Task.checkCancellation()
            let chunk = try input.read(upToCount: 1 << 20) ?? Data()
            if chunk.isEmpty { break }
            try output.write(contentsOf: chunk)
            done += Int64(chunk.count)
            progress?(done, total)
        }
    }

    public func makeFolder(_ path: String) async throws {
        let target = try url(path)
        if FileManager.default.fileExists(atPath: target.path) { throw FileSourceError("Something with that name is already there.") }
        do {
            try FileManager.default.createDirectory(at: target, withIntermediateDirectories: false)
        } catch {
            throw FileSourceError("Could not create the folder.")
        }
    }

    public func makeFile(_ path: String) async throws {
        let target = try url(path)
        if FileManager.default.fileExists(atPath: target.path) { throw FileSourceError("Something with that name is already there.") }
        guard FileManager.default.createFile(atPath: target.path, contents: Data()) else { throw FileSourceError("Could not create the file.") }
    }

    public func delete(_ path: String, isFolder: Bool) async throws {
        guard !FileBrowsing.clean(path).isEmpty else { throw FileSourceError("The top folder cannot be deleted.") }
        let target = try url(path)
        guard target.path.hasPrefix(root.path + "/") else { throw FileSourceError("That path leaves the folder.") }
        try erase(target.path)
    }

    private func erase(_ path: String) throws {
        guard let type = (try? FileManager.default.attributesOfItem(atPath: path))?[.type] as? FileAttributeType else {
            throw FileSourceError("That file or folder no longer exists.")
        }
        if type == .typeDirectory {
            for name in (try? FileManager.default.contentsOfDirectory(atPath: path)) ?? [] {
                try Task.checkCancellation()
                try erase(path + "/" + name)
            }
            guard rmdir(path) == 0 else { throw FileSourceError("Could not delete \((path as NSString).lastPathComponent).") }
        } else {
            guard unlink(path) == 0 else { throw FileSourceError("Could not delete \((path as NSString).lastPathComponent).") }
        }
    }

    public func rename(_ path: String, to target: String) async throws {
        let from = try url(path), to = try url(target)
        if FileManager.default.fileExists(atPath: to.path) { throw FileSourceError("Something with that name is already there.") }
        do {
            try FileManager.default.moveItem(at: from, to: to)
        } catch {
            throw FileSourceError("Could not rename \(FileBrowsing.name(path)).")
        }
    }

    public func describe(_ path: String) -> String {
        (try? url(path).path) ?? root.path
    }
}

public final class SmbFileSource: FileSource, @unchecked Sendable {
    public let id: String
    public let title: String
    public let share: NetworkShare
    public let client: SmbClient
    public var isNetwork: Bool { true }

    public init(share: NetworkShare, client: SmbClient) {
        id = "smb:" + share.id
        title = share.title
        self.share = share
        self.client = client
    }

    deinit {
        let client = client
        Task { await client.disconnect() }
    }

    public func list(_ path: String) async throws -> [FileEntry] {
        try await client.list(path).map { entry in
            FileEntry(
                name: entry.name, path: FileBrowsing.join(path, entry.name), isFolder: entry.attributes & SmbAttributes.directory != 0,
                size: Int64(entry.size), modified: Ntlm.date(fileTime: entry.modified),
                hidden: entry.attributes & SmbAttributes.hidden != 0 || entry.name.hasPrefix("."))
        }
    }

    public func read(_ path: String, limit: Int64, progress: FileProgress?) async throws -> Data {
        Data(try await client.read(path, limit: limit, progress: progress))
    }

    public func write(_ path: String, data: Data) async throws { try await client.write(path, data: Array(data)) }
    public func download(_ path: String, to url: URL, progress: FileProgress?) async throws { try await client.download(path, to: url, progress: progress) }
    public func upload(_ url: URL, to path: String, progress: FileProgress?) async throws { try await client.upload(url, to: path, progress: progress) }
    public func makeFolder(_ path: String) async throws { try await client.makeFolder(path) }
    public func makeFile(_ path: String) async throws { try await client.makeFile(path) }
    public func delete(_ path: String, isFolder: Bool) async throws { try await client.delete(path, isFolder: isFolder) }
    public func rename(_ path: String, to target: String) async throws { try await client.rename(path, to: target) }
    public func describe(_ path: String) -> String { share.url(for: path) }
}

public enum FileTransferMode: Sendable {
    case copy, move
}

public enum FileTransfer {
    public static func temporaryURL(_ name: String) -> URL {
        FileManager.default.temporaryDirectory.appendingPathComponent("files-\(UUID().uuidString)-\(name)")
    }

    public static func discard(_ url: URL) {
        _ = unlink(url.path)
    }

    @discardableResult
    public static func paste(
        _ entry: FileEntry, from source: FileSource, to destination: FileSource, into folder: String, mode: FileTransferMode,
        progress: FileProgress? = nil
    ) async throws -> String {
        if source === destination, entry.isFolder, FileBrowsing.isInside(folder, folder: entry.path) {
            throw FileSourceError("A folder cannot go inside itself.")
        }
        if source === destination, mode == .move, FileBrowsing.parent(entry.path) == FileBrowsing.clean(folder) {
            return entry.path
        }
        let existing = Set(try await destination.list(folder).map(\.name))
        let target = FileBrowsing.join(folder, FileBrowsing.uniqueName(entry.name, existing: existing))
        if source === destination, mode == .move {
            try await source.rename(entry.path, to: target)
            return target
        }
        try await copy(entry, from: source, to: destination, target: target, progress: progress)
        if mode == .move { try await source.delete(entry.path, isFolder: entry.isFolder) }
        return target
    }

    static func copy(_ entry: FileEntry, from source: FileSource, to destination: FileSource, target: String, progress: FileProgress?) async throws {
        try Task.checkCancellation()
        if entry.isFolder {
            try await destination.makeFolder(target)
            for child in try await source.list(entry.path) {
                try await copy(child, from: source, to: destination, target: FileBrowsing.join(target, child.name), progress: progress)
            }
            return
        }
        let temp = temporaryURL(entry.name)
        defer { discard(temp) }
        try await source.download(entry.path, to: temp, progress: progress)
        try await destination.upload(temp, to: target, progress: progress)
    }
}
