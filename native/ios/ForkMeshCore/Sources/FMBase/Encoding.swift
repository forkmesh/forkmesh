// Base64 and UTF-8 codecs with strict acceptance rules, plus hex, byte
// helpers and a Unicode-aware trim. After the non-crypto half of
// native/android core/crypto/Encoding.kt.

import Foundation

public struct EncodingError: Error, Equatable, CustomStringConvertible, Sendable {
    public let message: String
    public init(_ message: String) { self.message = message }
    public var description: String { message }
}

/// Base64 in the standard and URL-safe alphabets.
///
/// One decoder serves both: both alphabets are accepted, padding may be `=` or `%3D`, unpadded input
/// must be a multiple of four characters, and the bits before padding must be
/// zero. Foundation's decoder is more forgiving in some places and stricter
/// in others, so it is not used.
public enum B64 {
    private static let standard = Array("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/".utf8)
    private static let urlSafe = Array("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_".utf8)
    private static let padding = -2
    private static let invalid = -1

    private static let inverse: [Int] = {
        var table = [Int](repeating: invalid, count: 128)
        for (index, char) in standard.enumerated() { table[Int(char)] = index }
        table[Int(UInt8(ascii: "-"))] = 62
        table[Int(UInt8(ascii: "_"))] = 63
        table[Int(UInt8(ascii: "="))] = padding
        table[Int(UInt8(ascii: "%"))] = padding
        return table
    }()

    /// Standard alphabet with `=` padding.
    public static func encode<D: Sequence>(_ bytes: D) -> String where D.Element == UInt8 {
        encode(Array(bytes), alphabet: standard, pad: true)
    }

    /// URL-safe alphabet with `=` padding.
    public static func encodeUrl<D: Sequence>(_ bytes: D) -> String where D.Element == UInt8 {
        encode(Array(bytes), alphabet: urlSafe, pad: true)
    }

    /// URL-safe alphabet without padding: the form every signed field uses.
    public static func encodeUrlNoPadding<D: Sequence>(_ bytes: D) -> String where D.Element == UInt8 {
        encode(Array(bytes), alphabet: urlSafe, pad: false)
    }

    /// Either alphabet; throws an `EncodingError` on anything the rules above refuse.
    public static func decode(_ text: String) throws -> Data {
        let input = Array(text.utf8)
        let end = input.count
        var out = [UInt8]()
        out.reserveCapacity(end / 4 * 3 + 3)
        var bits = 0
        var count = 0
        var i = 0
        while i < end {
            let char = Int(input[i])
            let code = char < 128 ? inverse[char] : invalid
            if code >= 0 {
                bits = ((bits << 6) | code) & 0xFFFFFF
                count = (count + 1) & 3
                if count == 0 {
                    out.append(UInt8(bits >> 16))
                    out.append(UInt8((bits >> 8) & 0xFF))
                    out.append(UInt8(bits & 0xFF))
                    bits = 0
                }
                i += 1
                continue
            }
            if code == padding, count > 1 {
                if count == 3 {
                    if bits & 0x03 != 0 { throw EncodingError("Invalid encoding before padding") }
                    out.append(UInt8((bits >> 10) & 0xFF))
                    out.append(UInt8((bits >> 2) & 0xFF))
                } else {
                    if bits & 0x0F != 0 { throw EncodingError("Invalid encoding before padding") }
                    out.append(UInt8((bits >> 4) & 0xFF))
                }
                var expected = (3 - count) * 3
                if char == Int(UInt8(ascii: "%")) { expected += 2 }
                if try checkPadding(input, from: i + 1, expected: expected) != 0 {
                    throw EncodingError("Missing padding character")
                }
                return Data(out)
            }
            throw EncodingError("Invalid character")
        }
        if count != 0 { throw EncodingError("Invalid length, must be multiple of four") }
        return Data(out)
    }

    /// `decode`, or nil where it would throw.
    public static func decodeOrNil(_ text: String) -> Data? { try? decode(text) }

