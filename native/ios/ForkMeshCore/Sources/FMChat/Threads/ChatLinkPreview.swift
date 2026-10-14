import FMBase
import Foundation

public struct ChatLinkPreview: Codable, Equatable, Sendable {
    public var url: String
    public var title: String
    public var detail: String
    public var author: String
    public var siteName: String
    public var siteKey: String
    public var imageUrl: String
    public var kind: ChatLinkKind
    public var fetchedAt: Date

    public init(
        url: String, title: String = "", detail: String = "", author: String = "", siteName: String = "",
        siteKey: String = "", imageUrl: String = "", kind: ChatLinkKind = .page, fetchedAt: Date = Date(timeIntervalSince1970: 0)
    ) {
        self.url = url
        self.title = title
        self.detail = detail
        self.author = author
        self.siteName = siteName
        self.siteKey = siteKey
        self.imageUrl = imageUrl
        self.kind = kind
        self.fetchedAt = fetchedAt
    }

    public var isEmpty: Bool { title.isEmpty && imageUrl.isEmpty }

    public var site: ChatLinkSite? { siteKey.isEmpty ? nil : ChatLinkSites.site(key: siteKey) }

    public var isVideo: Bool { kind == .video }

    public var byline: String {
        if !author.isEmpty { return author }
        return detail
    }
}

public enum ChatLinkPreviewParsing {
    public static let maxTitle = 200
    public static let maxDetail = 300
    public static let imageExtensions: Set<String> = ["png", "jpg", "jpeg", "gif", "webp", "heic", "avif"]

    public static func candidates(_ hrefs: [String], limit: Int = 3) -> [URL] {
        var seen: Set<String> = []
        var out: [URL] = []
        for href in hrefs {
            guard let url = previewable(href) else { continue }
            let key = url.absoluteString
            if seen.contains(key) { continue }
            seen.insert(key)
            out.append(url)
            if out.count >= limit { break }
        }
        return out
    }

    public static func previewable(_ href: String) -> URL? {
        let trimmed = href.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let url = URL(string: trimmed), let scheme = url.scheme?.lowercased(), scheme == "https" || scheme == "http",
            let host = url.host, isPublicHost(host)
        else { return nil }
        if url.user != nil || url.password != nil { return nil }
        if let port = url.port, port != 443 && port != 80 { return nil }
        return url
    }

    public static func isPublicHost(_ raw: String) -> Bool {
        var host = raw.lowercased()
        if host.hasPrefix("[") && host.hasSuffix("]") { host = String(host.dropFirst().dropLast()) }
        if host.hasSuffix(".") { host.removeLast() }
        if host.isEmpty || host.count > 253 { return false }
        if host.contains(":") { return isPublicIPv6(host) }
        if let octets = ipv4(host) { return isPublicIPv4(octets) }
        if !host.contains(".") { return false }
        let numericLabel: (Substring) -> Bool = { label in
            label.allSatisfy(\.isNumber) || (label.hasPrefix("0x") && label.dropFirst(2).allSatisfy(\.isHexDigit))
        }
        if host.split(separator: ".").allSatisfy(numericLabel) { return false }
        let blocked = [
            "localhost", ".localhost", ".local", ".lan", ".home", ".internal", ".intranet", ".corp", ".home.arpa",
            ".localdomain", ".test", ".invalid", ".example", ".onion",
        ]
        for suffix in blocked {
            if suffix.hasPrefix(".") ? host.hasSuffix(suffix) : host == suffix { return false }
        }
        return true
    }

    static func ipv4(_ host: String) -> [Int]? {
        let parts = host.split(separator: ".", omittingEmptySubsequences: false)
        guard parts.count == 4 else { return nil }
        var out: [Int] = []
        for part in parts {
            guard !part.isEmpty, part.count <= 3, part.allSatisfy(\.isNumber), let value = Int(part), value <= 255 else {
                return nil
            }
            out.append(value)
        }
        return out
    }

    static func isPublicIPv4(_ o: [Int]) -> Bool {
        switch o[0] {
        case 0, 10, 127: return false
        case 100: return !(64...127).contains(o[1])
        case 169: return o[1] != 254
        case 172: return !(16...31).contains(o[1])
        case 192: return !(o[1] == 168 || (o[1] == 0 && (o[2] == 0 || o[2] == 2)))
        case 198: return !(o[1] == 18 || o[1] == 19 || (o[1] == 51 && o[2] == 100))
        case 203: return !(o[1] == 0 && o[2] == 113)
        default: return o[0] < 224
        }
    }

