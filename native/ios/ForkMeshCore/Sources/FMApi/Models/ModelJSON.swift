// The scalar helpers the models share (number, bool and string readers, title
// case, truncation, compact durations) and the parsing and printing rules they
// lean on, after native/android model/ModelJson.kt.
//
// The models read JSON with string interpolation and `num` casts, which is more
// lenient than FMBase's accessors in a few corners (hex strings, "1.5" is not
// an int, null prints as "null"); these keep those answers exactly.

import Foundation
import FMBase

/// The parsing and printing semantics the models rely on.
public enum DartCore {
    /// `int.tryParse(text)`: surrounding whitespace ignored, optional sign,
    /// decimal or `0x` hex; nil for anything else (including "1.5" and overflow).
    public static func intTryParse(_ text: String) -> Int? {
        var s = Substring(text.dartTrimmed)
        if s.isEmpty { return nil }
        var negative = false
        if s.first == "+" || s.first == "-" {
            negative = s.first == "-"
            s = s.dropFirst()
        }
        var radix = 10
        if s.count > 2, s.hasPrefix("0x") || s.hasPrefix("0X") {
            radix = 16
            s = s.dropFirst(2)
        }
        if s.isEmpty { return nil }
        for c in s.unicodeScalars {
            let digit = ("0"..."9").contains(c)
            let hex = radix == 16 && (("a"..."f").contains(c) || ("A"..."F").contains(c))
            if !digit && !hex { return nil }
        }
        return Int((negative ? "-" : "") + s, radix: radix)
    }

    private static let doublePattern = try! NSRegularExpression(
        pattern: "^[+-]?(?:[0-9]+\\.?[0-9]*|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?$")

    /// `double.tryParse(text)`: decimal and exponent forms, "NaN", "Infinity".
    public static func doubleTryParse(_ text: String) -> Double? {
        let s = text.dartTrimmed
        if s.isEmpty { return nil }
        switch s {
        case "NaN", "+NaN", "-NaN": return .nan
        case "Infinity", "+Infinity": return .infinity
        case "-Infinity": return -.infinity
        default: break
        }
        guard DartRegex.matches(doublePattern, s) else { return nil }
        return Double(s)
    }

    /// `'$value'`: like `JSONValue.text`, except a null prints as "null"
    /// (what `value.toString()` gives, unlike `value ?? ''`).
    public static func toString(_ value: JSONValue?) -> String {
        guard let value, !value.isNull else { return "null" }
        return value.text
    }

    /// A double as text: `1.0`, `1e-7`, `1.5e+21`.
    public static func doubleToString(_ value: Double) -> String { JSON.formatDouble(value) }

    /// `double.toStringAsFixed(digits)`: the exact binary value, rounded half
    /// away from zero (0.125 gives "0.13", where printf would give "0.12").
    public static func toStringAsFixed(_ value: Double, _ digits: Int) -> String {
        if !value.isFinite || abs(value) >= 1e21 { return doubleToString(value) }
        // Enough fractional digits for the exact expansion of any double.
        let exact = value == 0 ? 0 : max(0, value.significandWidth - Int(value.exponent))
        let text = String(format: "%.\(max(exact, digits + 1))f", abs(value))
        let parts = text.split(separator: ".", omittingEmptySubsequences: false)
        var whole = Array(parts[0])
        let fraction = parts.count > 1 ? Array(parts[1]) : []
        var kept = Array(fraction.prefix(digits))
        while kept.count < digits { kept.append("0") }
        let roundUp = fraction.count > digits && fraction[digits] >= "5"
        if roundUp {
            var carry = true
            var index = kept.count - 1
            while carry && index >= 0 {
                if kept[index] == "9" { kept[index] = "0"; index -= 1 } else {
                    kept[index] = Character(UnicodeScalar(kept[index].asciiValue! + 1)); carry = false
                }
            }
            index = whole.count - 1
            while carry && index >= 0 {
                if whole[index] == "9" { whole[index] = "0"; index -= 1 } else {
                    whole[index] = Character(UnicodeScalar(whole[index].asciiValue! + 1)); carry = false
                }
            }
            if carry { whole.insert("1", at: 0) }
        }
        let sign = value.sign == .minus ? "-" : ""
        return sign + String(whole) + (digits > 0 ? "." + String(kept) : "")
    }

