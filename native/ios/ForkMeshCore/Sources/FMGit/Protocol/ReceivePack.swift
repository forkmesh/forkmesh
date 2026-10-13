public struct GitPushRefUpdate: Sendable {
    public let refName: String
    public let expectedOld: GitObjectId
    public let next: GitObjectId

    public init(refName: String, expectedOld: GitObjectId, next: GitObjectId) throws {
        _ = try GitRefName.parse(refName)
        if next == GitReceivePack.zeroId {
            throw GitException(.invalidRef, "ref deletion is not supported by push")
        }
        self.refName = refName
        self.expectedOld = expectedOld
        self.next = next
    }
}

public struct GitPushRefStatus: Sendable {
    public let refName: String
    public let code: GitErrorCode?
    public let message: String?

    public init(_ refName: String, _ code: GitErrorCode?, message: String? = nil) {
        self.refName = refName
        self.code = code
        self.message = message
    }

    public var accepted: Bool { code == nil }
}

public struct GitReceivePackStatus: Sendable {
    public let unpackOk: Bool
    public let refs: [GitPushRefStatus]
    public let unpackMessage: String?

    public init(_ unpackOk: Bool, _ refs: [GitPushRefStatus], unpackMessage: String? = nil) {
        self.unpackOk = unpackOk
        self.refs = refs
        self.unpackMessage = unpackMessage
    }
}

/// Parsed v0 `git-receive-pack` reference advertisement.
public struct GitReceivePackAdvertisement: Sendable {
    /// Advertised refs by full name (an empty repository advertises none).
    public let refs: [String: GitObjectId]

    /// Raw capability tokens, for example `report-status-v2` or `agent=git/2`.
    public let capabilities: Set<String>

    /// The tokens in advertisement order, which decides `valueOf` when a
    /// remote repeats a capability.
    private let orderedCapabilities: [String]

    init(refs: [String: GitObjectId], orderedCapabilities: [String]) {
        self.refs = refs
        self.orderedCapabilities = orderedCapabilities
        capabilities = Set(orderedCapabilities)
    }

    public func supports(_ name: String) -> Bool {
        capabilities.contains { $0.sameText(name) || $0.u8HasPrefix("\(name)=") }
    }

    public func valueOf(_ name: String) -> String? {
        orderedCapabilities.first { $0.u8HasPrefix("\(name)=") }
            .map { $0.u8Substring(name.utf8.count + 1) }
    }

    /// Parses the packets that follow the smart HTTP service banner.
    public static func parse(
        _ packets: [GitPktLine],
        maxRefCount: Int = GitResourceLimits.defaultMaxRefCount
    ) throws -> GitReceivePackAdvertisement {
        guard let last = packets.last, last.kind == .flush else {
            throw GitException(.protocolUnsupported, "receive-pack advertisement is not flush terminated")
        }
        var refs = [String: GitObjectId]()
        var capabilities = [String]()
        var first = true
        for packet in packets.dropLast() {
            if packet.kind != .data {
                throw GitException(.protocolUnsupported, "receive-pack advertisement has unexpected control packet")
            }
            let payload = packet.payload
            if first && payload == Array("version 1\n".utf8) { continue }
            var refPart: [UInt8]
            if first {
                let separator = payload.indexOfByte(0)
                if separator == -1 {
                    throw GitException(.protocolUnsupported, "receive-pack advertisement lacks capabilities")
                }
                refPart = try advertisementAscii(Array(payload[0..<separator]))
                let capabilityText = try advertisementAscii(Array(payload[(separator + 1)...]))
                if capabilityText.last != 0x0a {
                    throw GitException(.protocolUnsupported, "receive-pack capabilities are malformed")
                }
                let trimmed = String(decoding: capabilityText, as: UTF8.self).dartTrim()
                for token in trimmed.u8Split(0x20) {
                    if token.utf8.isEmpty { continue }
                    if !isCapabilityToken(Array(token.utf8)) {
                        throw GitException(.protocolUnsupported, "receive-pack capability token is invalid")
                    }
                    if !capabilities.contains(where: { $0.sameText(token) }) { capabilities.append(token) }
                }
                first = false
            } else {
                if payload.contains(0) {
                    throw GitException(.protocolUnsupported, "receive-pack advertisement repeats capabilities")
                }
                refPart = try advertisementAscii(payload)
                if refPart.last != 0x0a {
                    throw GitException(.protocolUnsupported, "receive-pack ref record is malformed")
                }
                refPart.removeLast()
            }
            if refPart.count < 42 || refPart[40] != 0x20 {
                throw GitException(.protocolUnsupported, "receive-pack ref record is invalid")
            }
            let objectId = try GitObjectId.parseSha1(String(decoding: refPart[0..<40], as: UTF8.self))
            let name = String(decoding: refPart[41...], as: UTF8.self)
            if name.sameText("capabilities^{}") {
                if objectId != GitReceivePack.zeroId || !refs.isEmpty {
                    throw GitException(.protocolUnsupported, "receive-pack empty advertisement is invalid")
                }
                continue
            }
            if name.u8HasSuffix("^{}") {
                // Peeled tag values are informational for push; keep them out of the
                // ref map but accept them as git advertises them.
                _ = try GitRefName.parse(name.u8Substring(0, name.utf8.count - 3))
                continue
            }
            if !name.sameText("HEAD") { _ = try GitRefName.parse(name) }
            if refs.count >= maxRefCount {
                throw GitException(.resourceLimitExceeded, "receive-pack advertisement exceeds the ref limit")
            }
            if refs[name] != nil {
                throw GitException(.protocolUnsupported, "receive-pack advertisement repeats a ref")
            }
            refs[name] = objectId
        }
        return GitReceivePackAdvertisement(refs: refs, orderedCapabilities: capabilities)
    }
}

