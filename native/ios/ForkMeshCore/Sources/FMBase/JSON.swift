// JSON decoding and encoding, after native/android core/json/Json.kt.
//
// Not JSONSerialization: it forgets key order and number forms, and the app
// signs the encoded text. `encode` writes one exact form, byte for byte: no
// whitespace, keys in insertion order, non-ASCII and "/" left alone, only
// quotes, backslashes and control characters escaped, whole doubles with
// their ".0".

import Foundation

public struct JSONError: Error, Equatable, CustomStringConvertible, Sendable {
    public let message: String
    /// Byte offset into the UTF-8 input.
    public let offset: Int

    public init(message: String, offset: Int) {
        self.message = message
        self.offset = offset
    }

    public var description: String { "\(message) at \(offset)" }
}

public enum JSON {
    public static func parse(_ text: String) throws -> JSONValue {
        var parser = Parser(bytes: Array(text.utf8))
        return try parser.parseDocument()
    }

    public static func parse(_ data: Data) throws -> JSONValue {
        var bytes = [UInt8](data)
        // `utf8.decode` drops a leading byte order mark before jsonDecode sees it.
        if bytes.starts(with: [0xEF, 0xBB, 0xBF]) { bytes.removeFirst(3) }
        var parser = Parser(bytes: bytes)
        return try parser.parseDocument()
    }

    /// The top-level object, or an empty one for invalid input or any other type.
    public static func parseObject(_ text: String) -> JSONObject {
        (try? parse(text))?.objectValue ?? JSONObject()
    }

    public static func parseObject(_ data: Data) -> JSONObject {
        (try? parse(data))?.objectValue ?? JSONObject()
    }

    /// NaN and the infinities have no JSON form; nothing the app encodes can
    /// hold one, so they are written as `null`.
    public static func encode(_ value: JSONValue) -> String {
        var out = [UInt8]()
        write(value, into: &out, indent: nil, depth: 0)
        return String(decoding: out, as: UTF8.self)
    }

    public static func encode(_ object: JSONObject) -> String { encode(.object(object)) }

    public static func encodeData(_ value: JSONValue) -> Data {
        var out = [UInt8]()
        write(value, into: &out, indent: nil, depth: 0)
        return Data(out)
    }

    /// `encode` with each nesting level indented by `indent`.
    public static func encodePretty(_ value: JSONValue, indent: String = "  ") -> String {
        var out = [UInt8]()
        write(value, into: &out, indent: Array(indent.utf8), depth: 0)
        return String(decoding: out, as: UTF8.self)
    }

    /// The shortest digits that round-trip, a
    /// trailing ".0" on whole values, exponent form from 1e21 and below 1e-6.
    public static func formatDouble(_ number: Double) -> String {
        if number.isNaN { return "NaN" }
        if number.isInfinite { return number < 0 ? "-Infinity" : "Infinity" }
        if number == 0 { return number.sign == .minus ? "-0.0" : "0.0" }

        // Swift's description already carries the shortest round-trip digits;
        // only their layout is rewritten.
        var mantissa = Substring(abs(number).description)
        var exponent = 0
        if let e = mantissa.firstIndex(where: { $0 == "e" || $0 == "E" }) {
            exponent = Int(mantissa[mantissa.index(after: e)...]) ?? 0
            mantissa = mantissa[..<e]
        }
        var digits = [Character]()
        var pointAt = 0 // digits before the decimal point
        var seenPoint = false
        for c in mantissa {
            if c == "." { seenPoint = true; continue }
            digits.append(c)
            if !seenPoint { pointAt += 1 }
        }
        while digits.count > 1, digits.first == "0" {
            digits.removeFirst()
            pointAt -= 1
        }
        while digits.count > 1, digits.last == "0" { digits.removeLast() }
        // value = 0.DIGITS x 10^n
        let n = pointAt + exponent
        let k = digits.count
        var out = number < 0 ? "-" : ""
        if k <= n, n <= 21 {
            out += String(digits) + String(repeating: "0", count: n - k) + ".0"
        } else if n > 0, n <= 21 {
            out += String(digits[..<n]) + "." + String(digits[n...])
        } else if n > -6, n <= 0 {
            out += "0." + String(repeating: "0", count: -n) + String(digits)
        } else {
            out += String(digits[0])
            if k > 1 { out += "." + String(digits[1...]) }
            out += (n - 1 < 0 ? "e-" : "e+") + String(abs(n - 1))
        }
        return out
    }

