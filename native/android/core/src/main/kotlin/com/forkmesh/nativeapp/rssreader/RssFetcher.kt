package com.forkmesh.nativeapp.rssreader

import java.io.ByteArrayOutputStream
import java.io.IOException
import java.io.InputStream
import java.net.HttpURLConnection
import java.net.SocketTimeoutException
import java.net.URL
import java.net.UnknownHostException
import java.nio.charset.Charset
import java.util.concurrent.Callable
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import javax.net.ssl.SSLException

data class RssHttpResponse(
    val status: Int,
    val url: String,
    val body: ByteArray = ByteArray(0),
    val contentType: String = "",
    val etag: String = "",
    val lastModified: String = "",
    val permanent: Boolean = false,
) {
    fun text(): String = RssCharsets.decode(body, contentType)
}

class RssFetchException(val code: String) : IOException(code)

fun interface RssHttp {
    fun get(url: String, headers: Map<String, String>, maxBytes: Int): RssHttpResponse
}

object RssCharsets {
    private val XML_ENCODING = Regex("<\\?xml[^>]*encoding=[\"']([A-Za-z0-9._-]+)[\"']")
    private val META_CHARSET = Regex("<meta[^>]+charset=[\"']?([A-Za-z0-9._-]+)", RegexOption.IGNORE_CASE)

    fun decode(body: ByteArray, contentType: String): String {
        if (body.size >= 3 && body[0] == 0xEF.toByte() && body[1] == 0xBB.toByte() && body[2] == 0xBF.toByte()) {
            return String(body, 3, body.size - 3, Charsets.UTF_8)
        }
        if (body.size >= 2 && body[0] == 0xFE.toByte() && body[1] == 0xFF.toByte()) return String(body, Charsets.UTF_16)
        if (body.size >= 2 && body[0] == 0xFF.toByte() && body[1] == 0xFE.toByte()) return String(body, Charsets.UTF_16)
        val declared = contentType.split(';').map { it.trim() }.firstOrNull { it.lowercase().startsWith("charset=") }
            ?.substringAfter('=')?.trim('"', '\'', ' ')
        val head = String(body, 0, minOf(body.size, 1024), Charsets.ISO_8859_1)
        val name = declared ?: XML_ENCODING.find(head)?.groupValues?.get(1) ?: META_CHARSET.find(head)?.groupValues?.get(1) ?: "UTF-8"
        val charset = runCatching { Charset.forName(name) }.getOrDefault(Charsets.UTF_8)
        return String(body, charset)
    }
}

class RssUrlConnectionHttp(
    private val userAgent: String = "ForkMesh-Reader/1.0",
    private val cleartextAllowed: (String) -> Boolean = { true },
) : RssHttp {
    override fun get(url: String, headers: Map<String, String>, maxBytes: Int): RssHttpResponse {
        var current = url
        var permanent = true
        var hops = 0
        while (true) {
            val connection = URL(current).openConnection() as HttpURLConnection
            try {
                connection.instanceFollowRedirects = false
                connection.connectTimeout = 15_000
                connection.readTimeout = 30_000
                connection.useCaches = false
                connection.setRequestProperty("User-Agent", userAgent)
                for ((name, value) in headers) connection.setRequestProperty(name, value)
                val status = connection.responseCode
                if (status in listOf(301, 302, 303, 307, 308)) {
                    val location = connection.getHeaderField("Location").orEmpty()
                    var next = RssUrls.httpUrl(location, current)
                    if (next.isEmpty() || hops >= 5) throw RssFetchException("too_many_redirects")
                    if (RssUrls.split(next)?.let { it.scheme == "http" && !cleartextAllowed(it.host) } == true) next = RssUrls.withScheme(next, "https")
                    if (status != 301 && status != 308) permanent = false
                    current = next
                    hops++
                    continue
                }
                val stream: InputStream? = if (status >= 400) connection.errorStream else connection.inputStream
                val body = if (status == 304 || status >= 400) ByteArray(0) else stream?.let { read(it, maxBytes) } ?: ByteArray(0)
                runCatching { stream?.close() }
                return RssHttpResponse(
                    status = status,
                    url = current,
                    body = body,
                    contentType = connection.contentType.orEmpty(),
                    etag = connection.getHeaderField("ETag").orEmpty(),
                    lastModified = connection.getHeaderField("Last-Modified").orEmpty(),
                    permanent = hops > 0 && permanent,
                )
            } finally {
                connection.disconnect()
            }
        }
    }

    private fun read(stream: InputStream, limit: Int): ByteArray = stream.use { input ->
        val out = ByteArrayOutputStream()
        val buffer = ByteArray(16 * 1024)
        var total = 0
        while (true) {
            val count = input.read(buffer)
            if (count < 0) break
            total += count
            if (total > limit) throw RssFetchException("too_large")
            out.write(buffer, 0, count)
        }
        out.toByteArray()
    }
}

