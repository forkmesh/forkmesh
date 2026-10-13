// Byte-for-byte port of the Worker's catalog canonicalization.
//
// `app/src/catalog_routes.py` hashes the normalized record with
// `json.dumps(record, sort_keys=True, separators=(",", ":"))` — CPython's
// default `ensure_ascii=True` — and signs
// `forkmesh-catalog-v2\n{sha256hex(...)}`. A general JSON encoder differs in
// ways that would silently break every signature: key order, and non-ASCII
// characters emitted literally instead of as `\uXXXX`. `json(_:)` reproduces
// CPython exactly instead.
//
// It needs no ordered-JSON encoder (keys are sorted here), which is why it
// can live in FMDiff. The one thing it does need is SHA-256; FMDiff has no
// FMCrypto dependency, so a small private implementation sits at the bottom
// of this file.

import Foundation

/// The value shapes a catalog record can contain.
public indirect enum CatalogValue: Equatable, Sendable {
    case string(String)
    case int(Int)
    case bool(Bool)
    case array([CatalogValue])
    case object([String: CatalogValue])
}

public struct CatalogCanonicalError: Error, Equatable, CustomStringConvertible, Sendable {
    public let message: String

    public init(_ message: String) {
        self.message = message
    }

    public var description: String { message }
}

public enum CatalogCanonical {
    /// Prefix of the v2 catalog publication signature.
    public static let v2Prefix = "forkmesh-catalog-v2"

    /// `json.dumps(value, sort_keys=True, separators=(",", ":"))` in CPython's
    /// default ASCII-safe mode.
    public static func json(_ value: CatalogValue) -> String {
        var out: [UInt8] = []
        write(value, into: &out)
        return String(decoding: out, as: UTF8.self)
    }

    /// The same for loosely typed values: `String`, `Bool`, the integer types,
    /// `[Any]` and `[String: Any]`. Anything else (nil, a double) throws, so a
    /// mistake shows up as a test failure rather than an unverifiable
    /// signature.
    public static func json(_ value: Any?) throws -> String {
        json(try CatalogValue(any: value))
    }

    /// `sha256hex(json(record))` — the digest the v2 signature binds.
    public static func recordHash(_ record: [String: CatalogValue]) -> String {
        Sha256.hex(Array(json(.object(record)).utf8))
    }

    public static func recordHash(_ record: [String: Any]) throws -> String {
        Sha256.hex(Array(try json(record).utf8))
    }

    /// The exact bytes an owner key signs for a `catalogSigVersion: 2` publish.
    public static func v2Canonical(_ recordWithoutSignature: [String: CatalogValue]) -> String {
        "\(v2Prefix)\n\(recordHash(recordWithoutSignature))"
    }

    public static func v2Canonical(_ recordWithoutSignature: [String: Any]) throws -> String {
        "\(v2Prefix)\n\(try recordHash(recordWithoutSignature))"
    }

    private static func write(_ value: CatalogValue, into out: inout [UInt8]) {
        switch value {
        case .string(let text):
            writeString(text, into: &out)
        case .bool(let flag):
            out.append(contentsOf: Array((flag ? "true" : "false").utf8))
        case .int(let number):
            out.append(contentsOf: Array(String(number).utf8))
        case .array(let items):
            out.append(0x5B)
            for (index, item) in items.enumerated() {
                if index > 0 { out.append(0x2C) }
                write(item, into: &out)
            }
            out.append(0x5D)
        case .object(let members):
            // CPython sorts by code point; every catalog key is ASCII, where
            // UTF-16 code unit order agrees.
            let keys = members.keys.sorted { $0.utf16.lexicographicallyPrecedes($1.utf16) }
            out.append(0x7B)
            for (index, key) in keys.enumerated() {
                if index > 0 { out.append(0x2C) }
                writeString(key, into: &out)
                out.append(0x3A)
                if let member = members[key] { write(member, into: &out) }
            }
            out.append(0x7D)
        }
    }

    private static let hexDigits = Array("0123456789abcdef".utf8)

