// zlib reports the consumed input directly, so the reader validates the zlib
// header and lets one inflate call find the end of each entry (as the Kotlin
// port does with java.util.zip.Inflater). Every malformed stream fails
// `packCorrupt`.

public struct GitPackObject: Sendable {
    public let offset: Int
    public let type: GitObjectType
    public let body: [UInt8]
    public let objectId: GitObjectId

    /// CRC-32 of the raw pack entry bytes (header, base reference, and
    /// compressed data) as recorded by pack index version 2.
    public let crc32: UInt32
}

public struct GitPack: Sendable {
    public let objects: [GitPackObject]

    /// The verified 20-byte SHA-1 trailer of the pack.
    public let checksum: [UInt8]

    private let positions: [GitObjectId: Int]

    fileprivate init(_ objects: [GitPackObject], _ checksum: [UInt8]) {
        self.objects = objects
        self.checksum = checksum
        var positions = [GitObjectId: Int]()
        // The first entry wins.
        for (index, object) in objects.enumerated() where positions[object.objectId] == nil {
            positions[object.objectId] = index
        }
        self.positions = positions
    }

    public func lookup(_ objectId: GitObjectId) -> GitPackObject? {
        positions[objectId].map { objects[$0] }
    }
}

public enum GitPackReader {
    public static func parse(
        _ bytes: [UInt8],
        _ limits: GitResourceLimits,
        origin: GitSha1ObjectOrigin = .trustedLocal,
        cancellation: GitCancellationToken? = nil,
        detector: GitSha1CollisionDetector? = nil,
        progress: ((_ resolved: Int, _ total: Int) -> Void)? = nil
    ) throws -> GitPack {
        try cancellation?.throwIfCancelled()
        if bytes.count < 32 {
            throw GitException(.packCorrupt, "pack is truncated")
        }
        let trailerOffset = bytes.count - 20
        try verifyTrailer(bytes, trailerOffset)
        if Array(bytes[0..<4]) != Array("PACK".utf8) {
            throw GitException(.packCorrupt, "pack signature is invalid")
        }
        if bytes.readUint32(4) != 2 {
            throw GitException(.unsupportedRepositoryFormat, "only pack version 2 is supported")
        }
        let count = bytes.readUint32(8)
        if count > GitResourceLimits.hardMaxRefCount || count > limits.maxRefCount {
            throw GitException(.resourceLimitExceeded, "pack object count exceeds configured limit")
        }
        var offset = 12
        var entries = [PackEntry]()
        entries.reserveCapacity(count)
        for _ in 0..<count {
            if offset >= trailerOffset {
                throw GitException(.packCorrupt, "pack object header is truncated")
            }
            let objectOffset = offset
            let header = try readObjectHeader(bytes, offset, trailerOffset, limits)
            offset = header.nextOffset
            var baseOffset: Int?
            var baseId: GitObjectId?
            if header.kind == .ofsDelta {
                let parsed = try readOffsetDeltaBase(bytes, offset, trailerOffset)
                offset = parsed.nextOffset
                let base = objectOffset - parsed.distance
                if base < 12 {
                    throw GitException(.packCorrupt, "offset delta base is before pack data")
                }
                baseOffset = base
            } else if header.kind == .refDelta {
                if trailerOffset - offset < 20 {
                    throw GitException(.packCorrupt, "reference delta base is truncated")
                }
                baseId = try GitObjectId.fromSha1Bytes(bytes[offset..<(offset + 20)])
                offset += 20
            }
            let inflated = try inflateEntry(bytes, offset, trailerOffset, header.size, limits)
            entries.append(PackEntry(
                offset: objectOffset,
                kind: header.kind,
                declaredSize: header.size,
                body: inflated.body,
                crc32: gitCrc32(bytes, objectOffset, inflated.end),
                baseOffset: baseOffset,
                baseId: baseId
            ))
            offset = inflated.end
        }
        if offset != trailerOffset {
            throw GitException(.packCorrupt, "pack has trailing object data")
        }
        let state = ResolutionState(limits, entries, origin, cancellation, detector)
        var objects = [GitPackObject]()
        objects.reserveCapacity(entries.count)
        progress?(0, entries.count)
        for (index, entry) in entries.enumerated() {
            objects.append(try state.resolve(entry, 0))
            if let progress, (index + 1) % 256 == 0 || index + 1 == entries.count { progress(index + 1, entries.count) }
        }
        return GitPack(objects, Array(bytes[trailerOffset...]))
    }
}

