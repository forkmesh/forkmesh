import FMBase
import Foundation

public struct RSSIconLink: Equatable, Sendable {
    public enum Kind: String, Sendable {
        case icon
        case touch
    }

    public var url: String
    public var kind: Kind
    public var sizes: [Int]
    public var square: Bool
    public var order: Int

    public init(url: String, kind: Kind, sizes: [Int] = [], square: Bool = true, order: Int = 0) {
        self.url = url
        self.kind = kind
        self.sizes = sizes
        self.square = square
        self.order = order
    }
}

public enum RSSIconLookup: Equatable, Sendable {
    case found(url: String, data: Data)
    case missing
    case unreachable
}

public enum RSSFeedIcons {
    public static let retryMs: Int64 = RSSReaderLimits.dayMs
    public static let refreshMs: Int64 = 30 * RSSReaderLimits.dayMs
    public static let maxPageCandidates = 4
    public static let accept = "image/png, image/x-icon, image/vnd.microsoft.icon, image/webp, image/jpeg, image/gif;q=0.9, image/*;q=0.8, */*;q=0.1"
    public static let pageAccept = "text/html, application/xhtml+xml;q=0.9, */*;q=0.5"
    public static let proxyHosts: Set<String> = [
        "feedburner.com", "feedproxy.google.com", "feedblitz.com", "feedpress.me", "feedpress.it", "rsshub.app", "rss.app",
        "fetchrss.com", "feed43.com", "politepol.com",
    ]

    public static func isProxy(_ host: String) -> Bool {
        let name = host.lowercased()
        if name.isEmpty { return false }
        return proxyHosts.contains { name == $0 || name.hasSuffix("." + $0) }
    }

    public static func homePage(_ feed: RSSFeed) -> String {
        let site = RSSURL.http(feed.siteUrl)
        if !site.isEmpty, !isProxy(RSSURL.host(site)), RSSURL.identity(site) != RSSURL.identity(feed.url) { return site }
        let address = RSSURL.http(feed.url)
        if !address.isEmpty, !isProxy(RSSURL.host(address)) { return RSSURL.origin(address) + "/" }
        return ""
    }

    public static func pages(_ feed: RSSFeed) -> [String] {
        let home = homePage(feed)
        guard !home.isEmpty else { return [] }
        let root = RSSURL.origin(home) + "/"
        return RSSURL.identity(root) == RSSURL.identity(home) ? [home] : [home, root]
    }

    public static func declared(_ feed: RSSFeed) -> [String] {
        let icon = RSSURL.http(feed.iconUrl)
        return icon.isEmpty ? [] : [icon]
    }

    public static func fallbacks(_ feed: RSSFeed) -> [String] {
        let home = homePage(feed)
        let address = RSSURL.http(feed.url)
        var origins: [String] = []
        if !home.isEmpty { origins.append(RSSURL.origin(home)) }
        if !address.isEmpty, !isProxy(RSSURL.host(address)) {
            let own = RSSURL.origin(address)
            if !origins.contains(own) { origins.append(own) }
        }
        return ["/favicon.ico", "/apple-touch-icon.png"].flatMap { path in origins.map { $0 + path } }
    }

    public static func plan(_ feed: RSSFeed, page: [RSSIconLink] = []) -> [String] {
        unique(declared(feed) + Array(ranked(page).prefix(maxPageCandidates)) + fallbacks(feed))
    }

    public static func unique(_ list: [String]) -> [String] {
        var seen: Set<String> = []
        var out: [String] = []
        for url in list where !url.isEmpty && seen.insert(RSSURL.upgraded(url)).inserted { out.append(url) }
        return out
    }

    static func dimensions(_ text: String) -> [(Int, Int)] {
        var out: [(Int, Int)] = []
        for token in text.lowercased().split(whereSeparator: { $0 == " " || $0 == "\t" || $0 == "\n" || $0 == "," }) {
            let parts = token.split(separator: "x", omittingEmptySubsequences: false)
            guard parts.count == 2, let width = Int(parts[0]), let height = Int(parts[1]), width > 0, height > 0 else { continue }
            out.append((width, height))
        }
        return out
    }

