public final class CheckedBytesReader {
    private let bytes: [UInt8]
    public private(set) var offset = 0

    public init(_ bytes: [UInt8]) { self.bytes = bytes }

    public var remaining: Int { bytes.count - offset }
    public var isAtEnd: Bool { remaining == 0 }

    public func readByte() throws -> UInt8 {
        if remaining == 0 {
            throw GitException(.invalidObject, "unexpected end of input")
        }
        defer { offset += 1 }
        return bytes[offset]
    }

    public func readBytes(_ length: Int) throws -> [UInt8] {
        if length < 0 || length > remaining {
            throw GitException(.resourceLimitExceeded, "declared byte range exceeds remaining input")
        }
        let result = Array(bytes[offset..<(offset + length)])
        offset += length
        return result
    }

    public func readUint32() throws -> UInt32 {
        let b = try readBytes(4)
        return (UInt32(b[0]) << 24) | (UInt32(b[1]) << 16) | (UInt32(b[2]) << 8) | UInt32(b[3])
    }
}

public final class CheckedBytesWriter {
    private var buffer: [UInt8] = []

    public init() {}

    public var length: Int { buffer.count }

    public func writeByte(_ value: Int) throws {
        if value < 0 || value > 0xff {
            throw GitArgumentError("value", "must be a byte")
        }
        buffer.append(UInt8(value))
    }

    public func writeBytes(_ values: [UInt8]) { buffer.append(contentsOf: values) }

    public func writeUint32(_ value: Int) throws {
        if value < 0 || value > 0xffff_ffff {
            throw GitArgumentError("value", "must be an unsigned 32-bit integer")
        }
        buffer.append(contentsOf: uint32Bytes(UInt32(value)))
    }

    public func takeBytes() -> [UInt8] {
        defer { buffer = [] }
        return buffer
    }
}

func uint32Bytes(_ value: UInt32) -> [UInt8] {
    [UInt8(truncatingIfNeeded: value >> 24), UInt8(truncatingIfNeeded: value >> 16),
     UInt8(truncatingIfNeeded: value >> 8), UInt8(truncatingIfNeeded: value)]
}

func uint16Bytes(_ value: Int) -> [UInt8] {
    [UInt8(truncatingIfNeeded: value >> 8), UInt8(truncatingIfNeeded: value)]
}

extension Array where Element == UInt8 {
    func readUint32(_ offset: Int) -> Int {
        (Int(self[offset]) << 24) | (Int(self[offset + 1]) << 16) | (Int(self[offset + 2]) << 8) | Int(self[offset + 3])
    }

    func readUint16(_ offset: Int) -> Int {
        (Int(self[offset]) << 8) | Int(self[offset + 1])
    }

    /// Index of the first `value` at or after `start`; -1 when absent.
    func indexOfByte(_ value: UInt8, _ start: Int = 0) -> Int {
        var index = Swift.max(start, 0)
        while index < count {
            if self[index] == value { return index }
            index += 1
        }
        return -1
    }
}

/// Content comparison without an early exit, used wherever the engine
/// compares digests and object IDs.
func constantTimeEquals(_ left: [UInt8], _ right: [UInt8]) -> Bool {
    if left.count != right.count { return false }
    var different: UInt8 = 0
    for index in 0..<left.count { different |= left[index] ^ right[index] }
    return different == 0
}
