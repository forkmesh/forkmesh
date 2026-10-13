public enum GitIndexMode: Int, Sendable {
    case regular = 33188
    case executable = 33261
    case symlink = 40960

    public var fileMode: Int { rawValue }

    public static func parse(_ fileMode: Int) throws -> GitIndexMode {
        if let mode = GitIndexMode(rawValue: fileMode) { return mode }
        if fileMode == 57344 {
            throw GitException(.unsupportedRepositoryFormat, "gitlinks are unsupported")
        }
        throw GitException(.indexCorrupt, "unsupported index file mode")
    }
}

public struct GitIndexEntry: Sendable {
    public let path: [UInt8]
    public let objectId: GitObjectId
    public let mode: GitIndexMode
    public let size: Int
    public let stage: Int

    public init(path: [UInt8], objectId: GitObjectId, mode: GitIndexMode, size: Int = 0, stage: Int = 0) throws {
        if size < 0 || size > 0xffff_ffff {
            throw GitArgumentError("size", "must fit in uint32")
        }
        if stage != 0 {
            throw GitException(.unsupportedRepositoryFormat, "conflict index stages are unsupported")
        }
        try validateIndexPath(path)
        self.path = path
        self.objectId = objectId
        self.mode = mode
        self.size = size
        self.stage = stage
    }

    /// The path decoded as UTF-8 (validated at construction).
    public var pathText: String { String(decoding: path, as: UTF8.self) }
}

public struct GitIndexV2: Sendable {
    public let entries: [GitIndexEntry]

    public init(_ entries: [GitIndexEntry]) throws {
        let sorted = entries.sorted { compareBytes($0.path, $1.path) < 0 }
        if sorted.count > 1 {
            for index in 1..<sorted.count where sorted[index - 1].path == sorted[index].path {
                throw GitException(.indexCorrupt, "duplicate index path")
            }
        }
        self.entries = sorted
    }

    private init(parsed entries: [GitIndexEntry]) { self.entries = entries }

    public static func parse(_ bytes: [UInt8], _ limits: GitResourceLimits) throws -> GitIndexV2 {
        if bytes.count < 32 {
            throw GitException(.indexCorrupt, "index is truncated")
        }
        let checksumOffset = bytes.count - 20
        try requireDigest(bytes, checksumOffset)
        if !constantTimeEquals(Array(bytes[0..<4]), Array("DIRC".utf8)) {
            throw GitException(.indexCorrupt, "index signature is invalid")
        }
        if bytes.readUint32(4) != 2 {
            throw GitException(.unsupportedRepositoryFormat, "only index version 2 is supported")
        }
        let count = bytes.readUint32(8)
        if count > GitResourceLimits.hardMaxRefCount || count > limits.maxRefCount {
            throw GitException(.resourceLimitExceeded, "index entry count exceeds configured limit")
        }
        var offset = 12
        var entries = [GitIndexEntry]()
        entries.reserveCapacity(count)
        for _ in 0..<count {
            let entryOffset = offset
            if checksumOffset - offset < 62 {
                throw GitException(.indexCorrupt, "index entry is truncated")
            }
            let mode = try GitIndexMode.parse(bytes.readUint32(offset + 24))
            let size = bytes.readUint32(offset + 36)
            let id = try GitObjectId.fromSha1Bytes(bytes[(offset + 40)..<(offset + 60)])
            let flags = bytes.readUint16(offset + 60)
            let stage = (flags >> 12) & 0x3
            if stage != 0 || flags & 0xc000 != 0 {
                throw GitException(.unsupportedRepositoryFormat, "conflict or extended index entry is unsupported")
            }
            let pathStart = offset + 62
            let nul = bytes.indexOfByte(0, pathStart)
            if nul < 0 || nul >= checksumOffset {
                throw GitException(.indexCorrupt, "index path is unterminated")
            }
            let path = Array(bytes[pathStart..<nul])
            let declaredLength = flags & 0x0fff
            if declaredLength != 0x0fff && declaredLength != path.count {
                throw GitException(.indexCorrupt, "index path length flag is invalid")
            }
            let entryLength = nul + 1 - entryOffset
            offset = nul + 1 + ((8 - entryLength % 8) % 8)
            if offset > checksumOffset {
                throw GitException(.indexCorrupt, "index entry padding is truncated")
            }
            entries.append(try GitIndexEntry(path: path, objectId: id, mode: mode, size: size, stage: stage))
        }
        try skipExtensions(bytes, offset, checksumOffset, limits)
        if entries.count > 1 {
            for index in 1..<entries.count where compareBytes(entries[index - 1].path, entries[index].path) >= 0 {
                throw GitException(.indexCorrupt, "index entries are not strictly sorted")
            }
        }
        return GitIndexV2(parsed: entries)
    }

