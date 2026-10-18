import Foundation

typealias Units = [UInt16]

enum U {
    static let newline: UInt16 = 10
    static let space: UInt16 = 32
    static let tab: UInt16 = 9
    static let quote: UInt16 = 34
    static let apostrophe: UInt16 = 39
    static let backtick: UInt16 = 96
    static let slash: UInt16 = 47
    static let backslash: UInt16 = 92
    static let star: UInt16 = 42
    static let hash: UInt16 = 35
    static let dash: UInt16 = 45
    static let dollar: UInt16 = 36
    static let openBrace: UInt16 = 123
    static let closeBrace: UInt16 = 125
    static let openParen: UInt16 = 40
    static let closeParen: UInt16 = 41
    static let openBracket: UInt16 = 91
    static let closeBracket: UInt16 = 93
    static let less: UInt16 = 60
    static let greater: UInt16 = 62
    static let semicolon: UInt16 = 59
    static let comma: UInt16 = 44
    static let colon: UInt16 = 58
    static let equals: UInt16 = 61
    static let bang: UInt16 = 33
    static let question: UInt16 = 63
    static let dot: UInt16 = 46
    static let at: UInt16 = 64
    static let underscore: UInt16 = 95

    @inline(__always)
    static func c(_ scalar: Unicode.Scalar) -> UInt16 { UInt16(scalar.value) }

    @inline(__always)
    static func isSpace(_ c: UInt16) -> Bool {
        if c == 32 || (c >= 9 && c <= 13) { return true }
        if c < 128 { return false }
        switch c {
        case 0x85, 0xA0, 0x1680, 0x2000...0x200A, 0x2028, 0x2029, 0x202F, 0x205F, 0x3000: return true
        default: return false
        }
    }

    private static func category(_ c: UInt16) -> Unicode.GeneralCategory? {
        guard let scalar = Unicode.Scalar(c) else { return nil }
        return scalar.properties.generalCategory
    }

    static func isLetter(_ c: UInt16) -> Bool {
        if c < 128 { return (c >= 65 && c <= 90) || (c >= 97 && c <= 122) }
        switch category(c) {
        case .uppercaseLetter?, .lowercaseLetter?, .titlecaseLetter?, .modifierLetter?, .otherLetter?: return true
        default: return false
        }
    }

    static func isDigit(_ c: UInt16) -> Bool {
        if c < 128 { return c >= 48 && c <= 57 }
        return category(c) == .decimalNumber
    }

    static func isLetterOrNumber(_ c: UInt16) -> Bool {
        if c < 128 { return (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || (c >= 48 && c <= 57) }
        switch category(c) {
        case .uppercaseLetter?, .lowercaseLetter?, .titlecaseLetter?, .modifierLetter?, .otherLetter?,
             .decimalNumber?, .letterNumber?, .otherNumber?: return true
        default: return false
        }
    }

    static func units(_ text: String) -> Units { Array(text.utf16) }

    static func string(_ units: Units) -> String { String(decoding: units, as: UTF16.self) }

    static func string(_ units: Units, _ range: Range<Int>) -> String {
        String(decoding: units[range], as: UTF16.self)
    }

    static func decode(_ bytes: [UInt8]) -> Units {
        Array(String(decoding: bytes, as: UTF8.self).utf16)
    }

    static func trim(_ s: Units, _ range: Range<Int>) -> Range<Int> {
        var lo = range.lowerBound
        var hi = range.upperBound
        while lo < hi && isSpace(s[lo]) { lo += 1 }
        while hi > lo && isSpace(s[hi - 1]) { hi -= 1 }
        return lo..<hi
    }

    static func trimmed(_ s: Units) -> Units {
        let range = trim(s, 0..<s.count)
        return range.count == s.count ? s : Array(s[range])
    }

    static func lines(_ s: Units) -> [Range<Int>] {
        var result: [Range<Int>] = []
        var start = 0
        for i in 0..<s.count where s[i] == newline {
            result.append(start..<i)
            start = i + 1
        }
        result.append(start..<s.count)
        return result
    }

    static func split(_ s: Units, _ separator: UInt16) -> [Units] {
        var parts: [Units] = []
        var current: Units = []
        for c in s {
            if c == separator {
                parts.append(current)
                current = []
            } else {
                current.append(c)
            }
        }
        parts.append(current)
        return parts
    }

    static func simplified(_ s: Units, _ range: Range<Int>) -> Units {
        var out: Units = []
        out.reserveCapacity(range.count)
        var pendingSpace = false
        for i in range {
            let c = s[i]
            if isSpace(c) {
                pendingSpace = !out.isEmpty
            } else {
                if pendingSpace { out.append(space) }
                pendingSpace = false
                out.append(c)
            }
        }
        return out
    }

    static func indexOf(_ s: Units, _ c: UInt16, from: Int = 0) -> Int {
        var i = max(0, from)
        while i < s.count {
            if s[i] == c { return i }
            i += 1
        }
        return -1
    }

    static func lastIndexOf(_ s: Units, _ c: UInt16) -> Int {
        var i = s.count - 1
        while i >= 0 {
            if s[i] == c { return i }
            i -= 1
        }
        return -1
    }

    static func indexOf(_ s: Units, _ needle: Units, from: Int = 0, startsBefore: Int? = nil) -> Int {
        let n = s.count
        let m = needle.count
        let first = max(0, from)
        if m == 0 { return first <= n ? first : -1 }
        let lastStart = min(n - m, (startsBefore ?? Int.max) - 1)
        if first > lastStart { return -1 }
        let head = needle[0]
        var i = first
        while i <= lastStart {
            if s[i] == head {
                var k = 1
                while k < m && s[i + k] == needle[k] { k += 1 }
                if k == m { return i }
            }
            i += 1
        }
        return -1
    }

    static func indexOf(_ s: Units, _ needle: String, from: Int = 0) -> Int {
        indexOf(s, units(needle), from: from)
    }

    static func contains(_ s: Units, _ needle: String) -> Bool { indexOf(s, units(needle)) >= 0 }

    static func contains(_ s: Units, _ c: UInt16) -> Bool { s.contains(c) }

    static func hasPrefix(_ s: Units, _ prefix: String) -> Bool {
        hasPrefix(s, 0..<s.count, prefix)
    }

    static func hasPrefix(_ s: Units, _ range: Range<Int>, _ prefix: String) -> Bool {
        var i = range.lowerBound
        for c in prefix.utf16 {
            if i >= range.upperBound || s[i] != c { return false }
            i += 1
        }
        return true
    }

    static func hasSuffix(_ s: Units, _ suffix: String) -> Bool {
        let tail = Array(suffix.utf16)
        if tail.count > s.count { return false }
        let offset = s.count - tail.count
        for k in 0..<tail.count where s[offset + k] != tail[k] { return false }
        return true
    }

    static func matchingParen(_ s: Units, _ open: Int) -> Int {
        var depth = 0
        var i = open
        while i < s.count {
            let c = s[i]
            if c == openParen {
                depth += 1
            } else if c == closeParen {
                depth -= 1
                if depth == 0 { return i }
            }
            i += 1
        }
        return -1
    }

    static func less(_ a: String, _ b: String) -> Bool {
        var x = a.utf16.makeIterator()
        var y = b.utf16.makeIterator()
        while true {
            switch (x.next(), y.next()) {
            case (nil, nil): return false
            case (nil, _): return true
            case (_, nil): return false
            case (let p?, let q?):
                if p != q { return p < q }
            }
        }
    }

    static func grouped(_ value: Int) -> String {
        let digits = String(abs(value))
        var out = ""
        for (k, ch) in digits.enumerated() {
            if k > 0 && (digits.count - k) % 3 == 0 { out.append(",") }
            out.append(ch)
        }
        return value < 0 ? "-" + out : out
    }
}

enum PathText {
    static func lastSlash(_ path: String) -> String.Index? {
        path.utf8.lastIndex(of: UInt8(ascii: "/"))
    }

