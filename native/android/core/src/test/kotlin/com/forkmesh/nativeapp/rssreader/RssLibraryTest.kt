package com.forkmesh.nativeapp.rssreader

import com.forkmesh.nativeapp.git.withTempDir
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.io.IOException

class RssLibraryTest {
    private val now = 1_791_987_600_000L

    private class FakeHttp : RssHttp {
        val pages = HashMap<String, RssHttpResponse>()
        val requests = ArrayList<Pair<String, Map<String, String>>>()
        val failing = HashSet<String>()

        fun serve(url: String, body: String, type: String = "application/rss+xml", etag: String = "", finalUrl: String = url, permanent: Boolean = false) {
            pages[url] = RssHttpResponse(200, finalUrl, body.toByteArray(), type, etag, "", permanent)
        }

        override fun get(url: String, headers: Map<String, String>, maxBytes: Int): RssHttpResponse {
            synchronized(this) { requests.add(url to headers) }
            if (url in failing) throw IOException("unreachable")
            val page = pages[url] ?: return RssHttpResponse(404, url)
            if (page.etag.isNotEmpty() && headers["If-None-Match"] == page.etag) return RssHttpResponse(304, url)
            return page
        }
    }

    private fun rss(vararg items: Pair<String, Long>): String = buildString {
        append("<rss><channel><title>Example Engineering</title><link>https://blog.example.org/</link>")
        for ((guid, published) in items) {
            append("<item><guid>$guid</guid><title>Post $guid</title><link>https://blog.example.org/$guid</link>")
            append("<pubDate>${java.time.format.DateTimeFormatter.RFC_1123_DATE_TIME.format(java.time.Instant.ofEpochMilli(published).atOffset(java.time.ZoneOffset.UTC))}</pubDate>")
            append("<description>Summary $guid</description></item>")
        }
        append("</channel></rss>")
    }

    fun testDiscoverAddRefreshAndConditionalGet() {
        val http = FakeHttp()
        http.serve("https://blog.example.org", "<html><head><link rel=\"alternate\" type=\"application/rss+xml\" href=\"/feed.xml\"></head></html>", "text/html")
        http.serve("https://blog.example.org/feed.xml", rss("a" to now - 1000, "b" to now - 2000), etag = "\"v1\"")
        val fetcher = RssFetcher(http)
        val library = RssLibrary()
        val found = fetcher.discover("blog.example.org")
        assertEq("https://blog.example.org/feed.xml", found.outcome.url)
        val feed = library.addFetched(found.outcome, now = now)
        assertEq("Example Engineering", feed.displayTitle)
        assertEq(2, library.counts(now).unread)
        assertEq("\"v1\"", feed.etag)
        assertEq("feed_exists", assertFails<RssLibraryException> { library.addFetched(found.outcome, now = now) }.code)
        assertEq("feed_exists", assertFails<RssLibraryException> { library.subscribe("http://BLOG.example.org/feed.xml", now = now) }.code)
        assertEq(emptyList<RssFeed>(), library.due(now))
        val later = now + RssLibrary.FETCH_INTERVAL_MS
        assertEq(listOf(feed.id), library.due(later).map { it.id })
        val summary = RssRefresher(library, fetcher).refresh(library.due(later), { later })
        assertEq(RssRefreshSummary(1, 0, 0), summary)
        assertEq("\"v1\"", http.requests.last().second["If-None-Match"])
        assertEq(later, library.feed(feed.id)!!.lastSuccess)
    }

    fun testReadStarCountsAndPrune() {
        val http = FakeHttp()
        val library = RssLibrary()
        val fetcher = RssFetcher(http)
        val feed = library.subscribe("https://blog.example.org/feed.xml", now = now)
        val item = { index: Int -> "p$index" to now - index * 60_000L }
        for (batch in listOf(205 until 405, 5 until 205, 0 until 5)) {
            http.serve("https://blog.example.org/feed.xml", rss(*batch.map(item).toTypedArray()))
            library.applyFetch(feed.id, fetcher.fetch(library.feed(feed.id)!!), now)
        }
        assertEq(400, library.articles(RssScope.Feed(feed.id), now).size)
        val list = library.timeline(RssScope.Feed(feed.id), now)
        assertEq("Post p0", list.first().title)
        val oldest = list.last()
        library.setStarred(listOf(oldest.id), true)
        library.setRead(list.take(10).map { it.id }, true)
        val counts = library.counts(now)
        assertEq(390, counts.unread)
        assertEq(1, counts.starred)
        assertEq(390, counts.byFeed[feed.id])
        val today = library.timeline(RssScope.Today, now)
        assertEq(400, today.size)
        assertEq(11, library.timeline(RssScope.Today, now, query = "Post p39").size)
        http.serve("https://blog.example.org/feed.xml", rss("new1" to now + 1000, "new2" to now + 2000, item(0)))
        library.applyFetch(feed.id, fetcher.fetch(library.feed(feed.id)!!), now + 5000)
        val after = library.articles(RssScope.Feed(feed.id), now)
        expect(after.any { it.id == oldest.id && it.starred })
        assertEq(401, after.size)
        assertEq(true, library.article(list.first().id)!!.read)
        library.updateSettings { it.copy(sortOldestFirst = true) }
        assertEq(oldest.id, library.timeline(RssScope.Feed(feed.id), now).first().id)
    }