    public func encode() -> [UInt8] {
        var output = Array("DIRC".utf8)
        output.append(contentsOf: uint32Bytes(2))
        output.append(contentsOf: uint32Bytes(UInt32(entries.count)))
        for entry in entries {
            let entryOffset = output.count
            output.append(contentsOf: [UInt8](repeating: 0, count: 24))
            output.append(contentsOf: uint32Bytes(UInt32(entry.mode.fileMode)))
            output.append(contentsOf: uint32Bytes(0))
            output.append(contentsOf: uint32Bytes(0))
            output.append(contentsOf: uint32Bytes(UInt32(entry.size)))
            output.append(contentsOf: entry.objectId.bytes)
            let pathLength = entry.path.count > 0xffe ? 0xfff : entry.path.count
            output.append(contentsOf: uint16Bytes(pathLength))
            output.append(contentsOf: entry.path)
            output.append(0)
            while (output.count - entryOffset) % 8 != 0 { output.append(0) }
        }
        output.append(contentsOf: ForkMeshSha1.hash(output))
        return output
    }
}

private func skipExtensions(_ bytes: [UInt8], _ start: Int, _ checksumOffset: Int, _ limits: GitResourceLimits) throws {
    var offset = start
    while offset < checksumOffset {
        if checksumOffset - offset < 8 {
            throw GitException(.indexCorrupt, "index extension header is truncated")
        }
        let first = bytes[offset]
        let length = bytes.readUint32(offset + 4)
        if length > limits.maxInflatedBytes || length > checksumOffset - offset - 8 {
            throw GitException(.resourceLimitExceeded, "index extension length is invalid")
        }
        // Git marks optional extensions with an uppercase first letter.
        if first < 0x41 || first > 0x5a {
            throw GitException(.unsupportedRepositoryFormat, "unknown mandatory index extension")
        }
        offset += 8 + length
    }
}

private func validateIndexPath(_ path: [UInt8]) throws {
    if path.isEmpty || path.count > 4096 || path.contains(0) {
        throw GitException(.pathEscapesWorktree, "invalid path")
    }
    guard let value = utf8DecodeStrict(path) else {
        throw GitException(.pathEscapesWorktree, "path is not valid UTF-8")
    }
    let escapes = path[0] == 0x2f
        || path[0] == 0x5c
        || path.contains(0x5c)
        || value.u8Split(0x2f).contains { part in
            let raw = Array(part.utf8)
            return raw.isEmpty || raw == [0x2e] || raw == [0x2e, 0x2e]
        }
    if escapes {
        throw GitException(.pathEscapesWorktree, "path escapes worktree")
    }
}

private func requireDigest(_ bytes: [UInt8], _ checksumOffset: Int) throws {
    let digest = ForkMeshSha1()
    try digest.add(bytes[0..<checksumOffset])
    if !constantTimeEquals(try digest.close(), Array(bytes[checksumOffset...])) {
        throw GitException(.indexCorrupt, "index checksum does not match")
    }
}
