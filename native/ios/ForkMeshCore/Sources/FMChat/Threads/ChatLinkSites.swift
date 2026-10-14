import Foundation

public enum ChatLinkKind: String, Codable, Sendable, CaseIterable {
    case video, music, social, code, package, document, article, shop, app, map, image, chat, reference, page
}

public enum ChatLinkResolver: Equatable, Sendable {
    case page
    case youtube
    case oembed(String)
    case wikipedia
    case hackerNews
    case npm
    case pypi
    case crates
    case appStore
    case steam
    case xkcd
    case tweet
    case stackExchange
}

public struct ChatLinkSite: Equatable, Sendable, Identifiable {
    public let key: String
    public let name: String
    public let hosts: [String]
    public let color: UInt32
    public let symbol: String
    public let kind: ChatLinkKind
    public let resolver: ChatLinkResolver

    public var id: String { key }

    public init(
        _ key: String, _ name: String, _ hosts: [String], color: UInt32, symbol: String, kind: ChatLinkKind,
        resolver: ChatLinkResolver = .page
    ) {
        self.key = key
        self.name = name
        self.hosts = hosts
        self.color = color
        self.symbol = symbol
        self.kind = kind
        self.resolver = resolver
    }

    public func matches(host: String, path: String) -> Bool {
        let host = ChatLinkSites.bareHost(host)
        for entry in hosts {
            let parts = entry.split(separator: "/", maxSplits: 1).map(String.init)
            let domain = parts[0]
            guard host == domain || host.hasSuffix("." + domain) else { continue }
            if parts.count == 1 { return true }
            if path.lowercased().hasPrefix("/" + parts[1]) { return true }
        }
        return false
    }
}

