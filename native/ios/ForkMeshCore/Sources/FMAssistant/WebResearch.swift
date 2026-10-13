import Foundation

public struct WebResult: Equatable, Sendable {
    public var title: String
    public var url: String
    public var snippet: String

    public init(title: String, url: String, snippet: String = "") {
        self.title = title
        self.url = url
        self.snippet = snippet
    }
}

public enum WebAddress {
    static let blockedSuffixes = [".local", ".localhost", ".internal", ".lan", ".home.arpa", ".intranet", ".corp"]

    public static func refusal(_ url: URL) -> String? {
        guard let scheme = url.scheme?.lowercased(), scheme == "https" || scheme == "http" else {
            return "only http and https pages can be read"
        }
        guard let rawHost = url.host?.lowercased(), !rawHost.isEmpty else { return "the address has no host" }
        let host = rawHost.trimmingCharacters(in: CharacterSet(charactersIn: "[]."))
        if url.user != nil || url.password != nil { return "addresses with credentials are not read" }
        if host == "localhost" || blockedSuffixes.contains(where: { host.hasSuffix($0) }) {
            return "local network addresses are not read"
        }
        if let octets = ipv4(host) {
            return isPublic(octets) ? nil : "private and local IP addresses are not read"
        }
        if host.contains(":") {
            return isPublicIPv6(host) ? nil : "private and local IP addresses are not read"
        }
        if !host.contains(".") { return "single-word host names (local network) are not read" }
        return nil
    }

    static func ipv4(_ host: String) -> [Int]? {
        let parts = host.split(separator: ".", omittingEmptySubsequences: false)
        guard parts.count == 4 else { return nil }
        let octets = parts.compactMap { Int($0) }
        guard octets.count == 4, octets.allSatisfy({ (0...255).contains($0) }) else { return nil }
        return octets
    }

    static func isPublic(_ o: [Int]) -> Bool {
        switch (o[0], o[1]) {
        case (0, _), (10, _), (127, _), (169, 254), (192, 168), (255, _): return false
        case (172, 16...31), (100, 64...127), (198, 18...19): return false
        case (192, 0) where o[2] == 0 || o[2] == 2: return false
        case (224...239, _), (240...254, _): return false
        default: return true
        }
    }

    static func isPublicIPv6(_ host: String) -> Bool {
        if host == "::" || host == "::1" { return false }
        if host.hasPrefix("fc") || host.hasPrefix("fd") || host.hasPrefix("fe8") || host.hasPrefix("fe9")
            || host.hasPrefix("fea") || host.hasPrefix("feb") || host.hasPrefix("ff") {
            return false
        }
        if host.hasPrefix("::ffff:") {
            let mapped = String(host.dropFirst("::ffff:".count))
            if let octets = ipv4(mapped) { return isPublic(octets) }
            return false
        }
        return true
    }
}

public enum WebText {
    public static let maxHTMLChars = 1_500_000

    public static func title(fromHTML html: String) -> String {
        guard let raw = firstMatch("<title[^>]*>(.*?)</title>", in: html) else { return "" }
        return collapse(decodeEntities(stripTags(raw)))
    }

    public static func text(fromHTML html: String) -> String {
        var s = String(html.prefix(maxHTMLChars))
        for tag in ["script", "style", "noscript", "svg", "template", "head", "iframe"] {
            s = replace("<\(tag)\\b[^>]*>.*?</\(tag)\\s*>", in: s, with: " ")
        }
        s = replace("<!--.*?-->", in: s, with: " ")
        s = replace("<li\\b[^>]*>", in: s, with: "\n- ")
        s = replace("<(br|hr)\\b[^>]*>", in: s, with: "\n")
        s = replace("</(p|div|section|article|header|footer|li|ul|ol|h[1-6]|tr|table|blockquote|pre|dt|dd)\\s*>", in: s, with: "\n")
        s = replace("</t[dh]\\s*>", in: s, with: " | ")
        s = decodeEntities(stripTags(s))
        let lines = s.components(separatedBy: "\n").map(collapse).filter { !$0.isEmpty && $0 != "|" && $0 != "-" }
        return lines.joined(separator: "\n")
    }

    public static func stripTags(_ html: String) -> String {
        replace("<[^>]*>", in: html, with: "")
    }

