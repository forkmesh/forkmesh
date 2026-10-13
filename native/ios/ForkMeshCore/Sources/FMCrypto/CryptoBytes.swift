// Port of the byte helpers in native/android/.../crypto/Encoding.kt (`Bytes`): the random bytes,
// constant-time comparison and wiping that owner crypto, room seal, room crypto, identity and
// portable identity use.

/// Random bytes, concatenation, wiping and constant-time comparison over `[UInt8]`, the byte type
/// of every primitive in this module.
public enum CryptoBytes {
    /// `count` bytes from the platform CSPRNG (`SystemRandomNumberGenerator`: `arc4random_buf` on
    /// Apple platforms, `getrandom` on Linux).
    public static func random(_ count: Int) -> [UInt8] {
        var generator = SystemRandomNumberGenerator()
        var out = [UInt8](repeating: 0, count: count)
        var i = 0
        while i < count {
            var word = generator.next()
            var n = 0
            while n < 8 && i < count {
                out[i] = UInt8(truncatingIfNeeded: word)
                word >>= 8
                i += 1
                n += 1
            }
        }
        return out
    }

    public static func concat(_ parts: [UInt8]...) -> [UInt8] {
        concat(parts: parts)
    }

    public static func concat(parts: [[UInt8]]) -> [UInt8] {
        var out = [UInt8]()
        out.reserveCapacity(parts.reduce(0) { $0 + $1.count })
        for part in parts { out.append(contentsOf: part) }
        return out
    }

    /// Equal length and content, in time independent of where the arrays differ. For tags and MACs.
    public static func constantTimeEquals(_ a: [UInt8], _ b: [UInt8]) -> Bool {
        if a.count != b.count { return false }
        var diff: UInt8 = 0
        for i in 0..<a.count { diff |= a[i] ^ b[i] }
        return diff == 0
    }

    /// Whether every byte is zero, in constant time.
    public static func isAllZero(_ bytes: [UInt8]) -> Bool {
        var acc: UInt8 = 0
        for b in bytes { acc |= b }
        return acc == 0
    }

    /// Overwrites the array's storage with zeros. Arrays are values: this clears the copy the
    /// caller holds, which is the one that would otherwise outlive the operation.
    public static func wipe(_ bytes: inout [UInt8]) {
        bytes.withUnsafeMutableBufferPointer { buffer in
            for i in 0..<buffer.count { buffer[i] = 0 }
        }
    }

    private static let hexDigits: [UInt8] = Array("0123456789abcdef".utf8)

    /// Lowercase hex.
    public static func hex(_ bytes: [UInt8]) -> String {
        var out = [UInt8]()
        out.reserveCapacity(bytes.count * 2)
        for b in bytes {
            out.append(hexDigits[Int(b >> 4)])
            out.append(hexDigits[Int(b & 15)])
        }
        return String(decoding: out, as: UTF8.self)
    }

    /// Bytes of an even-length hex string (either case), or nil on a non-hex digit or odd length.
    public static func fromHex(_ text: String) -> [UInt8]? {
        let digits = Array(text.utf8)
        if digits.count % 2 != 0 { return nil }
        var out = [UInt8]()
        out.reserveCapacity(digits.count / 2)
        var i = 0
        while i < digits.count {
            guard let hi = nibble(digits[i]), let lo = nibble(digits[i + 1]) else { return nil }
            out.append(hi << 4 | lo)
            i += 2
        }
        return out
    }

    private static func nibble(_ c: UInt8) -> UInt8? {
        switch c {
        case 0x30...0x39: return c - 0x30
        case 0x61...0x66: return c - 0x61 + 10
        case 0x41...0x46: return c - 0x41 + 10
        default: return nil
        }
    }
}