data class RssFound(val outcome: RssFetchOutcome.Fetched, val icon: String = "")

class RssFetcher(
    private val http: RssHttp,
    private val cleartextAllowed: (String) -> Boolean = { true },
) {
    companion object {
        const val MAX_FEED_BYTES = 2 * 1024 * 1024
        const val MAX_PAGE_BYTES = 3 * 1024 * 1024
        const val MAX_ICON_BYTES = 256 * 1024
        const val FEED_ACCEPT = "application/rss+xml, application/atom+xml, application/feed+json, " +
            "application/json;q=0.9, application/xml;q=0.9, text/xml;q=0.9, text/html;q=0.8, */*;q=0.5"
        val GUESS_PATHS = listOf("/feed", "/rss", "/feed.xml", "/atom.xml", "/rss/")
        const val OPML_ACCEPT = "text/x-opml, application/xml;q=0.9, text/xml;q=0.9, */*;q=0.5"
    }

    data class Response(val response: RssHttpResponse, val viaHttps: Boolean)

    fun errorCode(error: Throwable): String = when (error) {
        is RssFetchException -> error.code
        is RssFeedException -> error.code
        is SocketTimeoutException -> "timeout"
        is UnknownHostException -> "dns_failed"
        is SSLException -> "tls_failed"
        is IOException -> if (error.message.orEmpty().contains("CLEARTEXT", ignoreCase = true)) "http_only" else "network_error"
        else -> "fetch_failed"
    }

    fun request(url: String, headers: Map<String, String>, maxBytes: Int): Response {
        val parts = RssUrls.split(url) ?: throw RssFetchException("invalid_url")
        if (parts.scheme == "http" && !cleartextAllowed(parts.host)) {
            val secure = RssUrls.withScheme(url, "https")
            val response = try {
                http.get(secure, headers, maxBytes)
            } catch (error: IOException) {
                if (error is RssFetchException && error.code == "too_large") throw error
                throw RssFetchException("http_only")
            }
            return Response(response, true)
        }
        return Response(http.get(url, headers, maxBytes), false)
    }

    private fun checked(url: String, headers: Map<String, String> = emptyMap(), maxBytes: Int = MAX_FEED_BYTES): Response {
        val result = request(url, mapOf("Accept" to FEED_ACCEPT) + headers, maxBytes)
        val status = result.response.status
        if (status == 304) return result
        if (status < 200 || status >= 300) throw RssFetchException("http_${if (status == 0) 502 else status}")
        return result
    }

    fun fetch(feed: RssFeed): RssFetchOutcome = try {
        val headers = LinkedHashMap<String, String>()
        if (feed.etag.isNotEmpty()) headers["If-None-Match"] = feed.etag
        if (feed.lastModified.isNotEmpty()) headers["If-Modified-Since"] = feed.lastModified
        val result = checked(feed.url, headers)
        val response = result.response
        if (response.status == 304) {
            RssFetchOutcome.NotModified(result.viaHttps)
        } else {
            val final = if (result.viaHttps && response.url == RssUrls.withScheme(feed.url, "https")) feed.url else response.url
            val parsed = RssFeedParser.parse(response.text(), final)
            RssFetchOutcome.Fetched(final, response.permanent, parsed, response.etag, response.lastModified, result.viaHttps)
        }
    } catch (error: Exception) {
        RssFetchOutcome.Failed(errorCode(error))
    }

    fun fetchNow(feed: RssFeed): RssFetchOutcome = fetch(feed.copy(etag = "", lastModified = ""))

    fun opml(input: String): String {
        val url = RssUrls.normalizeFeedInput(input)
        if (url.isEmpty()) throw RssFetchException("invalid_url")
        val result = request(url, mapOf("Accept" to OPML_ACCEPT), MAX_FEED_BYTES)
        val status = result.response.status
        if (status < 200 || status >= 300) throw RssFetchException("http_${if (status == 0) 502 else status}")
        return result.response.text()
    }

    private fun feedAt(candidate: String): RssFound? {
        val found = runCatching { checked(candidate) }.getOrNull() ?: return null
        if (found.response.status == 304) return null
        val parsed = runCatching { RssFeedParser.parse(found.response.text(), found.response.url) }.getOrNull() ?: return null
        return RssFound(RssFetchOutcome.Fetched(stored(candidate, found), false, parsed, found.response.etag, found.response.lastModified, found.viaHttps))
    }

    fun discover(input: String): RssFound {
        val url = RssUrls.normalizeFeedInput(input)
        if (url.isEmpty()) throw RssFetchException("invalid_url")
        for (candidate in RssFeedFinder.candidates(url)) feedAt(candidate)?.let { return it }
        if (RssFeedFinder.youTubeNeedsPage(url)) {
            val id = runCatching { RssFeedFinder.youTubeChannelId(page(url).text()) }.getOrDefault("")
            if (id.isNotEmpty()) feedAt(RssFeedFinder.channelFeed(id))?.let { return it }
        }
        val first = checked(url)
        val response = first.response
        val final = response.url
        val document = response.text()
        runCatching { RssFeedParser.parse(document, final) }.getOrNull()?.let { feed ->
            return RssFound(RssFetchOutcome.Fetched(stored(url, first), false, feed, response.etag, response.lastModified, first.viaHttps))
        }
        val links = RssPages.discover(document, final)
        var candidates = links.feeds.map { it.url }.take(3)
        if (candidates.isEmpty()) {
            val origin = RssUrls.origin(final)
            candidates = GUESS_PATHS.map { origin + it }
        }
        for (candidate in candidates) {
            val found = runCatching { checked(candidate) }.getOrNull() ?: continue
            val target = found.response.url
            val parsed = runCatching { RssFeedParser.parse(found.response.text(), target) }.getOrNull() ?: continue
            var feed = parsed
            if (feed.siteUrl.isEmpty() || feed.siteUrl == target) feed = feed.copy(siteUrl = final)
            if (feed.iconUrl.isEmpty() && links.icon.isNotEmpty()) feed = feed.copy(iconUrl = links.icon)
            return RssFound(RssFetchOutcome.Fetched(stored(candidate, found), false, feed, found.response.etag, found.response.lastModified, found.viaHttps), links.icon)
        }
        throw RssFetchException("no_feed_found")
    }

    private fun stored(requested: String, result: Response): String {
        val final = result.response.url
        return if (result.viaHttps && final == RssUrls.withScheme(requested, "https")) requested else final
    }

    fun page(url: String): RssHttpResponse {
        val result = request(url, mapOf("Accept" to "text/html,application/xhtml+xml;q=0.9,*/*;q=0.5"), MAX_PAGE_BYTES)
        val status = result.response.status
        if (status < 200 || status >= 300) throw RssFetchException("http_$status")
        return result.response
    }

    fun readerView(article: RssArticle): RssReadable {
        val link = article.link
        if (link.isEmpty()) throw RssFetchException("no_link")
        val response = page(link)
        return RssPages.readable(response.text(), response.url)
    }

    fun icon(feed: RssFeed): ByteArray? {
        val candidates = listOf(feed.iconUrl, RssUrls.favicon(feed.siteUrl.ifEmpty { feed.url })).filter { it.isNotEmpty() }.distinct()
        for (candidate in candidates) {
            val bytes = runCatching {
                val result = request(candidate, mapOf("Accept" to "image/*"), MAX_ICON_BYTES)
                result.response.takeIf { it.status in 200..299 && it.body.isNotEmpty() && !it.contentType.startsWith("text/") }?.body
            }.getOrNull()
            if (bytes != null) return bytes
        }
        return null
    }
}

