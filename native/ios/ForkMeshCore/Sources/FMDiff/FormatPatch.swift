// Parsing a `git format-patch` series: its commits, MIME-encoded headers and
// the RFC 2822 date.

import Foundation

/// A commit from a `git format-patch --stdout` series.
public struct PatchCommit: Equatable, Sendable, Identifiable {
    public let hash: String
    public let subject: String
    public let author: String
    public let email: String
    public let date: Date?
    public let body: String

    /// This commit's own diff against its parent.
    public let patch: String

    public init(
        hash: String,
        subject: String,
        author: String = "",
        email: String = "",
        date: Date? = nil,
        body: String = "",
        patch: String = ""
    ) {
        self.hash = hash
        self.subject = subject
        self.author = author
        self.email = email
        self.date = date
        self.body = body
        self.patch = patch
    }

    public var id: String { hash }
    public var shortHash: String { hash.count > 7 ? String(hash.prefix(7)) : hash }
}

extension UnifiedDiff {
    /// Splits a format-patch series into commits, oldest first as git wrote them.
    public static func parseFormatPatchSeries(_ series: String) -> [PatchCommit] {
        let units = U16.units(series)
        if U16.isBlank(units) { return [] }
        let lines = U16.splitLines(units)
        var starts: [Int] = []
        for i in lines.indices where mboxHash(lines[i]) != nil { starts.append(i) }
        var commits: [PatchCommit] = []
        for s in starts.indices {
            let end = s + 1 < starts.count ? starts[s + 1] : lines.count
            commits.append(parseMboxMessage(Array(lines[starts[s]..<end])))
        }
        return commits
    }

    private static let mboxFrom = U16.units("From ")
    private static let mboxTail = U16.units(" Mon Sep 17 00:00:00 2001")
    private static let diffGit = U16.units("diff --git ")
    private static let statRule = U16.units("---")
    private static let signatureRule = U16.units("-- ")
    private static let patchTag = U16.units("[PATCH")

    /// `^From ([0-9a-f]{40}|[0-9a-f]{64}) Mon Sep 17 00:00:00 2001$`
    private static func mboxHash(_ line: ArraySlice<UInt16>) -> String? {
        guard U16.hasPrefix(line, mboxFrom) else { return nil }
        let hashLength = line.count - mboxFrom.count - mboxTail.count
        guard hashLength == 40 || hashLength == 64 else { return nil }
        let begin = line.startIndex + mboxFrom.count
        let hash = line[begin..<(begin + hashLength)]
        guard hash.allSatisfy({ U16.isDigit($0) || ($0 >= 0x61 && $0 <= 0x66) }),
              U16.hasPrefix(line, mboxTail, at: mboxFrom.count + hashLength) else { return nil }
        return U16.string(hash)
    }

    private static func parseMboxMessage(_ lines: [ArraySlice<UInt16>]) -> PatchCommit {
        let hash = mboxHash(lines[0]) ?? ""
        var headers: [String: String] = [:]
        var i = 1
        var lastKey: String?
        while i < lines.count {
            let line = lines[i]
            if line.isEmpty {
                i += 1
                break
            }
            if let key = lastKey, line.first == U16.space || line.first == U16.tab {
                headers[key] = "\(headers[key] ?? "") \(U16.string(U16.trim(line)))"
                i += 1
                continue
            }
            if let colon = line.firstIndex(of: U16.colon), colon > line.startIndex {
                let key = U16.string(line[line.startIndex..<colon]).lowercased()
                lastKey = key
                headers[key] = U16.string(U16.trim(line[(colon + 1)...]))
            }
            i += 1
        }

        var body: [ArraySlice<UInt16>] = []
        var diffStart = -1
        var inStat = false
        while i < lines.count {
            let line = lines[i]
            if U16.hasPrefix(line, diffGit) {
                diffStart = i
                break
            }
            if line.elementsEqual(statRule) {
                inStat = true
            } else if !inStat {
                body.append(line)
            }
            i += 1
        }
        var patchLines = diffStart < 0 ? [] : Array(lines[diffStart...])
        // An mbox message ends with a blank separator line; a signature, when
        // git was not told to omit it, starts at "-- ".
        if let signature = patchLines.lastIndex(where: { $0.elementsEqual(signatureRule) }),
           signature >= patchLines.count - 3 {
            patchLines = Array(patchLines[..<signature])
        }

        let from = decodeMimeHeader(headers["from"] ?? "")
        var author = from
        var email = ""
        if let parts = splitAddress(U16.units(from)) {
            author = parts.name
            email = parts.email
        }
        var subject = U16.units(decodeMimeHeader(headers["subject"] ?? ""))[...]
        subject = U16.trim(dropPatchTag(subject))
        return PatchCommit(
            hash: hash,
            subject: subject.isEmpty ? "(no subject)" : U16.string(subject),
            author: author.isEmpty ? email : author,
            email: email,
            date: parseRfc2822(headers["date"] ?? ""),
            body: U16.string(U16.trim(join(body)[...])),
            patch: U16.string(join(patchLines))
        )
    }

