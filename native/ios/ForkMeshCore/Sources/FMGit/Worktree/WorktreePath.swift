// This file is a security boundary: every byte the engine writes into a
// working tree goes through `GitWorktreePath` (what names are allowed) and
// `GitWorktree.safeFile` (no parent or destination may be a symlink). All
// checks work on UTF-8 bytes and UTF-16 units, never on Swift Characters:
// "/" followed by a combining mark is a single Character and would slip
// through a `split(separator: "/")`.

import Foundation

public struct GitWorktreePath: Hashable, CustomStringConvertible, Sendable {
    public let value: String
    public let bytes: [UInt8]
    public let components: [String]

    public init(_ value: String) throws {
        let bytes = Array(value.utf8)
        let components = value.u8Split(0x2f)
        if bytes.isEmpty
            || bytes[0] == 0x2f
            || bytes[0] == 0x5c
            || bytes.count > 4096
            || bytes.contains(0x5c)
            || bytes.contains(where: { $0 < 0x20 || $0 == 0x7f })
        {
            throw GitException(.pathEscapesWorktree, "path is not portable")
        }
        for component in components {
            let raw = Array(component.utf8)
            if raw.isEmpty
                || raw == [0x2e]
                || raw == [0x2e, 0x2e]
                || raw.count > 255
                || raw.last == 0x2e
                || raw.last == 0x20
                || raw.contains(0x3a)
                || isReservedDeviceName(component)
            {
                throw GitException(.pathEscapesWorktree, "path is not portable")
            }
        }
        self.value = value
        self.bytes = bytes
        self.components = components
    }

    public static func == (left: GitWorktreePath, right: GitWorktreePath) -> Bool { left.bytes == right.bytes }

    public func hash(into hasher: inout Hasher) { hasher.combine(bytes) }

    public var description: String { value }
}

public final class GitWorktree {
    public let root: String

    public init(_ root: String) { self.root = root }

    public func writeFile(_ path: GitWorktreePath, _ bytes: [UInt8], flush: Bool = true) throws {
        let file = try safeFile(path, createParents: true)
        try GitFS.writeBytes(file, bytes, flush: flush)
    }

    public func removeEmptyParents(_ path: GitWorktreePath) {
        var components = Array(path.components.dropLast())
        while !components.isEmpty {
            let directory = components.reduce(root) { GitFS.join($0, $1) }
            if GitFS.entityType(directory) != .directory || (try? GitFS.deleteEmptyDirectory(directory)) == nil { return }
            components.removeLast()
        }
    }

    public func readFile(_ path: GitWorktreePath) throws -> [UInt8] {
        let file = try safeFile(path, createParents: false)
        if !GitFS.fileExists(file) {
            throw GitException(.invalidRepository, "worktree file is missing")
        }
        return try GitFS.readBytes(file)
    }

    public func readSymlink(_ path: GitWorktreePath) throws -> String {
        let file = try safeFile(path, createParents: false, allowFinalSymlink: true)
        if GitFS.entityType(file) != .link {
            throw GitException(.invalidRepository, "worktree path is not a symlink")
        }
        let target = try GitFS.readLink(file)
        try validateSymlinkTarget(target)
        return target
    }

    public func deleteFile(_ path: GitWorktreePath) throws {
        let file = try safeFile(path, createParents: false)
        if GitFS.fileExists(file) { try GitFS.deleteFile(file) }
    }

    public func deleteEntry(_ path: GitWorktreePath) throws {
        let file = try safeFile(path, createParents: false, allowFinalSymlink: true)
        let type = GitFS.entityType(file)
        if type == .file || type == .link { try GitFS.deleteFile(file) }
    }

    public func writeSymlink(_ path: GitWorktreePath, _ target: String) throws {
        try validateSymlinkTarget(target)
        let file = try safeFile(path, createParents: true, allowFinalSymlink: true)
        let type = GitFS.entityType(file)
        if type == .directory {
            throw GitException(.pathEscapesWorktree, "cannot replace a directory with a symlink")
        }
        if type != .notFound { try GitFS.deleteFile(file) }
        try GitFS.createLink(file, target: target)
    }

    public func typeOf(_ path: GitWorktreePath) throws -> GitFileSystemEntityType {
        let file = try safeFile(path, createParents: false, allowFinalSymlink: true)
        return GitFS.entityType(file)
    }

    /// The absolute location of `path`, after the same symlink checks every
    /// read and write goes through.
    func safeFile(_ path: GitWorktreePath, createParents: Bool, allowFinalSymlink: Bool = false) throws -> String {
        if GitFS.entityType(root) == .link {
            throw GitException(.symlinkEscape, "worktree root must not be a symlink")
        }
        var current = root
        for component in path.components.dropLast() {
            let next = GitFS.join(current, component)
            var type = GitFS.entityType(next)
            if type == .link {
                throw GitException(.symlinkEscape, "worktree parent is a symlink")
            }
            if type == .notFound && createParents {
                try GitFS.createDirectory(next)
                type = GitFS.entityType(next)
            }
            if type != .directory {
                throw GitException(.pathEscapesWorktree, "worktree parent is not a directory")
            }
            current = next
        }
        let file = GitFS.join(current, path.components[path.components.count - 1])
        if !allowFinalSymlink && GitFS.entityType(file) == .link {
            throw GitException(.symlinkEscape, "worktree destination is a symlink")
        }
        return file
    }
}

private func isReservedDeviceName(_ component: String) -> Bool {
    let stem = Array(component.u8Split(0x2e)[0].uppercased().utf8)
    for reserved in ["CON", "PRN", "AUX", "NUL"] where stem == Array(reserved.utf8) { return true }
    // ^(COM|LPT)[1-9]$
    if stem.count == 4, stem[3] >= 0x31, stem[3] <= 0x39 {
        let prefix = Array(stem[0..<3])
        return prefix == Array("COM".utf8) || prefix == Array("LPT".utf8)
    }
    return false
}

private func validateSymlinkTarget(_ target: String) throws {
    let bytes = Array(target.utf8)
    let escapes = bytes.isEmpty
        || bytes[0] == 0x2f
        || bytes[0] == 0x5c
        || bytes.contains(0x5c)
        || target.u8Split(0x2f).contains { part in
            let raw = Array(part.utf8)
            return raw.isEmpty || raw == [0x2e] || raw == [0x2e, 0x2e]
        }
    if escapes {
        throw GitException(.symlinkEscape, "symlink target escapes worktree")
    }
}
