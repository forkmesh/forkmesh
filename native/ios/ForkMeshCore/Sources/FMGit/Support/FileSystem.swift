// File-system helpers (compare the Kotlin port's worktree/DartIo.kt).
// FileManager does the directory work; lstat, O_EXCL, fsync and rename(2) go
// straight to the C library because FileManager has no equivalent with the
// same guarantees (exclusive create, replace-on-rename, data on disk before
// the rename).

import Foundation

#if canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
#elseif canImport(Musl)
import Musl
#endif

/// The kind of a file-system entry; pipes, sockets and devices are `other`.
public enum GitFileSystemEntityType: Sendable {
    case file, directory, link, notFound, other
}

/// A failed file-system operation.
public struct GitFileSystemError: Error, CustomStringConvertible, Sendable {
    public let operation: String
    public let path: String
    public let code: Int32

    public var description: String {
        "GitFileSystemError(\(operation) \(path): errno \(code))"
    }
}

enum GitFS {
    static let separator = "/"

    static func join(_ parent: String, _ child: String) -> String { parent + separator + child }

    /// `File(path).parent.path`.
    static func parent(_ path: String) -> String {
        let bytes = Array(path.utf8)
        var end = bytes.count
        while end > 1, bytes[end - 1] == 0x2f { end -= 1 }
        var index = end - 1
        while index >= 0, bytes[index] != 0x2f { index -= 1 }
        if index < 0 { return "." }
        if index == 0 { return "/" }
        return String(decoding: bytes[0..<index], as: UTF8.self)
    }

    /// The last path component.
    static func basename(_ path: String) -> String {
        let bytes = Array(path.utf8)
        var end = bytes.count
        while end > 1, bytes[end - 1] == 0x2f { end -= 1 }
        var index = end - 1
        while index >= 0, bytes[index] != 0x2f { index -= 1 }
        return String(decoding: bytes[(index + 1)..<end], as: UTF8.self)
    }

    /// `FileSystemEntity.type(path, followLinks:)`.
    static func entityType(_ path: String, followLinks: Bool = false) -> GitFileSystemEntityType {
        var info = stat()
        let status = followLinks ? stat(path, &info) : lstat(path, &info)
        if status != 0 { return .notFound }
        switch info.st_mode & S_IFMT {
        case S_IFLNK: return .link
        case S_IFDIR: return .directory
        case S_IFREG: return .file
        default: return .other
        }
    }

    /// `File(path).exists()`: a regular file, following links.
    static func fileExists(_ path: String) -> Bool { entityType(path, followLinks: true) == .file }

    /// `Directory(path).exists()`: a directory, following links.
    static func directoryExists(_ path: String) -> Bool { entityType(path, followLinks: true) == .directory }

    /// `Directory(path).create(recursive: true)`.
    static func createDirectories(_ path: String) throws {
        do {
            try FileManager.default.createDirectory(atPath: path, withIntermediateDirectories: true)
        } catch {
            if !directoryExists(path) { throw GitFileSystemError(operation: "mkdir -p", path: path, code: errno) }
        }
    }

    /// `Directory(path).create()`: no error when the directory already exists.
    static func createDirectory(_ path: String) throws {
        if mkdir(path, 0o777) != 0 {
            let code = errno
            if code == EEXIST, directoryExists(path) { return }
            throw GitFileSystemError(operation: "mkdir", path: path, code: code)
        }
    }