    // MARK: Writer

    private static func write(_ value: JSONValue, into out: inout [UInt8], indent: [UInt8]?, depth: Int) {
        switch value {
        case .null:
            out.append(contentsOf: "null".utf8)
        case .bool(let flag):
            out.append(contentsOf: (flag ? "true" : "false").utf8)
        case .int(let number):
            out.append(contentsOf: String(number).utf8)
        case .double(let number):
            out.append(contentsOf: (number.isFinite ? formatDouble(number) : "null").utf8)
        case .string(let text):
            quote(text, into: &out)
        case .array(let items):
            if items.isEmpty {
                out.append(contentsOf: "[]".utf8)
                return
            }
            out.append(UInt8(ascii: "["))
            for (index, item) in items.enumerated() {
                if index > 0 { out.append(UInt8(ascii: ",")) }
                newline(into: &out, indent: indent, depth: depth + 1)
                write(item, into: &out, indent: indent, depth: depth + 1)
            }
            newline(into: &out, indent: indent, depth: depth)
            out.append(UInt8(ascii: "]"))
        case .object(let object):
            if object.isEmpty {
                out.append(contentsOf: "{}".utf8)
                return
            }
            out.append(UInt8(ascii: "{"))
            var first = true
            for (key, item) in object {
                if !first { out.append(UInt8(ascii: ",")) }
                first = false
                newline(into: &out, indent: indent, depth: depth + 1)
                quote(key, into: &out)
                out.append(UInt8(ascii: ":"))
                if indent != nil { out.append(UInt8(ascii: " ")) }
                write(item, into: &out, indent: indent, depth: depth + 1)
            }
            newline(into: &out, indent: indent, depth: depth)
            out.append(UInt8(ascii: "}"))
        }
    }

    private static func newline(into out: inout [UInt8], indent: [UInt8]?, depth: Int) {
        guard let indent else { return }
        out.append(UInt8(ascii: "\n"))
        for _ in 0..<depth { out.append(contentsOf: indent) }
    }

    private static let hexDigits = Array("0123456789abcdef".utf8)

    private static func quote(_ text: String, into out: inout [UInt8]) {
        out.append(UInt8(ascii: "\""))
        for byte in text.utf8 {
            switch byte {
            case UInt8(ascii: "\""): out.append(contentsOf: "\\\"".utf8)
            case UInt8(ascii: "\\"): out.append(contentsOf: "\\\\".utf8)
            case 0x08: out.append(contentsOf: "\\b".utf8)
            case 0x0C: out.append(contentsOf: "\\f".utf8)
            case 0x0A: out.append(contentsOf: "\\n".utf8)
            case 0x0D: out.append(contentsOf: "\\r".utf8)
            case 0x09: out.append(contentsOf: "\\t".utf8)
            case 0..<0x20:
                out.append(contentsOf: "\\u00".utf8)
                out.append(hexDigits[Int(byte >> 4)])
                out.append(hexDigits[Int(byte & 0x0F)])
            default:
                out.append(byte)
            }
        }
        out.append(UInt8(ascii: "\""))
    }

    // MARK: Parser

    private struct Parser {
        let bytes: [UInt8]
        var pos = 0

        init(bytes: [UInt8]) { self.bytes = bytes }

        func fail(_ message: String, at offset: Int? = nil) -> JSONError {
            JSONError(message: message, offset: offset ?? pos)
        }

        mutating func parseDocument() throws -> JSONValue {
            skipWhitespace()
            let value = try readValue(depth: 0)
            skipWhitespace()
            if pos != bytes.count { throw fail("trailing characters") }
            return value
        }