    fun testFailuresBackOffAndPermanentRedirectsRename() {
        val http = FakeHttp()
        val library = RssLibrary()
        val fetcher = RssFetcher(http)
        val feed = library.subscribe("https://old.example.org/rss", now = now)
        http.failing.add("https://old.example.org/rss")
        library.applyFetch(feed.id, fetcher.fetch(library.feed(feed.id)!!), now)
        var stored = library.feed(feed.id)!!
        assertEq("network_error", stored.error)
        assertEq(now + RssLibrary.FETCH_INTERVAL_MS, stored.nextCheck)
        library.applyFetch(feed.id, RssFetchOutcome.Failed("timeout"), now)
        assertEq(now + 2 * RssLibrary.FETCH_INTERVAL_MS, library.feed(feed.id)!!.nextCheck)
        assertEq(RssLibrary.MAX_BACKOFF_MS, RssLibrary.backoff(30))
        assertEq(1, library.failingFeeds().size)
        http.failing.clear()
        http.serve("https://old.example.org/rss", rss("x" to now), finalUrl = "https://new.example.org/feed.xml", permanent = true)
        library.applyFetch(feed.id, fetcher.fetch(library.feed(feed.id)!!), now)
        stored = library.feed(feed.id)!!
        assertEq("https://new.example.org/feed.xml", stored.url)
        assertEq(listOf("https://old.example.org/rss"), stored.previousUrls)
        assertEq(0, stored.errorCount)
        assertEq(feed.id, library.findFeed("https://old.example.org/rss")!!.id)
        http.serve("https://new.example.org/feed.xml", rss("x" to now), finalUrl = "https://temp.example.org/x", permanent = false)
        library.applyFetch(feed.id, fetcher.fetch(library.feed(feed.id)!!), now)
        assertEq("https://new.example.org/feed.xml", library.feed(feed.id)!!.url)
    }

    fun testCleartextUpgradesToHttps() {
        val http = FakeHttp()
        http.serve("https://plain.example.org/rss", rss("a" to now))
        val fetcher = RssFetcher(http) { false }
        val library = RssLibrary()
        val feed = library.subscribe("http://plain.example.org/rss", now = now)
        library.applyFetch(feed.id, fetcher.fetch(library.feed(feed.id)!!), now)
        val stored = library.feed(feed.id)!!
        assertEq("http://plain.example.org/rss", stored.url)
        assertEq(true, stored.viaHttps)
        assertEq("", stored.error)
        assertEq(listOf("https://plain.example.org/rss"), http.requests.map { it.first })
        val other = library.subscribe("http://only-http.example.org/rss", now = now)
        library.applyFetch(other.id, fetcher.fetch(library.feed(other.id)!!), now)
        assertEq("http_404", library.feed(other.id)!!.error)
        http.failing.add("https://down.example.org/rss")
        val down = library.subscribe("http://down.example.org/rss", now = now)
        library.applyFetch(down.id, fetcher.fetch(library.feed(down.id)!!), now)
        assertEq("http_only", library.feed(down.id)!!.error)
        expect(RssErrors.describe("http_only").contains("HTTP"))
    }

