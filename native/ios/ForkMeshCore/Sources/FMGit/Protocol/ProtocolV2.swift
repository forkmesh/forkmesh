public struct GitProtocolV2Advertisement: Sendable {
    private let capabilities: [String: String?]

    public func supports(_ command: String) -> Bool { capabilities[command] != nil }

    public func valueOf(_ capability: String) -> String? { capabilities[capability] ?? nil }

    public static func parse(_ packets: [GitPktLine]) throws -> GitProtocolV2Advertisement {
        guard let last = packets.last, last.kind == .flush else {
            throw GitException(.protocolUnsupported, "protocol v2 advertisement is not flush terminated")
        }
        var lines = [String]()
        for packet in packets.dropLast() {
            if packet.kind != .data {
                throw GitException(.protocolUnsupported, "protocol v2 advertisement has unexpected control packet")
            }
            lines.append(try protocolAsciiLine(packet.payload))
        }
        if lines.isEmpty || !lines[0].sameText("version 2") {
            throw GitException(.protocolUnsupported, "protocol v2 version marker is missing")
        }
        var capabilities = [String: String?]()
        for line in lines.dropFirst() {
            let separator = line.u8IndexOf("=")
            let name = separator == -1 ? line : line.u8Substring(0, separator)
            if !isCapabilityName(Array(name.utf8)) || capabilities[name] != nil {
                throw GitException(.protocolUnsupported, "protocol v2 capability is invalid")
            }
            capabilities[name] = .some(separator == -1 ? nil : line.u8Substring(separator + 1))
        }
        return GitProtocolV2Advertisement(capabilities: capabilities)
    }
}

public struct GitRemoteRef: Sendable {
    public let name: String
    public let objectId: GitObjectId
    public let peeled: GitObjectId?
    public let symrefTarget: String?

    public init(name: String, objectId: GitObjectId, peeled: GitObjectId? = nil, symrefTarget: String? = nil) {
        self.name = name
        self.objectId = objectId
        self.peeled = peeled
        self.symrefTarget = symrefTarget
    }
}

public enum GitProtocolV2 {
    /// The agent string sent with every v2 command.
    public static let agent = "forkmesh-git/0.0.0"

    /// Builds an `ls-refs` command requesting peeled tags and symref targets
    /// for refs under `refPrefixes` (all refs when empty).
    public static func lsRefsRequest(
        refPrefixes: [String] = [],
        capabilities: [String] = ["agent=\(GitProtocolV2.agent)"]
    ) throws -> [GitPktLine] {
        if refPrefixes.count > 64 {
            throw GitException(.resourceLimitExceeded, "too many ls-refs prefixes")
        }
        for prefix in refPrefixes {
            let bytes = Array(prefix.utf8)
            // ^[A-Za-z0-9/_.-]{1,255}$
            let shaped = !bytes.isEmpty && bytes.count <= 255 && bytes.allSatisfy { byte in
                isAsciiAlphanumeric(byte) || byte == 0x2f || byte == 0x5f || byte == 0x2e || byte == 0x2d
            }
            if !shaped || prefix.u8Contains("..") {
                throw GitException(.invalidRef, "ls-refs prefix is invalid")
            }
        }
        var packets = [GitPktLine.text("command=ls-refs\n")]
        for capability in capabilities { packets.append(.text("\(try requestCapability(capability))\n")) }
        packets.append(.delimiter())
        packets.append(.text("peel\n"))
        packets.append(.text("symrefs\n"))
        for prefix in refPrefixes { packets.append(.text("ref-prefix \(prefix)\n")) }
        packets.append(.flush())
        return packets
    }

    /// Builds a `fetch` command.
    ///
    /// Thin packs are never requested, so every delta base is present in the
    /// returned pack. Progress is suppressed with `no-progress`.
    public static func fetchRequest(
        wants: [GitObjectId],
        haves: [GitObjectId] = [],
        done: Bool,
        ofsDelta: Bool = true,
        noProgress: Bool = true,
        capabilities: [String] = ["agent=\(GitProtocolV2.agent)"],
        shallow: [GitObjectId] = [],
        deepen: Int? = nil,
        deepenRelative: Bool = false,
        deepenSince: Int64? = nil
    ) throws -> [GitPktLine] {
        if wants.isEmpty {
            throw GitException(.protocolUnsupported, "fetch requires at least one want")
        }
        if wants.count > GitResourceLimits.hardMaxRefCount || haves.count > GitResourceLimits.hardMaxRefCount {
            throw GitException(.resourceLimitExceeded, "fetch request exceeds the want or have limit")
        }
        var packets = [GitPktLine.text("command=fetch\n")]
        for capability in capabilities { packets.append(.text("\(try requestCapability(capability))\n")) }
        packets.append(.delimiter())
        if ofsDelta { packets.append(.text("ofs-delta\n")) }
        if noProgress { packets.append(.text("no-progress\n")) }
        for want in wants { packets.append(.text("want \(want.hex)\n")) }
        for id in shallow { packets.append(.text("shallow \(id.hex)\n")) }
        if let deepen {
            if deepen < 1 { throw GitException(.protocolUnsupported, "fetch deepen must be positive") }
            packets.append(.text("deepen \(deepen)\n"))
            if deepenRelative { packets.append(.text("deepen-relative\n")) }
        }
        if let deepenSince {
            if deepen != nil || deepenSince < 0 {
                throw GitException(.protocolUnsupported, "fetch deepen-since is invalid")
            }
            packets.append(.text("deepen-since \(deepenSince)\n"))
        }
        for have in haves { packets.append(.text("have \(have.hex)\n")) }
        if done { packets.append(.text("done\n")) }
        packets.append(.flush())
        return packets
    }