/// `^[A-Za-z0-9_-]+(=[\x21-\x7e]*)?$`
private func isCapabilityToken(_ bytes: [UInt8]) -> Bool {
    let separator = bytes.indexOfByte(0x3d)
    let name = separator == -1 ? bytes[...] : bytes[0..<separator]
    if name.isEmpty || !name.allSatisfy({ isAsciiAlphanumeric($0) || $0 == 0x5f || $0 == 0x2d }) { return false }
    if separator == -1 { return true }
    return bytes[(separator + 1)...].allSatisfy { $0 >= 0x21 && $0 <= 0x7e }
}

public enum GitReceivePack {
    public static let zeroId = try! GitObjectId.parseSha1(String(repeating: "0", count: 40))

    public static let agent = "forkmesh-git/0.0.0"

    /// Chooses the request capabilities for a push against `advertisement`.
    ///
    /// `report-status-v2` (or `report-status`) is mandatory so that every ref
    /// receives a typed result; `side-band-64k`, `quiet`, `atomic`, and
    /// `object-format=sha1` are used when advertised. `delete-refs` is never
    /// requested.
    public static func selectCapabilities(_ advertisement: GitReceivePackAdvertisement) throws -> [String] {
        let objectFormat = advertisement.valueOf("object-format")
        if let objectFormat, !objectFormat.sameText("sha1") {
            throw GitException(.unsupportedRepositoryFormat, "remote object format is unsupported")
        }
        var selected = [String]()
        if advertisement.supports("report-status-v2") {
            selected.append("report-status-v2")
        } else if advertisement.supports("report-status") {
            selected.append("report-status")
        } else {
            throw GitException(.protocolUnsupported, "remote does not support report-status")
        }
        if advertisement.supports("side-band-64k") { selected.append("side-band-64k") }
        if advertisement.supports("quiet") { selected.append("quiet") }
        if advertisement.supports("atomic") { selected.append("atomic") }
        if objectFormat != nil { selected.append("object-format=sha1") }
        selected.append("agent=\(agent)")
        return selected
    }

    public static func commandPackets(
        _ updates: [GitPushRefUpdate],
        capabilities: [String] = ["report-status-v2", "side-band-64k"]
    ) throws -> [GitPktLine] {
        if updates.isEmpty || updates.count > 100_000 {
            throw GitException(.invalidRef, "push must contain a bounded non-empty set of updates")
        }
        for token in capabilities where !isRequestCapability(Array(token.utf8)) || token.sameText("delete-refs") {
            throw GitException(.protocolUnsupported, "push capability is invalid")
        }
        var names = Set<[UInt8]>()
        var packets = [GitPktLine]()
        for (index, update) in updates.enumerated() {
            if !names.insert(Array(update.refName.utf8)).inserted {
                throw GitException(.invalidRef, "duplicate push ref")
            }
            let suffix = index == 0 ? "\u{0}\(capabilities.joined(separator: " "))" : ""
            let line = "\(update.expectedOld.hex) \(update.next.hex) \(update.refName)\(suffix)\n"
            guard let encoded = asciiEncode(line) else {
                throw GitArgumentError("refName", "push command is not ASCII")
            }
            packets.append(.data(encoded))
        }
        packets.append(.flush())
        return packets
    }

