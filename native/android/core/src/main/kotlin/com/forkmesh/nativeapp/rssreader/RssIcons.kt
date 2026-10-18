package com.forkmesh.nativeapp.rssreader

import java.util.Base64

data class RssIconLink(val href: String, val rel: List<String>, val sizes: String, val type: String)

class RssIconResult(val url: String, val bytes: ByteArray)

object RssIcons {
    const val RECHECK_MS = 30L * 24 * 60 * 60 * 1000
    const val RETRY_MS = 24L * 60 * 60 * 1000
    const val MIN_SIZE = 32
    const val MAX_SIZE = 192

    val PROXY_HOSTS = setOf(
        "feedburner.com", "feedproxy.google.com", "feedblitz.com", "feedpress.me", "rss.app", "rsshub.app",
        "fetchrss.com", "politepol.com", "feed43.com", "feedcat.net", "feeds.megaphone.fm", "feeds.simplecast.com",
        "feeds.buzzsprout.com", "feeds.transistor.fm", "rss.art19.com", "feeds.libsyn.com", "feeds.soundcloud.com",
        "anchor.fm", "rss.acast.com", "feeds.acast.com", "feeds.captivate.fm", "feeds.redcircle.com", "omnycontent.com",
        "feeds.podcastmirror.com", "pinecast.com", "feeds.fireside.fm", "rss.libsyn.com", "feeds.feedburner.com",
    )

    private val FEED_SUBDOMAINS = listOf("feeds.", "feed.", "rss.", "atom.")

    fun isProxy(host: String): Boolean {
        val name = host.lowercase().trimEnd('.')
        return name.isNotEmpty() && PROXY_HOSTS.any { name == it || name.endsWith(".$it") }
    }

    fun imageType(bytes: ByteArray): String {
        fun at(index: Int): Int = if (index < bytes.size) bytes[index].toInt() and 0xff else -1
        fun ascii(index: Int, text: String): Boolean = text.indices.all { at(index + it) == text[it].code }
        return when {
            at(0) == 0x89 && ascii(1, "PNG") -> "image/png"
            at(0) == 0xFF && at(1) == 0xD8 && at(2) == 0xFF -> "image/jpeg"
            ascii(0, "GIF8") -> "image/gif"
            ascii(0, "RIFF") && ascii(8, "WEBP") -> "image/webp"
            at(0) == 0 && at(1) == 0 && at(2) == 1 && at(3) == 0 && (at(4) > 0 || at(5) > 0) && bytes.size > 22 -> "image/x-icon"
            ascii(0, "BM") && bytes.size > 26 -> "image/bmp"
            else -> ""
        }
    }

    fun dataBytes(uri: String): ByteArray? {
        if (!uri.startsWith("data:image/", ignoreCase = true)) return null
        val comma = uri.indexOf(',')
        if (comma < 0) return null
        val meta = uri.substring(5, comma).lowercase()
        if (!meta.endsWith(";base64") || "svg" in meta) return null
        return runCatching { Base64.getMimeDecoder().decode(uri.substring(comma + 1)) }.getOrNull()?.takeIf { it.isNotEmpty() }
    }

    fun isVector(link: RssIconLink): Boolean {
        val path = RssUrls.split(link.href)?.path.orEmpty().lowercase()
        return "svg" in link.type.lowercase() || path.endsWith(".svg") || path.endsWith(".svgz") ||
            link.href.startsWith("data:image/svg", ignoreCase = true) || link.sizes.trim().equals("any", ignoreCase = true)
    }

    fun size(link: RssIconLink): Int {
        var best = 0
        for (token in link.sizes.lowercase().split(' ', ',').filter { it.isNotEmpty() }) {
            val parts = token.split('x')
            val width = parts.getOrNull(0)?.toIntOrNull() ?: continue
            val height = parts.getOrNull(1)?.toIntOrNull() ?: continue
            if (width != height || width <= 0) continue
            best = if (best == 0) width else closer(best, width)
        }
        if (best == 0 && link.rel.any { it.startsWith("apple-touch-icon") }) return 180
        return best
    }

    private fun closer(a: Int, b: Int): Int = if (score(a) <= score(b)) a else b

    private fun score(size: Int): Int = when {
        size in MIN_SIZE..MAX_SIZE -> MAX_SIZE - size
        size == 0 -> 1_000
        size > MAX_SIZE -> 2_000 + minOf(size, 50_000)
        else -> 60_000 + (MIN_SIZE - size)
    }

    fun rank(links: List<RssIconLink>): List<String> = links
        .filter { !isVector(it) && it.href.isNotEmpty() }
        .withIndex()
        .sortedWith(compareBy({ score(size(it.value)) }, { it.index }))
        .map { it.value.href }
        .distinct()

    fun origin(url: String): String = RssUrls.origin(url).takeIf { it.startsWith("http://") || it.startsWith("https://") }.orEmpty()

    fun isGuess(feed: RssFeed, url: String): Boolean {
        val parts = RssUrls.split(url) ?: return false
        if (parts.path != "/favicon.ico" || !parts.query.isNullOrEmpty()) return false
        val origin = parts.origin.lowercase()
        return origin == origin(feed.url).lowercase() || origin == origin(feed.siteUrl).lowercase()
    }

    fun declared(feed: RssFeed): String = feed.iconUrl.takeIf { it.isNotEmpty() && !isGuess(feed, it) }.orEmpty()

    fun homePages(feed: RssFeed, hint: String = ""): List<String> {
        val out = LinkedHashSet<String>()
        val site = RssUrls.httpUrl(feed.homePage).takeIf { !isProxy(RssUrls.host(it)) }.orEmpty()
        if (site.isNotEmpty()) {
            out.add(site)
            out.add(origin(site) + "/")
        } else if (!isProxy(RssUrls.host(feed.url))) {
            out.add(origin(feed.url) + "/")
        } else {
            val article = RssUrls.httpUrl(hint)
            if (article.isNotEmpty() && !isProxy(RssUrls.host(article))) out.add(origin(article) + "/")
        }
        return out.filter { it.length > 1 }.distinctBy { RssUrls.identity(it) }
    }

    fun fallbacks(feed: RssFeed, hint: String = "", seen: List<String> = emptyList()): List<String> {
        val homes = LinkedHashSet<String>()
        for (page in seen + homePages(feed, hint)) origin(page).takeIf { it.isNotEmpty() && !isProxy(RssUrls.host(it)) }?.let(homes::add)
        val feedOrigin = origin(feed.url).takeIf { !isProxy(RssUrls.host(feed.url)) }.orEmpty()
        val out = LinkedHashSet<String>()
        for (home in homes) out.add("$home/favicon.ico")
        if (feedOrigin.isNotEmpty()) out.add("$feedOrigin/favicon.ico")
        val host = RssUrls.host(feed.url)
        FEED_SUBDOMAINS.firstOrNull { host.startsWith(it) && host.count { c -> c == '.' } >= 2 }?.let { prefix ->
            if (!isProxy(host)) out.add("https://${host.removePrefix(prefix)}/favicon.ico")
        }
        for (home in homes) out.add("$home/apple-touch-icon.png")
        return out.toList()
    }
}
