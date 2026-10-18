import Foundation

public struct SmbWriter: Sendable {
    public private(set) var bytes: [UInt8] = []

    public init() {}

    public var count: Int { bytes.count }

    public mutating func u8(_ value: UInt8) { bytes.append(value) }

    public mutating func u16(_ value: UInt16) {
        bytes.append(UInt8(value & 0xff))
        bytes.append(UInt8(value >> 8))
    }

    public mutating func u32(_ value: UInt32) {
        for shift in stride(from: 0, to: 32, by: 8) { bytes.append(UInt8((value >> UInt32(shift)) & 0xff)) }
    }

    public mutating func u64(_ value: UInt64) {
        for shift in stride(from: 0, to: 64, by: 8) { bytes.append(UInt8((value >> UInt64(shift)) & 0xff)) }
    }

    public mutating func append(_ data: [UInt8]) { bytes.append(contentsOf: data) }

    public mutating func zeros(_ count: Int) { bytes.append(contentsOf: [UInt8](repeating: 0, count: count)) }

    public mutating func align(_ boundary: Int) {
        while bytes.count % boundary != 0 { bytes.append(0) }
    }

    public mutating func put16(_ value: UInt16, at offset: Int) {
        bytes[offset] = UInt8(value & 0xff)
        bytes[offset + 1] = UInt8(value >> 8)
    }

    public mutating func put32(_ value: UInt32, at offset: Int) {
        for index in 0..<4 { bytes[offset + index] = UInt8((value >> UInt32(index * 8)) & 0xff) }
    }
}

public struct SmbReader: Sendable {
    public let bytes: [UInt8]

    public init(_ bytes: [UInt8]) { self.bytes = bytes }

    public func has(_ offset: Int, _ count: Int) -> Bool { offset >= 0 && count >= 0 && offset + count <= bytes.count }

    public func u8(_ offset: Int) -> UInt8 { has(offset, 1) ? bytes[offset] : 0 }

    public func u16(_ offset: Int) -> UInt16 {
        guard has(offset, 2) else { return 0 }
        return UInt16(bytes[offset]) | UInt16(bytes[offset + 1]) << 8
    }

    public func u32(_ offset: Int) -> UInt32 {
        guard has(offset, 4) else { return 0 }
        var value: UInt32 = 0
        for index in 0..<4 { value |= UInt32(bytes[offset + index]) << UInt32(index * 8) }
        return value
    }

    public func u64(_ offset: Int) -> UInt64 {
        guard has(offset, 8) else { return 0 }
        var value: UInt64 = 0
        for index in 0..<8 { value |= UInt64(bytes[offset + index]) << UInt64(index * 8) }
        return value
    }

    public func slice(_ offset: Int, _ count: Int) -> [UInt8] {
        guard has(offset, count) else { return [] }
        return Array(bytes[offset..<(offset + count)])
    }
}

public enum SmbText {
    public static func utf16(_ text: String) -> [UInt8] {
        var out: [UInt8] = []
        out.reserveCapacity(text.utf16.count * 2)
        for unit in text.utf16 {
            out.append(UInt8(unit & 0xff))
            out.append(UInt8(unit >> 8))
        }
        return out
    }

    public static func string(_ bytes: [UInt8]) -> String {
        var units: [UInt16] = []
        units.reserveCapacity(bytes.count / 2)
        var index = 0
        while index + 1 < bytes.count {
            units.append(UInt16(bytes[index]) | UInt16(bytes[index + 1]) << 8)
            index += 2
        }
        while units.last == 0 { units.removeLast() }
        return String(decoding: units, as: UTF16.self)
    }

    public static func hex(_ bytes: [UInt8]) -> String {
        bytes.map { String(format: "%02x", $0) }.joined()
    }

    public static func bytes(hex: String) -> [UInt8] {
        let clean = hex.filter { $0.isHexDigit }
        var out: [UInt8] = []
        var index = clean.startIndex
        while index < clean.endIndex {
            let next = clean.index(index, offsetBy: 2, limitedBy: clean.endIndex) ?? clean.endIndex
            out.append(UInt8(clean[index..<next], radix: 16) ?? 0)
            index = next
        }
        return out
    }
}
