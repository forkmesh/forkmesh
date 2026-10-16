import Foundation

public enum RSSYouTubeRule: Equatable, Sendable {
    case none
    case feed(String)
    case page
}

public enum RSSDiscoveryRules {
    public static let youTubeHosts: Set<String> = ["youtube.com", "www.youtube.com", "m.youtube.com", "youtu.be"]
    public static let youTubeFeeds = "https://www.youtube.com/feeds/videos.xml"

    static func segments(_ path: String) -> [String] {
        path.split(separator: "/", omittingEmptySubsequences: true).map(String.init)
    }

    static func query(_ parts: RSSURLParts, _ name: String) -> String {
        for pair in (parts.query ?? "").split(separator: "&") {
            let pieces = pair.split(separator: "=", maxSplits: 1, omittingEmptySubsequences: false)
            if pieces.count == 2, pieces[0] == name { return String(pieces[1]) }
        }
        return ""
    }

    static func encoded(_ value: String) -> String {
        let allowed = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: "-._~"))
        let raw = value.removingPercentEncoding ?? value
        return raw.addingPercentEncoding(withAllowedCharacters: allowed) ?? raw
    }

    public static func isYouTube(_ url: String) -> Bool { youTubeHosts.contains(RSSURL.host(url)) }

    public static func youTube(_ url: String) -> RSSYouTubeRule {
        let parts = RSSURL.split(url)
        guard youTubeHosts.contains(parts.host) else { return .none }
        let path = segments(parts.path)
        if parts.host == "youtu.be" { return path.isEmpty ? .none : .page }
        guard let first = path.first else { return .none }
        switch first.lowercased() {
        case "channel":
            if path.count >= 2, path[1].hasPrefix("UC") { return .feed(channelFeed(path[1])) }
            return .none
        case "playlist":
            let list = query(parts, "list")
            return list.isEmpty ? .none : .feed("\(youTubeFeeds)?playlist_id=\(encoded(list))")
        case "user":
            return path.count >= 2 ? .feed("\(youTubeFeeds)?user=\(encoded(path[1]))") : .none
        case "c", "shorts":
            return path.count >= 2 ? .page : .none
        case "watch":
            return query(parts, "v").isEmpty ? .none : .page
        default:
            return first.hasPrefix("@") && first.count > 1 ? .page : .none
        }
    }

    public static func channelFeed(_ id: String) -> String { "\(youTubeFeeds)?channel_id=\(id)" }

    static func validChannel(_ value: Substring) -> String? {
        let id = value.prefix(24)
        guard id.count == 24, id.hasPrefix("UC"),
              id.dropFirst(2).allSatisfy({ $0.isASCII && ($0.isLetter || $0.isNumber || $0 == "_" || $0 == "-") })
        else { return nil }
        if let next = value.dropFirst(24).first, next.isASCII && (next.isLetter || next.isNumber || next == "_" || next == "-") {
            return nil
        }
        return String(id)
    }

    public static func youTubeChannelId(_ page: String) -> String? {
        let text = Substring(page)
        var search = text.startIndex
        while let range = text.range(of: "\"channelId\":\"", range: search..<text.endIndex) {
            if let id = validChannel(text[range.upperBound...]) { return id }
            search = range.upperBound
        }
        for token in RSSHTMLTokenizer.tokens(RSSText.prefix(page, 1_500_000)) {
            guard case let .start(name, attributes, _) = token else { continue }
            var values: [String: String] = [:]
            for attribute in attributes { values[attribute.name] = attribute.value }
            if name == "meta", (values["itemprop"] ?? "").lowercased() == "channelid",
               let id = validChannel(Substring(values["content"] ?? "")) {
                return id
            }
            if name == "link", (values["rel"] ?? "").lowercased().split(separator: " ").contains("canonical"),
               let href = values["href"], let range = href.range(of: "/channel/"),
               let id = validChannel(href[range.upperBound...]) {
                return id
            }
        }
        return nil
    }

    public static func mastodon(_ url: String) -> String? {
        let parts = RSSURL.split(url)
        guard !parts.host.isEmpty, !youTubeHosts.contains(parts.host), parts.query == nil else { return nil }
        var path = parts.path
        if path.hasSuffix("/") { path.removeLast() }
        guard path.hasPrefix("/@"), path.count > 2, !path.dropFirst().contains("/"), !path.hasSuffix(".rss") else { return nil }
        return "https://\(parts.authority)\(path).rss"
    }

    public static func bluesky(_ url: String) -> String? {
        let parts = RSSURL.split(url)
        guard parts.host == "bsky.app" else { return nil }
        let path = segments(parts.path)
        guard path.count == 2, path[0] == "profile" else { return nil }
        return "https://bsky.app/profile/\(path[1])/rss"
    }

    public static func rewrites(_ url: String) -> [String] {
        if case .feed(let feed) = youTube(url) { return [feed] }
        if let feed = mastodon(url) { return [feed] }
        if let feed = bluesky(url) { return [feed] }
        return []
    }
}