        mutating func skipWhitespace() {
            while pos < bytes.count {
                let c = bytes[pos]
                if c == 0x20 || c == 0x09 || c == 0x0A || c == 0x0D { pos += 1 } else { break }
            }
        }

        func peek() throws -> UInt8 {
            guard pos < bytes.count else { throw fail("unexpected end of input") }
            return bytes[pos]
        }

        mutating func readValue(depth: Int) throws -> JSONValue {
            // Recursion guard: a hostile body must fail, not overflow the stack.
            if depth > 512 { throw fail("nesting too deep") }
            let c = try peek()
            switch c {
            case UInt8(ascii: "{"): return .object(try readObject(depth: depth))
            case UInt8(ascii: "["): return .array(try readArray(depth: depth))
            case UInt8(ascii: "\""): return .string(try readString())
            case UInt8(ascii: "t"): try expectWord("true"); return .bool(true)
            case UInt8(ascii: "f"): try expectWord("false"); return .bool(false)
            case UInt8(ascii: "n"): try expectWord("null"); return .null
            case UInt8(ascii: "-"), UInt8(ascii: "0")...UInt8(ascii: "9"): return try readNumber()
            default: throw fail("unexpected character")
            }
        }

        mutating func expectWord(_ word: String) throws {
            let expected = Array(word.utf8)
            guard pos + expected.count <= bytes.count,
                  bytes[pos..<(pos + expected.count)].elementsEqual(expected) else {
                throw fail("expected \(word)")
            }
            pos += expected.count
        }

        mutating func readObject(depth: Int) throws -> JSONObject {
            var object = JSONObject()
            pos += 1
            skipWhitespace()
            if try peek() == UInt8(ascii: "}") {
                pos += 1
                return object
            }
            while true {
                skipWhitespace()
                if try peek() != UInt8(ascii: "\"") { throw fail("expected key") }
                let key = try readString()
                skipWhitespace()
                if try peek() != UInt8(ascii: ":") { throw fail("expected ':'") }
                pos += 1
                skipWhitespace()
                // A repeated key overwrites in place.
                object[key] = try readValue(depth: depth + 1)
                skipWhitespace()
                let c = try peek()
                pos += 1
                if c == UInt8(ascii: "}") { return object }
                if c != UInt8(ascii: ",") { throw fail("expected ',' or '}'", at: pos - 1) }
            }
        }

        mutating func readArray(depth: Int) throws -> [JSONValue] {
            var list = [JSONValue]()
            pos += 1
            skipWhitespace()
            if try peek() == UInt8(ascii: "]") {
                pos += 1
                return list
            }
            while true {
                skipWhitespace()
                list.append(try readValue(depth: depth + 1))
                skipWhitespace()
                let c = try peek()
                pos += 1
                if c == UInt8(ascii: "]") { return list }
                if c != UInt8(ascii: ",") { throw fail("expected ',' or ']'", at: pos - 1) }
            }
        }

        mutating func readString() throws -> String {
            pos += 1
            var out = [UInt8]()
            var start = pos
            while true {
                guard pos < bytes.count else { throw fail("unterminated string") }
                let c = bytes[pos]
                if c == UInt8(ascii: "\"") {
                    out.append(contentsOf: bytes[start..<pos])
                    pos += 1
                    // Invalid UTF-8 becomes U+FFFD, as utf8.decode(allowMalformed) would.
                    return String(decoding: out, as: UTF8.self)
                }
                if c == UInt8(ascii: "\\") {
                    out.append(contentsOf: bytes[start..<pos])
                    pos += 1
                    guard pos < bytes.count else { throw fail("unterminated escape") }
                    let escape = bytes[pos]
                    pos += 1
                    switch escape {
                    case UInt8(ascii: "\""): out.append(UInt8(ascii: "\""))
                    case UInt8(ascii: "\\"): out.append(UInt8(ascii: "\\"))
                    case UInt8(ascii: "/"): out.append(UInt8(ascii: "/"))
                    case UInt8(ascii: "b"): out.append(0x08)
                    case UInt8(ascii: "f"): out.append(0x0C)
                    case UInt8(ascii: "n"): out.append(0x0A)
                    case UInt8(ascii: "r"): out.append(0x0D)
                    case UInt8(ascii: "t"): out.append(0x09)
                    case UInt8(ascii: "u"): appendScalar(try readEscapedScalar(), to: &out)
                    default: throw fail("bad escape", at: pos - 1)
                    }
                    start = pos
                    continue
                }
                if c < 0x20 { throw fail("control character in string") }
                pos += 1
            }
        }