public enum ChatLinkSites {
    public static let all: [ChatLinkSite] = [
        ChatLinkSite(
            "youtube", "YouTube", ["youtube.com", "youtu.be", "youtube-nocookie.com"], color: 0xFF0000,
            symbol: "play.fill", kind: .video, resolver: .youtube),
        ChatLinkSite(
            "vimeo", "Vimeo", ["vimeo.com"], color: 0x1AB7EA, symbol: "play.fill", kind: .video,
            resolver: .oembed("https://vimeo.com/api/oembed.json?url=")),
        ChatLinkSite("twitch", "Twitch", ["twitch.tv"], color: 0x9146FF, symbol: "play.tv.fill", kind: .video),
        ChatLinkSite(
            "tiktok", "TikTok", ["tiktok.com"], color: 0xFE2C55, symbol: "music.note", kind: .video,
            resolver: .oembed("https://www.tiktok.com/oembed?url=")),
        ChatLinkSite(
            "dailymotion", "Dailymotion", ["dailymotion.com", "dai.ly"], color: 0x0066DC, symbol: "play.fill",
            kind: .video, resolver: .oembed("https://www.dailymotion.com/services/oembed?format=json&url=")),
        ChatLinkSite(
            "loom", "Loom", ["loom.com"], color: 0x625DF5, symbol: "video.fill", kind: .video,
            resolver: .oembed("https://www.loom.com/v1/oembed?format=json&url=")),
        ChatLinkSite(
            "spotify", "Spotify", ["open.spotify.com", "spotify.link"], color: 0x1DB954, symbol: "waveform",
            kind: .music, resolver: .oembed("https://open.spotify.com/oembed?url=")),
        ChatLinkSite(
            "soundcloud", "SoundCloud", ["soundcloud.com"], color: 0xFF5500, symbol: "cloud.fill", kind: .music,
            resolver: .oembed("https://soundcloud.com/oembed?format=json&url=")),
        ChatLinkSite("applemusic", "Apple Music", ["music.apple.com"], color: 0xFA243C, symbol: "music.note", kind: .music),
        ChatLinkSite("applepodcasts", "Apple Podcasts", ["podcasts.apple.com"], color: 0x9933CC, symbol: "mic.fill", kind: .music),
        ChatLinkSite("bandcamp", "Bandcamp", ["bandcamp.com"], color: 0x1DA0C3, symbol: "music.quarternote.3", kind: .music),
        ChatLinkSite(
            "x", "X", ["x.com", "twitter.com", "fxtwitter.com", "vxtwitter.com"], color: 0x000000, symbol: "xmark",
            kind: .social, resolver: .tweet),
        ChatLinkSite("bluesky", "Bluesky", ["bsky.app"], color: 0x0085FF, symbol: "cloud.fill", kind: .social),
        ChatLinkSite(
            "mastodon", "Mastodon",
            ["mastodon.social", "mastodon.online", "fosstodon.org", "hachyderm.io", "mas.to", "infosec.exchange", "mstdn.social"],
            color: 0x6364FF, symbol: "bubble.left.and.bubble.right.fill", kind: .social),
        ChatLinkSite("threads", "Threads", ["threads.net", "threads.com"], color: 0x000000, symbol: "at", kind: .social),
        ChatLinkSite("reddit", "Reddit", ["reddit.com", "redd.it"], color: 0xFF4500, symbol: "bubble.left.fill", kind: .social),
        ChatLinkSite(
            "hackernews", "Hacker News", ["news.ycombinator.com"], color: 0xFF6600, symbol: "y.square.fill",
            kind: .social, resolver: .hackerNews),
        ChatLinkSite("instagram", "Instagram", ["instagram.com", "instagr.am"], color: 0xE1306C, symbol: "camera.fill", kind: .social),
        ChatLinkSite("facebook", "Facebook", ["facebook.com", "fb.watch", "fb.com"], color: 0x1877F2, symbol: "f.square.fill", kind: .social),
        ChatLinkSite("linkedin", "LinkedIn", ["linkedin.com", "lnkd.in"], color: 0x0A66C2, symbol: "briefcase.fill", kind: .social),
        ChatLinkSite("pinterest", "Pinterest", ["pinterest.com", "pin.it"], color: 0xE60023, symbol: "pin.fill", kind: .image),
        ChatLinkSite("tumblr", "Tumblr", ["tumblr.com"], color: 0x36465D, symbol: "t.square.fill", kind: .social),
        ChatLinkSite("github", "GitHub", ["github.com"], color: 0x24292F, symbol: "chevron.left.forwardslash.chevron.right", kind: .code),
        ChatLinkSite("gitlab", "GitLab", ["gitlab.com"], color: 0xFC6D26, symbol: "chevron.left.forwardslash.chevron.right", kind: .code),
        ChatLinkSite("bitbucket", "Bitbucket", ["bitbucket.org"], color: 0x0052CC, symbol: "chevron.left.forwardslash.chevron.right", kind: .code),
        ChatLinkSite(
            "stackoverflow", "Stack Overflow",
            ["stackoverflow.com", "stackexchange.com", "superuser.com", "serverfault.com", "askubuntu.com"],
            color: 0xF48024, symbol: "square.stack.3d.up.fill", kind: .reference, resolver: .stackExchange),
        ChatLinkSite("npm", "npm", ["npmjs.com"], color: 0xCB3837, symbol: "shippingbox.fill", kind: .package, resolver: .npm),
        ChatLinkSite("pypi", "PyPI", ["pypi.org"], color: 0x3775A9, symbol: "shippingbox.fill", kind: .package, resolver: .pypi),
        ChatLinkSite("crates", "crates.io", ["crates.io"], color: 0xE6B14C, symbol: "shippingbox.fill", kind: .package, resolver: .crates),
        ChatLinkSite("huggingface", "Hugging Face", ["huggingface.co", "hf.co"], color: 0xFFB000, symbol: "face.smiling.fill", kind: .code),
        ChatLinkSite(
            "codepen", "CodePen", ["codepen.io"], color: 0x1E1F26, symbol: "curlybraces", kind: .code,
            resolver: .oembed("https://codepen.io/api/oembed?format=json&url=")),
        ChatLinkSite("figma", "Figma", ["figma.com"], color: 0xF24E1E, symbol: "paintpalette.fill", kind: .document),
        ChatLinkSite("notion", "Notion", ["notion.so", "notion.site"], color: 0x191919, symbol: "doc.text.fill", kind: .document),
        ChatLinkSite("googledocs", "Google Docs", ["docs.google.com", "drive.google.com"], color: 0x4285F4, symbol: "doc.text.fill", kind: .document),
        ChatLinkSite("dropbox", "Dropbox", ["dropbox.com", "db.tt"], color: 0x0061FF, symbol: "shippingbox", kind: .document),
        ChatLinkSite("medium", "Medium", ["medium.com"], color: 0x000000, symbol: "text.alignleft", kind: .article),
        ChatLinkSite("substack", "Substack", ["substack.com"], color: 0xFF6719, symbol: "envelope.fill", kind: .article),
        ChatLinkSite("devto", "DEV", ["dev.to"], color: 0x0A0A0A, symbol: "text.alignleft", kind: .article),
        ChatLinkSite("wikipedia", "Wikipedia", ["wikipedia.org"], color: 0x636466, symbol: "w.square.fill", kind: .reference, resolver: .wikipedia),
        ChatLinkSite("arxiv", "arXiv", ["arxiv.org"], color: 0xB31B1B, symbol: "doc.richtext.fill", kind: .reference),
        ChatLinkSite("imdb", "IMDb", ["imdb.com"], color: 0xF5C518, symbol: "film.fill", kind: .reference),
        ChatLinkSite(
            "amazon", "Amazon", ["amazon.com", "amazon.co.uk", "amazon.de", "amazon.ca", "amazon.fr", "amazon.co.jp", "amzn.to", "a.co"],
            color: 0xFF9900, symbol: "cart.fill", kind: .shop),
        ChatLinkSite("ebay", "eBay", ["ebay.com", "ebay.co.uk", "ebay.de"], color: 0xE53238, symbol: "tag.fill", kind: .shop),
        ChatLinkSite("etsy", "Etsy", ["etsy.com"], color: 0xF1641E, symbol: "bag.fill", kind: .shop),
        ChatLinkSite("appstore", "App Store", ["apps.apple.com", "itunes.apple.com"], color: 0x0D96F6, symbol: "app.badge.fill", kind: .app, resolver: .appStore),
        ChatLinkSite("googleplay", "Google Play", ["play.google.com"], color: 0x01875F, symbol: "play.fill", kind: .app),
        ChatLinkSite("steam", "Steam", ["store.steampowered.com", "steamcommunity.com"], color: 0x1B2838, symbol: "gamecontroller.fill", kind: .app, resolver: .steam),
        ChatLinkSite("producthunt", "Product Hunt", ["producthunt.com"], color: 0xDA552F, symbol: "p.circle.fill", kind: .app),
        ChatLinkSite(
            "googlemaps", "Google Maps", ["maps.google.com", "maps.app.goo.gl", "google.com/maps", "goo.gl/maps"],
            color: 0x34A853, symbol: "map.fill", kind: .map),
        ChatLinkSite("applemaps", "Apple Maps", ["maps.apple.com"], color: 0x4CD964, symbol: "map.fill", kind: .map),
        ChatLinkSite("nytimes", "The New York Times", ["nytimes.com", "nyti.ms"], color: 0x000000, symbol: "newspaper.fill", kind: .article),
        ChatLinkSite("bbc", "BBC", ["bbc.com", "bbc.co.uk"], color: 0xBB1919, symbol: "newspaper.fill", kind: .article),
        ChatLinkSite("guardian", "The Guardian", ["theguardian.com"], color: 0x052962, symbol: "newspaper.fill", kind: .article),
        ChatLinkSite("theverge", "The Verge", ["theverge.com"], color: 0x5200FF, symbol: "newspaper.fill", kind: .article),
        ChatLinkSite("techcrunch", "TechCrunch", ["techcrunch.com"], color: 0x0A9E01, symbol: "newspaper.fill", kind: .article),
        ChatLinkSite("arstechnica", "Ars Technica", ["arstechnica.com"], color: 0xFF4E00, symbol: "newspaper.fill", kind: .article),
        ChatLinkSite("reuters", "Reuters", ["reuters.com"], color: 0xFF8000, symbol: "newspaper.fill", kind: .article),
        ChatLinkSite("xkcd", "xkcd", ["xkcd.com"], color: 0x96A8C8, symbol: "scribble", kind: .image, resolver: .xkcd),
        ChatLinkSite("giphy", "GIPHY", ["giphy.com"], color: 0x9933FF, symbol: "sparkles", kind: .image),
        ChatLinkSite("imgur", "Imgur", ["imgur.com"], color: 0x1BB76E, symbol: "photo.fill", kind: .image),
        ChatLinkSite("flickr", "Flickr", ["flickr.com", "flic.kr"], color: 0xFF0084, symbol: "camera.aperture", kind: .image,
            resolver: .oembed("https://www.flickr.com/services/oembed/?format=json&url=")),
        ChatLinkSite("discord", "Discord", ["discord.gg", "discord.com"], color: 0x5865F2, symbol: "gamecontroller.fill", kind: .chat),
        ChatLinkSite("telegram", "Telegram", ["t.me", "telegram.me"], color: 0x26A5E4, symbol: "paperplane.fill", kind: .chat),
        ChatLinkSite("forkmesh", "ForkMesh", ["forkmesh.com"], color: 0x7C5CFF, symbol: "point.3.connected.trianglepath.dotted", kind: .code),
    ]

    public static func bareHost(_ host: String) -> String {
        var host = host.lowercased()
        if host.hasSuffix(".") { host.removeLast() }
        for prefix in ["www.", "m.", "mobile.", "old.", "new."] where host.hasPrefix(prefix) {
            host = String(host.dropFirst(prefix.count))
        }
        return host
    }

    public static func site(for url: URL) -> ChatLinkSite? {
        guard let host = url.host else { return nil }
        let path = url.path
        return all.first { $0.matches(host: host, path: path) }
    }

    public static func site(key: String) -> ChatLinkSite? { all.first { $0.key == key } }
}
