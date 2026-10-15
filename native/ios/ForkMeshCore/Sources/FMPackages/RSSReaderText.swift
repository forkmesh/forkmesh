import Foundation

public enum RSSText {
    static func isSpace(_ scalar: Unicode.Scalar) -> Bool {
        if (0x1C...0x1F).contains(scalar.value) { return true }
        return scalar.properties.isWhitespace
    }

    public static func prefix(_ text: String, _ limit: Int) -> String {
        guard limit >= 0 else { return "" }
        var view = String.UnicodeScalarView()
        var count = 0
        for scalar in text.unicodeScalars {
            if count >= limit { break }
            view.append(scalar)
            count += 1
        }
        return String(view)
    }

    public static func length(_ text: String) -> Int { text.unicodeScalars.count }

    public static func clean(_ value: String, _ limit: Int) -> String {
        var words: [String] = []
        var current = String.UnicodeScalarView()
        for scalar in value.unicodeScalars {
            if scalar.value == 0 { continue }
            if isSpace(scalar) {
                if !current.isEmpty {
                    words.append(String(current))
                    current = String.UnicodeScalarView()
                }
            } else {
                current.append(scalar)
            }
        }
        if !current.isEmpty { words.append(String(current)) }
        return prefix(words.joined(separator: " "), limit)
    }

    public static func strip(_ value: String) -> String {
        let scalars = Array(value.unicodeScalars)
        var start = 0
        var end = scalars.count
        while start < end && isSpace(scalars[start]) { start += 1 }
        while end > start && isSpace(scalars[end - 1]) { end -= 1 }
        var view = String.UnicodeScalarView()
        view.append(contentsOf: scalars[start..<end])
        return String(view)
    }

    public static func strip(_ value: String, characters: Set<Character>) -> String {
        var chars = Substring(value)
        while let first = chars.first, characters.contains(first) { chars.removeFirst() }
        while let last = chars.last, characters.contains(last) { chars.removeLast() }
        return String(chars)
    }

    public static func escape(_ text: String, quote: Bool) -> String {
        var out = ""
        out.reserveCapacity(text.utf8.count)
        for character in text {
            switch character {
            case "&": out += "&amp;"
            case "<": out += "&lt;"
            case ">": out += "&gt;"
            case "\"" where quote: out += "&quot;"
            case "'" where quote: out += "&#x27;"
            default: out.append(character)
            }
        }
        return out
    }

    public static func letter(_ title: String) -> String {
        for character in title where character.isLetter || character.isNumber {
            return String(character).uppercased()
        }
        return "#"
    }

    public static func fnv(_ text: String) -> String {
        var hash: UInt64 = 0xcbf2_9ce4_8422_2325
        for byte in text.utf8 {
            hash ^= UInt64(byte)
            hash = hash &* 0x0000_0100_0000_01B3
        }
        let hex = String(hash, radix: 16)
        return String(repeating: "0", count: max(0, 16 - hex.count)) + hex
    }
}

public struct RSSURLParts: Equatable, Sendable {
    public var scheme: String
    public var authority: String
    public var path: String
    public var query: String?
    public var fragment: String?

    public var userinfo: String? {
        guard let at = authority.lastIndex(of: "@") else { return nil }
        return String(authority[..<at])
    }

    public var hostPort: String {
        guard let at = authority.lastIndex(of: "@") else { return authority }
        return String(authority[authority.index(after: at)...])
    }

    public var host: String {
        let value = hostPort
        if value.hasPrefix("[") {
            guard let close = value.firstIndex(of: "]") else { return "" }
            return String(value[value.index(after: value.startIndex)..<close]).lowercased()
        }
        if let colon = value.lastIndex(of: ":") { return String(value[..<colon]).lowercased() }
        return value.lowercased()
    }