enum PackObjectKind { case commit, tree, blob, tag, ofsDelta, refDelta }

func readObjectHeader<Bytes: RandomAccessCollection>(
    _ bytes: Bytes, _ start: Int, _ limit: Int, _ limits: GitResourceLimits
) throws -> (kind: PackObjectKind, size: Int, nextOffset: Int) where Bytes.Element == UInt8, Bytes.Index == Int {
    var offset = start
    var current = Int(try readByte(bytes, offset, limit))
    offset += 1
    let kind: PackObjectKind
    switch (current >> 4) & 7 {
    case 1: kind = .commit
    case 2: kind = .tree
    case 3: kind = .blob
    case 4: kind = .tag
    case 6: kind = .ofsDelta
    case 7: kind = .refDelta
    default: throw GitException(.packCorrupt, "pack object type is invalid")
    }
    var size = current & 0x0f
    var shift = 4
    while current & 0x80 != 0 {
        if shift > 56 || offset >= limit {
            throw GitException(.packCorrupt, "pack object size is invalid")
        }
        current = Int(try readByte(bytes, offset, limit))
        offset += 1
        // `&<<` is a wrapping shift; a wrapped size fails the limit below.
        size |= (current & 0x7f) &<< shift
        shift += 7
    }
    if size < 0 || size > limits.maxObjectBytes {
        throw GitException(.resourceLimitExceeded, "pack object exceeds configured limit")
    }
    return (kind, size, offset)
}

func readOffsetDeltaBase<Bytes: RandomAccessCollection>(
    _ bytes: Bytes, _ start: Int, _ limit: Int
) throws -> (distance: Int, nextOffset: Int) where Bytes.Element == UInt8, Bytes.Index == Int {
    var offset = start
    var current = Int(try readByte(bytes, offset, limit))
    offset += 1
    var distance = current & 0x7f
    while current & 0x80 != 0 {
        if offset >= limit || distance > 0x00ff_ffff_ffff_ffff {
            throw GitException(.packCorrupt, "offset delta distance is invalid")
        }
        current = Int(try readByte(bytes, offset, limit))
        offset += 1
        distance = ((distance + 1) << 7) | (current & 0x7f)
    }
    if distance == 0 {
        throw GitException(.packCorrupt, "offset delta distance is zero")
    }
    return (distance, offset)
}

private final class PackEntry {
    let offset: Int
    let kind: PackObjectKind
    let crc32: UInt32
    let declaredSize: Int
    var body: [UInt8]
    let baseOffset: Int?
    let baseId: GitObjectId?
    var resolved: GitPackObject?
    var resolving = false

    init(
        offset: Int, kind: PackObjectKind, declaredSize: Int, body: [UInt8], crc32: UInt32,
        baseOffset: Int?, baseId: GitObjectId?
    ) {
        self.offset = offset
        self.kind = kind
        self.declaredSize = declaredSize
        self.body = body
        self.crc32 = crc32
        self.baseOffset = baseOffset
        self.baseId = baseId
    }
}

private final class ResolutionState {
    let limits: GitResourceLimits
    let entries: [PackEntry]
    let origin: GitSha1ObjectOrigin
    let cancellation: GitCancellationToken?
    let screen: Sha1CollisionScreen
    var byOffset = [Int: PackEntry]()
    var byId = [GitObjectId: PackEntry]()
    var totalBytes = 0

    init(
        _ limits: GitResourceLimits, _ entries: [PackEntry], _ origin: GitSha1ObjectOrigin,
        _ cancellation: GitCancellationToken?, _ detector: GitSha1CollisionDetector?
    ) {
        self.limits = limits
        self.entries = entries
        self.origin = origin
        self.cancellation = cancellation
        screen = Sha1CollisionScreen(detector: detector)
        for entry in entries { byOffset[entry.offset] = entry }
    }