    /// The unpadded form the signed fields carry: the missing `=` is restored
    /// before decoding.
    public static func decodeUnpadded(_ text: String) throws -> Data {
        let remainder = text.utf8.count % 4
        if remainder == 0 || text.utf8.contains(UInt8(ascii: "=")) || text.utf8.contains(UInt8(ascii: "%")) {
            return try decode(text)
        }
        if remainder == 1 { throw EncodingError("Invalid length") }
        return try decode(text + String(repeating: "=", count: 4 - remainder))
    }

    /// Returns the padding still expected.
    private static func checkPadding(_ input: [UInt8], from: Int, expected expectedPadding: Int) throws -> Int {
        let end = input.count
        var start = from
        var expected = expectedPadding
        if start == end { return expected }
        while expected > 0 {
            var char = input[start]
            if expected == 3 {
                if char == UInt8(ascii: "=") {
                    expected -= 3
                    start += 1
                    break
                }
                if char == UInt8(ascii: "%") {
                    expected -= 1
                    start += 1
                    if start == end { break }
                    char = input[start]
                } else {
                    break
                }
            }
            var partial = expected
            if partial > 3 { partial -= 3 }
            if partial == 2 {
                if char != UInt8(ascii: "3") { break }
                start += 1
                expected -= 1
                if start == end { break }
                char = input[start]
            }
            if char | 0x20 != UInt8(ascii: "d") { break }
            start += 1
            expected -= 1
            if start == end { break }
        }
        if start != end { throw EncodingError("Invalid padding character") }
        return expected
    }

    private static func encode(_ bytes: [UInt8], alphabet: [UInt8], pad: Bool) -> String {
        var out = [UInt8]()
        out.reserveCapacity((bytes.count + 2) / 3 * 4)
        var i = 0
        while i + 3 <= bytes.count {
            let n = Int(bytes[i]) << 16 | Int(bytes[i + 1]) << 8 | Int(bytes[i + 2])
            out.append(alphabet[n >> 18])
            out.append(alphabet[(n >> 12) & 63])
            out.append(alphabet[(n >> 6) & 63])
            out.append(alphabet[n & 63])
            i += 3
        }
        switch bytes.count - i {
        case 1:
            let n = Int(bytes[i]) << 16
            out.append(alphabet[n >> 18])
            out.append(alphabet[(n >> 12) & 63])
            if pad { out.append(contentsOf: "==".utf8) }
        case 2:
            let n = Int(bytes[i]) << 16 | Int(bytes[i + 1]) << 8
            out.append(alphabet[n >> 18])
            out.append(alphabet[(n >> 12) & 63])
            out.append(alphabet[(n >> 6) & 63])
            if pad { out.append(UInt8(ascii: "=")) }
        default:
            break
        }
        return String(decoding: out, as: UTF8.self)
    }
}

/// Lower-case hex.
public enum Hex {
    private static let digits = Array("0123456789abcdef".utf8)

    public static func encode<D: Sequence>(_ bytes: D) -> String where D.Element == UInt8 {
        var out = [UInt8]()
        for byte in bytes {
            out.append(digits[Int(byte >> 4)])
            out.append(digits[Int(byte & 15)])
        }
        return String(decoding: out, as: UTF8.self)
    }

    /// Bytes of an even-length hex string, either case; throws on anything else.
    public static func decode(_ text: String) throws -> Data {
        let input = Array(text.utf8)
        if input.count % 2 != 0 { throw EncodingError("Odd-length hex string") }
        var out = [UInt8]()
        out.reserveCapacity(input.count / 2)
        var i = 0
        while i < input.count {
            guard let high = nibble(input[i]), let low = nibble(input[i + 1]) else {
                throw EncodingError("Invalid hex digit")
            }
            out.append(high << 4 | low)
            i += 2
        }
        return Data(out)
    }

    public static func decodeOrNil(_ text: String) -> Data? { try? decode(text) }

    private static func nibble(_ c: UInt8) -> UInt8? {
        switch c {
        case UInt8(ascii: "0")...UInt8(ascii: "9"): return c - UInt8(ascii: "0")
        case UInt8(ascii: "a")...UInt8(ascii: "f"): return c - UInt8(ascii: "a") + 10
        case UInt8(ascii: "A")...UInt8(ascii: "F"): return c - UInt8(ascii: "A") + 10
        default: return nil
        }
    }
}

