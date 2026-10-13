import Foundation

public final class GitPackObjectStore {
    public static let maxDeltaChain = 4096

    public let gitDirectory: String
    public let limits: GitResourceLimits
    public let detector: GitSha1CollisionDetector?

    private var indexFileList: [String]?
    private var indexes = [String: GitPackIndexV2]()
    private var packs = [String: Data]()
    private var caches = [String: PackDeltaBaseCache]()

    public init(_ gitDirectory: String, _ limits: GitResourceLimits, detector: GitSha1CollisionDetector? = nil) {
        self.gitDirectory = gitDirectory
        self.limits = limits
        self.detector = detector
    }

    private var packDirectory: String { GitFS.join(GitFS.join(gitDirectory, "objects"), "pack") }

    public func invalidate() {
        indexFileList = nil
        indexes.removeAll()
        packs.removeAll()
        caches.removeAll()
    }

    private func indexFiles() throws -> [String] {
        if let indexFileList { return indexFileList }
        let directory = packDirectory
        var files = [String]()
        if GitFS.directoryExists(directory) {
            files = try GitFS.listNames(directory)
                .filter { $0.u8HasSuffix(".idx") }
                .map { GitFS.join(directory, $0) }
                .filter { GitFS.entityType($0) == .file }
        }
        indexFileList = files
        return files
    }

    private func locate(_ objectId: GitObjectId) throws -> (file: String, index: GitPackIndexV2, entry: GitPackIndexEntry)? {
        let listedBefore = indexFileList != nil
        if let found = try search(objectId) { return found }
        if !listedBefore { return nil }
        indexFileList = nil
        return try search(objectId)
    }

    private func search(_ objectId: GitObjectId) throws -> (file: String, index: GitPackIndexV2, entry: GitPackIndexEntry)? {
        for file in try indexFiles() {
            let index = try indexFor(file)
            if let entry = index.lookup(objectId) { return (file, index, entry) }
        }
        return nil
    }

    public func contains(_ objectId: GitObjectId) throws -> Bool {
        try locate(objectId) != nil
    }

    public func read(_ objectId: GitObjectId) throws -> GitObjectFrame {
        guard let found = try locate(objectId) else {
            throw GitException(.invalidObject, "object is absent")
        }
        let packFile = found.file.u8Substring(0, found.file.utf8.count - 4) + ".pack"
        let pack = try packFor(packFile, found.index)
        var cache = caches[packFile] ?? PackDeltaBaseCache()
        defer { caches[packFile] = cache }
        let object = try pack.withUnsafeBytes { raw in
            try resolve(raw, found.entry.offset, found.index, &cache)
        }
        let screening = try Sha1CollisionScreen(detector: detector).screenObject(
            type: object.type,
            body: object.body,
            origin: .trustedLocal,
            limits: limits
        )
        try screening.requireApproved()
        if try GitObjectId.fromSha1Bytes(screening.digest) != objectId {
            throw GitException(.objectHashMismatch, "packed object does not match its index entry")
        }
        return GitObjectFrame(object.type, object.body)
    }

    private func indexFor(_ file: String) throws -> GitPackIndexV2 {
        if let cached = indexes[file] { return cached }
        let index = try GitPackIndexV2.parse(try GitFS.readBytes(file), limits)
        indexes[file] = index
        return index
    }

    private func packFor(_ packFile: String, _ index: GitPackIndexV2) throws -> Data {
        if let cached = packs[packFile] { return cached }
        if !GitFS.fileExists(packFile) {
            throw GitException(.packCorrupt, "pack file is missing")
        }
        let data: Data
        do {
            data = try Data(contentsOf: URL(fileURLWithPath: packFile), options: .alwaysMapped)
        } catch {
            throw GitFileSystemError(operation: "open", path: packFile, code: EIO)
        }
        if data.count < 32 || !constantTimeEquals(index.packChecksum, Array(data.suffix(20))) {
            throw GitException(.packChecksumMismatch, "pack and index checksums do not match")
        }
        let header = [UInt8](data.prefix(8))
        if Array(header[0..<4]) != Array("PACK".utf8) {
            throw GitException(.packCorrupt, "pack signature is invalid")
        }
        if header.readUint32(4) != 2 {
            throw GitException(.unsupportedRepositoryFormat, "only pack version 2 is supported")
        }
        packs[packFile] = data
        return data
    }