        mutating func readHex4() throws -> UInt32 {
            guard pos + 4 <= bytes.count else { throw fail("bad unicode escape") }
            var code: UInt32 = 0
            for i in 0..<4 {
                let c = bytes[pos + i]
                let digit: UInt8
                switch c {
                case UInt8(ascii: "0")...UInt8(ascii: "9"): digit = c - UInt8(ascii: "0")
                case UInt8(ascii: "a")...UInt8(ascii: "f"): digit = c - UInt8(ascii: "a") + 10
                case UInt8(ascii: "A")...UInt8(ascii: "F"): digit = c - UInt8(ascii: "A") + 10
                default: throw fail("bad unicode escape")
                }
                code = code << 4 | UInt32(digit)
            }
            pos += 4
            return code
        }

        /// A `\uXXXX` escape, joined with the low surrogate that follows a
        /// high one. A Swift string cannot hold half a pair, so a lone
        /// surrogate becomes U+FFFD.
        mutating func readEscapedScalar() throws -> Unicode.Scalar {
            let first = try readHex4()
            if (0xD800...0xDBFF).contains(first) {
                if pos + 6 <= bytes.count, bytes[pos] == UInt8(ascii: "\\"), bytes[pos + 1] == UInt8(ascii: "u") {
                    let saved = pos
                    pos += 2
                    let second = try readHex4()
                    if (0xDC00...0xDFFF).contains(second) {
                        let joined = 0x10000 + ((first - 0xD800) << 10) + (second - 0xDC00)
                        return Unicode.Scalar(joined) ?? "\u{FFFD}"
                    }
                    pos = saved
                }
                return "\u{FFFD}"
            }
            return Unicode.Scalar(first) ?? "\u{FFFD}"
        }

        func appendScalar(_ scalar: Unicode.Scalar, to out: inout [UInt8]) {
            out.append(contentsOf: String(Character(scalar)).utf8)
        }

        mutating func readNumber() throws -> JSONValue {
            let start = pos
            func isDigit(_ c: UInt8) -> Bool { c >= UInt8(ascii: "0") && c <= UInt8(ascii: "9") }
            func digits(_ parser: inout Parser) throws {
                guard parser.pos < parser.bytes.count, isDigit(parser.bytes[parser.pos]) else {
                    throw parser.fail("bad number", at: start)
                }
                while parser.pos < parser.bytes.count, isDigit(parser.bytes[parser.pos]) { parser.pos += 1 }
            }

            if bytes[pos] == UInt8(ascii: "-") { pos += 1 }
            // No leading zeros: "01" is not JSON.
            if pos < bytes.count, bytes[pos] == UInt8(ascii: "0") {
                pos += 1
            } else {
                try digits(&self)
            }
            var integral = true
            if pos < bytes.count, bytes[pos] == UInt8(ascii: ".") {
                integral = false
                pos += 1
                try digits(&self)
            }
            if pos < bytes.count, bytes[pos] == UInt8(ascii: "e") || bytes[pos] == UInt8(ascii: "E") {
                integral = false
                pos += 1
                if pos < bytes.count, bytes[pos] == UInt8(ascii: "+") || bytes[pos] == UInt8(ascii: "-") { pos += 1 }
                try digits(&self)
            }
            let text = String(decoding: bytes[start..<pos], as: UTF8.self)
            // Integers past 64 bits fall back to a double.
            if integral, let number = Int64(text) { return .int(number) }
            guard let number = Double(text) else { throw fail("bad number", at: start) }
            return .double(number)
        }
    }
}
