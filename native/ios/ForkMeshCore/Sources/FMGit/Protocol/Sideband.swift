/// Demultiplexed `side-band-64k` payload.
public struct GitSidebandResult: Sendable {
    /// Concatenated channel 1 bytes.
    public let data: [UInt8]

    /// Channel 2 bytes retained up to the configured sideband limit.
    public let progress: [UInt8]
}

public enum GitSideband {
    /// Splits sideband packets into channel 1 data and bounded channel 2
    /// progress, and converts a channel 3 error into a typed rejection.
    ///
    /// `packets` must not include the terminating flush packet.
    public static func demux<S: Sequence>(_ packets: S, maxProgressBytes: Int) throws -> GitSidebandResult
    where S.Element == GitPktLine {
        var data = [UInt8]()
        var progress = [UInt8]()
        for packet in packets {
            if packet.kind != .data || packet.payload.isEmpty {
                throw GitException(.protocolUnsupported, "sideband packet is invalid")
            }
            switch packet.payload[0] {
            case 1:
                data.append(contentsOf: packet.payload[1...])
            case 2:
                let remaining = maxProgressBytes - progress.count
                if remaining > 0 {
                    progress.append(contentsOf: packet.payload[1...].prefix(remaining))
                }
            case 3:
                throw GitException(
                    .remoteRejected,
                    "remote reported an error: \(redactRemoteText(Array(packet.payload[1...])))"
                )
            default:
                throw GitException(.protocolUnsupported, "sideband channel is invalid")
            }
        }
        return GitSidebandResult(data: data, progress: progress)
    }
}

/// Bounds and sanitizes remote-controlled text for error messages.
///
/// Only printable ASCII is retained, at most `maxLength` characters.
public func redactRemoteText(_ bytes: [UInt8], maxLength: Int = 200) -> String {
    var buffer = [UInt8]()
    for byte in bytes {
        if buffer.count >= maxLength {
            buffer.append(contentsOf: Array("...".utf8))
            break
        }
        if byte >= 0x20 && byte < 0x7f {
            buffer.append(byte)
        } else if byte == 0x0a || byte == 0x09 {
            buffer.append(0x20)
        }
    }
    return String(decoding: buffer, as: UTF8.self).dartTrim()
}