    public var portText: String? {
        let value = hostPort
        if value.hasPrefix("[") {
            guard let close = value.firstIndex(of: "]") else { return nil }
            let rest = value[value.index(after: close)...]
            guard rest.hasPrefix(":") else { return nil }
            return String(rest.dropFirst())
        }
        guard let colon = value.lastIndex(of: ":") else { return nil }
        return String(value[value.index(after: colon)...])
    }

    public var port: Int?? {
        guard let text = portText else { return .some(nil) }
        if text.isEmpty { return .some(nil) }
        guard text.allSatisfy({ $0.isASCII && $0.isNumber }), let number = Int(text), number <= 65535 else { return nil }
        return .some(number)
    }

    public var hasAuthority: Bool

    public init(scheme: String, authority: String, path: String, query: String?, fragment: String?, hasAuthority: Bool) {
        self.scheme = scheme
        self.authority = authority
        self.path = path
        self.query = query
        self.fragment = fragment
        self.hasAuthority = hasAuthority
    }

    public var string: String {
        var out = ""
        if !scheme.isEmpty { out += scheme + ":" }
        if hasAuthority { out += "//" + authority }
        out += path
        if let query { out += "?" + query }
        if let fragment { out += "#" + fragment }
        return out
    }
}

public enum RSSURL {
    public static func schemeOf(_ text: String) -> String? {
        guard let colon = text.firstIndex(of: ":") else { return nil }
        let scheme = text[..<colon]
        guard let first = scheme.first, first.isASCII, first.isLetter else { return nil }
        for character in scheme where !(character.isASCII && (character.isLetter || character.isNumber || "+-.".contains(character))) {
            return nil
        }
        return String(scheme)
    }

    public static func split(_ text: String) -> RSSURLParts {
        var rest = Substring(text)
        var fragment: String?
        if let hash = rest.firstIndex(of: "#") {
            fragment = String(rest[rest.index(after: hash)...])
            rest = rest[..<hash]
        }
        var query: String?
        if let mark = rest.firstIndex(of: "?") {
            query = String(rest[rest.index(after: mark)...])
            rest = rest[..<mark]
        }
        var scheme = ""
        if let found = schemeOf(String(rest)) {
            scheme = found
            rest = rest.dropFirst(found.count + 1)
        }
        var authority = ""
        var hasAuthority = false
        if rest.hasPrefix("//") {
            hasAuthority = true
            rest = rest.dropFirst(2)
            if let slash = rest.firstIndex(of: "/") {
                authority = String(rest[..<slash])
                rest = rest[slash...]
            } else {
                authority = String(rest)
                rest = ""
            }
        }
        return RSSURLParts(
            scheme: scheme, authority: authority, path: String(rest), query: query, fragment: fragment,
            hasAuthority: hasAuthority)
    }

    static func removeDotSegments(_ path: String) -> String {
        guard path.contains(".") else { return path }
        let absolute = path.hasPrefix("/")
        var segments = path.split(separator: "/", omittingEmptySubsequences: false).map(String.init)
        if absolute { segments.removeFirst() }
        var output: [String] = []
        let last = segments.last
        for segment in segments {
            if segment == ".." {
                if !output.isEmpty { output.removeLast() }
            } else if segment != "." {
                output.append(segment)
            }
        }
        if last == "." || last == ".." { output.append("") }
        return (absolute ? "/" : "") + output.joined(separator: "/")
    }