    func resolve(_ entry: PackEntry, _ depth: Int) throws -> GitPackObject {
        try cancellation?.throwIfCancelled()
        if let cached = entry.resolved { return cached }
        if depth >= 32 {
            throw GitException(.deltaDepthExceeded, "pack delta chain exceeds depth limit")
        }
        if entry.resolving {
            throw GitException(.packCorrupt, "pack delta cycle")
        }
        entry.resolving = true
        defer { entry.resolving = false }
        let type: GitObjectType
        let body: [UInt8]
        switch entry.kind {
        case .commit: (type, body) = (.commit, entry.body)
        case .tree: (type, body) = (.tree, entry.body)
        case .blob: (type, body) = (.blob, entry.body)
        case .tag: (type, body) = (.tag, entry.body)
        case .ofsDelta, .refDelta: (type, body) = try delta(entry, depth)
        }
        // The screen's digest is the object's SHA-1, so it is reused as the
        // object ID instead of hashing the frame twice.
        let screening = try screen.screenObject(
            type: type,
            body: body,
            origin: origin,
            limits: limits,
            cancellation: cancellation
        )
        try screening.requireApproved()
        let result = GitPackObject(
            offset: entry.offset,
            type: type,
            body: body,
            objectId: try GitObjectId.fromSha1Bytes(screening.digest),
            crc32: entry.crc32
        )
        entry.resolved = result
        // The resolved object owns the body now; a delta's instructions are done with.
        entry.body = []
        byId[result.objectId] = entry
        totalBytes += body.count
        if totalBytes > limits.maxInflatedBytes {
            throw GitException(.resourceLimitExceeded, "resolved pack bytes exceed configured limit")
        }
        return result
    }

    private func delta(_ entry: PackEntry, _ depth: Int) throws -> (GitObjectType, [UInt8]) {
        var baseEntry: PackEntry?
        if let baseOffset = entry.baseOffset {
            baseEntry = byOffset[baseOffset]
        } else if let baseId = entry.baseId {
            baseEntry = try byId[baseId] ?? findAndResolve(baseId, depth + 1)
        }
        guard let baseEntry else {
            throw GitException(.unsupportedRepositoryFormat, "thin packs are unsupported")
        }
        let base = try resolve(baseEntry, depth + 1)
        return (base.type, try applyDelta(base.body, entry.body, limits))
    }

    private func findAndResolve(_ id: GitObjectId, _ depth: Int) throws -> PackEntry? {
        for candidate in entries where try resolve(candidate, depth).objectId == id { return candidate }
        return nil
    }
}

func applyDelta(_ base: [UInt8], _ delta: [UInt8], _ limits: GitResourceLimits) throws -> [UInt8] {
    var offset = 0
    func readByte() throws -> Int {
        if offset == delta.count { throw GitException(.packCorrupt, "delta is truncated") }
        defer { offset += 1 }
        return Int(delta[offset])
    }
    func readVariableInteger() throws -> Int {
        var value = 0
        var shift = 0
        while true {
            if shift > 56 { throw GitException(.packCorrupt, "delta variable integer is invalid") }
            let byte = try readByte()
            value |= (byte & 0x7f) &<< shift
            if byte & 0x80 == 0 { return value }
            shift += 7
        }
    }
    let sourceSize = try readVariableInteger()
    let targetSize = try readVariableInteger()
    if sourceSize != base.count || targetSize < 0 || targetSize > limits.maxObjectBytes {
        throw GitException(.packCorrupt, "delta header is invalid")
    }
    var output = [UInt8]()
    output.reserveCapacity(targetSize)
    while offset != delta.count {
        let instruction = try readByte()
        if instruction == 0 {
            throw GitException(.packCorrupt, "delta instruction is zero")
        }
        if instruction & 0x80 == 0 {
            if instruction > delta.count - offset { throw GitException(.packCorrupt, "delta is truncated") }
            if output.count + instruction > targetSize {
                throw GitException(.packCorrupt, "delta insert exceeds target size")
            }
            output.append(contentsOf: delta[offset..<(offset + instruction)])
            offset += instruction
            continue
        }
        var sourceOffset = 0
        var size = 0
        if instruction & 0x01 != 0 { sourceOffset |= try readByte() }
        if instruction & 0x02 != 0 { sourceOffset |= try readByte() << 8 }
        if instruction & 0x04 != 0 { sourceOffset |= try readByte() << 16 }
        if instruction & 0x08 != 0 { sourceOffset |= try readByte() << 24 }
        if instruction & 0x10 != 0 { size |= try readByte() }
        if instruction & 0x20 != 0 { size |= try readByte() << 8 }
        if instruction & 0x40 != 0 { size |= try readByte() << 16 }
        if size == 0 { size = 0x10000 }
        if sourceOffset > base.count || size > base.count - sourceOffset || output.count + size > targetSize {
            throw GitException(.packCorrupt, "delta copy is invalid")
        }
        output.append(contentsOf: base[sourceOffset..<(sourceOffset + size)])
    }
    if output.count != targetSize {
        throw GitException(.packCorrupt, "delta result size does not match header")
    }
    return output
}

