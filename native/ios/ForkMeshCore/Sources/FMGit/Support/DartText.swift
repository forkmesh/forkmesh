// Swift compares and splits `String` by grapheme cluster and canonical
// equivalence ("/" followed by a combining mark is one Character, and
// U+212A equals "K"). Every check that guards a path, a ref name or a wire
// token therefore goes through the code-unit helpers here instead of `==`,
// `split`, `hasPrefix` or `contains`.

import Foundation

/// `utf8.decode(bytes)` with `allowMalformed: false`.
func utf8DecodeStrict(_ bytes: [UInt8]) -> String? {
    let text = String(decoding: bytes, as: UTF8.self)
    // A repaired string carries U+FFFD where the input had an invalid
    // sequence, so its UTF-8 can only equal well-formed input.
    return text.utf8.elementsEqual(bytes) ? text : nil
}

func utf8DecodeStrict(_ bytes: ArraySlice<UInt8>) -> String? { utf8DecodeStrict(Array(bytes)) }

/// `ascii.decode(bytes)`: nil for any byte above 0x7f.
func asciiDecode(_ bytes: [UInt8]) -> String? {
    for byte in bytes where byte > 0x7f { return nil }
    return String(decoding: bytes, as: UTF8.self)
}

/// `ascii.decode(bytes, allowInvalid: true)`: bytes above 0x7f become U+FFFD.
func asciiDecodeAllowInvalid(_ bytes: [UInt8]) -> String {
    var scalars = String.UnicodeScalarView()
    for byte in bytes {
        scalars.append(byte > 0x7f ? Unicode.Scalar(0xfffd)! : Unicode.Scalar(byte))
    }
    return String(scalars)
}

/// `latin1.decode(bytes)` / `String.fromCharCodes(bytes)`.
func latin1Decode(_ bytes: [UInt8]) -> String {
    var scalars = String.UnicodeScalarView()
    for byte in bytes { scalars.append(Unicode.Scalar(byte)) }
    return String(scalars)
}

/// `ascii.encode(value)`; nil when a character is above 0x7f.
func asciiEncode(_ value: String) -> [UInt8]? {
    let bytes = Array(value.utf8)
    for byte in bytes where byte > 0x7f { return nil }
    return bytes
}

func utf8Encode(_ value: String) -> [UInt8] { Array(value.utf8) }

extension String {
    /// Code-unit equality, not canonical equivalence.
    func sameText(_ other: String) -> Bool { utf8.elementsEqual(other.utf8) }

    func u8HasPrefix(_ prefix: String) -> Bool { utf8.starts(with: prefix.utf8) }

    func u8HasSuffix(_ suffix: String) -> Bool {
        let own = utf8.count
        let other = suffix.utf8.count
        if other > own { return false }
        return utf8.dropFirst(own - other).elementsEqual(suffix.utf8)
    }

    /// Index of `needle` at or after `start`, in UTF-8 offsets; -1 when absent.
    func u8IndexOf(_ needle: String, from start: Int = 0) -> Int {
        Array(utf8).indexOfSequence(Array(needle.utf8), from: start)
    }

    func u8Contains(_ needle: String) -> Bool { u8IndexOf(needle) >= 0 }

    /// Splits at a one-byte ASCII separator: empty parts are kept, and the
    /// split never lands inside a grapheme by mistake.
    func u8Split(_ separator: UInt8) -> [String] {
        var parts = [String]()
        var current = [UInt8]()
        for byte in utf8 {
            if byte == separator {
                parts.append(String(decoding: current, as: UTF8.self))
                current.removeAll(keepingCapacity: true)
            } else {
                current.append(byte)
            }
        }
        parts.append(String(decoding: current, as: UTF8.self))
        return parts
    }

    /// `substring(start, end)` in UTF-8 offsets; the offsets must be on
    /// scalar boundaries (they always come from ASCII delimiters here).
    func u8Substring(_ start: Int, _ end: Int? = nil) -> String {
        let bytes = Array(utf8)
        return String(decoding: bytes[start..<(end ?? bytes.count)], as: UTF8.self)
    }

    /// Strips leading and trailing whitespace (`isDartWhitespace`).
    func dartTrim() -> String {
        var scalars = Array(unicodeScalars)
        while let last = scalars.last, isDartWhitespace(last) { scalars.removeLast() }
        var start = 0
        while start < scalars.count, isDartWhitespace(scalars[start]) { start += 1 }
        var view = String.UnicodeScalarView()
        view.append(contentsOf: scalars[start...])
        return String(view)
    }