    static func lastDot(_ path: String) -> String.Index? {
        path.utf8.lastIndex(of: UInt8(ascii: "."))
    }

    static func offset(_ path: String, _ index: String.Index?) -> Int {
        guard let index else { return -1 }
        return path.utf8.distance(from: path.utf8.startIndex, to: index)
    }

    static func extensionOf(_ path: String) -> String {
        let slash = offset(path, lastSlash(path))
        let dotIndex = lastDot(path)
        let dot = offset(path, dotIndex)
        if dot <= slash + 1 { return "" }
        return String(path[path.utf8.index(after: dotIndex!)...]).lowercased()
    }

    static func dirOf(_ path: String) -> String {
        guard let slash = lastSlash(path) else { return "" }
        return String(path[..<slash])
    }

    static func fileNameOf(_ path: String) -> String {
        guard let slash = lastSlash(path) else { return path }
        return String(path[path.utf8.index(after: slash)...])
    }

    static func stemOf(_ path: String) -> String {
        let name = fileNameOf(path)
        guard let dot = lastDot(name), dot != name.startIndex else { return name }
        return String(name[..<dot])
    }

    static func withoutExtension(_ path: String) -> String {
        let slash = offset(path, lastSlash(path))
        let dotIndex = lastDot(path)
        let dot = offset(path, dotIndex)
        if dot <= slash + 1 { return path }
        return String(path[..<dotIndex!])
    }

    static func join(_ dir: String, _ rest: String) -> String {
        if dir.isEmpty { return rest }
        if rest.isEmpty { return dir }
        return dir + "/" + rest
    }

    static func cleanPath(_ path: String) -> String {
        if path.isEmpty { return path }
        let absolute = path.hasPrefix("/")
        var out: [Substring] = []
        for part in path.split(separator: "/", omittingEmptySubsequences: true) {
            if part == "." { continue }
            if part == ".." {
                if let last = out.last, last != ".." {
                    out.removeLast()
                } else if !absolute {
                    out.append(part)
                }
                continue
            }
            out.append(part)
        }
        let joined = out.joined(separator: "/")
        if absolute { return "/" + joined }
        return joined.isEmpty ? "." : joined
    }

    static func normalized(_ path: String) -> String {
        if path.isEmpty { return "" }
        var clean = cleanPath(path)
        while clean.hasPrefix("./") { clean = String(clean.dropFirst(2)) }
        if clean == "." || clean.hasPrefix("../") || clean == ".." || clean.hasPrefix("/") { return "" }
        return clean
    }

    static func commonPrefix(_ a: String, _ b: String) -> Int {
        var count = 0
        for (x, y) in zip(a.utf16, b.utf16) {
            if x != y { break }
            count += 1
        }
        return count
    }

    static func section(_ text: String, _ separator: String, _ start: Int, _ end: Int = -1) -> String {
        let sections = text.components(separatedBy: separator)
        let size = sections.count
        var start = start
        var end = end
        if start < 0 { start += size }
        if end < 0 { end += size }
        if start >= size || end < 0 || start > end { return "" }
        var ret = ""
        var x = 0
        for i in 0..<size where x <= end {
            if x >= start {
                if x > start && i > 0 { ret += separator }
                ret += sections[i]
            }
            x += 1
        }
        return ret
    }
}