    private static func requestCapability(_ value: String) throws -> String {
        if !isRequestCapability(Array(value.utf8)) {
            throw GitException(.protocolUnsupported, "protocol v2 request capability is invalid")
        }
        return value
    }

    public static func parseLsRefs(
        _ packets: [GitPktLine],
        maxRefCount: Int = GitResourceLimits.defaultMaxRefCount
    ) throws -> [GitRemoteRef] {
        guard let last = packets.last, last.kind == .flush else {
            throw GitException(.protocolUnsupported, "ls-refs response is not flush terminated")
        }
        var refs = [GitRemoteRef]()
        var names = Set<[UInt8]>()
        for packet in packets.dropLast() {
            if packet.kind != .data {
                throw GitException(.protocolUnsupported, "ls-refs response has unexpected control packet")
            }
            if refs.count >= maxRefCount {
                throw GitException(.resourceLimitExceeded, "ls-refs response exceeds the ref limit")
            }
            let parts = try protocolAsciiLine(packet.payload).u8Split(0x20)
            if parts.count < 2 || parts.contains(where: { $0.utf8.isEmpty }) {
                throw GitException(.protocolUnsupported, "ls-refs record is invalid")
            }
            let name = parts[1]
            if !name.sameText("HEAD") { _ = try GitRefName.parse(name) }
            if !names.insert(Array(name.utf8)).inserted {
                throw GitException(.protocolUnsupported, "ls-refs response repeats a ref")
            }
            var peeled: GitObjectId?
            var symrefTarget: String?
            for attribute in parts.dropFirst(2) {
                if attribute.u8HasPrefix("peeled:") && peeled == nil {
                    peeled = try GitObjectId.parseSha1(attribute.u8Substring(7))
                } else if attribute.u8HasPrefix("symref-target:") && symrefTarget == nil {
                    let target = attribute.u8Substring(14)
                    _ = try GitRefName.branch(target)
                    symrefTarget = target
                } else {
                    throw GitException(.protocolUnsupported, "ls-refs attribute is unsupported")
                }
            }
            refs.append(GitRemoteRef(
                name: name,
                objectId: try GitObjectId.parseSha1(parts[0]),
                peeled: peeled,
                symrefTarget: symrefTarget
            ))
        }
        return refs
    }
}

func isAsciiAlphanumeric(_ byte: UInt8) -> Bool {
    (byte >= 0x30 && byte <= 0x39) || (byte >= 0x41 && byte <= 0x5a) || (byte >= 0x61 && byte <= 0x7a)
}

/// `^[a-z][a-z0-9-]*$`
func isCapabilityName(_ bytes: [UInt8]) -> Bool {
    guard let first = bytes.first, first >= 0x61, first <= 0x7a else { return false }
    return bytes.dropFirst().allSatisfy { ($0 >= 0x61 && $0 <= 0x7a) || ($0 >= 0x30 && $0 <= 0x39) || $0 == 0x2d }
}

/// `^[a-z][a-z0-9-]*(=[\x21-\x7e]+)?$`
func isRequestCapability(_ bytes: [UInt8]) -> Bool {
    let separator = bytes.indexOfByte(0x3d)
    if separator == -1 { return isCapabilityName(bytes) }
    let value = bytes[(separator + 1)...]
    return isCapabilityName(Array(bytes[0..<separator])) && !value.isEmpty
        && value.allSatisfy { $0 >= 0x21 && $0 <= 0x7e }
}

private func protocolAsciiLine(_ bytes: [UInt8]) throws -> String {
    for byte in bytes where byte > 0x7f {
        throw GitException(.protocolUnsupported, "protocol field is not ASCII")
    }
    if bytes.last != 0x0a || bytes.contains(0x0d) || bytes.count == 1 {
        throw GitException(.protocolUnsupported, "protocol field is not newline terminated")
    }
    return String(decoding: bytes.dropLast(), as: UTF8.self)
}
