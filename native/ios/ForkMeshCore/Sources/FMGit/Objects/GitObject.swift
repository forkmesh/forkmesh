public enum GitObjectType: String, CaseIterable, Sendable {
    case blob, tree, commit, tag

    /// The wire spelling.
    public var name: String { rawValue }

    static func parse(_ text: String) -> GitObjectType? {
        for type in allCases where type.rawValue.sameText(text) { return type }
        return nil
    }
}

public struct GitObjectId: Hashable, CustomStringConvertible, Sendable {
    public let bytes: [UInt8]

    private init(unchecked bytes: [UInt8]) { self.bytes = bytes }

    public static func parseSha1(_ value: String) throws -> GitObjectId {
        let bytes = try decodeLowerHex(value)
        if bytes.count != 20 {
            throw GitException(.invalidObject, "expected SHA-1 object ID")
        }
        return GitObjectId(unchecked: bytes)
    }

    public static func fromSha1Bytes(_ value: [UInt8]) throws -> GitObjectId {
        if value.count != 20 {
            throw GitException(.invalidObject, "expected SHA-1 object ID")
        }
        return GitObjectId(unchecked: value)
    }

    static func fromSha1Bytes(_ value: ArraySlice<UInt8>) throws -> GitObjectId {
        try fromSha1Bytes(Array(value))
    }

    public static func sha1Of(_ canonicalBytes: [UInt8]) -> GitObjectId {
        GitObjectId(unchecked: ForkMeshSha1.hash(canonicalBytes))
    }

    public var hex: String { encodeLowerHex(bytes) }

    public static func == (left: GitObjectId, right: GitObjectId) -> Bool {
        constantTimeEquals(left.bytes, right.bytes)
    }

    public func hash(into hasher: inout Hasher) { hasher.combine(bytes) }

    public var description: String { hex }
}

public struct GitObjectFrame: Sendable {
    public let type: GitObjectType
    public let body: [UInt8]

    public init(_ type: GitObjectType, _ body: [UInt8]) {
        self.type = type
        self.body = body
    }

    public static func encode(_ type: GitObjectType, _ body: [UInt8]) -> [UInt8] {
        var output = Array("\(type.name) \(body.count)\u{0}".utf8)
        output.reserveCapacity(output.count + body.count)
        output.append(contentsOf: body)
        return output
    }

    public static func decode(_ bytes: [UInt8], _ limits: GitResourceLimits) throws -> GitObjectFrame {
        let separator = bytes.indexOfByte(0)
        if separator <= 0 {
            throw GitException(.invalidObject, "object header is missing NUL")
        }
        let header = Array(bytes[0..<separator])
        for byte in header where byte > 0x7f {
            throw GitException(.invalidObject, "object header is not ASCII")
        }
        let separatorIndex = header.indexOfByte(0x20)
        var lastSpace = -1
        for (index, byte) in header.enumerated() where byte == 0x20 { lastSpace = index }
        if separatorIndex <= 0 || separatorIndex != lastSpace {
            throw GitException(.invalidObject, "object header is not canonical")
        }
        let typeText = String(decoding: header[0..<separatorIndex], as: UTF8.self)
        let sizeBytes = Array(header[(separatorIndex + 1)...])
        // ^(0|[1-9][0-9]*)$
        let canonicalSize = !sizeBytes.isEmpty
            && sizeBytes.allSatisfy { $0 >= 0x30 && $0 <= 0x39 }
            && (sizeBytes.count == 1 || sizeBytes[0] != 0x30)
        // 18 digits always fit in Int64; longer sizes are far past every limit.
        guard canonicalSize else {
            throw GitException(.invalidObject, "object size is not canonical")
        }
        let declaredSize = sizeBytes.count > 18
            ? Int.max
            : Int(String(decoding: sizeBytes, as: UTF8.self)) ?? Int.max
        let actualSize = bytes.count - separator - 1
        if declaredSize > limits.maxObjectBytes || actualSize > limits.maxObjectBytes {
            throw GitException(.resourceLimitExceeded, "object exceeds configured limit")
        }
        if declaredSize != actualSize {
            throw GitException(.invalidObject, "object body length does not match header")
        }
        guard let type = GitObjectType.parse(typeText) else {
            throw GitException(.invalidObject, "unsupported object type")
        }
        return GitObjectFrame(type, Array(bytes[(separator + 1)...]))
    }
}
