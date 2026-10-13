import FMBase
import Foundation

public enum FileRootKind: String, Equatable, Sendable {
    case repo
    case worktree

    public init(wire: String) { self = FileRootKind(rawValue: wire) ?? .repo }
}

public struct FileRoot: Equatable, Sendable, Identifiable, Hashable {
    public var id: String
    public var label: String
    public var kind: FileRootKind
    public var branch: String
    public var repo: String

    public init(id: String, label: String, kind: FileRootKind = .repo, branch: String = "", repo: String = "") {
        self.id = id
        self.label = label
        self.kind = kind
        self.branch = branch
        self.repo = repo
    }

    public init(json: JSONObject) {
        self.init(
            id: json.str("id"), label: json.str("label"), kind: FileRootKind(wire: json.str("kind")),
            branch: json.str("branch"), repo: json.str("repo"))
    }
}

public struct FileRootGroup: Equatable, Sendable, Identifiable {
    public var repo: FileRoot
    public var worktrees: [FileRoot]

    public var id: String { repo.id }

    public static func group(_ roots: [FileRoot]) -> [FileRootGroup] {
        var groups: [FileRootGroup] = []
        var index: [String: Int] = [:]
        for root in roots where root.kind == .repo {
            index[root.id.lowercased()] = groups.count
            groups.append(FileRootGroup(repo: root, worktrees: []))
        }
        for root in roots where root.kind == .worktree {
            let key = root.repo.lowercased()
            if let position = index[key] {
                groups[position].worktrees.append(root)
            } else {
                index[key] = groups.count
                groups.append(FileRootGroup(repo: FileRoot(id: root.repo, label: root.repo), worktrees: [root]))
            }
        }
        for position in groups.indices {
            groups[position].worktrees.sort { $0.label.localizedCaseInsensitiveCompare($1.label) == .orderedAscending }
        }
        return groups
    }
}

public enum FileEntryKind: String, Equatable, Sendable {
    case dir
    case file
    case symlink
    case dirlink

    public init(wire: String) { self = FileEntryKind(rawValue: wire) ?? .file }

    public var isFolder: Bool { self == .dir || self == .dirlink }
}

public struct FileEntry: Equatable, Sendable, Identifiable {
    public var name: String
    public var kind: FileEntryKind
    public var size: Double
    public var modifiedMs: Int
    public var escapes: Bool
    public var sensitive: Bool

    public var id: String { name }

    public init(json: JSONObject) {
        name = json.str("name")
        kind = FileEntryKind(wire: json.str("kind"))
        size = json.double("size")
        modifiedMs = json.int("modifiedMs")
        escapes = json.bool("escapes")
        sensitive = json.bool("sensitive")
    }

    public var canOpen: Bool { !escapes && !sensitive }

    public var sizeLabel: String { kind.isFolder ? "" : RemoteFormat.bytes(size) }
}

public struct FileListing: Equatable, Sendable {
    public var root: String
    public var path: String
    public var entries: [FileEntry]
    public var total: Int
    public var truncated: Bool

    public init(json: JSONObject) {
        root = json.str("root")
        path = json.str("path")
        entries = json.maps("entries").map(FileEntry.init(json:))
        total = json.int("total")
        truncated = json.bool("truncated")
    }
}

public struct FileSlice: Equatable, Sendable {
    public var root: String
    public var path: String
    public var size: Double
    public var offset: Double
    public var length: Double
    public var eof: Bool
    public var binary: Bool
    public var text: String
    public var data: Data?
    public var modifiedMs: Int

    public init(json: JSONObject) {
        root = json.str("root")
        path = json.str("path")
        size = json.double("size")
        offset = json.double("offset")
        length = json.double("length")
        eof = json.bool("eof")
        binary = json.bool("binary")
        text = json.str("text")
        data = binary ? Data(base64Encoded: json.str("base64")) : nil
        modifiedMs = json.int("modifiedMs")
    }

    public var nextOffset: Double { offset + length }
}

public enum RemotePath {
    public static func join(_ base: String, _ name: String) -> String {
        base.isEmpty ? name : base + "/" + name
    }

