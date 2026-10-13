import Foundation

public struct WatchDiffLine: Codable, Equatable, Hashable, Sendable {
    public enum Kind: String, Codable, Sendable {
        case hunk = "@"
        case added = "+"
        case removed = "-"
        case context = " "
    }

    public var kind: Kind
    public var text: String
    public var number: Int

    public init(_ kind: Kind, _ text: String, number: Int = 0) {
        self.kind = kind
        self.text = text
        self.number = number
    }

    enum CodingKeys: String, CodingKey {
        case kind = "k"
        case text = "t"
        case number = "n"
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        kind = Kind(rawValue: try c.decodeIfPresent(String.self, forKey: .kind) ?? " ") ?? .context
        text = try c.decodeIfPresent(String.self, forKey: .text) ?? ""
        number = try c.decodeIfPresent(Int.self, forKey: .number) ?? 0
    }

    public func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode(kind.rawValue, forKey: .kind)
        try c.encode(text, forKey: .text)
        if number > 0 { try c.encode(number, forKey: .number) }
    }
}

public struct WatchDiffFile: Codable, Equatable, Hashable, Sendable, Identifiable {
    public enum Group: String, Codable, Sendable {
        case committed, uncommitted, untracked
    }

    public var path: String
    public var oldPath: String
    public var status: String
    public var group: Group
    public var added: Int
    public var removed: Int
    public var binary: Bool
    public var lines: [WatchDiffLine]
    public var hiddenLines: Int

    public init(
        path: String, oldPath: String = "", status: String = "M", group: Group = .committed, added: Int = 0,
        removed: Int = 0, binary: Bool = false, lines: [WatchDiffLine] = [], hiddenLines: Int = 0
    ) {
        self.path = path
        self.oldPath = oldPath
        self.status = status
        self.group = group
        self.added = added
        self.removed = removed
        self.binary = binary
        self.lines = lines
        self.hiddenLines = hiddenLines
    }

    public var id: String { "\(group.rawValue):\(path)" }

    public var name: String { path.split(separator: "/").last.map(String.init) ?? path }

    public var folder: String {
        guard let slash = path.lastIndex(of: "/") else { return "" }
        return String(path[..<slash])
    }

    public var reviewMark: String { "\(id)#\(added)-\(removed)-\(lines.count + hiddenLines)" }

    public var note: String {
        if binary { return "Binary file." }
        if group == .untracked { return "New file, not added to Git yet." }
        if lines.isEmpty && hiddenLines == 0 {
            return status == "R" ? "Renamed from \(oldPath)." : "No line changes."
        }
        return ""
    }

    enum CodingKeys: String, CodingKey {
        case path, oldPath = "old", status = "s", group = "g", added = "a", removed = "r", binary = "b"
        case lines = "l", hiddenLines = "h"
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        path = try c.decode(String.self, forKey: .path)
        oldPath = try c.decodeIfPresent(String.self, forKey: .oldPath) ?? ""
        status = try c.decodeIfPresent(String.self, forKey: .status) ?? "M"
        group = Group(rawValue: try c.decodeIfPresent(String.self, forKey: .group) ?? "") ?? .committed
        added = try c.decodeIfPresent(Int.self, forKey: .added) ?? 0
        removed = try c.decodeIfPresent(Int.self, forKey: .removed) ?? 0
        binary = try c.decodeIfPresent(Bool.self, forKey: .binary) ?? false
        lines = try c.decodeIfPresent([WatchDiffLine].self, forKey: .lines) ?? []
        hiddenLines = try c.decodeIfPresent(Int.self, forKey: .hiddenLines) ?? 0
    }

    public func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode(path, forKey: .path)
        if !oldPath.isEmpty { try c.encode(oldPath, forKey: .oldPath) }
        try c.encode(status, forKey: .status)
        try c.encode(group.rawValue, forKey: .group)
        try c.encode(added, forKey: .added)
        try c.encode(removed, forKey: .removed)
        if binary { try c.encode(binary, forKey: .binary) }
        if !lines.isEmpty { try c.encode(lines, forKey: .lines) }
        if hiddenLines > 0 { try c.encode(hiddenLines, forKey: .hiddenLines) }
    }
}

public struct WatchDiffSection: Equatable, Sendable, Identifiable {
    public var group: WatchDiffFile.Group
    public var files: [WatchDiffFile]

    public var id: String { group.rawValue }