    private static func join(_ lines: [ArraySlice<UInt16>]) -> Units {
        var out = Units()
        for (index, line) in lines.enumerated() {
            if index > 0 { out.append(U16.lf) }
            out.append(contentsOf: line)
        }
        return out
    }

    /// `^\[PATCH[^\]]*\]\s*`, removed once from the front.
    private static func dropPatchTag(_ subject: ArraySlice<UInt16>) -> ArraySlice<UInt16> {
        guard U16.hasPrefix(subject, patchTag),
              let close = subject.firstIndex(of: 0x5D) else { return subject }
        var i = close + 1
        while i < subject.endIndex, U16.isRegexSpace(subject[i]) { i += 1 }
        return subject[i...]
    }

    /// `^(.*?)\s*<([^>]*)>\s*$`: the display name and the address in the
    /// last `<…>` that closes the header.
    private static func splitAddress(_ from: Units) -> (name: String, email: String)? {
        if from.contains(where: U16.isLineTerminator) { return nil }
        var open = U16.indexOf(from, unit: U16.less, from: 0)
        while open >= 0 {
            let close = U16.indexOf(from, unit: U16.greater, from: open + 1)
            if close < 0 { return nil }
            if from[(close + 1)...].allSatisfy(U16.isRegexSpace) {
                var nameEnd = open
                while nameEnd > 0, U16.isRegexSpace(from[nameEnd - 1]) { nameEnd -= 1 }
                let name = from[..<nameEnd].filter { $0 != U16.quote }
                return (
                    U16.string(U16.trim(name[...])),
                    U16.string(U16.trim(from[(open + 1)..<close]))
                )
            }
            open = U16.indexOf(from, unit: U16.less, from: open + 1)
        }
        return nil
    }

    // MARK: - RFC 2047

    /// Undoes RFC 2047 encoded words, which format-patch uses for any
    /// non-ASCII author name or subject.
    public static func decodeMimeHeader(_ value: String) -> String {
        guard value.contains("=?") else { return value }
        let joined = joinEncodedWords(U16.units(value))
        var out = Units()
        var i = 0
        while i < joined.count {
            if let word = encodedWord(joined, at: i) {
                out.append(contentsOf: word.decoded ?? Units(joined[i..<word.end]))
                i = word.end
            } else {
                out.append(joined[i])
                i += 1
            }
        }
        return U16.string(out)
    }

    /// Whitespace between two adjacent encoded words is not part of the text:
    /// `\?=\s+=\?` becomes `?==?`.
    private static func joinEncodedWords(_ value: Units) -> Units {
        var out = Units()
        out.reserveCapacity(value.count)
        var i = 0
        while i < value.count {
            if value[i] == U16.question, i + 1 < value.count, value[i + 1] == U16.equals {
                var j = i + 2
                while j < value.count, U16.isRegexSpace(value[j]) { j += 1 }
                if j > i + 2, j + 1 < value.count, value[j] == U16.equals, value[j + 1] == U16.question {
                    out.append(contentsOf: [U16.question, U16.equals, U16.equals, U16.question])
                    i = j + 2
                    continue
                }
            }
            out.append(value[i])
            i += 1
        }
        return out
    }

    /// `=\?([^?]+)\?([bBqQ])\?([^?]*)\?=` anchored at `start`. `decoded` is
    /// nil when the payload is not valid, and the word is then left as it is.
    private static func encodedWord(_ text: Units, at start: Int) -> (end: Int, decoded: Units?)? {
        let count = text.count
        guard start + 1 < count, text[start] == U16.equals, text[start + 1] == U16.question else { return nil }
        var i = start + 2
        let charsetStart = i
        while i < count, text[i] != U16.question { i += 1 }
        guard i > charsetStart, i + 2 < count else { return nil }
        let encoding = text[i + 1] | 0x20
        guard encoding == 0x62 || encoding == 0x71, text[i + 2] == U16.question else { return nil }
        let payloadStart = i + 3
        i = payloadStart
        while i < count, text[i] != U16.question { i += 1 }
        guard i + 1 < count, text[i + 1] == U16.equals else { return nil }
        let payload = text[payloadStart..<i]
        let bytes = encoding == 0x62 ? base64Bytes(payload) : quotedPrintableBytes(payload)
        return (i + 2, bytes.map { U16.units(String(decoding: $0, as: UTF8.self)) })
    }

    private static func base64Bytes(_ payload: ArraySlice<UInt16>) -> [UInt8]? {
        // The URL-safe alphabet is accepted as well.
        let text = U16.string(payload.map { unit -> UInt16 in
            unit == U16.minus ? U16.plus : (unit == U16.underscore ? U16.slash : unit)
        })
        guard let data = Data(base64Encoded: text) else { return nil }
        return [UInt8](data)
    }