    static func hinted(_ href: String) -> [(Int, Int)] {
        let name = String(RSSURL.split(href).path.split(separator: "/").last ?? "").lowercased()
        var out: [(Int, Int)] = []
        let characters = Array(name)
        var index = 0
        while index < characters.count {
            guard characters[index].isNumber else {
                index += 1
                continue
            }
            var end = index
            while end < characters.count && characters[end].isNumber { end += 1 }
            if end < characters.count, characters[end] == "x" {
                var tail = end + 1
                while tail < characters.count && characters[tail].isNumber { tail += 1 }
                if tail > end + 1, let width = Int(String(characters[index..<end])), let height = Int(String(characters[(end + 1)..<tail])),
                   (8...4096).contains(width), (8...4096).contains(height) {
                    out.append((width, height))
                }
                index = tail
            } else {
                index = end
            }
        }
        return out
    }

    public static func link(rel: String, href: String, sizes: String = "", type: String = "", base: String, order: Int = 0) -> RSSIconLink? {
        let tokens = Set(rel.lowercased().split(whereSeparator: { $0 == " " || $0 == "\t" || $0 == "\n" }).map(String.init))
        let kind: RSSIconLink.Kind
        if tokens.contains("apple-touch-icon") || tokens.contains("apple-touch-icon-precomposed") {
            kind = .touch
        } else if tokens.contains("icon") {
            kind = .icon
        } else {
            return nil
        }
        let mime = RSSText.strip(type).lowercased()
        if mime.contains("svg") { return nil }
        let raw = RSSText.strip(href)
        if raw.lowercased().hasPrefix("data:") { return nil }
        let url = RSSURL.http(raw, base: base)
        if url.isEmpty { return nil }
        let path = RSSURL.split(url).path.lowercased()
        if path.hasSuffix(".svg") || path.hasSuffix(".svgz") { return nil }
        var found = dimensions(sizes)
        if found.isEmpty { found = hinted(url) }
        let squares = found.filter { $0.0 == $0.1 }.map(\.0)
        if !squares.isEmpty { return RSSIconLink(url: url, kind: kind, sizes: squares, square: true, order: order) }
        if let widest = found.max(by: { max($0.0, $0.1) < max($1.0, $1.1) }) {
            return RSSIconLink(url: url, kind: kind, sizes: [max(widest.0, widest.1)], square: false, order: order)
        }
        return RSSIconLink(url: url, kind: kind, sizes: [], square: true, order: order)
    }

    public static func links(_ document: String, base: String) -> [RSSIconLink] {
        var raw: [(rel: String, href: String, sizes: String, type: String)] = []
        var baseHref = ""
        for token in RSSHTMLTokenizer.tokens(RSSText.prefix(document, 600_000)) {
            guard case let .start(name, attributes, _) = token, name == "link" || name == "base" else { continue }
            var values: [String: String] = [:]
            for attr in attributes where values[attr.name] == nil { values[attr.name] = attr.value }
            if name == "base", let href = values["href"], !href.isEmpty, baseHref.isEmpty { baseHref = href }
            guard name == "link", let href = values["href"], !href.isEmpty else { continue }
            raw.append((values["rel"] ?? "", href, values["sizes"] ?? "", values["type"] ?? ""))
        }
        let root = RSSURL.http(baseHref, base: base).isEmpty ? base : RSSURL.http(baseHref, base: base)
        return raw.enumerated().compactMap { entry in
            link(rel: entry.element.rel, href: entry.element.href, sizes: entry.element.sizes, type: entry.element.type, base: root, order: entry.offset)
        }
    }

    static func rank(_ size: Int, square: Bool) -> (Int, Int) {
        if !square { return (5, abs(size - 180)) }
        if (32...180).contains(size) { return (0, 180 - size) }
        if size > 180 { return (3, size - 180) }
        return (4, 32 - size)
    }

    static func rank(_ link: RSSIconLink) -> (Int, Int) {
        if link.sizes.isEmpty { return link.kind == .touch ? (0, 1) : (2, 0) }
        return link.sizes.map { rank($0, square: link.square) }.min { $0 < $1 } ?? (2, 0)
    }

    public static func ranked(_ links: [RSSIconLink]) -> [String] {
        let sorted = links.sorted { lhs, rhs in
            let left = rank(lhs)
            let right = rank(rhs)
            if left != right { return left < right }
            return lhs.order < rhs.order
        }
        return unique(sorted.map(\.url))
    }

    public static func best(_ links: [RSSIconLink]) -> String { ranked(links).first ?? "" }