    /// `Uri.encodeComponent`: everything but `A-Za-z0-9-_.!~*'()` is escaped.
    public static func encodeComponent(_ value: String) -> String {
        value.addingPercentEncoding(withAllowedCharacters: componentAllowed) ?? value
    }

    private static let componentAllowed = CharacterSet(
        charactersIn: "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.!~*'()")

    /// Percent-decodes, or nil for a malformed escape.
    /// A '+' stays a '+'.
    public static func decodeComponent(_ value: String) -> String? {
        value.removingPercentEncoding
    }

    private static let dateTimePattern = try! NSRegularExpression(pattern:
        "^([+-]?[0-9]{4,6})-?([0-9][0-9])-?([0-9][0-9])"
        + "(?:[ T]([0-9][0-9])(?::?([0-9][0-9])(?::?([0-9][0-9])(?:[.,]([0-9]+))?)?)?"
        + "( ?[zZ]| ?([-+])([0-9][0-9])(?::?([0-9][0-9]))?)?)?$")

    /// `DateTime.tryParse(text)?.millisecondsSinceEpoch`. A stamp without a
    /// zone is wall-clock time in `timeZone`. Out-of-range fields roll over.
    public static func dateTimeTryParseMs(_ text: String, timeZone: TimeZone = .current) -> Int? {
        guard let g = DartRegex.groups(dateTimePattern, text) else { return nil }
        func num(_ i: Int) -> Int { Int(g[i]) ?? 0 }
        guard let year = Int(g[1].hasPrefix("+") ? String(g[1].dropFirst()) : g[1]) else { return nil }
        let month = g[2].isEmpty ? 1 : num(2)
        let day = g[3].isEmpty ? 1 : num(3)
        let micros = g[7].isEmpty ? 0 : Int(String(g[7].prefix(6)).padding(toLength: 6, withPad: "0", startingAt: 0)) ?? 0
        // Months roll into years first, then everything else is plain arithmetic.
        let monthIndex = year * 12 + (month - 1)
        let y = floorDiv(monthIndex, 12)
        let m = monthIndex - y * 12 + 1
        let days = daysFromCivil(y, m, 1) + (day - 1)
        var wall = days * 86_400_000 + num(4) * 3_600_000 + num(5) * 60_000 + num(6) * 1_000 + micros / 1_000
        if g[8].isEmpty {
            let guess = wall - timeZone.secondsFromGMT(for: Date(epochMillis: Int64(wall))) * 1_000
            return wall - timeZone.secondsFromGMT(for: Date(epochMillis: Int64(guess))) * 1_000
        }
        if !g[9].isEmpty {
            let offset = (num(10) * 60 + num(11)) * 60_000
            wall += g[9] == "-" ? offset : -offset
        }
        return wall
    }

    /// Days from 1970-01-01 to a proleptic Gregorian date (Howard Hinnant).
    static func daysFromCivil(_ year: Int, _ month: Int, _ day: Int) -> Int {
        let y = month <= 2 ? year - 1 : year
        let era = floorDiv(y, 400)
        let yoe = y - era * 400
        let mp = (month + 9) % 12
        let doy = (153 * mp + 2) / 5 + day - 1
        let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy
        return era * 146_097 + doe - 719_468
    }

    static func floorDiv(_ a: Int, _ b: Int) -> Int {
        let q = a / b
        return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q
    }

    /// RegExp `\s`: JavaScript white space and line terminators.
    static func isWhitespace(_ c: Unicode.Scalar) -> Bool {
        let v = c.value
        return (0x09...0x0D).contains(v) || v == 0x20 || v == 0xA0 || v == 0x1680
            || (0x2000...0x200A).contains(v) || v == 0x2028 || v == 0x2029 || v == 0x202F
            || v == 0x205F || v == 0x3000 || v == 0xFEFF
    }

    /// `text.length`: UTF-16 code units.
    static func length(_ text: String) -> Int { text.utf16.count }

    /// `text.substring(0, end)` by UTF-16 code units; a split surrogate pair
    /// becomes U+FFFD.
    static func prefix(_ text: String, _ end: Int) -> String {
        String(decoding: Array(text.utf16.prefix(end)), as: UTF16.self)
    }