    /// `File(path).create(exclusive: true)`; false when the file exists or cannot be created.
    static func createFileExclusive(_ path: String) -> Bool {
        let descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0o666)
        if descriptor < 0 { return false }
        close(descriptor)
        return true
    }

    /// `File(path).writeAsBytes(bytes, flush: true)`.
    static func writeBytesFlushed(_ path: String, _ bytes: [UInt8]) throws {
        try writeBytes(path, bytes, flush: true)
    }

    static func writeBytes(_ path: String, _ bytes: [UInt8], flush: Bool) throws {
        let descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0o666)
        if descriptor < 0 { throw GitFileSystemError(operation: "open", path: path, code: errno) }
        var failure: Int32?
        bytes.withUnsafeBytes { buffer in
            var offset = 0
            while offset < buffer.count {
                let written = write(descriptor, buffer.baseAddress! + offset, buffer.count - offset)
                if written < 0 {
                    if errno == EINTR { continue }
                    failure = errno
                    return
                }
                offset += written
            }
        }
        if failure == nil, flush, fsync(descriptor) != 0, errno != EINVAL, errno != EROFS { failure = errno }
        close(descriptor)
        if let failure { throw GitFileSystemError(operation: "write", path: path, code: failure) }
    }

    static func writeStringFlushed(_ path: String, _ text: String) throws {
        try writeBytesFlushed(path, Array(text.utf8))
    }

    /// `File(path).readAsBytes()`.
    static func readBytes(_ path: String) throws -> [UInt8] {
        let descriptor = open(path, O_RDONLY)
        if descriptor < 0 { throw GitFileSystemError(operation: "open", path: path, code: errno) }
        defer { close(descriptor) }
        var output = [UInt8]()
        var info = stat()
        if fstat(descriptor, &info) == 0, info.st_size > 0 { output.reserveCapacity(Int(info.st_size)) }
        var chunk = [UInt8](repeating: 0, count: 256 * 1024)
        while true {
            let count = chunk.withUnsafeMutableBytes { read(descriptor, $0.baseAddress, $0.count) }
            if count < 0 {
                if errno == EINTR { continue }
                throw GitFileSystemError(operation: "read", path: path, code: errno)
            }
            if count == 0 { break }
            output.append(contentsOf: chunk[0..<count])
        }
        return output
    }

    /// `File(path).length()`.
    static func fileLength(_ path: String) throws -> Int {
        var info = stat()
        if stat(path, &info) != 0 { throw GitFileSystemError(operation: "stat", path: path, code: errno) }
        return Int(info.st_size)
    }

    /// `File(path).rename(newPath)`: atomic, replaces an existing file.
    static func rename(_ from: String, _ to: String) throws {
        #if canImport(Darwin)
        let status = Darwin.rename(from, to)
        #elseif canImport(Glibc)
        let status = Glibc.rename(from, to)
        #else
        let status = Musl.rename(from, to)
        #endif
        if status != 0 { throw GitFileSystemError(operation: "rename", path: from, code: errno) }
    }

    /// `File(path).delete()` / `Link(path).delete()`.
    static func deleteFile(_ path: String) throws {
        if unlink(path) != 0 { throw GitFileSystemError(operation: "unlink", path: path, code: errno) }
    }

    /// `Directory(path).delete()`: the directory must be empty.
    static func deleteEmptyDirectory(_ path: String) throws {
        if rmdir(path) != 0 { throw GitFileSystemError(operation: "rmdir", path: path, code: errno) }
    }

    /// `FileSystemEntity.delete(recursive: true)` without following links.
    static func deleteRecursive(_ path: String) throws {
        switch entityType(path) {
        case .notFound:
            return
        case .directory:
            for name in try listNames(path) { try deleteRecursive(join(path, name)) }
            try deleteEmptyDirectory(path)
        default:
            try deleteFile(path)
        }
    }

    /// `Directory(path).list(followLinks: false)`: names of the direct entries.
    static func listNames(_ path: String) throws -> [String] {
        do {
            return try FileManager.default.contentsOfDirectory(atPath: path)
        } catch {
            throw GitFileSystemError(operation: "list", path: path, code: errno)
        }
    }

    /// `Directory(path).list(recursive: true, followLinks: false)`: every
    /// entity below `root` (not `root` itself) with its type, links not entered.
    static func walk(_ root: String, _ visit: (String, GitFileSystemEntityType) throws -> Void) throws {
        for name in try listNames(root) {
            let path = join(root, name)
            let type = entityType(path)
            try visit(path, type)
            if type == .directory { try walk(path, visit) }
        }
    }

    /// `Link(path).target()`.
    static func readLink(_ path: String) throws -> String {
        var buffer = [UInt8](repeating: 0, count: 4097)
        let count = buffer.withUnsafeMutableBytes {
            readlink(path, $0.baseAddress!.assumingMemoryBound(to: CChar.self), 4096)
        }
        if count < 0 { throw GitFileSystemError(operation: "readlink", path: path, code: errno) }
        return String(decoding: buffer[0..<count], as: UTF8.self)
    }

    /// `Link(path).create(target)`.
    static func createLink(_ path: String, target: String) throws {
        if symlink(target, path) != 0 { throw GitFileSystemError(operation: "symlink", path: path, code: errno) }
    }

    /// POSIX permission bits (following links), or nil when absent.
    static func mode(_ path: String) -> Int? {
        var info = stat()
        if stat(path, &info) != 0 { return nil }
        return Int(info.st_mode) & 0o7777
    }

    static func setMode(_ path: String, _ mode: Int) throws {
        if chmod(path, mode_t(mode)) != 0 { throw GitFileSystemError(operation: "chmod", path: path, code: errno) }
    }

    /// `File(path).lastModified()` without following a final link.
    static func modified(_ path: String) -> Date? {
        (try? FileManager.default.attributesOfItem(atPath: path))?[.modificationDate] as? Date
    }

    /// `DateTime.now().microsecondsSinceEpoch`.
    static func nowMicros() -> Int64 {
        var time = timeval()
        gettimeofday(&time, nil)
        return Int64(time.tv_sec) * 1_000_000 + Int64(time.tv_usec)
    }
}
