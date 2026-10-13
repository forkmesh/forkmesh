// File-system and text helpers for the engine in this folder.
//
// FMGit has the same helpers (Support/FileSystem.swift, Support/DartText.swift)
// but keeps them internal to FMGit, so this is a private copy for FMCode. The
// names carry an `eng` prefix so they can never clash with another FMCode
// file's String extensions. Like FMGit: lstat, O_EXCL, fsync and rename(2) go
// straight to the C library, and every check on a path, ref or wire token
// works on UTF-8 bytes, never on Swift Characters.

import Foundation

#if canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
#elseif canImport(Musl)
import Musl
#endif

public enum EngineFS {
    public static let separator = "/"

    public static func join(_ parent: String, _ child: String) -> String { parent + separator + child }

    /// The last path component (`entity.uri.pathSegments.last`).
    public static func basename(_ path: String) -> String {
        let bytes = Array(path.utf8)
        var end = bytes.count
        while end > 1, bytes[end - 1] == 0x2f { end -= 1 }
        var index = end - 1
        while index >= 0, bytes[index] != 0x2f { index -= 1 }
        return String(decoding: bytes[(index + 1)..<end], as: UTF8.self)
    }

    /// `FileSystemEntity.type(path, followLinks:)`.
    public static func entityType(_ path: String, followLinks: Bool = false) -> GitFileSystemEntityType {
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
    public static func fileExists(_ path: String) -> Bool { entityType(path, followLinks: true) == .file }

    /// `Directory(path).exists()`: a directory, following links.
    public static func directoryExists(_ path: String) -> Bool { entityType(path, followLinks: true) == .directory }

    /// `Directory(path).create(recursive: true)`.
    public static func createDirectories(_ path: String) throws {
        do {
            try FileManager.default.createDirectory(atPath: path, withIntermediateDirectories: true)
        } catch {
            if !directoryExists(path) { throw EngineFileSystemError(operation: "mkdir -p", path: path, code: errno) }
        }
    }

    /// `File(path).create(exclusive: true)`; false when the file exists or cannot be created.
    public static func createFileExclusive(_ path: String) -> Bool {
        let descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0o666)
        if descriptor < 0 { return false }
        close(descriptor)
        return true
    }

    /// `File(path).writeAsBytes(bytes, flush: true)`.
    public static func writeBytesFlushed(_ path: String, _ bytes: [UInt8]) throws {
        let descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0o666)
        if descriptor < 0 { throw EngineFileSystemError(operation: "open", path: path, code: errno) }
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
        if failure == nil, fsync(descriptor) != 0, errno != EINVAL, errno != EROFS { failure = errno }
        close(descriptor)
        if let failure { throw EngineFileSystemError(operation: "write", path: path, code: failure) }
    }

    public static func writeStringFlushed(_ path: String, _ text: String) throws {
        try writeBytesFlushed(path, Array(text.utf8))
    }

    /// `File(path).readAsBytes()`.
    public static func readBytes(_ path: String) throws -> [UInt8] {
        let descriptor = open(path, O_RDONLY)
        if descriptor < 0 { throw EngineFileSystemError(operation: "open", path: path, code: errno) }
        defer { close(descriptor) }
        var output = [UInt8]()
        var info = stat()
        if fstat(descriptor, &info) == 0, info.st_size > 0 { output.reserveCapacity(Int(info.st_size)) }
        var chunk = [UInt8](repeating: 0, count: 256 * 1024)
        while true {
            let count = chunk.withUnsafeMutableBytes { read(descriptor, $0.baseAddress, $0.count) }
            if count < 0 {
                if errno == EINTR { continue }
                throw EngineFileSystemError(operation: "read", path: path, code: errno)
            }
            if count == 0 { break }
            output.append(contentsOf: chunk[0..<count])
        }
        return output
    }

    /// `File(path).readAsString()`: strict UTF-8.
    public static func readString(_ path: String) throws -> String {
        guard let text = engUtf8DecodeStrict(try readBytes(path)) else {
            throw GitFormatError("file is not valid UTF-8: \(path)")
        }
        return text
    }

    /// `File(path).rename(newPath)`: atomic, replaces an existing file.
    public static func rename(_ from: String, _ to: String) throws {
        #if canImport(Darwin)
        let status = Darwin.rename(from, to)
        #elseif canImport(Glibc)
        let status = Glibc.rename(from, to)
        #else
        let status = Musl.rename(from, to)
        #endif
        if status != 0 { throw EngineFileSystemError(operation: "rename", path: from, code: errno) }
    }

    /// `File(path).delete()` / `Link(path).delete()`.
    public static func deleteFile(_ path: String) throws {
        if unlink(path) != 0 { throw EngineFileSystemError(operation: "unlink", path: path, code: errno) }
    }

    /// `FileSystemEntity.delete(recursive: true)` without following links.
    public static func deleteRecursive(_ path: String) throws {
        switch entityType(path) {
        case .notFound:
            return
        case .directory:
            for name in try listNames(path) { try deleteRecursive(join(path, name)) }
            if rmdir(path) != 0 { throw EngineFileSystemError(operation: "rmdir", path: path, code: errno) }
        default:
            try deleteFile(path)
        }
    }

    /// `Directory(path).list(followLinks: false)`: names of the direct
    /// entries, sorted by bytes so a walk is deterministic.
    public static func listNames(_ path: String) throws -> [String] {
        do {
            return try FileManager.default.contentsOfDirectory(atPath: path)
                .sorted { engCompareBytes(Array($0.utf8), Array($1.utf8)) < 0 }
        } catch {
            throw EngineFileSystemError(operation: "list", path: path, code: errno)
        }
    }

    /// `Directory(path).list(recursive: true, followLinks: false)`: every
    /// entity below `root` with its type, links not entered.
    public static func walk(_ root: String, _ visit: (String, GitFileSystemEntityType) throws -> Void) throws {
        for name in try listNames(root) {
            let path = join(root, name)
            let type = entityType(path)
            try visit(path, type)
            if type == .directory { try walk(path, visit) }
        }
    }

    /// POSIX permission bits (following links), or nil when absent.
    public static func mode(_ path: String) -> Int? {
        var info = stat()
        if stat(path, &info) != 0 { return nil }
        return Int(info.st_mode) & 0o7777
    }

    /// `lstat(path).modified`, or nil when the entity is gone.
    public static func modified(_ path: String) -> Date? {
        var info = stat()
        if lstat(path, &info) != 0 { return nil }
        #if canImport(Darwin)
        let time = info.st_mtimespec
        #else
        let time = info.st_mtim
        #endif
        return Date(timeIntervalSince1970: TimeInterval(time.tv_sec) + TimeInterval(time.tv_nsec) / 1_000_000_000)
    }
}