    /// Parses a receive-pack response body.
    ///
    /// When `sideband` is true the body is demultiplexed first and the report
    /// is read from channel 1.
    public static func parseResponse(
        _ body: [UInt8],
        sideband: Bool,
        maxPackets: Int = GitResourceLimits.defaultMaxPktLines,
        maxProgressBytes: Int = GitResourceLimits.defaultMaxSidebandBytes
    ) throws -> GitReceivePackStatus {
        let packets = try GitPktLine.decodeAll(body, maximumPackets: maxPackets)
        if !sideband { return try parseReportStatus(packets) }
        guard let last = packets.last, last.kind == .flush else {
            throw GitException(.protocolUnsupported, "receive-pack sideband response is not flush terminated")
        }
        let demuxed = try GitSideband.demux(packets.dropLast(), maxProgressBytes: maxProgressBytes)
        return try parseReportStatus(try GitPktLine.decodeAll(demuxed.data, maximumPackets: maxPackets))
    }

    public static func parseReportStatus(_ packets: [GitPktLine]) throws -> GitReceivePackStatus {
        if packets.count < 2 || packets[packets.count - 1].kind != .flush {
            throw GitException(.protocolUnsupported, "receive-pack status framing is invalid")
        }
        var lines = [String]()
        for packet in packets.dropLast() {
            if packet.kind != .data {
                throw GitException(.protocolUnsupported, "receive-pack status has unexpected control packet")
            }
            lines.append(try statusLine(packet.payload))
        }
        let unpackOk = lines[0].sameText("unpack ok")
        if !unpackOk && !lines[0].u8HasPrefix("unpack ") {
            throw GitException(.protocolUnsupported, "receive-pack unpack status is invalid")
        }
        let unpackMessage = unpackOk ? nil : redactRemoteText(Array(lines[0].u8Substring(7).utf8))
        var refs = [GitPushRefStatus]()
        var seen = Set<[UInt8]>()
        for line in lines.dropFirst() {
            if line.u8HasPrefix("option ") {
                if refs.isEmpty || !refs[refs.count - 1].accepted {
                    throw GitException(.protocolUnsupported, "receive-pack option line has no preceding ok status")
                }
                continue
            }
            if line.u8HasPrefix("ok ") {
                let ref = line.u8Substring(3)
                _ = try GitRefName.parse(ref)
                if !seen.insert(Array(ref.utf8)).inserted {
                    throw GitException(.protocolUnsupported, "receive-pack status repeats a ref")
                }
                refs.append(GitPushRefStatus(ref, nil))
                continue
            }
            if !line.u8HasPrefix("ng ") {
                throw GitException(.protocolUnsupported, "receive-pack ref status is invalid")
            }
            let separator = line.u8IndexOf(" ", from: 3)
            if separator == -1 || separator == line.utf8.count - 1 {
                throw GitException(.protocolUnsupported, "receive-pack rejected ref status is invalid")
            }
            let ref = line.u8Substring(3, separator)
            let message = redactRemoteText(Array(line.u8Substring(separator + 1).utf8))
            _ = try GitRefName.parse(ref)
            if !seen.insert(Array(ref.utf8)).inserted {
                throw GitException(.protocolUnsupported, "receive-pack status repeats a ref")
            }
            let lowered = message.lowercased()
            refs.append(GitPushRefStatus(
                ref,
                lowered.u8Contains("non-fast-forward") || lowered.u8Contains("fetch first")
                    ? .nonFastForward : .remoteRejected,
                message: message
            ))
        }
        return GitReceivePackStatus(unpackOk, refs, unpackMessage: unpackMessage)
    }
}

private func advertisementAscii(_ bytes: [UInt8]) throws -> [UInt8] {
    for byte in bytes where byte > 0x7f {
        throw GitException(.protocolUnsupported, "receive-pack advertisement is not ASCII")
    }
    return bytes
}

private func statusLine(_ bytes: [UInt8]) throws -> String {
    for byte in bytes where byte > 0x7f {
        throw GitException(.protocolUnsupported, "receive-pack status is not ASCII")
    }
    if bytes.last != 0x0a || bytes.contains(0x0d) || bytes.count == 1 {
        throw GitException(.protocolUnsupported, "receive-pack status is malformed")
    }
    return String(decoding: bytes.dropLast(), as: UTF8.self)
}
