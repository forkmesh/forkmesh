import CZlib

/// IEEE CRC-32 over `bytes` in the range `[start, end)`, computed by zlib,
/// which is already linked.
public func gitCrc32(_ bytes: [UInt8], _ start: Int = 0, _ end: Int? = nil) -> UInt32 {
    let stop = end ?? bytes.count
    precondition(start >= 0 && start <= stop && stop <= bytes.count)
    return bytes.withUnsafeBufferPointer { buffer in
        var crc = CZlib.crc32(0, nil, 0)
        var offset = start
        while offset < stop {
            let count = min(stop - offset, 1 << 30)
            crc = CZlib.crc32(crc, buffer.baseAddress! + offset, UInt32(count))
            offset += count
        }
        return UInt32(truncatingIfNeeded: crc)
    }
}