    public static func collapse(_ text: String) -> String {
        replace("[ \\t\\r\\u{00A0}]+", in: text, with: " ").trimmingCharacters(in: .whitespaces)
    }

    public static func decodeEntities(_ text: String) -> String {
        guard text.contains("&") else { return text }
        let named = ["amp": "&", "lt": "<", "gt": ">", "quot": "\"", "apos": "'", "nbsp": " ", "#39": "'", "mdash": "—", "ndash": "–", "hellip": "…", "rsquo": "’", "lsquo": "‘", "rdquo": "”", "ldquo": "“", "copy": "©", "middot": "·"]
        var out = ""
        var index = text.startIndex
        while index < text.endIndex {
            let char = text[index]
            if char == "&", let semi = text[index...].prefix(12).firstIndex(of: ";") {
                let name = String(text[text.index(after: index)..<semi])
                var decoded: String?
                if let value = named[name] {
                    decoded = value
                } else if name.hasPrefix("#x") || name.hasPrefix("#X"), let code = UInt32(name.dropFirst(2), radix: 16), let scalar = Unicode.Scalar(code) {
                    decoded = String(Character(scalar))
                } else if name.hasPrefix("#"), let code = UInt32(name.dropFirst()), let scalar = Unicode.Scalar(code) {
                    decoded = String(Character(scalar))
                }
                if let decoded {
                    out += decoded
                    index = text.index(after: semi)
                    continue
                }
            }
            out.append(char)
            index = text.index(after: index)
        }
        return out
    }

    static func regex(_ pattern: String) -> NSRegularExpression? {
        try? NSRegularExpression(pattern: pattern, options: [.caseInsensitive, .dotMatchesLineSeparators])
    }

    static func replace(_ pattern: String, in text: String, with template: String) -> String {
        guard let regex = regex(pattern) else { return text }
        let range = NSRange(text.startIndex..., in: text)
        return regex.stringByReplacingMatches(in: text, range: range, withTemplate: NSRegularExpression.escapedTemplate(for: template))
    }

    static func firstMatch(_ pattern: String, in text: String) -> String? {
        guard let regex = regex(pattern),
              let match = regex.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)),
              match.numberOfRanges > 1, let range = Range(match.range(at: 1), in: text)
        else { return nil }
        return String(text[range])
    }
}

public enum DuckDuckGo {
    public static func searchURL(_ query: String) -> URL? {
        var components = URLComponents(string: "https://html.duckduckgo.com/html/")
        components?.queryItems = [URLQueryItem(name: "q", value: query)]
        return components?.url
    }

    public static func isChallenge(status: Int, html: String) -> Bool {
        status == 202 || status == 403 || html.contains("anomaly-modal") || html.contains("challenge-form")
    }

    public static func results(_ html: String, limit: Int = 8) -> [WebResult] {
        guard let anchor = WebText.regex("<a\\b([^>]*)>(.*?)</a>") else { return [] }
        let range = NSRange(html.startIndex..., in: html)
        var results: [WebResult] = []
        var skipping = false
        for match in anchor.matches(in: html, range: range) {
            guard let attrsRange = Range(match.range(at: 1), in: html), let bodyRange = Range(match.range(at: 2), in: html) else { continue }
            let attrs = String(html[attrsRange])
            let text = WebText.collapse(WebText.decodeEntities(WebText.stripTags(String(html[bodyRange]))))
            if attrs.contains("result__a") {
                if results.count >= limit { break }
                let href = WebText.decodeEntities(WebText.firstMatch("href=\"([^\"]*)\"", in: attrs) ?? "")
                guard let target = resolve(href), !text.isEmpty else {
                    skipping = true
                    continue
                }
                skipping = false
                results.append(WebResult(title: text, url: target))
            } else if attrs.contains("result__snippet"), !skipping, !results.isEmpty, results[results.count - 1].snippet.isEmpty {
                results[results.count - 1].snippet = text
            }
        }
        return results
    }

    static func resolve(_ href: String) -> String? {
        let full = href.hasPrefix("//") ? "https:" + href : href
        guard let components = URLComponents(string: full) else { return nil }
        if components.host?.hasSuffix("duckduckgo.com") == true {
            if components.path.hasPrefix("/y.js") { return nil }
            guard let target = components.queryItems?.first(where: { $0.name == "uddg" })?.value else { return nil }
            return target
        }
        return full.hasPrefix("http") ? full : nil
    }
}
