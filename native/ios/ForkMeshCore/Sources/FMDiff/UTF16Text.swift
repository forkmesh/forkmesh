// Shared text plumbing for FMDiff.
//
// Strings are indexed by UTF-16 code unit, and every offset produced (diff
// emphasis ranges, highlight spans, caret positions) is a UTF-16 offset:
// offsets are plain `Int` UTF-16 offsets, which is also what NSRange, UITextView and
// NSAttributedString speak. Internally everything scans `[UInt16]` once, so
// work stays linear in the size of the text; `String.index(offsetBy:)` is
// never called in a loop.

import Foundation

/// A half-open UTF-16 range `[start, end)` inside one line or buffer.
public struct DiffRange: Equatable, Hashable, Sendable, CustomStringConvertible {
    public let start: Int
    public let end: Int

    public init(_ start: Int, _ end: Int) {
        self.start = start
        self.end = end
    }

    public var length: Int { end - start }
    public var description: String { "[\(start),\(end))" }

    /// For NSAttributedString / UITextView, which take UTF-16 ranges as is.
    public var nsRange: NSRange { NSRange(location: start, length: end - start) }
}

public extension String {
    /// Length in UTF-16 code units.
    var utf16Length: Int { utf16.count }

    /// The index at a UTF-16 offset, clamped into the string. An offset that
    /// falls inside a surrogate pair rounds down to the scalar's start.
    func index(utf16Offset offset: Int) -> String.Index {
        let clamped = Swift.max(0, Swift.min(offset, utf16.count))
        let raw = utf16.index(utf16.startIndex, offsetBy: clamped)
        return raw.samePosition(in: unicodeScalars) ?? utf16.index(before: raw)
    }

    /// `substring(start, end)` over UTF-16 offsets, clamped.
    func substring(utf16 start: Int, _ end: Int) -> String {
        let lower = index(utf16Offset: start)
        let upper = index(utf16Offset: Swift.max(start, end))
        return String(self[lower..<upper])
    }

    func substring(_ range: DiffRange) -> String {
        substring(utf16: range.start, range.end)
    }

    /// The Swift range for a UTF-16 range, for `AttributedString` work.
    func range(_ range: DiffRange) -> Range<String.Index> {
        let lower = index(utf16Offset: range.start)
        let upper = index(utf16Offset: Swift.max(range.start, range.end))
        return lower..<upper
    }
}

// MARK: - Internal helpers over code units

typealias Units = [UInt16]

enum U16 {
    static let tab: UInt16 = 0x09
    static let lf: UInt16 = 0x0A
    static let cr: UInt16 = 0x0D
    static let space: UInt16 = 0x20
    static let quote: UInt16 = 0x22
    static let hash: UInt16 = 0x23
    static let dollar: UInt16 = 0x24
    static let apostrophe: UInt16 = 0x27
    static let openParen: UInt16 = 0x28
    static let star: UInt16 = 0x2A
    static let plus: UInt16 = 0x2B
    static let comma: UInt16 = 0x2C
    static let minus: UInt16 = 0x2D
    static let dot: UInt16 = 0x2E
    static let slash: UInt16 = 0x2F
    static let colon: UInt16 = 0x3A
    static let less: UInt16 = 0x3C
    static let equals: UInt16 = 0x3D
    static let greater: UInt16 = 0x3E
    static let question: UInt16 = 0x3F
    static let at: UInt16 = 0x40
    static let backslash: UInt16 = 0x5C
    static let underscore: UInt16 = 0x5F
    static let backtick: UInt16 = 0x60

    static func units(_ text: String) -> Units { Array(text.utf16) }

    static func string<C: Collection>(_ units: C) -> String where C.Element == UInt16 {
        String(decoding: units, as: UTF16.self)
    }

    static func isDigit(_ unit: UInt16) -> Bool { unit >= 0x30 && unit <= 0x39 }

    static func isAlpha(_ unit: UInt16) -> Bool {
        (unit >= 0x41 && unit <= 0x5A) || (unit >= 0x61 && unit <= 0x7A)
    }

    static func isUpper(_ unit: UInt16) -> Bool { unit >= 0x41 && unit <= 0x5A }

    static func isHexDigit(_ unit: UInt16) -> Bool {
        isDigit(unit) || (unit >= 0x41 && unit <= 0x46) || (unit >= 0x61 && unit <= 0x66)
    }

    /// `\w` of a non-unicode ECMAScript regex.
    static func isWord(_ unit: UInt16) -> Bool {
        isDigit(unit) || isAlpha(unit) || unit == underscore
    }