    public var title: String {
        switch group {
        case .committed: return "Committed"
        case .uncommitted: return "Uncommitted"
        case .untracked: return "Untracked"
        }
    }
}

public struct WatchDiff: Codable, Equatable, Sendable {
    public var key: String
    public var ok: Bool
    public var message: String
    public var branch: String
    public var base: String
    public var merged: Bool
    public var files: [WatchDiffFile]
    public var hiddenFiles: Int
    public var truncated: Bool

    public init(
        key: String, ok: Bool = true, message: String = "", branch: String = "", base: String = "",
        merged: Bool = false, files: [WatchDiffFile] = [], hiddenFiles: Int = 0, truncated: Bool = false
    ) {
        self.key = key
        self.ok = ok
        self.message = message
        self.branch = branch
        self.base = base
        self.merged = merged
        self.files = files
        self.hiddenFiles = hiddenFiles
        self.truncated = truncated
    }

    public static func refused(_ key: String, _ message: String) -> WatchDiff {
        WatchDiff(key: key, ok: false, message: message)
    }

    public var added: Int { files.reduce(0) { $0 + $1.added } }
    public var removed: Int { files.reduce(0) { $0 + $1.removed } }

    public var sections: [WatchDiffSection] {
        [WatchDiffFile.Group.committed, .uncommitted, .untracked].compactMap { group in
            let inGroup = files.filter { $0.group == group }
            return inGroup.isEmpty ? nil : WatchDiffSection(group: group, files: inGroup)
        }
    }

    public var branchLine: String {
        if branch.isEmpty { return "" }
        return base.isEmpty ? branch : "\(branch) \u{2192} \(base)"
    }

    public var emptyNote: String {
        merged ? "Merged. Nothing left to review." : "No changes yet."
    }

    public func next(after id: String) -> WatchDiffFile? {
        guard let index = files.firstIndex(where: { $0.id == id }), index + 1 < files.count else { return nil }
        return files[index + 1]
    }

    public static let maxLineChars = 160
    public static let maxLinesPerFile = 600

    enum CodingKeys: String, CodingKey {
        case key, ok, message, branch, base, merged, files, hiddenFiles, truncated
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        key = try c.decodeIfPresent(String.self, forKey: .key) ?? ""
        ok = try c.decodeIfPresent(Bool.self, forKey: .ok) ?? false
        message = try c.decodeIfPresent(String.self, forKey: .message) ?? ""
        branch = try c.decodeIfPresent(String.self, forKey: .branch) ?? ""
        base = try c.decodeIfPresent(String.self, forKey: .base) ?? ""
        merged = try c.decodeIfPresent(Bool.self, forKey: .merged) ?? false
        files = try c.decodeIfPresent([WatchDiffFile].self, forKey: .files) ?? []
        hiddenFiles = try c.decodeIfPresent(Int.self, forKey: .hiddenFiles) ?? 0
        truncated = try c.decodeIfPresent(Bool.self, forKey: .truncated) ?? false
    }
}

extension WatchWire {
    public static func fitted(_ diff: WatchDiff, maxBytes: Int = maxBodyBytes) -> WatchDiff {
        var out = diff
        for i in out.files.indices {
            out.files[i].lines = out.files[i].lines.map { line in
                guard line.text.count > WatchDiff.maxLineChars else { return line }
                var cut = line
                cut.text = String(line.text.prefix(WatchDiff.maxLineChars - 1)) + "\u{2026}"
                return cut
            }
            clip(&out.files[i], to: WatchDiff.maxLinesPerFile)
        }
        if encodedSize(out) <= maxBytes { return out }
        for limit in [300, 160, 80, 40, 16, 0] {
            for i in out.files.indices { clip(&out.files[i], to: limit) }
            if encodedSize(out) <= maxBytes { return out }
        }
        let all = out.files
        var low = 0
        var high = all.count
        while low < high {
            let mid = (low + high + 1) / 2
            out.files = Array(all.prefix(mid))
            if encodedSize(out) <= maxBytes { low = mid } else { high = mid - 1 }
        }
        out.files = Array(all.prefix(low))
        out.hiddenFiles += all.count - low
        return out
    }

    static func clip(_ file: inout WatchDiffFile, to limit: Int) {
        guard file.lines.count > limit else { return }
        file.hiddenLines += file.lines.count - limit
        file.lines = Array(file.lines.prefix(limit))
    }
}
