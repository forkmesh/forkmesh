public enum GitPktLineKind: Sendable { case data, flush, delimiter, responseEnd }

public struct GitPktLine: Sendable {
    public let kind: GitPktLineKind
    public let payload: [UInt8]

    private init(_ kind: GitPktLineKind, _ payload: [UInt8]) {
        self.kind = kind
        self.payload = payload
    }

    public static func data(_ payload: [UInt8]) -> GitPktLine { GitPktLine(.data, payload) }
    public static func flush() -> GitPktLine { GitPktLine(.flush, []) }
    public static func delimiter() -> GitPktLine { GitPktLine(.delimiter, []) }
    public static func responseEnd() -> GitPktLine { GitPktLine(.responseEnd, []) }

    /// A data packet holding the ASCII `text`.
    static func text(_ text: String) -> GitPktLine { GitPktLine(.data, Array(text.utf8)) }

    public func encode() throws -> [UInt8] {
        let length: Int
        switch kind {
        case .flush: length = 0
        case .delimiter: length = 1
        case .responseEnd: length = 2
        case .data: length = payload.count + 4
        }
        if length > 0xfff0 {
            throw GitException(.resourceLimitExceeded, "pkt-line exceeds hard length limit")
        }
        let digits = Array("0123456789abcdef".utf8)
        var output: [UInt8] = [
            digits[(length >> 12) & 0xf], digits[(length >> 8) & 0xf], digits[(length >> 4) & 0xf], digits[length & 0xf],
        ]
        output.append(contentsOf: payload)
        return output
    }

    /// Every packet of `packets`, encoded and concatenated.
    public static func encodeAll(_ packets: [GitPktLine]) throws -> [UInt8] {
        var output = [UInt8]()
        for packet in packets { output.append(contentsOf: try packet.encode()) }
        return output
    }

    public static func decodeAll(_ bytes: [UInt8], maximumPackets: Int = 50000) throws -> [GitPktLine] {
        if maximumPackets <= 0 || maximumPackets > 200_000 {
            throw GitArgumentError("maximumPackets")
        }
        var packets = [GitPktLine]()
        var offset = 0
        while offset < bytes.count {
            if bytes.count - offset < 4 || packets.count >= maximumPackets {
                throw GitException(.resourceLimitExceeded, "pkt-line stream is truncated or exceeds packet limit")
            }
            let length = try parseLength(bytes, offset)
            offset += 4
            switch length {
            case 0: packets.append(.flush())
            case 1: packets.append(.delimiter())
            case 2: packets.append(.responseEnd())
            case 3:
                throw GitException(.protocolUnsupported, "pkt-line length 3 is invalid")
            default:
                if length > 0xfff0 || length - 4 > bytes.count - offset {
                    throw GitException(.protocolUnsupported, "pkt-line payload is truncated or oversized")
                }
                packets.append(.data(Array(bytes[offset..<(offset + length - 4)])))
                offset += length - 4
            }
        }
        return packets
    }
}

private func parseLength(_ bytes: [UInt8], _ offset: Int) throws -> Int {
    var length = 0
    for index in 0..<4 {
        let byte = bytes[offset + index]
        let value: Int
        switch byte {
        case 0x30...0x39: value = Int(byte) - 0x30
        case 0x61...0x66: value = Int(byte) - 0x61 + 10
        case 0x41...0x46: value = Int(byte) - 0x41 + 10
        default:
            throw GitException(.protocolUnsupported, "pkt-line prefix is not hexadecimal")
        }
        length = length * 16 + value
    }
    return length
}