    public static func isImage(_ data: Data) -> Bool {
        let bytes = [UInt8](data.prefix(16))
        guard bytes.count >= 4 else { return false }
        func starts(_ prefix: [UInt8], at offset: Int = 0) -> Bool {
            guard bytes.count >= offset + prefix.count else { return false }
            return Array(bytes[offset..<(offset + prefix.count)]) == prefix
        }
        if starts([0x89, 0x50, 0x4E, 0x47]) { return true }
        if starts([0xFF, 0xD8, 0xFF]) { return true }
        if starts(Array("GIF8".utf8)) { return true }
        if starts([0x00, 0x00, 0x01, 0x00]) || starts([0x00, 0x00, 0x02, 0x00]) { return data.count > 6 }
        if starts(Array("BM".utf8)) && data.count > 26 { return true }
        if starts(Array("RIFF".utf8)) && starts(Array("WEBP".utf8), at: 8) { return true }
        if starts(Array("ftyp".utf8), at: 4), bytes.count >= 12 {
            let brand = String(decoding: bytes[8..<12], as: UTF8.self)
            return ["heic", "heix", "mif1", "msf1", "avif", "avis", "hevc"].contains(brand)
        }
        return false
    }

    public static let maxInlineBytes = 192 * 1024

    public static func mime(_ data: Data) -> String {
        guard isImage(data) else { return "" }
        let bytes = [UInt8](data.prefix(12))
        if bytes.count >= 12, String(decoding: bytes[4..<8], as: UTF8.self) == "ftyp" {
            return String(decoding: bytes[8..<12], as: UTF8.self).hasPrefix("avi") ? "image/avif" : "image/heic"
        }
        switch bytes[0] {
        case 0x89: return "image/png"
        case 0xFF: return "image/jpeg"
        case 0x47: return "image/gif"
        case 0x42: return "image/bmp"
        case 0x52: return "image/webp"
        default: return "image/x-icon"
        }
    }

    public static func inline(_ data: Data?) -> String {
        guard let data, data.count <= maxInlineBytes else { return "" }
        let type = mime(data)
        return type.isEmpty ? "" : "data:\(type);base64,\(data.base64EncodedString())"
    }

    public static func source(_ feed: RSSFeed) -> String {
        RSSText.fnv([feed.iconUrl, feed.siteUrl, feed.url].joined(separator: "\n"))
    }

    public static func needsLookup(_ feed: RSSFeed, now: Int64, hasImage: Bool) -> Bool {
        if feed.iconCheckedAt <= 0 || feed.iconSource != source(feed) { return true }
        if now < feed.iconCheckedAt { return true }
        let age = now - feed.iconCheckedAt
        if feed.iconFound.isEmpty { return age >= retryMs }
        if !hasImage { return true }
        return age >= refreshMs
    }
}

extension RSSReaderFetcher {
    func probeIcon(_ address: String, tried: inout Set<String>, answered: inout Bool, decodes: @Sendable (Data) -> Bool) async
        -> RSSIconLookup?
    {
        let target = RSSURL.http(address)
        guard !target.isEmpty, tried.insert(RSSURL.upgraded(target)).inserted else { return nil }
        switch await get(target, accept: RSSFeedIcons.accept) {
        case .failure(let error):
            if case .http = error { answered = true }
            if case .tooLarge = error { answered = true }
            return nil
        case .success(let answer):
            answered = true
            guard (200...299).contains(answer.status), !answer.body.isEmpty, RSSFeedIcons.isImage(answer.body), decodes(answer.body) else {
                return nil
            }
            return .found(url: answer.finalUrl, data: answer.body)
        }
    }

    public func icon(for feed: RSSFeed, decodes: @escaping @Sendable (Data) -> Bool = { _ in true }) async -> RSSIconLookup {
        var tried: Set<String> = []
        var answered = false
        for address in RSSFeedIcons.declared(feed) {
            if let hit = await probeIcon(address, tried: &tried, answered: &answered, decodes: decodes) { return hit }
        }
        for page in RSSFeedIcons.pages(feed) {
            switch await get(page, accept: RSSFeedIcons.pageAccept) {
            case .failure(let error):
                if case .http = error { answered = true }
            case .success(let answer):
                answered = true
                guard (200...299).contains(answer.status) else { continue }
                let found = RSSFeedIcons.ranked(RSSFeedIcons.links(answer.text, base: answer.finalUrl))
                for address in found.prefix(RSSFeedIcons.maxPageCandidates) {
                    if let hit = await probeIcon(address, tried: &tried, answered: &answered, decodes: decodes) { return hit }
                }
            }
        }
        for address in RSSFeedIcons.fallbacks(feed) {
            if let hit = await probeIcon(address, tried: &tried, answered: &answered, decodes: decodes) { return hit }
        }
        return answered ? .missing : .unreachable
    }
}
