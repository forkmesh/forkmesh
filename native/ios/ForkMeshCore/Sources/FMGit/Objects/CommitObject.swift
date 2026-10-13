import Foundation

/// A parsed `author`, `committer`, or `tagger` identity line:
/// `Name <email> <seconds> <+HHMM>`.
public struct GitCommitIdentity: Equatable, Sendable {
    public let name: String
    public let email: String
    public let timestampSeconds: Int
    public let timezoneOffsetMinutes: Int

    public init(name: String, email: String, timestampSeconds: Int, timezoneOffsetMinutes: Int) {
        self.name = name
        self.email = email
        self.timestampSeconds = timestampSeconds
        self.timezoneOffsetMinutes = timezoneOffsetMinutes
    }

    /// The identity timestamp as a UTC instant.
    public var timeUtc: Date { Date(timeIntervalSince1970: TimeInterval(timestampSeconds)) }

    /// Parses a Git identity value, returning nil when it does not follow
    /// the canonical `name <email> seconds offset` shape.
    ///
    /// Hand-rolled equivalent of the pattern
    /// `^(.*?)\s*<([^<>\n]*)>\s(\d{1,19})\s([+-]\d{4})$`, read from the end
    /// (the tail is fixed-shape, so the match is unique).
    public static func tryParse(_ value: String) -> GitCommitIdentity? {
        let scalars = Array(value.unicodeScalars)
        func digit(_ scalar: Unicode.Scalar) -> Bool { scalar.value >= 0x30 && scalar.value <= 0x39 }
        var end = scalars.count
        // [+-]\d{4}
        if end < 5 { return nil }
        let offsetScalars = Array(scalars[(end - 5)..<end])
        if offsetScalars[0] != "+" && offsetScalars[0] != "-" { return nil }
        for scalar in offsetScalars[1...] where !digit(scalar) { return nil }
        end -= 5
        // \s
        if end < 1 || !isRegexWhitespace(scalars[end - 1]) { return nil }
        end -= 1
        // \d{1,19}; the scalar before the run must be whitespace, so the run is maximal.
        var digitsStart = end
        while digitsStart > 0, digit(scalars[digitsStart - 1]) { digitsStart -= 1 }
        let digitCount = end - digitsStart
        if digitCount < 1 || digitCount > 19 { return nil }
        var secondsText = String.UnicodeScalarView()
        secondsText.append(contentsOf: scalars[digitsStart..<end])
        end = digitsStart
        if end < 1 || !isRegexWhitespace(scalars[end - 1]) { return nil }
        end -= 1
        // <([^<>\n]*)>
        if end < 1 || scalars[end - 1] != ">" { return nil }
        let emailEnd = end - 1
        var emailStart = emailEnd
        while true {
            if emailStart == 0 { return nil }
            let scalar = scalars[emailStart - 1]
            if scalar == "<" { break }
            if scalar == ">" || scalar == "\n" { return nil }
            emailStart -= 1
        }
        var emailText = String.UnicodeScalarView()
        emailText.append(contentsOf: scalars[emailStart..<emailEnd])
        // ^(.*?)\s*
        var nameEnd = emailStart - 1
        while nameEnd > 0, isRegexWhitespace(scalars[nameEnd - 1]) { nameEnd -= 1 }
        for scalar in scalars[0..<nameEnd] where isRegexLineTerminator(scalar) { return nil }
        var nameText = String.UnicodeScalarView()
        nameText.append(contentsOf: scalars[0..<nameEnd])

        guard let seconds = Int(String(secondsText)) else { return nil }
        let offsetHours = Int(offsetScalars[1].value - 0x30) * 10 + Int(offsetScalars[2].value - 0x30)
        let offsetMinutes = Int(offsetScalars[3].value - 0x30) * 10 + Int(offsetScalars[4].value - 0x30)
        if offsetMinutes > 59 { return nil }
        let magnitude = offsetHours * 60 + offsetMinutes
        return GitCommitIdentity(
            name: String(nameText).dartTrim(),
            email: String(emailText),
            timestampSeconds: seconds,
            timezoneOffsetMinutes: offsetScalars[0] == "-" ? -magnitude : magnitude
        )
    }

    /// Parses a Git identity value or throws a typed `invalidObject` error.
    public static func parse(_ value: String) throws -> GitCommitIdentity {
        guard let identity = tryParse(value) else {
            throw GitException(.invalidObject, "commit identity header is malformed")
        }
        return identity
    }
}

/// `\s` of an ECMAScript regular expression.
func isRegexWhitespace(_ scalar: Unicode.Scalar) -> Bool {
    scalar.value != 0x85 && isDartWhitespace(scalar)
}

/// What `.` does not match in an ECMAScript regular expression.
func isRegexLineTerminator(_ scalar: Unicode.Scalar) -> Bool {
    switch scalar.value {
    case 0x0a, 0x0d, 0x2028, 0x2029: return true
    default: return false
    }
}

public struct GitCommit: Sendable {
    public let tree: GitObjectId
    public let parents: [GitObjectId]
    public let headers: [String: [String]]
    public let message: [UInt8]

    /// Typed author identity, or nil when the header does not follow the
    /// canonical shape (foreign history is readable either way).
    public var author: GitCommitIdentity? {
        headers["author"].flatMap { $0.first }.flatMap(GitCommitIdentity.tryParse)
    }

    /// Typed committer identity; see `author`.
    public var committer: GitCommitIdentity? {
        headers["committer"].flatMap { $0.first }.flatMap(GitCommitIdentity.tryParse)
    }

    /// The message decoded leniently, for display.
    public var messageText: String { String(decoding: message, as: UTF8.self) }

    public static func parse(_ body: [UInt8]) throws -> GitCommit {
        let separator = findHeaderBodySeparator(body)
        let headerBytes = separator == -1 ? body : Array(body[0..<separator])
        let message = separator == -1 ? [UInt8]() : Array(body[(separator + 2)...])
        let headerText = try decodeUtf8Headers(headerBytes)
        var headers = [String: [String]]()
        var previousKey: String?
        for line in headerText.u8Split(0x0a) {
            if line.u8HasPrefix(" ") {
                guard let key = previousKey else {
                    throw GitException(.invalidObject, "orphan header continuation")
                }
                var values = headers[key]!
                values[values.count - 1] += "\n" + line.u8Substring(1)
                headers[key] = values
                continue
            }
            let separatorIndex = line.u8IndexOf(" ")
            if separatorIndex <= 0 {
                throw GitException(.invalidObject, "commit header is invalid")
            }
            let key = line.u8Substring(0, separatorIndex)
            let value = line.u8Substring(separatorIndex + 1)
            headers[key, default: []].append(value)
            previousKey = key
        }
        guard let treeValues = headers["tree"], treeValues.count == 1 else {
            throw GitException(.invalidObject, "commit must have one tree")
        }
        if headers["author"]?.count != 1 || headers["committer"]?.count != 1 {
            throw GitException(.invalidObject, "commit identity headers are invalid")
        }
        let parents = try (headers["parent"] ?? []).map { try GitObjectId.parseSha1($0) }
        return GitCommit(
            tree: try GitObjectId.parseSha1(treeValues[0]),
            parents: parents,
            headers: headers,
            message: message
        )
    }
}