    public static func resolve(_ reference: String, against base: String) -> String {
        if base.isEmpty { return reference }
        if reference.isEmpty { return base }
        let baseParts = split(base)
        if let scheme = schemeOf(reference) {
            guard scheme.lowercased() == baseParts.scheme.lowercased() else { return reference }
            var parts = split(reference)
            parts.scheme = scheme.lowercased()
            if parts.hasAuthority { return parts.string }
            let tail = String(reference.dropFirst(scheme.count + 1))
            return resolve(tail, against: base)
        }
        var target = split(reference)
        if target.hasAuthority {
            target.scheme = baseParts.scheme
            target.path = removeDotSegments(target.path)
            return target.string
        }
        target.scheme = baseParts.scheme
        target.authority = baseParts.authority
        target.hasAuthority = baseParts.hasAuthority
        if target.path.isEmpty {
            target.path = baseParts.path
            if target.query == nil { target.query = baseParts.query }
        } else if target.path.hasPrefix("/") {
            target.path = removeDotSegments(target.path)
        } else {
            var directory = baseParts.path
            if let slash = directory.lastIndex(of: "/") {
                directory = String(directory[...slash])
            } else {
                directory = baseParts.hasAuthority ? "/" : ""
            }
            target.path = removeDotSegments(directory + target.path)
        }
        return target.string
    }

    public static func http(_ value: String, base: String = "") -> String {
        var text = RSSText.strip(value)
        if text.isEmpty || RSSText.length(text) > 2048 { return "" }
        if !base.isEmpty { text = resolve(text, against: base) }
        let parts = split(text)
        let scheme = parts.scheme.lowercased()
        guard scheme == "http" || scheme == "https", parts.hasAuthority else { return "" }
        guard case .some = parts.port else { return "" }
        if parts.host.isEmpty { return "" }
        if let userinfo = parts.userinfo {
            let pieces = userinfo.split(separator: ":", maxSplits: 1, omittingEmptySubsequences: false)
            if pieces.contains(where: { !$0.isEmpty }) { return "" }
        }
        return text
    }

    public static func link(_ value: String, base: String = "") -> String {
        let trimmed = RSSText.strip(value)
        if trimmed.lowercased().hasPrefix("mailto:") { return trimmed }
        return http(trimmed, base: base)
    }

    public static func normalizeFeedInput(_ value: String) -> String {
        var text = RSSText.strip(value)
        if text.isEmpty { return "" }
        let lowered = text.lowercased()
        if lowered.hasPrefix("feed://") {
            text = "https://" + text.dropFirst(7)
        } else if lowered.hasPrefix("feed:") {
            text = String(text.dropFirst(5))
        } else if lowered.hasPrefix("//") {
            text = "https:" + text
        }
        if !text.contains("://") { text = "https://" + text }
        return http(text)
    }

    public static func identity(_ url: String) -> String {
        let parts = split(url)
        let host = parts.host
        var port = ""
        if case .some(.some(let number)) = parts.port, number != 0, number != 80, number != 443 {
            port = ":\(number)"
        }
        var path = parts.path
        if let lastSlash = path.lastIndex(of: "/") {
            let segment = path[path.index(after: lastSlash)...]
            if let semicolon = segment.firstIndex(of: ";") { path = String(path[..<semicolon]) }
        } else if let semicolon = path.firstIndex(of: ";") {
            path = String(path[..<semicolon])
        }
        if path.isEmpty { path = "/" }
        let query = (parts.query ?? "").isEmpty ? "" : "?" + (parts.query ?? "")
        return host + port + path + query
    }

    public static func favicon(_ site: String) -> String {
        let parts = split(site)
        let scheme = parts.scheme.lowercased()
        guard scheme == "http" || scheme == "https", !parts.host.isEmpty else { return "" }
        return "\(scheme)://\(parts.authority)/favicon.ico"
    }

    public static func origin(_ url: String) -> String {
        let parts = split(url)
        return "\(parts.scheme.lowercased())://\(parts.authority)"
    }

    public static func isInsecure(_ url: String) -> Bool { split(url).scheme.lowercased() == "http" }

    public static func upgraded(_ url: String) -> String {
        guard isInsecure(url) else { return url }
        var parts = split(url)
        parts.scheme = "https"
        if case .some(.some(80)) = parts.port {
            parts.authority = String(parts.authority.dropLast(3))
        }
        return parts.string
    }

    public static func host(_ url: String) -> String { split(url).host }
}