    public static func parent(_ path: String) -> String? {
        if path.isEmpty { return nil }
        var parts = path.split(separator: "/").map(String.init)
        parts.removeLast()
        return parts.joined(separator: "/")
    }

    public static func crumbs(_ path: String) -> [(name: String, path: String)] {
        var out: [(String, String)] = []
        var current = ""
        for part in path.split(separator: "/").map(String.init) {
            current = join(current, part)
            out.append((part, current))
        }
        return out
    }

    public static func fileExtension(_ path: String) -> String {
        guard let name = path.split(separator: "/").last, let dot = name.lastIndex(of: ".") else { return "" }
        return String(name[name.index(after: dot)...]).lowercased()
    }

    public static func isImage(_ path: String) -> Bool {
        ["png", "jpg", "jpeg", "gif", "webp", "heic", "bmp", "ico"].contains(fileExtension(path))
    }
}

public enum GitChangeKind: String, Equatable, Sendable {
    case modified, added, deleted, renamed, copied, untracked, conflicted

    public init(wire: String) { self = GitChangeKind(rawValue: wire) ?? .modified }

    public var letter: String {
        switch self {
        case .modified: return "M"
        case .added: return "A"
        case .deleted: return "D"
        case .renamed: return "R"
        case .copied: return "C"
        case .untracked: return "?"
        case .conflicted: return "!"
        }
    }
}

public struct GitFileChange: Equatable, Sendable, Identifiable {
    public var path: String
    public var from: String
    public var index: String
    public var worktree: String
    public var kind: GitChangeKind

    public var id: String { path }

    public init(json: JSONObject) {
        path = json.str("path")
        from = json.str("from")
        index = json.str("index")
        worktree = json.str("worktree")
        kind = GitChangeKind(wire: json.str("kind"))
    }

    public var isStaged: Bool { index != " " && index != "?" && !index.isEmpty && kind != .conflicted }
}

public struct GitWorkingStatus: Equatable, Sendable {
    public var root: String
    public var label: String
    public var branch: String
    public var upstream: String
    public var ahead: Int
    public var behind: Int
    public var total: Int
    public var staged: Int
    public var unstaged: Int
    public var untracked: Int
    public var conflicted: Int
    public var truncated: Bool
    public var files: [GitFileChange]

    public init(json: JSONObject) {
        root = json.str("root")
        label = json.str("label")
        branch = json.str("branch")
        upstream = json.str("upstream")
        ahead = json.int("ahead")
        behind = json.int("behind")
        total = json.int("total")
        staged = json.int("staged")
        unstaged = json.int("unstaged")
        untracked = json.int("untracked")
        conflicted = json.int("conflicted")
        truncated = json.bool("truncated")
        files = json.maps("files").map(GitFileChange.init(json:))
    }

    public var isClean: Bool { total == 0 }

    public var syncLine: String {
        if upstream.isEmpty { return "No upstream" }
        switch (ahead, behind) {
        case (0, 0): return "Up to date with \(upstream)"
        case (let a, 0): return "\(a) ahead of \(upstream)"
        case (0, let b): return "\(b) behind \(upstream)"
        case (let a, let b): return "\(a) ahead, \(b) behind \(upstream)"
        }
    }

    public var changesLine: String {
        if isClean { return "No changes" }
        var parts: [String] = []
        if staged > 0 { parts.append("\(staged) staged") }
        if unstaged > 0 { parts.append("\(unstaged) modified") }
        if untracked > 0 { parts.append("\(untracked) untracked") }
        if conflicted > 0 { parts.append("\(conflicted) conflicted") }
        return parts.joined(separator: " · ")
    }

    public var canFastForward: Bool { behind > 0 && ahead == 0 && conflicted == 0 }
}

public enum GitRemoteAction: String, CaseIterable, Sendable, Identifiable {
    case fetch
    case pull

    public var id: String { rawValue }

    public var title: String {
        switch self {
        case .fetch: return "Fetch"
        case .pull: return "Pull (fast-forward)"
        }
    }
}
