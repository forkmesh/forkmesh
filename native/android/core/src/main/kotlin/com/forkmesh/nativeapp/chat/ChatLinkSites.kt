package com.forkmesh.nativeapp.chat

import java.net.URI

enum class ChatLinkKind(val raw: String) {
    Video("video"),
    Music("music"),
    Social("social"),
    Code("code"),
    Package("package"),
    Document("document"),
    Article("article"),
    Shop("shop"),
    App("app"),
    Map("map"),
    Image("image"),
    Chat("chat"),
    Reference("reference"),
    Page("page");

    companion object {
        fun of(raw: String): ChatLinkKind = entries.firstOrNull { it.raw == raw } ?: Page
    }
}

sealed class ChatLinkResolver {
    data object Page : ChatLinkResolver()
    data object YouTube : ChatLinkResolver()
    data class OEmbed(val endpoint: String) : ChatLinkResolver()
    data object Wikipedia : ChatLinkResolver()
    data object HackerNews : ChatLinkResolver()
    data object Npm : ChatLinkResolver()
    data object PyPI : ChatLinkResolver()
    data object Crates : ChatLinkResolver()
    data object AppStore : ChatLinkResolver()
    data object Steam : ChatLinkResolver()
    data object Xkcd : ChatLinkResolver()
    data object Tweet : ChatLinkResolver()
    data object StackExchange : ChatLinkResolver()
}

data class ChatLinkSite(
    val key: String,
    val name: String,
    val hosts: List<String>,
    val color: Long,
    val icon: String,
    val kind: ChatLinkKind,
    val resolver: ChatLinkResolver = ChatLinkResolver.Page,
) {
    fun matches(host: String, path: String): Boolean {
        val bare = ChatLinkSites.bareHost(host)
        val lowerPath = path.lowercase()
        for (entry in hosts) {
            val slash = entry.indexOf('/')
            val domain = if (slash < 0) entry else entry.substring(0, slash)
            if (bare != domain && !bare.endsWith(".$domain")) continue
            if (slash < 0) return true
            if (lowerPath.startsWith("/" + entry.substring(slash + 1))) return true
        }
        return false
    }
}

object ChatLinkSites {
    private fun site(
        key: String,
        name: String,
        hosts: List<String>,
        color: Long,
        icon: String,
        kind: ChatLinkKind,
        resolver: ChatLinkResolver = ChatLinkResolver.Page,
    ) = ChatLinkSite(key, name, hosts, color, icon, kind, resolver)

