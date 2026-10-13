/// Parsed protocol v2 `fetch` response.
///
/// A response either carries only an `acknowledgments` section (negotiation
/// continues) or ends with a `packfile` section.
public struct GitFetchResponse: Sendable {
    /// Object IDs the server acknowledged as common.
    public let acknowledged: [GitObjectId]

    /// True when the server answered `NAK` to every `have`.
    public let nak: Bool

    /// True when the server found a cut point and sent (or will send) a pack.
    public let ready: Bool

    /// Pack bytes from the `packfile` section, or nil when negotiation must continue.
    public let packBytes: [UInt8]?

    /// Bounded channel 2 progress bytes.
    public let progress: [UInt8]

    public var shallow: [GitObjectId] = []
    public var unshallow: [GitObjectId] = []

    public var hasPack: Bool { packBytes != nil }

    public static func parse(
        _ packets: [GitPktLine],
        maxProgressBytes: Int = GitResourceLimits.defaultMaxSidebandBytes
    ) throws -> GitFetchResponse {
        if packets.count < 2 || packets[packets.count - 1].kind != .flush {
            throw GitException(.protocolUnsupported, "fetch response framing is invalid")
        }
        var index = 0
        var acknowledged = [GitObjectId]()
        var nak = false
        var ready = false
        var packBytes: [UInt8]?
        var progress = [UInt8]()
        var shallow = [GitObjectId]()
        var unshallow = [GitObjectId]()
        while index < packets.count {
            let header = packets[index]
            if header.kind != .data {
                throw GitException(.protocolUnsupported, "fetch response section header is invalid")
            }
            let section = try line(
                header.payload,
                notAscii: "fetch response section header is not ASCII",
                malformed: "fetch response section header is malformed"
            )
            index += 1
            let end = try sectionEnd(packets, index)
            let body = packets[index..<end]
            if section == Array("acknowledgments".utf8) {
                for packet in body {
                    let text = try line(
                        packet.payload,
                        notAscii: "fetch acknowledgment is not ASCII",
                        malformed: "fetch acknowledgment is malformed"
                    )
                    if text == Array("NAK".utf8) {
                        nak = true
                    } else if text == Array("ready".utf8) {
                        ready = true
                    } else if text.starts(with: Array("ACK ".utf8)) {
                        acknowledged.append(
                            try GitObjectId.parseSha1(String(decoding: text[4...], as: UTF8.self))
                        )
                    } else {
                        throw GitException(.protocolUnsupported, "fetch acknowledgment is invalid")
                    }
                }
                if nak && !acknowledged.isEmpty {
                    throw GitException(.protocolUnsupported, "fetch response mixes ACK and NAK")
                }
            } else if section == Array("shallow-info".utf8) {
                for packet in body {
                    let text = try line(
                        packet.payload.last == 0x0a ? packet.payload : packet.payload + [0x0a],
                        notAscii: "fetch shallow-info is not ASCII",
                        malformed: "fetch shallow-info is malformed"
                    )
                    if text.starts(with: Array("shallow ".utf8)) {
                        shallow.append(try GitObjectId.parseSha1(String(decoding: text[8...], as: UTF8.self)))
                    } else if text.starts(with: Array("unshallow ".utf8)) {
                        unshallow.append(try GitObjectId.parseSha1(String(decoding: text[10...], as: UTF8.self)))
                    } else {
                        throw GitException(.protocolUnsupported, "fetch shallow-info line is invalid")
                    }
                }
            } else if section == Array("packfile".utf8) {
                if packBytes != nil {
                    throw GitException(.protocolUnsupported, "fetch response has two packfile sections")
                }
                let demuxed = try GitSideband.demux(body, maxProgressBytes: maxProgressBytes)
                packBytes = demuxed.data
                progress = demuxed.progress
            } else {
                throw GitException(.protocolUnsupported, "fetch response section is unsupported")
            }
            let terminator = packets[end].kind
            index = end + 1
            if terminator == .flush {
                if index != packets.count {
                    throw GitException(.protocolUnsupported, "fetch response has data after flush")
                }
                break
            }
            if section == Array("packfile".utf8) {
                throw GitException(.protocolUnsupported, "packfile section must be flush terminated")
            }
        }
        if packBytes == nil && acknowledged.isEmpty && !nak && !ready {
            throw GitException(.protocolUnsupported, "fetch response carries neither acknowledgments nor a pack")
        }
        if packBytes != nil && !ready && acknowledged.isEmpty && !nak { ready = true }
        var response = GitFetchResponse(
            acknowledged: acknowledged, nak: nak, ready: ready, packBytes: packBytes, progress: progress
        )
        response.shallow = shallow
        response.unshallow = unshallow
        return response
    }

    public static func validateUntrustedPack(
        _ bytes: [UInt8],
        _ limits: GitResourceLimits,
        detector: GitSha1CollisionDetector? = nil
    ) throws {
        _ = try GitPackReader.parse(bytes, limits, origin: .untrustedTransport, detector: detector)
    }

    private static func sectionEnd(_ packets: [GitPktLine], _ start: Int) throws -> Int {
        var index = start
        while index < packets.count {
            let kind = packets[index].kind
            if kind == .flush || kind == .delimiter { return index }
            if kind != .data {
                throw GitException(.protocolUnsupported, "fetch response has unexpected control packet")
            }
            index += 1
        }
        throw GitException(.protocolUnsupported, "fetch response section is not terminated")
    }

    /// An ASCII payload without its mandatory trailing newline.
    private static func line(_ payload: [UInt8], notAscii: String, malformed: String) throws -> [UInt8] {
        for byte in payload where byte > 0x7f { throw GitException(.protocolUnsupported, notAscii) }
        if payload.last != 0x0a || payload.count < 2 { throw GitException(.protocolUnsupported, malformed) }
        return Array(payload.dropLast())
    }
}