    static func isPublicIPv6(_ host: String) -> Bool {
        let address = host.split(separator: "%").first.map(String.init) ?? host
        if address == "::" || address == "::1" { return false }
        if address.hasPrefix("::ffff:") {
            if let octets = ipv4(String(address.dropFirst(7))) { return isPublicIPv4(octets) }
            return false
        }
        let first = address.split(separator: ":", omittingEmptySubsequences: false).first.map(String.init) ?? ""
        guard let value = UInt32(first, radix: 16) else { return false }
        if value & 0xFE00 == 0xFC00 { return false }
        if value & 0xFFC0 == 0xFE80 { return false }
        if value & 0xFF00 == 0xFF00 { return false }
        if value == 0x2001 {
            let second = address.split(separator: ":", omittingEmptySubsequences: false).dropFirst().first.map(String.init) ?? ""
            if second.lowercased() == "db8" || second.lowercased() == "0db8" { return false }
        }
        return value & 0xE000 == 0x2000
    }

    public static func isImageLink(_ url: URL) -> Bool {
        imageExtensions.contains(url.pathExtension.lowercased())
    }

    public static func youtubeId(_ url: URL) -> String? {
        guard let host = url.host.map(ChatLinkSites.bareHost) else { return nil }
        let parts = url.path.split(separator: "/").map(String.init)
        var candidate: String?
        if host == "youtu.be" {
            candidate = parts.first
        } else if host.hasSuffix("youtube.com") || host.hasSuffix("youtube-nocookie.com") {
            if parts.first == "watch" || parts.isEmpty {
                candidate = query(url, "v")
            } else if ["shorts", "live", "embed", "v", "e"].contains(parts.first ?? ""), parts.count > 1 {
                candidate = parts[1]
            }
        }
        guard let id = candidate, id.count >= 6, id.count <= 20,
            id.allSatisfy({ $0.isASCII && ($0.isLetter || $0.isNumber || $0 == "-" || $0 == "_") })
        else { return nil }
        return id
    }

    public static func query(_ url: URL, _ name: String) -> String? {
        URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems?.first { $0.name == name }?.value
    }

    public static func wikipediaPage(_ url: URL) -> (host: String, title: String)? {
        guard let host = url.host?.lowercased(), host.hasSuffix("wikipedia.org") else { return nil }
        let parts = url.path.split(separator: "/", maxSplits: 1).map(String.init)
        guard parts.count == 2, parts[0] == "wiki", !parts[1].isEmpty, !parts[1].contains(":") else { return nil }
        return (host.replacingOccurrences(of: ".m.wikipedia.org", with: ".wikipedia.org"), parts[1])
    }

    public static func hackerNewsId(_ url: URL) -> Int? {
        guard url.path == "/item", let id = query(url, "id").flatMap(Int.init), id > 0 else { return nil }
        return id
    }

    public static func packageName(_ url: URL, after marker: String) -> String? {
        let parts = url.path.split(separator: "/").map(String.init)
        guard let at = parts.firstIndex(of: marker), at + 1 < parts.count else { return nil }
        var name = parts[at + 1]
        if name.hasPrefix("@"), at + 2 < parts.count { name += "/" + parts[at + 2] }
        return name.removingPercentEncoding ?? name
    }

    public static func appStoreId(_ url: URL) -> String? {
        for part in url.path.split(separator: "/") where part.hasPrefix("id") {
            let digits = part.dropFirst(2)
            if !digits.isEmpty && digits.allSatisfy(\.isNumber) { return String(digits) }
        }
        return nil
    }

    public static func steamAppId(_ url: URL) -> String? {
        let parts = url.path.split(separator: "/").map(String.init)
        guard let at = parts.firstIndex(of: "app"), at + 1 < parts.count, parts[at + 1].allSatisfy(\.isNumber) else {
            return nil
        }
        return parts[at + 1]
    }

    public static func xkcdNumber(_ url: URL) -> String? {
        let parts = url.path.split(separator: "/").map(String.init)
        if parts.isEmpty { return "" }
        guard parts.count == 1, parts[0].allSatisfy(\.isNumber) else { return nil }
        return parts[0]
    }