/// A failed file-system operation. FMGit's `GitFileSystemError` has an
/// internal initializer, so the engine throws its own value of the same shape.
public struct EngineFileSystemError: Error, CustomStringConvertible, Sendable {
    public let operation: String
    public let path: String
    public let code: Int32

    public var description: String { "FileSystemException(\(operation) \(path): errno \(code))" }
}

// MARK: - Text

/// `utf8.decode(bytes)` with `allowMalformed: false`.
public func engUtf8DecodeStrict(_ bytes: [UInt8]) -> String? {
    let text = String(decoding: bytes, as: UTF8.self)
    return text.utf8.elementsEqual(bytes) ? text : nil
}

/// `utf8.decode(bytes)` for bytes the engine validated earlier (index and
/// tree paths): never fails, a malformed byte becomes U+FFFD.
public func engUtf8Decode(_ bytes: [UInt8]) -> String { String(decoding: bytes, as: UTF8.self) }

/// Byte-wise ordering, the order Git itself uses for paths and names.
public func engCompareBytes(_ left: [UInt8], _ right: [UInt8]) -> Int {
    let shared = Swift.min(left.count, right.count)
    var index = 0
    while index < shared {
        if left[index] != right[index] { return Int(left[index]) - Int(right[index]) }
        index += 1
    }
    return left.count - right.count
}

/// The whitespace set that `engTrim` strips.
public func engIsDartWhitespace(_ scalar: Unicode.Scalar) -> Bool {
    switch scalar.value {
    case 0x09...0x0d, 0x20, 0x85, 0xa0, 0x1680, 0x2000...0x200a, 0x2028, 0x2029, 0x202f, 0x205f, 0x3000, 0xfeff:
        return true
    default:
        return false
    }
}

/// `const LineSplitter().convert(text)`: splits at `\n`, `\r\n` and `\r`; no trailing empty line.
public func engSplitLines(_ text: String) -> [String] {
    let bytes = Array(text.utf8)
    var lines = [String]()
    var start = 0
    var index = 0
    while index < bytes.count {
        let byte = bytes[index]
        if byte == 0x0a || byte == 0x0d {
            lines.append(String(decoding: bytes[start..<index], as: UTF8.self))
            if byte == 0x0d, index + 1 < bytes.count, bytes[index + 1] == 0x0a { index += 1 }
            start = index + 1
        }
        index += 1
    }
    if start < bytes.count { lines.append(String(decoding: bytes[start...], as: UTF8.self)) }
    return lines
}