data class RssRefreshSummary(val checked: Int, val failed: Int, val added: Int)

class RssRefresher(private val library: RssLibrary, private val fetcher: RssFetcher, private val threads: Int = 6) {
    fun refresh(feeds: List<RssFeed>, now: () -> Long, onEach: () -> Unit = {}): RssRefreshSummary {
        if (feeds.isEmpty()) return RssRefreshSummary(0, 0, 0)
        val pool = Executors.newFixedThreadPool(minOf(threads, feeds.size))
        var failed = 0
        var added = 0
        try {
            val tasks = feeds.map { feed ->
                pool.submit(Callable {
                    val outcome = fetcher.fetch(feed)
                    val count = library.applyFetch(feed.id, outcome, now())
                    onEach()
                    outcome to count
                })
            }
            for (task in tasks) {
                val (outcome, count) = task.get()
                if (outcome is RssFetchOutcome.Failed) failed++
                added += count
            }
        } finally {
            pool.shutdown()
            pool.awaitTermination(1, TimeUnit.MINUTES)
        }
        return RssRefreshSummary(feeds.size, failed, added)
    }
}

object RssErrors {
    fun describe(code: String): String = when {
        code.isEmpty() -> ""
        code == "invalid_url" -> "That doesn't look like a web address."
        code == "feed_exists" -> "You're already subscribed to this feed."
        code == "too_many_feeds" -> "You can follow up to ${RssLibrary.MAX_FEEDS} feeds."
        code == "too_many_folders" -> "You can have up to ${RssLibrary.MAX_FOLDERS} folders."
        code == "folder_exists" -> "A folder with that name already exists."
        code == "folder_name_required" -> "Give the folder a name."
        code == "no_feed_found" -> "No feed was found at that address."
        code == "not_a_feed" -> "The address didn't return an RSS, Atom or JSON feed."
        code == "empty_feed" -> "The feed was empty."
        code == "feed_unreadable" -> "The feed couldn't be read; it isn't valid XML."
        code == "opml_unreadable" -> "That file isn't a readable OPML subscription list."
        code == "too_large" -> "The response was larger than the reader accepts."
        code == "too_many_redirects" -> "The address redirected too many times."
        code == "timeout" -> "The site took too long to answer."
        code == "dns_failed" -> "The site's name couldn't be found."
        code == "tls_failed" -> "A secure connection couldn't be made."
        code == "http_only" -> "This feed is only available over unencrypted HTTP, which this app doesn't allow, and its HTTPS address didn't answer."
        code == "network_error" -> "The site couldn't be reached."
        code == "no_link" -> "This article has no web page."
        code == "invalid_regex" -> RssMatcher.INVALID_REGEX
        code == "pattern_required" -> "Enter the text to match."
        code == "too_many_filters" -> "You can have up to ${RssLibrary.MAX_FILTERS} filters."
        code == "too_many_smart_feeds" -> "You can have up to ${RssLibrary.MAX_SMART_FEEDS} smart feeds."
        code == "smart_feed_name_required" -> "Give the smart feed a name."
        code == "feed_not_found" -> "This feed is no longer subscribed."
        code.startsWith("http_") -> "The site answered with HTTP ${code.removePrefix("http_")}."
        else -> "Something went wrong ($code)."
    }
}