/// Validates the zlib header, inflates one pack entry and reports where its
/// zlib stream (including the Adler-32 trailer) ends.
private func inflateEntry(
    _ bytes: [UInt8], _ start: Int, _ limit: Int, _ declaredSize: Int, _ limits: GitResourceLimits
) throws -> (body: [UInt8], end: Int) {
    try bytes.withUnsafeBytes { raw in try inflatePackEntrySpan(raw, start, limit, declaredSize, limits) }
}

func inflatePackEntry(
    _ raw: UnsafeRawBufferPointer, _ start: Int, _ limit: Int, _ declaredSize: Int, _ limits: GitResourceLimits
) throws -> [UInt8] {
    try inflatePackEntrySpan(raw, start, limit, declaredSize, limits).body
}

private func inflatePackEntrySpan(
    _ bytes: UnsafeRawBufferPointer, _ start: Int, _ limit: Int, _ declaredSize: Int, _ limits: GitResourceLimits
) throws -> (body: [UInt8], end: Int) {
    if limit - start < 6 {
        throw GitException(.packCorrupt, "zlib stream is truncated")
    }
    let cmf = Int(bytes[start])
    let flg = Int(bytes[start + 1])
    if cmf & 0x0f != 8 || cmf >> 4 > 7 || ((cmf << 8) | flg) % 31 != 0 || flg & 0x20 != 0 {
        throw GitException(.packCorrupt, "zlib header is invalid")
    }
    do {
        // One spare byte detects a stream that inflates past its declared size.
        let inflated = try GitZlib.inflate(
            bytes, start: start, limit: limit, maximumOutput: declaredSize + 1, sizeHint: declaredSize
        )
        if inflated.output.count != declaredSize || inflated.output.count > limits.maxObjectBytes {
            throw GitException(.packCorrupt, "pack object size does not match header")
        }
        return (inflated.output, start + inflated.consumed)
    } catch GitZlibFailure.outputLimitExceeded {
        throw GitException(.packCorrupt, "pack object size does not match header")
    } catch GitZlibFailure.truncated {
        throw GitException(.packCorrupt, "deflate stream is truncated")
    } catch is GitZlibFailure {
        throw GitException(.packCorrupt, "pack zlib stream is invalid")
    }
}

private func verifyTrailer(_ bytes: [UInt8], _ trailerOffset: Int) throws {
    let digest = ForkMeshSha1()
    try digest.add(bytes[0..<trailerOffset])
    if !constantTimeEquals(try digest.close(), Array(bytes[trailerOffset...])) {
        throw GitException(.packChecksumMismatch, "pack trailer checksum does not match")
    }
}

private func readByte<Bytes: RandomAccessCollection>(
    _ bytes: Bytes, _ offset: Int, _ limit: Int
) throws -> UInt8 where Bytes.Element == UInt8, Bytes.Index == Int {
    if offset >= limit {
        throw GitException(.packCorrupt, "pack is truncated")
    }
    return bytes[offset]
}
