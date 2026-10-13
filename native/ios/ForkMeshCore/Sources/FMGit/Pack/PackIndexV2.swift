public struct GitPackIndexEntry: Sendable {
    public let objectId: GitObjectId
    public let crc32: UInt32
    public let offset: Int

    public init(_ objectId: GitObjectId, _ crc32: UInt32, _ offset: Int) {
        self.objectId = objectId
        self.crc32 = crc32
        self.offset = offset
    }
}

public struct GitPackIndexV2: Sendable {
    public let entries: [GitPackIndexEntry]
    public let packChecksum: [UInt8]

    public static func parse(_ bytes: [UInt8], _ limits: GitResourceLimits) throws -> GitPackIndexV2 {
        let headerLength = 8 + 256 * 4
        if bytes.count < headerLength + 40 {
            throw GitException(.packCorrupt, "pack index is truncated")
        }
        if bytes[0] != 0xff || bytes[1] != 0x74 || bytes[2] != 0x4f || bytes[3] != 0x63 || bytes.readUint32(4) != 2 {
            throw GitException(.unsupportedRepositoryFormat, "only pack index version 2 is supported")
        }
        try verifyIndexChecksum(bytes)
        var previous = 0
        for index in 0..<256 {
            let value = bytes.readUint32(8 + index * 4)
            if value < previous {
                throw GitException(.packCorrupt, "pack index fanout is not monotonic")
            }
            previous = value
        }
        let count = previous
        if count > limits.maxRefCount || count > GitResourceLimits.hardMaxRefCount {
            throw GitException(.resourceLimitExceeded, "pack index object count exceeds configured limit")
        }
        let fixedLength = headerLength + count * 28 + 40
        if fixedLength > bytes.count || (bytes.count - fixedLength) % 8 != 0 {
            throw GitException(.packCorrupt, "pack index size is invalid")
        }
        let nameOffset = headerLength
        let crcOffset = nameOffset + count * 20
        let offsetOffset = crcOffset + count * 4
        let largeOffsetOffset = offsetOffset + count * 4
        let largeOffsetCount = (bytes.count - fixedLength) / 8
        var entries = [GitPackIndexEntry]()
        entries.reserveCapacity(count)
        var previousId: GitObjectId?
        for index in 0..<count {
            let id = try GitObjectId.fromSha1Bytes(bytes[(nameOffset + index * 20)..<(nameOffset + (index + 1) * 20)])
            if let previousId, compareBytes(previousId.bytes, id.bytes) >= 0 {
                throw GitException(.packCorrupt, "pack index object IDs are not strictly sorted")
            }
            let packedOffset = bytes.readUint32(offsetOffset + index * 4)
            let offset = (packedOffset & 0x8000_0000) == 0
                ? packedOffset
                : try readLargeOffset(bytes, largeOffsetOffset, packedOffset & 0x7fff_ffff, largeOffsetCount)
            entries.append(GitPackIndexEntry(id, UInt32(bytes.readUint32(crcOffset + index * 4)), offset))
            previousId = id
        }
        return GitPackIndexV2(entries: entries, packChecksum: Array(bytes[(bytes.count - 40)..<(bytes.count - 20)]))
    }

    public func lookup(_ objectId: GitObjectId) -> GitPackIndexEntry? {
        var low = 0
        var high = entries.count - 1
        while low <= high {
            let middle = (low + high) / 2
            let comparison = compareBytes(entries[middle].objectId.bytes, objectId.bytes)
            if comparison == 0 { return entries[middle] }
            if comparison < 0 { low = middle + 1 } else { high = middle - 1 }
        }
        return nil
    }
}

private func readLargeOffset(_ bytes: [UInt8], _ start: Int, _ index: Int, _ count: Int) throws -> Int {
    if index >= count {
        throw GitException(.packCorrupt, "pack index large offset is invalid")
    }
    let offset = start + index * 8
    let high = bytes.readUint32(offset)
    // A set top bit would make a negative 64-bit offset, which no pack has;
    // reject it instead of trapping.
    if high > 0x7fff_ffff {
        throw GitException(.packCorrupt, "pack index large offset is invalid")
    }
    return (high << 32) | bytes.readUint32(offset + 4)
}

private func verifyIndexChecksum(_ bytes: [UInt8]) throws {
    let checksumOffset = bytes.count - 20
    let digest = ForkMeshSha1()
    try digest.add(bytes[0..<checksumOffset])
    if !constantTimeEquals(try digest.close(), Array(bytes[checksumOffset...])) {
        throw GitException(.packChecksumMismatch, "pack index checksum does not match")
    }
}
