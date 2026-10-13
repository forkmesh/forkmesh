public struct GitPackWriteObject: Sendable {
    public let type: GitObjectType
    public let body: [UInt8]
    public let objectId: GitObjectId

    public init(_ type: GitObjectType, _ body: [UInt8]) throws {
        if body.count > 256 * 1024 * 1024 {
            throw GitException(.resourceLimitExceeded, "pack object exceeds hard limit")
        }
        self.type = type
        self.body = body
        objectId = GitObjectId.sha1Of(GitObjectFrame.encode(type, body))
    }
}

public struct GitWrittenPack: Sendable {
    public let packBytes: [UInt8]
    public let indexBytes: [UInt8]
}

public enum GitPackWriter {
    public static func write(
        _ objects: [GitPackWriteObject],
        cancellation: GitCancellationToken? = nil
    ) throws -> GitWrittenPack {
        try cancellation?.throwIfCancelled()
        var knownIds = Set<GitObjectId>()
        for object in objects where !knownIds.insert(object.objectId).inserted {
            throw GitException(.packCorrupt, "pack write objects contain a duplicate object ID")
        }
        var pack = Array("PACK".utf8)
        pack.append(contentsOf: uint32Bytes(2))
        pack.append(contentsOf: uint32Bytes(UInt32(objects.count)))
        var indexed = [GitPackIndexWriteEntry]()
        for object in objects {
            try cancellation?.throwIfCancelled()
            let offset = pack.count
            pack.append(contentsOf: objectHeader(object.type, object.body.count))
            pack.append(contentsOf: try GitZlib.deflate(object.body))
            indexed.append(GitPackIndexWriteEntry(
                object.objectId, offset: offset, crc32: gitCrc32(pack, offset, pack.count)
            ))
        }
        try cancellation?.throwIfCancelled()
        let packChecksum = ForkMeshSha1.hash(pack)
        pack.append(contentsOf: packChecksum)
        let indexBytes = try GitPackIndexWriter.write(indexed, packChecksum)
        return GitWrittenPack(packBytes: pack, indexBytes: indexBytes)
    }
}

/// One pack index record for `GitPackIndexWriter`.
public struct GitPackIndexWriteEntry: Sendable {
    public let objectId: GitObjectId
    public let offset: Int
    public let crc32: UInt32

    public init(_ objectId: GitObjectId, offset: Int, crc32: UInt32) {
        self.objectId = objectId
        self.offset = offset
        self.crc32 = crc32
    }
}

/// Writes a pack index version 2 for objects whose offsets and CRCs are known.
public enum GitPackIndexWriter {
    public static func write(_ input: [GitPackIndexWriteEntry], _ packChecksum: [UInt8]) throws -> [UInt8] {
        if packChecksum.count != 20 {
            throw GitException(.packCorrupt, "pack checksum must be 20 bytes")
        }
        let entries = input.sorted { compareBytes($0.objectId.bytes, $1.objectId.bytes) < 0 }
        if entries.count > 1 {
            for index in 1..<entries.count where entries[index - 1].objectId == entries[index].objectId {
                throw GitException(.packCorrupt, "pack index entries contain a duplicate object ID")
            }
        }
        var fanout = [Int](repeating: 0, count: 256)
        for entry in entries { fanout[Int(entry.objectId.bytes[0])] += 1 }
        for index in 1..<256 { fanout[index] += fanout[index - 1] }
        var output: [UInt8] = [0xff, 0x74, 0x4f, 0x63]
        output.append(contentsOf: uint32Bytes(2))
        for value in fanout { output.append(contentsOf: uint32Bytes(UInt32(value))) }
        for entry in entries { output.append(contentsOf: entry.objectId.bytes) }
        for entry in entries { output.append(contentsOf: uint32Bytes(entry.crc32)) }
        for entry in entries {
            if entry.offset < 0 || entry.offset >= 0x8000_0000 {
                throw GitException(.resourceLimitExceeded, "large pack offsets are unsupported by the initial writer")
            }
            output.append(contentsOf: uint32Bytes(UInt32(entry.offset)))
        }
        output.append(contentsOf: packChecksum)
        output.append(contentsOf: ForkMeshSha1.hash(output))
        return output
    }
}

private func objectHeader(_ type: GitObjectType, _ size: Int) -> [UInt8] {
    let typeCode: Int
    switch type {
    case .commit: typeCode = 1
    case .tree: typeCode = 2
    case .blob: typeCode = 3
    case .tag: typeCode = 4
    }
    var output = [UInt8]()
    var remaining = size
    var first = (typeCode << 4) | (remaining & 0x0f)
    remaining >>= 4
    if remaining != 0 { first |= 0x80 }
    output.append(UInt8(first))
    while remaining != 0 {
        var byte = remaining & 0x7f
        remaining >>= 7
        if remaining != 0 { byte |= 0x80 }
        output.append(UInt8(byte))
    }
    return output
}