    fun testFoldersSidebarAndNextUnread() {
        val library = RssLibrary()
        val tech = library.addFolder("Tech")
        assertEq(tech, library.addFolder("tech"))
        val a = library.subscribe("https://a.example.org/rss", "Alpha Example", now = now)
        val b = library.subscribe("https://b.example.org/rss", "Beta Example", tech.id, now = now)
        val z = library.subscribe("https://z.example.org/rss", "Zeta Example", now = now)
        library.applyFetch(a.id, RssFetchOutcome.Fetched(a.url, false, RssParsedFeed(items = listOf(RssParsedItem(guid = "a1", title = "A1", published = now)))), now)
        library.applyFetch(b.id, RssFetchOutcome.Fetched(b.url, false, RssParsedFeed(items = listOf(RssParsedItem(guid = "b1", title = "B1", published = now)))), now)
        library.applyFetch(z.id, RssFetchOutcome.Fetched(z.url, false, RssParsedFeed(items = listOf(RssParsedItem(guid = "z1", title = "Z1", published = now), RssParsedItem(guid = "z2", title = "Z2", published = now - 1)))), now)
        val counts = library.counts(now)
        assertEq(1, counts.byFolder[tech.id])
        assertEq(listOf("Alpha Example", "Tech", "Zeta Example"), library.sidebar(false, counts).map { it.name })
        library.setRead(library.articles(RssScope.Feed(a.id), now).map { it.id }, true)
        assertEq(listOf("Tech", "Zeta Example"), library.sidebar(true, library.counts(now)).map { it.name })
        assertEq(listOf(a.id, b.id, z.id), library.feedOrder())
        val alpha = library.timeline(RssScope.Feed(a.id), now)
        val next = library.nextUnread(RssScope.Feed(a.id), alpha, alpha.first().id, now)!!
        assertEq(RssScope.Feed(b.id), next.first)
        val zeta = library.timeline(RssScope.Feed(z.id), now)
        assertEq(RssScope.Feed(z.id) to zeta[1].id, library.nextUnread(RssScope.Feed(z.id), zeta, zeta[0].id, now))
        library.renameFeed(z.id, "My Zeta")
        assertEq("My Zeta", library.feed(z.id)!!.displayTitle)
        library.moveFeed(z.id, tech.id)
        assertEq(3, library.counts(now).byFolder[tech.id])
        library.deleteFolder(tech.id)
        assertEq("", library.feed(b.id)!!.folderId)
        library.updateSettings { it.copy(groupByFeed = true) }
        assertEq(listOf("A1", "B1", "Z1", "Z2"), library.timeline(RssScope.Today, now).map { it.title })
        library.unsubscribe(z.id, now)
        assertEq(null, library.feed(z.id))
        assertEq(2, library.timeline(RssScope.Today, now).size)
        expect(library.activity().first().text.contains("My Zeta"))
        library.setRead(library.articles(RssScope.Unread, now).map { it.id }, true)
        assertEq(null, library.nextUnread(RssScope.Feed(b.id), library.timeline(RssScope.Feed(b.id), now), null, now))
    }

    fun testStoreRoundTrip() {
        withTempDir("rss-store") { dir ->
            val store = RssStore(dir)
            val library = store.load()
            val folder = library.addFolder("Reading")
            val feed = library.subscribe("https://blog.example.org/feed", "Example Engineering", folder.id, now = now)
            library.applyFetch(feed.id, RssFetchOutcome.Fetched(feed.url, false, RssParsedFeed(title = "Example Engineering", items = listOf(RssParsedItem(guid = "g1", title = "T", contentHtml = "<p>x</p>", published = now)))), now)
            val article = library.articles(RssScope.Feed(feed.id), now).single()
            library.setStarred(listOf(article.id), true)
            library.setReaderHtml(article.id, "<p>reader</p>")
            library.updateSettings { it.copy(theme = RssTheme.PAPER, timelineLines = 9) }
            store.save(library)
            val again = store.load()
            assertEq(folder.id, again.feed(feed.id)!!.folderId)
            val loaded = again.article(article.id)!!
            assertEq(true, loaded.starred)
            assertEq("<p>reader</p>", loaded.readerHtml)
            assertEq(RssTheme.PAPER, again.settings().theme)
            assertEq(4, again.settings().timelineLines)
            store.writeIcon(feed.id, byteArrayOf(1, 2, 3))
            again.unsubscribe(feed.id, now)
            store.save(again)
            assertEq(null, store.readIcon(feed.id))
            assertEq(null, store.load().feed(feed.id))
        }
    }

    fun testFutureAndMissingDatesBecomeFirstSeen() {
        val library = RssLibrary()
        val feed = library.subscribe("https://blog.example.org/feed", now = now)
        library.applyFetch(feed.id, RssFetchOutcome.Fetched(feed.url, false, RssParsedFeed(items = listOf(
            RssParsedItem(guid = "future", title = "F", published = now + 3 * RssLibrary.FUTURE_SLACK_MS),
            RssParsedItem(guid = "none", title = "N"),
            RssParsedItem(guid = "upd", title = "U", updated = now - 5000),
        ))), now)
        val byGuid = library.articles(RssScope.Feed(feed.id), now).associateBy { it.guid }
        assertEq(now, byGuid.getValue("future").published)
        assertEq(now, byGuid.getValue("none").published)
        assertEq(now - 5000, byGuid.getValue("upd").published)
    }
}