    val all: List<ChatLinkSite> = listOf(
        site("youtube", "YouTube", listOf("youtube.com", "youtu.be", "youtube-nocookie.com"), 0xFF0000, "play_arrow", ChatLinkKind.Video, ChatLinkResolver.YouTube),
        site("vimeo", "Vimeo", listOf("vimeo.com"), 0x1AB7EA, "play_arrow", ChatLinkKind.Video, ChatLinkResolver.OEmbed("https://vimeo.com/api/oembed.json?url=")),
        site("twitch", "Twitch", listOf("twitch.tv"), 0x9146FF, "tv", ChatLinkKind.Video),
        site("tiktok", "TikTok", listOf("tiktok.com"), 0xFE2C55, "music_note", ChatLinkKind.Video, ChatLinkResolver.OEmbed("https://www.tiktok.com/oembed?url=")),
        site("dailymotion", "Dailymotion", listOf("dailymotion.com", "dai.ly"), 0x0066DC, "play_arrow", ChatLinkKind.Video, ChatLinkResolver.OEmbed("https://www.dailymotion.com/services/oembed?format=json&url=")),
        site("loom", "Loom", listOf("loom.com"), 0x625DF5, "videocam", ChatLinkKind.Video, ChatLinkResolver.OEmbed("https://www.loom.com/v1/oembed?format=json&url=")),
        site("spotify", "Spotify", listOf("open.spotify.com", "spotify.link"), 0x1DB954, "graphic_eq", ChatLinkKind.Music, ChatLinkResolver.OEmbed("https://open.spotify.com/oembed?url=")),
        site("soundcloud", "SoundCloud", listOf("soundcloud.com"), 0xFF5500, "cloud", ChatLinkKind.Music, ChatLinkResolver.OEmbed("https://soundcloud.com/oembed?format=json&url=")),
        site("applemusic", "Apple Music", listOf("music.apple.com"), 0xFA243C, "music_note", ChatLinkKind.Music),
        site("applepodcasts", "Apple Podcasts", listOf("podcasts.apple.com"), 0x9933CC, "mic", ChatLinkKind.Music),
        site("bandcamp", "Bandcamp", listOf("bandcamp.com"), 0x1DA0C3, "library_music", ChatLinkKind.Music),
        site("x", "X", listOf("x.com", "twitter.com", "fxtwitter.com", "vxtwitter.com"), 0x000000, "close", ChatLinkKind.Social, ChatLinkResolver.Tweet),
        site("bluesky", "Bluesky", listOf("bsky.app"), 0x0085FF, "cloud", ChatLinkKind.Social),
        site(
            "mastodon", "Mastodon",
            listOf("mastodon.social", "mastodon.online", "fosstodon.org", "hachyderm.io", "mas.to", "infosec.exchange", "mstdn.social"),
            0x6364FF, "forum", ChatLinkKind.Social,
        ),
        site("threads", "Threads", listOf("threads.net", "threads.com"), 0x000000, "alternate_email", ChatLinkKind.Social),
        site("reddit", "Reddit", listOf("reddit.com", "redd.it"), 0xFF4500, "chat_bubble_outline", ChatLinkKind.Social),
        site("hackernews", "Hacker News", listOf("news.ycombinator.com"), 0xFF6600, "trending_up", ChatLinkKind.Social, ChatLinkResolver.HackerNews),
        site("instagram", "Instagram", listOf("instagram.com", "instagr.am"), 0xE1306C, "photo_camera", ChatLinkKind.Social),
        site("facebook", "Facebook", listOf("facebook.com", "fb.watch", "fb.com"), 0x1877F2, "facebook", ChatLinkKind.Social),
        site("linkedin", "LinkedIn", listOf("linkedin.com", "lnkd.in"), 0x0A66C2, "work", ChatLinkKind.Social),
        site("pinterest", "Pinterest", listOf("pinterest.com", "pin.it"), 0xE60023, "place", ChatLinkKind.Image),
        site("tumblr", "Tumblr", listOf("tumblr.com"), 0x36465D, "edit_note", ChatLinkKind.Social),
        site("github", "GitHub", listOf("github.com"), 0x24292F, "code", ChatLinkKind.Code),
        site("gitlab", "GitLab", listOf("gitlab.com"), 0xFC6D26, "code", ChatLinkKind.Code),
        site("bitbucket", "Bitbucket", listOf("bitbucket.org"), 0x0052CC, "code", ChatLinkKind.Code),
        site(
            "stackoverflow", "Stack Overflow",
            listOf("stackoverflow.com", "stackexchange.com", "superuser.com", "serverfault.com", "askubuntu.com"),
            0xF48024, "layers", ChatLinkKind.Reference, ChatLinkResolver.StackExchange,
        ),
        site("npm", "npm", listOf("npmjs.com"), 0xCB3837, "inventory_2", ChatLinkKind.Package, ChatLinkResolver.Npm),
        site("pypi", "PyPI", listOf("pypi.org"), 0x3775A9, "inventory_2", ChatLinkKind.Package, ChatLinkResolver.PyPI),
        site("crates", "crates.io", listOf("crates.io"), 0xE6B14C, "inventory_2", ChatLinkKind.Package, ChatLinkResolver.Crates),
        site("huggingface", "Hugging Face", listOf("huggingface.co", "hf.co"), 0xFFB000, "face", ChatLinkKind.Code),
        site("codepen", "CodePen", listOf("codepen.io"), 0x1E1F26, "data_object", ChatLinkKind.Code, ChatLinkResolver.OEmbed("https://codepen.io/api/oembed?format=json&url=")),
        site("figma", "Figma", listOf("figma.com"), 0xF24E1E, "palette", ChatLinkKind.Document),
        site("notion", "Notion", listOf("notion.so", "notion.site"), 0x191919, "description", ChatLinkKind.Document),
        site("googledocs", "Google Docs", listOf("docs.google.com", "drive.google.com"), 0x4285F4, "description", ChatLinkKind.Document),
        site("dropbox", "Dropbox", listOf("dropbox.com", "db.tt"), 0x0061FF, "inventory", ChatLinkKind.Document),
        site("medium", "Medium", listOf("medium.com"), 0x000000, "notes", ChatLinkKind.Article),
        site("substack", "Substack", listOf("substack.com"), 0xFF6719, "mail", ChatLinkKind.Article),
        site("devto", "DEV", listOf("dev.to"), 0x0A0A0A, "notes", ChatLinkKind.Article),
        site("wikipedia", "Wikipedia", listOf("wikipedia.org"), 0x636466, "menu_book", ChatLinkKind.Reference, ChatLinkResolver.Wikipedia),
        site("arxiv", "arXiv", listOf("arxiv.org"), 0xB31B1B, "article", ChatLinkKind.Reference),
        site("imdb", "IMDb", listOf("imdb.com"), 0xF5C518, "movie", ChatLinkKind.Reference),
        site(
            "amazon", "Amazon",
            listOf("amazon.com", "amazon.co.uk", "amazon.de", "amazon.ca", "amazon.fr", "amazon.co.jp", "amzn.to", "a.co"),
            0xFF9900, "shopping_cart", ChatLinkKind.Shop,
        ),
        site("ebay", "eBay", listOf("ebay.com", "ebay.co.uk", "ebay.de"), 0xE53238, "sell", ChatLinkKind.Shop),
        site("etsy", "Etsy", listOf("etsy.com"), 0xF1641E, "shopping_bag", ChatLinkKind.Shop),
        site("appstore", "App Store", listOf("apps.apple.com", "itunes.apple.com"), 0x0D96F6, "apps", ChatLinkKind.App, ChatLinkResolver.AppStore),
        site("googleplay", "Google Play", listOf("play.google.com"), 0x01875F, "play_arrow", ChatLinkKind.App),
        site("steam", "Steam", listOf("store.steampowered.com", "steamcommunity.com"), 0x1B2838, "sports_esports", ChatLinkKind.App, ChatLinkResolver.Steam),
        site("producthunt", "Product Hunt", listOf("producthunt.com"), 0xDA552F, "star", ChatLinkKind.App),
        site(
            "googlemaps", "Google Maps", listOf("maps.google.com", "maps.app.goo.gl", "google.com/maps", "goo.gl/maps"),
            0x34A853, "map", ChatLinkKind.Map,
        ),
        site("applemaps", "Apple Maps", listOf("maps.apple.com"), 0x4CD964, "map", ChatLinkKind.Map),
        site("nytimes", "The New York Times", listOf("nytimes.com", "nyti.ms"), 0x000000, "newspaper", ChatLinkKind.Article),
        site("bbc", "BBC", listOf("bbc.com", "bbc.co.uk"), 0xBB1919, "newspaper", ChatLinkKind.Article),
        site("guardian", "The Guardian", listOf("theguardian.com"), 0x052962, "newspaper", ChatLinkKind.Article),
        site("theverge", "The Verge", listOf("theverge.com"), 0x5200FF, "newspaper", ChatLinkKind.Article),
        site("techcrunch", "TechCrunch", listOf("techcrunch.com"), 0x0A9E01, "newspaper", ChatLinkKind.Article),
        site("arstechnica", "Ars Technica", listOf("arstechnica.com"), 0xFF4E00, "newspaper", ChatLinkKind.Article),
        site("reuters", "Reuters", listOf("reuters.com"), 0xFF8000, "newspaper", ChatLinkKind.Article),
        site("xkcd", "xkcd", listOf("xkcd.com"), 0x96A8C8, "gesture", ChatLinkKind.Image, ChatLinkResolver.Xkcd),
        site("giphy", "GIPHY", listOf("giphy.com"), 0x9933FF, "auto_awesome", ChatLinkKind.Image),
        site("imgur", "Imgur", listOf("imgur.com"), 0x1BB76E, "photo", ChatLinkKind.Image),
        site("flickr", "Flickr", listOf("flickr.com", "flic.kr"), 0xFF0084, "camera", ChatLinkKind.Image, ChatLinkResolver.OEmbed("https://www.flickr.com/services/oembed/?format=json&url=")),
        site("discord", "Discord", listOf("discord.gg", "discord.com"), 0x5865F2, "sports_esports", ChatLinkKind.Chat),
        site("telegram", "Telegram", listOf("t.me", "telegram.me"), 0x26A5E4, "send", ChatLinkKind.Chat),
        site("forkmesh", "ForkMesh", listOf("forkmesh.com"), 0x7C5CFF, "hub", ChatLinkKind.Code),
    )

    private val prefixes = listOf("www.", "m.", "mobile.", "old.", "new.")

    fun bareHost(host: String): String {
        var bare = host.lowercase()
        if (bare.endsWith(".")) bare = bare.dropLast(1)
        for (prefix in prefixes) if (bare.startsWith(prefix)) bare = bare.substring(prefix.length)
        return bare
    }

    fun site(url: URI): ChatLinkSite? {
        val host = url.host ?: return null
        val path = url.path.orEmpty()
        return all.firstOrNull { it.matches(host, path) }
    }

    fun site(url: String): ChatLinkSite? = ChatLinkPreviewParsing.uri(url)?.let(::site)

    fun siteForKey(key: String): ChatLinkSite? = all.firstOrNull { it.key == key }
}