    private static func quotedPrintableBytes(_ payload: ArraySlice<UInt16>) -> [UInt8]? {
        var bytes: [UInt8] = []
        var literal = Units()
        func flushLiteral() {
            guard !literal.isEmpty else { return }
            bytes.append(contentsOf: Array(U16.string(literal).utf8))
            literal.removeAll(keepingCapacity: true)
        }
        var i = payload.startIndex
        let end = payload.endIndex
        while i < end {
            let unit = payload[i]
            if unit == U16.underscore {
                flushLiteral()
                bytes.append(0x20)
            } else if unit == U16.equals, i + 2 < end {
                guard let high = hexValue(payload[i + 1]), let low = hexValue(payload[i + 2]) else { return nil }
                flushLiteral()
                bytes.append(UInt8(high * 16 + low))
                i += 2
            } else {
                literal.append(unit)
            }
            i += 1
        }
        flushLiteral()
        return bytes
    }

    private static func hexValue(_ unit: UInt16) -> Int? {
        switch unit {
        case 0x30...0x39: return Int(unit) - 0x30
        case 0x41...0x46: return Int(unit) - 0x41 + 10
        case 0x61...0x66: return Int(unit) - 0x61 + 10
        default: return nil
        }
    }

    // MARK: - RFC 2822 date

    private static let months: [String: Int] = [
        "jan": 1, "feb": 2, "mar": 3, "apr": 4, "may": 5, "jun": 6,
        "jul": 7, "aug": 8, "sep": 9, "oct": 10, "nov": 11, "dec": 12,
    ]

    /// `Thu, 18 Sep 2026 10:04:05 +0200` → UTC instant.
    ///
    /// `(\d{1,2})\s+([A-Za-z]{3})\s+(\d{4})\s+(\d{2}):(\d{2})(?::(\d{2}))?\s*([+-]\d{4})?`,
    /// searched for anywhere in the value.
    static func parseRfc2822(_ value: String) -> Date? {
        let text = U16.units(value)
        for start in text.indices where U16.isDigit(text[start]) {
            for dayDigits in [2, 1] {
                if let fields = dateFields(text, at: start, dayDigits: dayDigits) {
                    return instant(fields)
                }
            }
        }
        return nil
    }

    private struct DateFields {
        var day = 0, month = "", year = 0, hour = 0, minute = 0, second = 0
        var zone: (sign: Int, hours: Int, minutes: Int)?
    }

    private static func dateFields(_ text: Units, at start: Int, dayDigits: Int) -> DateFields? {
        var i = start
        let count = text.count
        func digits(_ n: Int) -> Int? {
            guard i + n <= count, text[i..<(i + n)].allSatisfy(U16.isDigit) else { return nil }
            defer { i += n }
            return U16.parseInt(text[i..<(i + n)])
        }
        func spaces() -> Bool {
            let begin = i
            while i < count, U16.isRegexSpace(text[i]) { i += 1 }
            return i > begin
        }
        var fields = DateFields()
        guard let day = digits(dayDigits), spaces() else { return nil }
        fields.day = day
        guard i + 3 <= count, text[i..<(i + 3)].allSatisfy(U16.isAlpha) else { return nil }
        fields.month = U16.string(text[i..<(i + 3)]).lowercased()
        i += 3
        guard spaces(), let year = digits(4), spaces(),
              let hour = digits(2), i < count, text[i] == U16.colon else { return nil }
        i += 1
        guard let minute = digits(2) else { return nil }
        fields.year = year
        fields.hour = hour
        fields.minute = minute
        if i < count, text[i] == U16.colon {
            let mark = i
            i += 1
            if let second = digits(2) { fields.second = second } else { i = mark }
        }
        _ = spaces()
        if i + 5 <= count, text[i] == U16.plus || text[i] == U16.minus {
            let sign = text[i] == U16.minus ? -1 : 1
            i += 1
            if let hours = digits(2), let minutes = digits(2) {
                fields.zone = (sign, hours, minutes)
            }
        }
        return fields
    }

    private static func instant(_ fields: DateFields) -> Date? {
        guard let month = months[fields.month] else { return nil }
        // Out-of-range days and hours roll over, as `DateTime.utc` does.
        let days = daysFromCivil(year: fields.year, month: month, day: 1) + fields.day - 1
        var seconds = days * 86400 + fields.hour * 3600 + fields.minute * 60 + fields.second
        if let zone = fields.zone {
            seconds -= zone.sign * (zone.hours * 3600 + zone.minutes * 60)
        }
        return Date(timeIntervalSince1970: TimeInterval(seconds))
    }

    /// Days since 1970-01-01 in the proleptic Gregorian calendar.
    private static func daysFromCivil(year: Int, month: Int, day: Int) -> Int {
        let y = month <= 2 ? year - 1 : year
        let era = (y >= 0 ? y : y - 399) / 400
        let yearOfEra = y - era * 400
        let shifted = (month + 9) % 12
        let dayOfYear = (153 * shifted + 2) / 5 + day - 1
        let dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear
        return era * 146097 + dayOfEra - 719468
    }
}