/// UTF-8 encoding and decoding.
public enum Utf8 {
    public static func encode(_ text: String) -> Data { Data(text.utf8) }

    /// Malformed input throws. A leading byte order mark is dropped.
    public static func decode<D: Collection>(_ bytes: D) throws -> String where D.Element == UInt8 {
        var input = Array(bytes)
        if input.starts(with: [0xEF, 0xBB, 0xBF]) { input.removeFirst(3) }
        // String(validatingUTF8:) wants a C string; validating through the
        // decoder keeps embedded NULs.
        var decoder = UTF8()
        var iterator = input.makeIterator()
        var scalars = String.UnicodeScalarView()
        while true {
            switch decoder.decode(&iterator) {
            case .scalarValue(let scalar): scalars.append(scalar)
            case .emptyInput: return String(scalars)
            case .error: throw EncodingError("Invalid UTF-8")
            }
        }
    }

    /// Malformed input becomes U+FFFD. A leading byte order mark is dropped.
    public static func decodeLossy<D: Collection>(_ bytes: D) -> String where D.Element == UInt8 {
        var input = Array(bytes)
        if input.starts(with: [0xEF, 0xBB, 0xBF]) { input.removeFirst(3) }
        return String(decoding: input, as: UTF8.self)
    }
}

/// Random bytes, concatenation and constant-time comparison.
public enum Bytes {
    /// `count` bytes from the system CSPRNG.
    public static func random(_ count: Int) -> Data {
        var generator = SystemRandomNumberGenerator()
        var out = [UInt8](repeating: 0, count: count)
        for i in out.indices { out[i] = UInt8.random(in: .min ... .max, using: &generator) }
        return Data(out)
    }

    public static func concat(_ parts: Data...) -> Data {
        var out = Data()
        for part in parts { out.append(part) }
        return out
    }

    /// Equal length and content, in time independent of where the inputs differ. For tags and MACs.
    public static func constantTimeEquals<A: Collection, B: Collection>(_ a: A, _ b: B) -> Bool
    where A.Element == UInt8, B.Element == UInt8 {
        if a.count != b.count { return false }
        var diff: UInt8 = 0
        for (x, y) in zip(a, b) { diff |= x ^ y }
        return diff == 0
    }

    /// Whether every byte is zero, in constant time.
    public static func isAllZero<A: Collection>(_ bytes: A) -> Bool where A.Element == UInt8 {
        var acc: UInt8 = 0
        for byte in bytes { acc |= byte }
        return acc == 0
    }
}

extension String {
    /// Strips Unicode White_Space plus U+FEFF from
    /// both ends. Foundation's whitespace set differs (it keeps U+FEFF and
    /// strips U+200B), which would change room names and therefore salts and
    /// storage keys.
    public var dartTrimmed: String {
        let scalars = unicodeScalars
        var start = scalars.startIndex
        var end = scalars.endIndex
        while start < end, String.isDartWhitespace(scalars[start].value) { start = scalars.index(after: start) }
        while end > start {
            let before = scalars.index(before: end)
            guard String.isDartWhitespace(scalars[before].value) else { break }
            end = before
        }
        if start == scalars.startIndex && end == scalars.endIndex { return self }
        return String(scalars[start..<end])
    }

    private static func isDartWhitespace(_ c: UInt32) -> Bool {
        (0x09...0x0D).contains(c) || c == 0x20 || c == 0x85 || c == 0xA0 || c == 0x1680
            || (0x2000...0x200A).contains(c) || c == 0x2028 || c == 0x2029 || c == 0x202F
            || c == 0x205F || c == 0x3000 || c == 0xFEFF
    }
}

extension JSONValue {
    /// Numeric equality between a decoded JSON value and an integer: `1 == 1.0`
    /// holds, so an envelope whose version arrives as a double still matches.
    public func numEquals(_ expected: Int64) -> Bool {
        switch self {
        case .int(let value): return value == expected
        case .double(let value): return value == Double(expected)
        default: return false
        }
    }
}
