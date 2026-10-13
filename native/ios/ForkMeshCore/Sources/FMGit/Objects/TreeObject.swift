public enum GitTreeMode: String, Sendable {
    case regular = "100644"
    case executable = "100755"
    case symlink = "120000"
    case directory = "40000"

    public var wireValue: String { rawValue }

    public static func parse(_ value: String) throws -> GitTreeMode {
        let bytes = Array(value.utf8)
        for mode in [GitTreeMode.regular, .executable, .symlink, .directory]
        where Array(mode.rawValue.utf8) == bytes {
            return mode
        }
        if bytes == Array("160000".utf8) {
            throw GitException(.unsupportedRepositoryFormat, "gitlinks are unsupported")
        }
        throw GitException(.invalidTree, "unsupported tree mode")
    }
}

public struct GitTreeEntry: Sendable {
    public let mode: GitTreeMode
    public let name: [UInt8]
    public let objectId: GitObjectId

    fileprivate init(_ mode: GitTreeMode, _ name: [UInt8], _ objectId: GitObjectId) throws {
        if name.isEmpty || name.contains(0) || name.contains(0x2f) {
            throw GitException(.invalidTree, "tree entry name is invalid")
        }
        self.mode = mode
        self.name = name
        self.objectId = objectId
    }

    public static func regular(_ name: [UInt8], _ objectId: GitObjectId) throws -> GitTreeEntry {
        try GitTreeEntry(.regular, name, objectId)
    }

    public static func executable(_ name: [UInt8], _ objectId: GitObjectId) throws -> GitTreeEntry {
        try GitTreeEntry(.executable, name, objectId)
    }

    public static func symlink(_ name: [UInt8], _ objectId: GitObjectId) throws -> GitTreeEntry {
        try GitTreeEntry(.symlink, name, objectId)
    }

    public static func directory(_ name: [UInt8], _ objectId: GitObjectId) throws -> GitTreeEntry {
        try GitTreeEntry(.directory, name, objectId)
    }

    /// The entry name decoded leniently, for display.
    public var nameText: String { String(decoding: name, as: UTF8.self) }
}

/// Git's tree order: bytewise, with a directory name compared as if it ended in `/`.
public func compareGitTreeEntries(_ left: GitTreeEntry, _ right: GitTreeEntry) -> Int {
    let leftCount = left.name.count + (left.mode == .directory ? 1 : 0)
    let rightCount = right.name.count + (right.mode == .directory ? 1 : 0)
    var index = 0
    while index < leftCount && index < rightCount {
        let leftByte = index < left.name.count ? Int(left.name[index]) : 0x2f
        let rightByte = index < right.name.count ? Int(right.name[index]) : 0x2f
        if leftByte != rightByte { return leftByte - rightByte }
        index += 1
    }
    return leftCount - rightCount
}

public struct GitTree: Sendable {
    public let entries: [GitTreeEntry]

    public init(_ entries: [GitTreeEntry]) throws {
        // A stable sort, though equal keys are rejected below anyway.
        let sorted = entries.enumerated().sorted { left, right in
            let comparison = compareGitTreeEntries(left.element, right.element)
            return comparison != 0 ? comparison < 0 : left.offset < right.offset
        }.map(\.element)
        if sorted.count > 1 {
            for index in 1..<sorted.count where compareGitTreeEntries(sorted[index - 1], sorted[index]) == 0 {
                throw GitException(.invalidTree, "duplicate tree entry")
            }
        }
        self.entries = sorted
    }

    private init(parsed entries: [GitTreeEntry]) { self.entries = entries }

    public static func parse(_ bytes: [UInt8]) throws -> GitTree {
        var parsed = [GitTreeEntry]()
        var offset = 0
        while offset < bytes.count {
            let modeEnd = bytes.indexOfByte(0x20, offset)
            if modeEnd <= offset {
                throw GitException(.invalidTree, "tree mode is missing")
            }
            let nameEnd = bytes.indexOfByte(0, modeEnd + 1)
            if nameEnd < modeEnd + 1 || nameEnd + 21 > bytes.count {
                throw GitException(.invalidTree, "tree entry is truncated")
            }
            guard let modeText = asciiDecode(Array(bytes[offset..<modeEnd])) else {
                throw GitException(.invalidTree, "unsupported tree mode")
            }
            let entry = try GitTreeEntry(
                try GitTreeMode.parse(modeText),
                Array(bytes[(modeEnd + 1)..<nameEnd]),
                try GitObjectId.fromSha1Bytes(bytes[(nameEnd + 1)..<(nameEnd + 21)])
            )
            if let last = parsed.last, compareGitTreeEntries(last, entry) >= 0 {
                throw GitException(.invalidTree, "tree entries are not strictly sorted")
            }
            parsed.append(entry)
            offset = nameEnd + 21
        }
        return GitTree(parsed: parsed)
    }

    public func encode() -> [UInt8] {
        var output = [UInt8]()
        for entry in entries {
            output.append(contentsOf: Array("\(entry.mode.wireValue) ".utf8))
            output.append(contentsOf: entry.name)
            output.append(0)
            output.append(contentsOf: entry.objectId.bytes)
        }
        return output
    }
}