    /// CPython's `py_encode_basestring_ascii`: escape `\` and `"`, use the
    /// short forms for `\b\f\n\r\t`, and `\uXXXX` for everything outside
    /// `0x20..0x7e` (astral characters become their UTF-16 surrogate pair).
    private static func writeString(_ value: String, into out: inout [UInt8]) {
        out.append(0x22)
        for unit in value.utf16 {
            switch unit {
            case 0x22: out.append(contentsOf: [0x5C, 0x22])
            case 0x5C: out.append(contentsOf: [0x5C, 0x5C])
            case 0x08: out.append(contentsOf: [0x5C, 0x62])
            case 0x0C: out.append(contentsOf: [0x5C, 0x66])
            case 0x0A: out.append(contentsOf: [0x5C, 0x6E])
            case 0x0D: out.append(contentsOf: [0x5C, 0x72])
            case 0x09: out.append(contentsOf: [0x5C, 0x74])
            default:
                if unit < 0x20 || unit > 0x7E {
                    out.append(contentsOf: [0x5C, 0x75])
                    for shift in [12, 8, 4, 0] {
                        out.append(hexDigits[Int((unit >> UInt16(shift)) & 0xF)])
                    }
                } else {
                    out.append(UInt8(unit))
                }
            }
        }
        out.append(0x22)
    }

    /// A value that survives the Worker's `clean_string(value, max_length)`
    /// unchanged: `value.strip()[:max_length]` must be the identity.
    ///
    /// Control characters and exotic Unicode whitespace are removed outright
    /// (the runtimes disagree at the edges of what counts as whitespace), then
    /// the text is trimmed and truncated by **code point** — Python slices
    /// code points.
    public static func cleanString(_ value: String, maxLength: Int) -> String {
        var kept: [Unicode.Scalar] = []
        for scalar in value.unicodeScalars {
            if scalar.value < 0x20 || scalar.value == 0x7F { continue }
            kept.append(isExoticSpace(scalar.value) ? " " : scalar)
        }
        var text = trimSpaces(kept[...])
        if text.count > maxLength {
            text = trimSpaces(text.prefix(max(0, maxLength)))
        }
        var result = String.UnicodeScalarView()
        result.append(contentsOf: text)
        return String(result)
    }

    /// After the fold above a plain space is the only whitespace left.
    private static func trimSpaces(_ scalars: ArraySlice<Unicode.Scalar>) -> ArraySlice<Unicode.Scalar> {
        var lower = scalars.startIndex
        var upper = scalars.endIndex
        while lower < upper, scalars[lower] == " " { lower += 1 }
        while upper > lower, scalars[upper - 1] == " " { upper -= 1 }
        return scalars[lower..<upper]
    }

    /// Whitespace beyond a plain space that `str.strip()` and the client
    /// runtimes classify differently; folded to a space so all agree.
    private static func isExoticSpace(_ scalar: UInt32) -> Bool {
        switch scalar {
        case 0x85, 0xA0, 0x1680, 0x2000...0x200A, 0x2028, 0x2029, 0x202F, 0x205F, 0x3000, 0xFEFF:
            return true
        default:
            return false
        }
    }
}

extension CatalogValue {
    /// Converts a loosely typed value; throws on any shape the Worker never
    /// stores.
    public init(any value: Any?) throws {
        guard let value else { throw CatalogCanonicalError("not canonicalizable JSON: nil") }
        switch value {
        case let known as CatalogValue:
            self = known
        case let text as String:
            self = .string(text)
        case let items as [Any]:
            self = .array(try items.map { try CatalogValue(any: $0) })
        case let members as [String: Any]:
            self = .object(try members.mapValues { try CatalogValue(any: $0) })
        default:
            // The static type decides between Bool and the integers, so a
            // bridged number can never turn `true` into `1` or the reverse.
            if type(of: value) == Bool.self, let flag = value as? Bool {
                self = .bool(flag)
            } else if let number = CatalogValue.integer(value) {
                self = .int(number)
            } else if let number = value as? NSNumber, let bridged = CatalogValue.bridged(number) {
                self = bridged
            } else {
                throw CatalogCanonicalError("not canonicalizable JSON: \(type(of: value))")
            }
        }
    }

    private static func integer(_ value: Any) -> Int? {
        switch value {
        case let n as Int where type(of: value) == Int.self: return n
        case let n as Int64 where type(of: value) == Int64.self: return Int(exactly: n)
        case let n as Int32 where type(of: value) == Int32.self: return Int(n)
        case let n as Int16 where type(of: value) == Int16.self: return Int(n)
        case let n as Int8 where type(of: value) == Int8.self: return Int(n)
        case let n as UInt where type(of: value) == UInt.self: return Int(exactly: n)
        case let n as UInt64 where type(of: value) == UInt64.self: return Int(exactly: n)
        case let n as UInt32 where type(of: value) == UInt32.self: return Int(n)
        case let n as UInt16 where type(of: value) == UInt16.self: return Int(n)
        case let n as UInt8 where type(of: value) == UInt8.self: return Int(n)
        default: return nil
        }
    }