    public static func parseHTML(_ html: String, page: URL) -> ChatLinkPreview {
        let head = headSection(html)
        var meta: [String: String] = [:]
        for tag in tags(named: "meta", in: head) {
            let attributes = attributes(of: tag)
            let key = (attributes["property"] ?? attributes["name"] ?? attributes["itemprop"] ?? "").lowercased()
            guard !key.isEmpty, let content = attributes["content"] else { continue }
            let value = clean(decodeEntities(content))
            if value.isEmpty || meta[key] != nil { continue }
            meta[key] = value
        }
        let title = first(meta, ["og:title", "twitter:title", "title"]) ?? titleTag(head)
        let detail = first(meta, ["og:description", "twitter:description", "description"]) ?? ""
        var image = first(meta, ["og:image:secure_url", "og:image", "og:image:url", "twitter:image", "twitter:image:src", "image"]) ?? ""
        if image.isEmpty { image = linkImage(head) }
        let siteName = first(meta, ["og:site_name", "application-name", "twitter:site"]) ?? ""
        var author = first(meta, ["author", "article:author", "twitter:creator"]) ?? ""
        if author.lowercased().hasPrefix("http") { author = "" }
        let type = (meta["og:type"] ?? "").lowercased()
        let video = type.hasPrefix("video") || meta["og:video"] != nil || meta["og:video:url"] != nil
            || meta["twitter:player"] != nil
        return ChatLinkPreview(
            url: page.absoluteString, title: truncate(title, maxTitle), detail: truncate(detail, maxDetail),
            author: truncate(author, 80), siteName: truncate(siteName.hasPrefix("@") ? String(siteName.dropFirst()) : siteName, 60),
            imageUrl: resolve(image, against: page), kind: video ? .video : .page)
    }

    static func first(_ meta: [String: String], _ keys: [String]) -> String? {
        for key in keys { if let value = meta[key], !value.isEmpty { return value } }
        return nil
    }

    static func headSection(_ html: String) -> String {
        let lower = html.lowercased()
        if let end = lower.range(of: "</head>") {
            return String(html[..<end.lowerBound])
        }
        return String(html.prefix(200_000))
    }

    static func tags(named name: String, in html: String) -> [String] {
        var out: [String] = []
        let lower = html.lowercased()
        let opener = "<" + name
        var cursor = lower.startIndex
        while let start = lower.range(of: opener, range: cursor..<lower.endIndex) {
            let after = start.upperBound
            if after < lower.endIndex {
                let next = lower[after]
                if !(next == " " || next == "\t" || next == "\n" || next == "\r" || next == "/" || next == ">") {
                    cursor = after
                    continue
                }
            }
            guard let close = closingBracket(lower, from: after) else { break }
            out.append(String(html[after..<close]))
            cursor = lower.index(after: close)
        }
        return out
    }

    static func closingBracket(_ text: String, from: String.Index) -> String.Index? {
        var quote: Character?
        var index = from
        while index < text.endIndex {
            let character = text[index]
            if let open = quote {
                if character == open { quote = nil }
            } else if character == "\"" || character == "'" {
                quote = character
            } else if character == ">" {
                return index
            }
            index = text.index(after: index)
        }
        return nil
    }

    static func attributes(of tag: String) -> [String: String] {
        var out: [String: String] = [:]
        let chars = Array(tag)
        var i = 0
        func isSpace(_ c: Character) -> Bool { c == " " || c == "\t" || c == "\n" || c == "\r" || c == "/" }
        while i < chars.count {
            while i < chars.count && isSpace(chars[i]) { i += 1 }
            var name = ""
            while i < chars.count && !isSpace(chars[i]) && chars[i] != "=" {
                name.append(chars[i])
                i += 1
            }
            while i < chars.count && (chars[i] == " " || chars[i] == "\t" || chars[i] == "\n" || chars[i] == "\r") { i += 1 }
            var value = ""
            if i < chars.count && chars[i] == "=" {
                i += 1
                while i < chars.count && (chars[i] == " " || chars[i] == "\t" || chars[i] == "\n" || chars[i] == "\r") { i += 1 }
                if i < chars.count && (chars[i] == "\"" || chars[i] == "'") {
                    let quote = chars[i]
                    i += 1
                    while i < chars.count && chars[i] != quote {
                        value.append(chars[i])
                        i += 1
                    }
                    i += 1
                } else {
                    while i < chars.count && !(chars[i] == " " || chars[i] == "\t" || chars[i] == "\n" || chars[i] == "\r") {
                        value.append(chars[i])
                        i += 1
                    }
                }
            }
            let key = name.lowercased()
            if !key.isEmpty && out[key] == nil { out[key] = value }
        }
        return out
    }