    /// `a.compareTo(b) < 0`: UTF-16 code unit order.
    static func lessThan(_ a: String, _ b: String) -> Bool {
        a.utf16.lexicographicallyPrecedes(b.utf16)
    }
}

/// Small NSRegularExpression helpers (UTF-16 ranges).
enum DartRegex {
    static func matches(_ regex: NSRegularExpression, _ text: String) -> Bool {
        regex.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)) != nil
    }

    /// Every capture group of the first match ("" for a group that did not take part).
    static func groups(_ regex: NSRegularExpression, _ text: String) -> [String]? {
        guard let match = regex.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)) else { return nil }
        return (0..<match.numberOfRanges).map { group(match, $0, text) }
    }

    static func group(_ match: NSTextCheckingResult, _ index: Int, _ text: String) -> String {
        let range = match.range(at: index)
        guard range.location != NSNotFound, let r = Range(range, in: text) else { return "" }
        return String(text[r])
    }

    static func allMatches(_ regex: NSRegularExpression, _ text: String) -> [NSTextCheckingResult] {
        regex.matches(in: text, range: NSRange(text.startIndex..., in: text))
    }

    static func contains(_ regex: NSRegularExpression, _ text: String) -> Bool { matches(regex, text) }

    /// `text.replaceAll(regex, template)` (template: "" or a literal).
    static func replaceAll(_ regex: NSRegularExpression, _ text: String, with literal: String) -> String {
        regex.stringByReplacingMatches(
            in: text, range: NSRange(text.startIndex..., in: text),
            withTemplate: NSRegularExpression.escapedTemplate(for: literal))
    }

    /// `text.replaceAllMapped(regex, transform)`.
    static func replaceAllMapped(
        _ regex: NSRegularExpression, _ text: String, _ transform: (NSTextCheckingResult) -> String
    ) -> String {
        var out = ""
        var cursor = text.startIndex
        for match in allMatches(regex, text) {
            guard let range = Range(match.range, in: text) else { continue }
            out += text[cursor..<range.lowerBound]
            out += transform(match)
            cursor = range.upperBound
        }
        out += text[cursor...]
        return out
    }
}

/// The models' own scalar readers.
public enum ModelJSON {
    /// `v is int ? v : (v is num ? v.toInt() : int.tryParse('$v') ?? 0)`.
    public static func int(_ value: JSONValue?) -> Int {
        int(value, fallback: 0)
    }

    /// `asInt` with another fallback for an unparsable value (null prints as
    /// "null", so it takes the fallback too).
    public static func int(_ value: JSONValue?, fallback: Int) -> Int {
        switch value {
        case .int(let n)?: return Int(truncatingIfNeeded: n)
        case .double(let d)?: return d.isNaN ? 0 : Int(truncatingIfNeeded: JSONValue.double(d).int64Value ?? 0)
        default: return DartCore.intTryParse(DartCore.toString(value)) ?? fallback
        }
    }

    /// `asOptionalInt` of AgentSession: -1 for null, else [int] with -1 as the fallback.
    public static func optionalInt(_ value: JSONValue?) -> Int {
        guard let value, !value.isNull else { return -1 }
        return int(value, fallback: -1)
    }

    /// `v is num ? v.toDouble() : double.tryParse('$v') ?? 0`.
    public static func double(_ value: JSONValue?) -> Double {
        if let number = value?.doubleValue { return number }
        return DartCore.doubleTryParse(DartCore.toString(value)) ?? 0
    }

    /// `_modelBool`: a real bool, or "true" / "1" / "yes" in any case.
    public static func bool(_ value: JSONValue?) -> Bool {
        if case .bool(let flag)? = value { return flag }
        let text = DartCore.toString(value).dartTrimmed.lowercased()
        return text == "true" || text == "1" || text == "yes"
    }

    /// `_modelString`: `value == null ? '' : '$value'`.
    public static func string(_ value: JSONValue?) -> String { value?.text ?? "" }

    /// `_firstModelString`: the first value non-empty once trimmed, trimmed.
    public static func firstString(_ json: JSONObject, _ keys: [String]) -> String {
        for key in keys {
            let value = string(json[key]).dartTrimmed
            if !value.isEmpty { return value }
        }
        return ""
    }

    /// `a ?? b ?? c`: the first value that is present and not null.
    public static func coalesce(_ values: JSONValue?...) -> JSONValue? {
        for value in values {
            if let value, !value.isNull { return value }
        }
        return nil
    }