extension String {
    /// Code-unit equality, not canonical equivalence.
    public func engSame(_ other: String) -> Bool { utf8.elementsEqual(other.utf8) }

    public func engHasPrefix(_ prefix: String) -> Bool { utf8.starts(with: prefix.utf8) }

    public func engHasSuffix(_ suffix: String) -> Bool {
        let own = utf8.count
        let other = suffix.utf8.count
        if other > own { return false }
        return utf8.dropFirst(own - other).elementsEqual(suffix.utf8)
    }

    /// Index of `needle` at or after `start`, in UTF-8 offsets; -1 when absent.
    public func engIndexOf(_ needle: String, from start: Int = 0) -> Int {
        let bytes = Array(utf8)
        let pattern = Array(needle.utf8)
        if pattern.isEmpty { return Swift.min(Swift.max(start, 0), bytes.count) }
        var index = Swift.max(start, 0)
        while index + pattern.count <= bytes.count {
            if bytes[index..<(index + pattern.count)].elementsEqual(pattern) { return index }
            index += 1
        }
        return -1
    }

    public func engContains(_ needle: String) -> Bool { engIndexOf(needle) >= 0 }

    /// Splits at a one-byte ASCII separator; empty parts are kept.
    public func engSplit(_ separator: UInt8) -> [String] {
        var parts = [String]()
        var current = [UInt8]()
        for byte in utf8 {
            if byte == separator {
                parts.append(String(decoding: current, as: UTF8.self))
                current.removeAll(keepingCapacity: true)
            } else {
                current.append(byte)
            }
        }
        parts.append(String(decoding: current, as: UTF8.self))
        return parts
    }

    /// `substring(start, end)` in UTF-8 offsets (always ASCII delimiters here).
    public func engSubstring(_ start: Int, _ end: Int? = nil) -> String {
        let bytes = Array(utf8)
        return String(decoding: bytes[start..<(end ?? bytes.count)], as: UTF8.self)
    }

    /// Strips leading and trailing whitespace (`engIsDartWhitespace`).
    public func engTrim() -> String {
        var scalars = Array(unicodeScalars)
        while let last = scalars.last, engIsDartWhitespace(last) { scalars.removeLast() }
        var start = 0
        while start < scalars.count, engIsDartWhitespace(scalars[start]) { start += 1 }
        var view = String.UnicodeScalarView()
        view.append(contentsOf: scalars[start...])
        return String(view)
    }

    /// Strips leading whitespace (`engIsDartWhitespace`).
    public func engTrimLeft() -> String {
        let scalars = Array(unicodeScalars)
        var start = 0
        while start < scalars.count, engIsDartWhitespace(scalars[start]) { start += 1 }
        var view = String.UnicodeScalarView()
        view.append(contentsOf: scalars[start...])
        return String(view)
    }

    /// Comparison in UTF-16 code unit order.
    public func engCompare(_ other: String) -> Int {
        var left = utf16.makeIterator()
        var right = other.utf16.makeIterator()
        while true {
            switch (left.next(), right.next()) {
            case (nil, nil): return 0
            case (nil, _): return -1
            case (_, nil): return 1
            case let (a?, b?): if a != b { return a < b ? -1 : 1 }
            }
        }
    }
}

/// A cached `NSRegularExpression` for the regular expressions of this layer.
/// Patterns are written for ICU: `\z` instead of `$` (ICU's `$` also matches
/// before a final newline).
public final class EngineRegex: @unchecked Sendable {
    private let expression: NSRegularExpression

    public init(_ pattern: String) {
        // Patterns are compile-time literals; a typo is a programmer error.
        expression = try! NSRegularExpression(pattern: pattern, options: [])
    }

    public func hasMatch(_ text: String) -> Bool {
        let range = NSRange(location: 0, length: (text as NSString).length)
        return expression.firstMatch(in: text, options: [], range: range) != nil
    }

    /// Capture groups of the first match (index 0 is the whole match).
    public func firstMatch(_ text: String) -> [String?]? {
        let source = text as NSString
        guard let match = expression.firstMatch(in: text, options: [], range: NSRange(location: 0, length: source.length))
        else { return nil }
        return (0..<match.numberOfRanges).map { index in
            let range = match.range(at: index)
            return range.location == NSNotFound ? nil : source.substring(with: range)
        }
    }
}