    private func resolve(
        _ raw: UnsafeRawBufferPointer,
        _ offset: Int,
        _ index: GitPackIndexV2,
        _ cache: inout PackDeltaBaseCache
    ) throws -> (type: GitObjectType, body: [UInt8]) {
        let trailer = raw.count - 20
        var chain = [(offset: Int, delta: [UInt8])]()
        var visited = Set<Int>()
        var current = offset
        var base: (type: GitObjectType, body: [UInt8])
        while true {
            if let cached = cache.lookup(current) {
                base = cached
                break
            }
            if !visited.insert(current).inserted {
                throw GitException(.packCorrupt, "pack delta cycle")
            }
            if chain.count >= Self.maxDeltaChain {
                throw GitException(.deltaDepthExceeded, "pack delta chain exceeds depth limit")
            }
            if current < 12 || current >= trailer {
                throw GitException(.packCorrupt, "pack object offset is invalid")
            }
            let header = try readObjectHeader(raw, current, trailer, limits)
            var dataOffset = header.nextOffset
            var next: Int?
            var type = GitObjectType.blob
            switch header.kind {
            case .commit: type = .commit
            case .tree: type = .tree
            case .blob: type = .blob
            case .tag: type = .tag
            case .ofsDelta:
                let parsed = try readOffsetDeltaBase(raw, dataOffset, trailer)
                dataOffset = parsed.nextOffset
                let baseOffset = current - parsed.distance
                if baseOffset < 12 {
                    throw GitException(.packCorrupt, "offset delta base is before pack data")
                }
                next = baseOffset
            case .refDelta:
                if trailer - dataOffset < 20 {
                    throw GitException(.packCorrupt, "reference delta base is truncated")
                }
                let baseId = try GitObjectId.fromSha1Bytes(Array(raw[dataOffset..<(dataOffset + 20)]))
                dataOffset += 20
                guard let baseEntry = index.lookup(baseId) else {
                    throw GitException(.unsupportedRepositoryFormat, "thin packs are unsupported")
                }
                next = baseEntry.offset
            }
            let body = try inflatePackEntry(raw, dataOffset, trailer, header.size, limits)
            if let next {
                chain.append((current, body))
                current = next
                continue
            }
            base = (type, body)
            cache.store(current, base)
            break
        }
        for link in chain.reversed() {
            base = (base.type, try applyDelta(base.body, link.delta, limits))
            cache.store(link.offset, base)
        }
        return base
    }
}

struct PackDeltaBaseCache {
    static let maxBytes = 32 * 1024 * 1024
    static let maxEntryBytes = 8 * 1024 * 1024

    private var entries = [Int: (type: GitObjectType, body: [UInt8])]()
    private var order = [Int]()
    private var head = 0
    private var bytes = 0

    func lookup(_ offset: Int) -> (type: GitObjectType, body: [UInt8])? { entries[offset] }

    mutating func store(_ offset: Int, _ object: (type: GitObjectType, body: [UInt8])) {
        if object.body.count > Self.maxEntryBytes || entries[offset] != nil { return }
        entries[offset] = object
        order.append(offset)
        bytes += object.body.count
        while bytes > Self.maxBytes && head < order.count {
            if let evicted = entries.removeValue(forKey: order[head]) { bytes -= evicted.body.count }
            head += 1
        }
        if head > 1024 && head * 2 > order.count {
            order.removeFirst(head)
            head = 0
        }
    }
}