    /// `_asEpochMs`: positive seconds below 10^12 are scaled to milliseconds.
    public static func epochMs(_ value: JSONValue?) -> Int {
        let n = int(value)
        if n <= 0 { return 0 }
        if n < 1_000_000_000_000 { return n * 1_000 }
        return n
    }

    /// `(json[key] as List?)?.map((e) => e.toString())`; anything but a list is empty.
    static func stringList(_ value: JSONValue?) -> [String] {
        (value?.arrayValue ?? []).map { DartCore.toString($0) }
    }

    /// `_titleCase`: underscores and dashes become spaces, each word capitalised.
    public static func titleCase(_ value: String) -> String {
        let clean = value.replacingOccurrences(of: "_", with: " ")
            .replacingOccurrences(of: "-", with: " ").dartTrimmed
        if clean.isEmpty { return "" }
        return clean.unicodeScalars.split(whereSeparator: DartCore.isWhitespace).map { part -> String in
            let word = String(String.UnicodeScalarView(part))
            guard let first = word.first else { return word }
            return first.uppercased() + word.dropFirst().lowercased()
        }.joined(separator: " ")
    }

    /// `_truncateText`: at most `maxLength` UTF-16 units, ending in "...".
    public static func truncate(_ value: String, _ maxLength: Int) -> String {
        if maxLength <= 0 || DartCore.length(value) <= maxLength { return value }
        if maxLength <= 3 { return "..." }
        return DartCore.prefix(value, maxLength - 3) + "..."
    }

    /// `_formatCompactDuration`, clamped to ten days: "2d 3h", "1h 2m", "4m 5s", "9s".
    public static func compactDuration(milliseconds: Int) -> String {
        let seconds = min(max(milliseconds / 1_000, 0), 10 * 24 * 60 * 60)
        let days = seconds / 86_400
        let hours = (seconds / 3_600) % 24
        let minutes = (seconds / 60) % 60
        let secs = seconds % 60
        if days > 0 { return "\(days)d \(hours)h" }
        if hours > 0 { return "\(hours)h \(minutes)m" }
        if minutes > 0 { return "\(minutes)m \(secs)s" }
        return "\(secs)s"
    }

    private static let trailingZeros = try! NSRegularExpression(pattern: "\\.?0+$")

    /// `_trimModelDouble`: "1.25", "2", "0.000009", "1e-7".
    public static func trimDouble(_ value: Double) -> String {
        let text = DartCore.doubleToString(value)
        guard text.contains(".") else { return text }
        guard let match = trailingZeros.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)),
              let range = Range(match.range, in: text) else { return text }
        return text.replacingCharacters(in: range, with: "")
    }

    /// `'${d.year}-$mm-$dd'` of `DateTime.fromMillisecondsSinceEpoch(ms)` in `timeZone`.
    public static func localDate(_ epochMs: Int, timeZone: TimeZone = .current) -> String {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = timeZone
        let c = calendar.dateComponents([.year, .month, .day], from: Date(epochMillis: Int64(epochMs)))
        let month = c.month ?? 1
        let day = c.day ?? 1
        return "\(c.year ?? 1970)-\(month < 10 ? "0" : "")\(month)-\(day < 10 ? "0" : "")\(day)"
    }

    /// `status.toLowerCase().replaceAll('_', '-')`.
    static func normalizedStatus(_ status: String) -> String {
        status.lowercased().replacingOccurrences(of: "_", with: "-")
    }
}

/// `'https://explorer.solana.com'`.
public let solanaExplorerBaseUrl = "https://explorer.solana.com"

/// The explorer page of a wallet, or "" for a blank address.
public func solanaExplorerAddressUrl(_ address: String) -> String {
    let clean = address.dartTrimmed
    if clean.isEmpty { return "" }
    return "\(solanaExplorerBaseUrl)/address/\(DartCore.encodeComponent(clean))"
}

/// The explorer page of a transaction, or "" for a blank signature.
public func solanaExplorerSignatureUrl(_ signature: String) -> String {
    let clean = signature.dartTrimmed
    if clean.isEmpty { return "" }
    return "\(solanaExplorerBaseUrl)/tx/\(DartCore.encodeComponent(clean))"
}