    /// An NSNumber (what JSONSerialization hands out): booleans and integers
    /// only, told apart by the encoded type.
    private static func bridged(_ number: NSNumber) -> CatalogValue? {
        switch String(cString: number.objCType) {
        case "c", "B":
            return .bool(number.boolValue)
        case "d", "f":
            return nil
        default:
            return .int(number.intValue)
        }
    }
}

// MARK: - SHA-256

/// FIPS 180-4 SHA-256. Private to this file: FMCrypto owns the app's hashing,
/// and this exists only because FMDiff may not depend on it.
private enum Sha256 {
    private static let k: [UInt32] = [
        0x428a_2f98, 0x7137_4491, 0xb5c0_fbcf, 0xe9b5_dba5, 0x3956_c25b, 0x59f1_11f1, 0x923f_82a4, 0xab1c_5ed5,
        0xd807_aa98, 0x1283_5b01, 0x2431_85be, 0x550c_7dc3, 0x72be_5d74, 0x80de_b1fe, 0x9bdc_06a7, 0xc19b_f174,
        0xe49b_69c1, 0xefbe_4786, 0x0fc1_9dc6, 0x240c_a1cc, 0x2de9_2c6f, 0x4a74_84aa, 0x5cb0_a9dc, 0x76f9_88da,
        0x983e_5152, 0xa831_c66d, 0xb003_27c8, 0xbf59_7fc7, 0xc6e0_0bf3, 0xd5a7_9147, 0x06ca_6351, 0x1429_2967,
        0x27b7_0a85, 0x2e1b_2138, 0x4d2c_6dfc, 0x5338_0d13, 0x650a_7354, 0x766a_0abb, 0x81c2_c92e, 0x9272_2c85,
        0xa2bf_e8a1, 0xa81a_664b, 0xc24b_8b70, 0xc76c_51a3, 0xd192_e819, 0xd699_0624, 0xf40e_3585, 0x106a_a070,
        0x19a4_c116, 0x1e37_6c08, 0x2748_774c, 0x34b0_bcb5, 0x391c_0cb3, 0x4ed8_aa4a, 0x5b9c_ca4f, 0x682e_6ff3,
        0x748f_82ee, 0x78a5_636f, 0x84c8_7814, 0x8cc7_0208, 0x90be_fffa, 0xa450_6ceb, 0xbef9_a3f7, 0xc671_78f2,
    ]

    static func hex(_ message: [UInt8]) -> String {
        var h: [UInt32] = [
            0x6a09_e667, 0xbb67_ae85, 0x3c6e_f372, 0xa54f_f53a, 0x510e_527f, 0x9b05_688c, 0x1f83_d9ab, 0x5be0_cd19,
        ]
        var data = message
        let bitLength = UInt64(message.count) &* 8
        data.append(0x80)
        while data.count % 64 != 56 { data.append(0) }
        for shift in stride(from: 56, through: 0, by: -8) {
            data.append(UInt8(truncatingIfNeeded: bitLength >> UInt64(shift)))
        }

        var w = [UInt32](repeating: 0, count: 64)
        for block in stride(from: 0, to: data.count, by: 64) {
            for t in 0..<16 {
                let i = block + t * 4
                w[t] = UInt32(data[i]) << 24 | UInt32(data[i + 1]) << 16 | UInt32(data[i + 2]) << 8 | UInt32(data[i + 3])
            }
            for t in 16..<64 {
                let s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3)
                let s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10)
                w[t] = w[t - 16] &+ s0 &+ w[t - 7] &+ s1
            }
            var a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7]
            for t in 0..<64 {
                let s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)
                let ch = (e & f) ^ (~e & g)
                let t1 = hh &+ s1 &+ ch &+ k[t] &+ w[t]
                let s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)
                let maj = (a & b) ^ (a & c) ^ (b & c)
                let t2 = s0 &+ maj
                hh = g
                g = f
                f = e
                e = d &+ t1
                d = c
                c = b
                b = a
                a = t1 &+ t2
            }
            h[0] = h[0] &+ a
            h[1] = h[1] &+ b
            h[2] = h[2] &+ c
            h[3] = h[3] &+ d
            h[4] = h[4] &+ e
            h[5] = h[5] &+ f
            h[6] = h[6] &+ g
            h[7] = h[7] &+ hh
        }
        return h.map { word in
            let text = String(word, radix: 16)
            return String(repeating: "0", count: 8 - text.count) + text
        }.joined()
    }

    private static func rotr(_ x: UInt32, _ n: UInt32) -> UInt32 {
        (x >> n) | (x << (32 - n))
    }
}