    static func titleTag(_ html: String) -> String {
        let lower = html.lowercased()
        guard let open = lower.range(of: "<title"), let openEnd = closingBracket(lower, from: open.upperBound),
            let close = lower.range(of: "</title>", range: openEnd..<lower.endIndex)
        else { return "" }
        return clean(decodeEntities(String(html[html.index(after: openEnd)..<close.lowerBound])))
    }

    static func linkImage(_ html: String) -> String {
        for tag in tags(named: "link", in: html) {
            let attributes = attributes(of: tag)
            if (attributes["rel"] ?? "").lowercased() == "image_src", let href = attributes["href"] {
                return decodeEntities(href)
            }
        }
        return ""
    }

    public static func resolve(_ raw: String, against page: URL) -> String {
        let trimmed = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty { return "" }
        guard let url = URL(string: trimmed, relativeTo: page)?.absoluteURL ?? URL(string: trimmed.replacingOccurrences(of: " ", with: "%20"), relativeTo: page)?.absoluteURL,
            previewable(url.absoluteString) != nil
        else { return "" }
        return url.absoluteString
    }

    public static func clean(_ text: String) -> String {
        text.split(whereSeparator: { $0.isWhitespace }).joined(separator: " ")
    }

    public static func truncate(_ text: String, _ limit: Int) -> String {
        let text = clean(text)
        if text.count <= limit { return text }
        return String(text.prefix(limit - 1)).trimmingCharacters(in: .whitespaces) + "\u{2026}"
    }

    static let namedEntities: [String: String] = [
        "amp": "&", "lt": "<", "gt": ">", "quot": "\"", "apos": "'", "nbsp": " ", "ndash": "\u{2013}",
        "mdash": "\u{2014}", "hellip": "\u{2026}", "rsquo": "\u{2019}", "lsquo": "\u{2018}", "rdquo": "\u{201D}",
        "ldquo": "\u{201C}", "middot": "\u{00B7}", "copy": "\u{00A9}", "reg": "\u{00AE}", "trade": "\u{2122}",
        "bull": "\u{2022}", "laquo": "\u{00AB}", "raquo": "\u{00BB}",
    ]

    public static func decodeEntities(_ text: String) -> String {
        guard text.contains("&") else { return text }
        var out = ""
        var index = text.startIndex
        while index < text.endIndex {
            let character = text[index]
            guard character == "&", let semi = text[index...].prefix(12).firstIndex(of: ";") else {
                out.append(character)
                index = text.index(after: index)
                continue
            }
            let body = String(text[text.index(after: index)..<semi])
            var replacement: String?
            if body.hasPrefix("#x") || body.hasPrefix("#X") {
                replacement = UInt32(body.dropFirst(2), radix: 16).flatMap(Unicode.Scalar.init).map { String(Character($0)) }
            } else if body.hasPrefix("#") {
                replacement = UInt32(body.dropFirst()).flatMap(Unicode.Scalar.init).map { String(Character($0)) }
            } else {
                replacement = namedEntities[body.lowercased()]
            }
            if let replacement {
                out += replacement
                index = text.index(after: semi)
            } else {
                out.append(character)
                index = text.index(after: index)
            }
        }
        return out
    }

    public static func stripTags(_ html: String) -> String {
        var out = ""
        var inTag = false
        for character in html {
            if character == "<" {
                inTag = true
                out.append(" ")
            } else if character == ">" {
                inTag = false
            } else if !inTag {
                out.append(character)
            }
        }
        return clean(decodeEntities(out))
    }

    public static func tweetText(_ html: String) -> String {
        let lower = html.lowercased()
        guard let open = lower.range(of: "<p"), let openEnd = closingBracket(lower, from: open.upperBound),
            let close = lower.range(of: "</p>", range: openEnd..<lower.endIndex)
        else { return stripTags(html) }
        return stripTags(String(html[html.index(after: openEnd)..<close.lowerBound]))
    }
}
