package com.forkmesh.nativeapp.rssreader

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class RssIconsTest {
    private val now = 1_791_987_600_000L

    private val png = byteArrayOf(0x89.toByte(), 'P'.code.toByte(), 'N'.code.toByte(), 'G'.code.toByte(), 13, 10, 26, 10) + ByteArray(40)
    private val ico = byteArrayOf(0, 0, 1, 0, 1, 0) + ByteArray(40)

    private class FakeHttp : RssHttp {
        val pages = HashMap<String, RssHttpResponse>()
        val requests = ArrayList<String>()

        fun serve(url: String, body: ByteArray, type: String) {
            pages[url] = RssHttpResponse(200, url, body, type)
        }

        fun html(url: String, body: String) = serve(url, body.toByteArray(), "text/html; charset=utf-8")

        override fun get(url: String, headers: Map<String, String>, maxBytes: Int): RssHttpResponse {
            synchronized(this) { requests.add(url) }
            return pages[url] ?: RssHttpResponse(404, url)
        }
    }

    private fun feed(url: String, site: String = "", icon: String = "") = RssFeed(id = "f", url = url, siteUrl = site, iconUrl = icon)

    fun testRankPrefersSquareMidSizeIcons() {
        val links = listOf(
            RssIconLink("https://a.example/16.png", listOf("icon"), "16x16", "image/png"),
            RssIconLink("https://a.example/mask.svg", listOf("icon"), "any", "image/svg+xml"),
            RssIconLink("https://a.example/favicon.ico", listOf("shortcut", "icon"), "", ""),
            RssIconLink("https://a.example/512.png", listOf("icon"), "512x512", "image/png"),
            RssIconLink("https://a.example/multi.png", listOf("icon"), "16x16 32x32 64x64", "image/png"),
            RssIconLink("https://a.example/touch.png", listOf("apple-touch-icon"), "", ""),
            RssIconLink("https://a.example/wide.png", listOf("icon"), "300x100", "image/png"),
        )
        assertEq(
            listOf(
                "https://a.example/touch.png", "https://a.example/multi.png", "https://a.example/favicon.ico",
                "https://a.example/wide.png", "https://a.example/512.png", "https://a.example/16.png",
            ),
            RssIcons.rank(links),
        )
    }

    fun testPageIconsResolveAgainstBase() {
        val page = """<html><head><base href="https://cdn.example.org/assets/">
            <link rel="shortcut icon" href="favicon.ico">
            <link rel="apple-touch-icon-precomposed" sizes="152x152" href="/touch-152.png">
            <link rel="mask-icon" href="mask.svg">
            <link rel="icon" type="image/svg+xml" href="logo.svg">
            <link rel="icon" href="data:image/png;base64,iVBORw0KGgo=" sizes="32x32"></head><body></body></html>"""
        assertEq(
            listOf("https://cdn.example.org/touch-152.png", "data:image/png;base64,iVBORw0KGgo=", "https://cdn.example.org/assets/favicon.ico"),
            RssPages.icons(page, "https://www.example.org/blog/"),
        )
        assertEq("https://cdn.example.org/touch-152.png", RssPages.discover(page, "https://www.example.org/blog/").icon)
    }

    fun testImageSniffing() {
        assertEq("image/png", RssIcons.imageType(png))
        assertEq("image/x-icon", RssIcons.imageType(ico))
        assertEq("image/gif", RssIcons.imageType("GIF89a".toByteArray() + ByteArray(10)))
        assertEq("", RssIcons.imageType("<!DOCTYPE html><html></html>".toByteArray()))
        assertEq("", RssIcons.imageType(ByteArray(0)))
        expect(RssIcons.dataBytes("data:image/svg+xml;base64,PHN2Zz4=") == null)
        assertEq("image/png", RssIcons.imageType(RssIcons.dataBytes("data:image/png;base64,iVBORw0KGgoAAAAN")!!))
    }

    fun testIconFallsThroughBadCandidates() {
        val http = FakeHttp()
        http.html("https://blog.example.org/", """<html><head>
            <link rel="apple-touch-icon" href="/touch.png"><link rel="icon" sizes="32x32" href="/icon-32.png"></head></html>""")
        http.html("https://blog.example.org/touch.png", "<html>not found</html>")
        http.serve("https://blog.example.org/icon-32.png", png, "image/png")
        val result = RssFetcher(http).icon(feed("https://blog.example.org/feed.xml", "https://blog.example.org/", "https://blog.example.org/gone.png"))!!
        assertEq("https://blog.example.org/icon-32.png", result.url)
        assertEq(
            listOf("https://blog.example.org/gone.png", "https://blog.example.org/", "https://blog.example.org/touch.png", "https://blog.example.org/icon-32.png"),
            http.requests,
        )
    }

    fun testIconFallsBackToFaviconsInOrder() {
        val http = FakeHttp()
        http.html("https://www.example.org/news/", "<html><head><title>No icons</title></head></html>")
        http.html("https://www.example.org/favicon.ico", "<html>soft 404</html>")
        http.serve("https://feeds.example.org/favicon.ico", ico, "image/x-icon")
        val result = RssFetcher(http).icon(feed("https://feeds.example.org/news.rss", "https://www.example.org/news/"))!!
        assertEq("https://feeds.example.org/favicon.ico", result.url)
        assertEq(
            listOf("https://www.example.org/news/", "https://www.example.org/", "https://www.example.org/favicon.ico", "https://feeds.example.org/favicon.ico"),
            http.requests,
        )
        val none = FakeHttp()
        assertEq(null, RssFetcher(none).icon(feed("https://feeds.example.org/news.rss", "https://www.example.org/news/")))
        expect("https://www.example.org/apple-touch-icon.png" in none.requests)
        expect("https://example.org/favicon.ico" in none.requests)
    }

    fun testAcceptCanRejectAndFallThrough() {
        val http = FakeHttp()
        http.serve("https://blog.example.org/banner.png", png, "image/png")
        http.serve("https://blog.example.org/favicon.ico", ico, "image/x-icon")
        val result = RssFetcher(http).icon(feed("https://blog.example.org/feed", "https://blog.example.org/", "https://blog.example.org/banner.png")) { !it.contentEquals(png) }
        assertEq("https://blog.example.org/favicon.ico", result!!.url)
    }

    fun testFeedProxyUsesSiteOrArticleOrigin() {
        val proxied = feed("https://feeds.feedburner.com/ExampleBlog", "https://blog.example.org/")
        assertEq(listOf("https://blog.example.org/"), RssIcons.homePages(proxied))
        expect(RssIcons.fallbacks(proxied).none { "feedburner" in it })
        val bare = feed("https://feeds.feedburner.com/ExampleBlog")
        assertEq(listOf("https://blog.example.org/"), RssIcons.homePages(bare, "https://blog.example.org/2026/10/post.html"))
        assertEq(emptyList<String>(), RssIcons.homePages(bare, "https://feedproxy.google.com/~r/ExampleBlog/~3/abc"))
        assertEq(emptyList<String>(), RssIcons.homePages(feed("https://feeds.feedburner.com/x")))
        val http = FakeHttp()
        http.html("https://blog.example.org/", "<html><head><link rel=icon href=/i.png></head></html>")
        http.serve("https://blog.example.org/i.png", png, "image/png")
        assertEq("https://blog.example.org/i.png", RssFetcher(http).icon(bare, "https://blog.example.org/2026/10/post.html")!!.url)
        expect(http.requests.none { "feedburner" in it })
        assertEq(listOf("https://news.example.org/"), RssIcons.homePages(feed("https://news.example.org/rss")))
    }

    fun testLegacyFaviconGuessDoesNotBlockLookup() {
        val legacy = feed("https://blog.example.org/feed.xml", "https://blog.example.org/", "https://blog.example.org/favicon.ico")
        assertEq("", RssIcons.declared(legacy))
        assertEq("https://cdn.example.org/favicon.ico", RssIcons.declared(legacy.copy(iconUrl = "https://cdn.example.org/favicon.ico")))
        val http = FakeHttp()
        http.html("https://blog.example.org/", "<html><head><link rel=apple-touch-icon href=/touch.png></head></html>")
        http.serve("https://blog.example.org/touch.png", png, "image/png")
        http.serve("https://blog.example.org/favicon.ico", ico, "image/x-icon")
        assertEq("https://blog.example.org/touch.png", RssFetcher(http).icon(legacy)!!.url)
    }

    fun testIconCheckScheduleAndReset() {
        val library = RssLibrary()
        val feed = library.subscribe("https://blog.example.org/feed.xml", siteUrl = "https://blog.example.org/", now = now)
        assertEq("", feed.iconUrl)
        val cached = HashSet<String>()
        assertEq(listOf(feed.id), library.iconsDue(now) { it in cached }.map { it.id })
        library.iconChecked(feed.id, "", now)
        assertEq(emptyList<String>(), library.iconsDue(now + 1000) { it in cached }.map { it.id })
        assertEq(listOf(feed.id), library.iconsDue(now + RssIcons.RETRY_MS) { it in cached }.map { it.id })
        library.iconChecked(feed.id, "https://blog.example.org/touch.png", now)
        cached.add(feed.id)
        assertEq(emptyList<String>(), library.iconsDue(now + RssIcons.RETRY_MS) { it in cached }.map { it.id })
        assertEq(listOf(feed.id), library.iconsDue(now + RssIcons.RECHECK_MS) { it in cached }.map { it.id })
        val parsed = RssParsedFeed(format = "RSS", title = "Blog", siteUrl = "https://blog.example.org/", iconUrl = "https://blog.example.org/logo.png", items = emptyList())
        library.applyFetch(feed.id, RssFetchOutcome.Fetched("https://blog.example.org/feed.xml", false, parsed), now)
        assertEq(0L, library.feed(feed.id)!!.iconChecked)
        assertEq("https://blog.example.org/logo.png", library.feed(feed.id)!!.iconUrl)
        library.iconChecked(feed.id, "https://blog.example.org/logo.png", now)
        library.applyFetch(feed.id, RssFetchOutcome.Fetched("https://blog.example.org/feed.xml", false, parsed), now + 1)
        assertEq(now, library.feed(feed.id)!!.iconChecked)
        val restored = RssFeed.fromJson(com.forkmesh.nativeapp.json.Json.parseObject(com.forkmesh.nativeapp.json.Json.stringify(library.feed(feed.id)!!.toJson())))
        assertEq("https://blog.example.org/logo.png", restored.iconFound)
        assertEq(now, restored.iconChecked)
        val legacy = RssFeed(id = "old", url = "https://old.example.org/rss", iconUrl = "https://old.example.org/favicon.ico", iconChecked = now - 5)
        val old = RssLibrary(RssLibraryData(feeds = listOf(legacy)))
        old.applyFetch("old", RssFetchOutcome.Fetched("https://old.example.org/rss", false, parsed.copy(iconUrl = "", siteUrl = "")), now)
        assertEq("", old.feed("old")!!.iconUrl)
        assertEq(0L, old.feed("old")!!.iconChecked)
    }

    fun testCommentsLinkIsNotDuplicated() {
        val style = RssPageStyle(RssTheme.DEFAULT, RssFont.SANS, 17, false, "#3478f6", true)
        val article = RssArticle(
            id = "f-1", feedId = "f", guid = "g", title = "Show HN", url = "https://example.org/project",
            contentHtml = "<p>Article URL: <a href=\"https://example.org/project\">https://example.org/project</a></p>" +
                "<p>Comments URL: <a href=\"https://news.ycombinator.com/item?id=41&amp;p=1\">https://news.ycombinator.com/item?id=41&amp;p=1</a></p>",
            commentsUrl = "HTTPS://News.YCombinator.com/item/?id=41&amp;p=1",
        )
        expect(RssArticlePage.linksTo(article.contentHtml, article.commentsUrl))
        expect(!RssArticlePage.html(article, "Hacker News", style).contains("class=\"comments\""))
        val other = article.copy(commentsUrl = "https://news.ycombinator.com/item?id=42")
        expect(RssArticlePage.html(other, "Hacker News", style).contains("<p class=\"comments\"><a href=\"https://news.ycombinator.com/item?id=42\">Comments</a></p>"))
        expect(!RssArticlePage.linksTo(article.contentHtml, "https://example.org/project#comments"))
        assertEq(RssUrls.linkKey("https://Example.org:443/a/"), RssUrls.linkKey("http://example.org/a"))
    }

    fun testShareText() {
        val article = RssArticle(id = "f-1", feedId = "f", guid = "g", title = "  A   story ", url = "https://example.org/a")
        assertEq("A story\nhttps://example.org/a", RssFormat.shareText(article))
        assertEq("https://example.org/a", RssFormat.shareText(article.copy(title = "")))
        assertEq("Only title", RssFormat.shareText(article.copy(title = "Only title", url = "")))
    }
}