    /// `\s` of an ECMAScript regex.
    static func isRegexSpace(_ unit: UInt16) -> Bool {
        switch unit {
        case 0x09...0x0D, 0x20, 0xA0, 0x1680, 0x2000...0x200A,
             0x2028, 0x2029, 0x202F, 0x205F, 0x3000, 0xFEFF:
            return true
        default:
            return false
        }
    }

    /// What trimming strips: `\s` plus NEL.
    static func isTrimSpace(_ unit: UInt16) -> Bool {
        unit == 0x85 || isRegexSpace(unit)
    }

    /// What `.` refuses to match in an ECMAScript regex.
    static func isLineTerminator(_ unit: UInt16) -> Bool {
        unit == lf || unit == cr || unit == 0x2028 || unit == 0x2029
    }

    static func isHighSurrogate(_ unit: UInt16) -> Bool { unit >= 0xD800 && unit <= 0xDBFF }
    static func isLowSurrogate(_ unit: UInt16) -> Bool { unit >= 0xDC00 && unit <= 0xDFFF }

    static func hasPrefix<C: RandomAccessCollection>(_ text: C, _ prefix: Units, at offset: Int = 0) -> Bool
    where C.Element == UInt16, C.Index == Int {
        let begin = text.startIndex + offset
        guard offset >= 0, begin + prefix.count <= text.endIndex else { return false }
        for i in 0..<prefix.count where text[begin + i] != prefix[i] { return false }
        return true
    }

    /// First position of `needle` at or after `from` (absolute index), or -1.
    static func indexOf<C: RandomAccessCollection>(_ text: C, _ needle: Units, from: Int) -> Int
    where C.Element == UInt16, C.Index == Int {
        guard !needle.isEmpty else { return from }
        var i = Swift.max(from, text.startIndex)
        let last = text.endIndex - needle.count
        while i <= last {
            if text[i] == needle[0] {
                var same = true
                var k = 1
                while k < needle.count {
                    if text[i + k] != needle[k] { same = false; break }
                    k += 1
                }
                if same { return i }
            }
            i += 1
        }
        return -1
    }

    static func indexOf<C: RandomAccessCollection>(_ text: C, unit: UInt16, from: Int) -> Int
    where C.Element == UInt16, C.Index == Int {
        var i = Swift.max(from, text.startIndex)
        while i < text.endIndex {
            if text[i] == unit { return i }
            i += 1
        }
        return -1
    }

    static func trim(_ text: ArraySlice<UInt16>) -> ArraySlice<UInt16> {
        trimRight(trimLeft(text))
    }

    static func trimLeft(_ text: ArraySlice<UInt16>) -> ArraySlice<UInt16> {
        var lower = text.startIndex
        while lower < text.endIndex, isTrimSpace(text[lower]) { lower += 1 }
        return text[lower..<text.endIndex]
    }

    static func trimRight(_ text: ArraySlice<UInt16>) -> ArraySlice<UInt16> {
        var upper = text.endIndex
        while upper > text.startIndex, isTrimSpace(text[upper - 1]) { upper -= 1 }
        return text[text.startIndex..<upper]
    }

    static func isBlank<C: Collection>(_ text: C) -> Bool where C.Element == UInt16 {
        text.allSatisfy(isTrimSpace)
    }

    /// Breaks on `\n`, `\r\n` and `\r`, and a final
    /// terminator does not produce a trailing empty line.
    static func splitLines(_ text: Units) -> [ArraySlice<UInt16>] {
        var lines: [ArraySlice<UInt16>] = []
        var start = 0
        var i = 0
        let count = text.count
        while i < count {
            let unit = text[i]
            if unit == lf || unit == cr {
                lines.append(text[start..<i])
                if unit == cr, i + 1 < count, text[i + 1] == lf { i += 1 }
                start = i + 1
            }
            i += 1
        }
        if start < count { lines.append(text[start..<count]) }
        return lines
    }

    /// Splits on `\n`: always at least one piece, `\r` left in place.
    static func splitNewlines(_ text: Units) -> [ArraySlice<UInt16>] {
        var lines: [ArraySlice<UInt16>] = []
        var start = 0
        for i in 0..<text.count where text[i] == lf {
            lines.append(text[start..<i])
            start = i + 1
        }
        lines.append(text[start..<text.count])
        return lines
    }

    /// A run of ASCII digits as a non-negative Int; nil when empty or too long.
    static func parseInt<C: Collection>(_ digits: C) -> Int? where C.Element == UInt16 {
        guard !digits.isEmpty else { return nil }
        var value = 0
        for unit in digits {
            guard isDigit(unit) else { return nil }
            let (scaled, overflowA) = value.multipliedReportingOverflow(by: 10)
            let (next, overflowB) = scaled.addingReportingOverflow(Int(unit - 0x30))
            if overflowA || overflowB { return nil }
            value = next
        }
        return value
    }
}