    /// Strips leading whitespace (`isDartWhitespace`).
    func dartTrimLeft() -> String {
        let scalars = Array(unicodeScalars)
        var start = 0
        while start < scalars.count, isDartWhitespace(scalars[start]) { start += 1 }
        var view = String.UnicodeScalarView()
        view.append(contentsOf: scalars[start...])
        return String(view)
    }

    /// Comparison in UTF-16 code unit order.
    func dartCompare(_ other: String) -> Int {
        var left = utf16.makeIterator()
        var right = other.utf16.makeIterator()
        while true {
            switch (left.next(), right.next()) {
            case (nil, nil): return 0
            case (nil, _): return -1
            case (_, nil): return 1
            case let (a?, b?): if a != b { return a < b ? -1 : 1 }
            }
        }
    }
}

/// The whitespace set that `dartTrim` strips.
func isDartWhitespace(_ scalar: Unicode.Scalar) -> Bool {
    switch scalar.value {
    case 0x09...0x0d, 0x20, 0x85, 0xa0, 0x1680, 0x2000...0x200a, 0x2028, 0x2029, 0x202f, 0x205f, 0x3000, 0xfeff:
        return true
    default:
        return false
    }
}

/// `const LineSplitter().convert(text)`: splits at `\n`, `\r\n` and `\r`; no trailing empty line.
func dartSplitLines(_ text: String) -> [String] {
    let bytes = Array(text.utf8)
    var lines = [String]()
    var start = 0
    var index = 0
    while index < bytes.count {
        let byte = bytes[index]
        if byte == 0x0a || byte == 0x0d {
            lines.append(String(decoding: bytes[start..<index], as: UTF8.self))
            if byte == 0x0d, index + 1 < bytes.count, bytes[index + 1] == 0x0a { index += 1 }
            start = index + 1
        }
        index += 1
    }
    if start < bytes.count { lines.append(String(decoding: bytes[start...], as: UTF8.self)) }
    return lines
}

extension Array where Element == UInt8 {
    func indexOfSequence(_ needle: [UInt8], from start: Int = 0) -> Int {
        if needle.isEmpty { return Swift.min(Swift.max(start, 0), count) }
        if needle.count > count { return -1 }
        var index = Swift.max(start, 0)
        while index + needle.count <= count {
            if self[index] == needle[0] {
                var matched = true
                var inner = 1
                while inner < needle.count {
                    if self[index + inner] != needle[inner] { matched = false; break }
                    inner += 1
                }
                if matched { return index }
            }
            index += 1
        }
        return -1
    }
}

/// Byte-wise ordering, the order Git itself uses for paths and names.
func compareBytes(_ left: [UInt8], _ right: [UInt8]) -> Int {
    let shared = Swift.min(left.count, right.count)
    var index = 0
    while index < shared {
        if left[index] != right[index] { return Int(left[index]) - Int(right[index]) }
        index += 1
    }
    return left.count - right.count
}

/// A cached `NSRegularExpression` used for the regular expressions that are not
/// worth hand-rolling. Patterns are written for ICU: `\z` instead of `$`
/// (ICU's `$` also matches before a final newline) and `[0-9]` instead of
/// `\d` (ICU's `\d` accepts every Unicode digit, not only ASCII).
final class GitRegex: @unchecked Sendable {
    private let expression: NSRegularExpression

    init(_ pattern: String) {
        // Patterns are compile-time literals; a typo is a programmer error.
        expression = try! NSRegularExpression(pattern: pattern, options: [])
    }

    func hasMatch(_ text: String) -> Bool {
        let range = NSRange(location: 0, length: (text as NSString).length)
        return expression.firstMatch(in: text, options: [], range: range) != nil
    }

    /// Capture groups of the first match (index 0 is the whole match).
    func firstMatch(_ text: String) -> [String?]? {
        let source = text as NSString
        guard let match = expression.firstMatch(in: text, options: [], range: NSRange(location: 0, length: source.length))
        else { return nil }
        return (0..<match.numberOfRanges).map { index in
            let range = match.range(at: index)
            return range.location == NSNotFound ? nil : source.substring(with: range)
        }
    }
}
