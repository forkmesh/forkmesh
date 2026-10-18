package com.forkmesh.nativeapp.rssreader

import com.forkmesh.nativeapp.git.withTempDir
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.io.File

class RssRoadmapTest {
    private val now = 1_791_987_600_000L
    private val day = 24L * 60 * 60 * 1000

    private class Http : RssHttp {
        val pages = HashMap<String, RssHttpResponse>()
        val requests = ArrayList<Pair<String, Map<String, String>>>()

        fun serve(url: String, body: String, type: String = "application/rss+xml", etag: String = "") {
            pages[url] = RssHttpResponse(200, url, body.toByteArray(), type, etag)
        }

        override fun get(url: String, headers: Map<String, String>, maxBytes: Int): RssHttpResponse {
            synchronized(this) { requests.add(url to headers) }
            val page = pages[url] ?: return RssHttpResponse(404, url)
            if (page.etag.isNotEmpty() && headers["If-None-Match"] == page.etag) return RssHttpResponse(304, url)
            return page
        }
    }

    private val youTube = """<?xml version="1.0" encoding="UTF-8"?>
<feed xmlns:yt="http://www.youtube.com/xml/schemas/2015" xmlns:media="http://search.yahoo.com/mrss/" xmlns="http://www.w3.org/2005/Atom">
 <link rel="self" href="http://www.youtube.com/feeds/videos.xml?channel_id=UCabcdefghijklmnopqrstuv"/>
 <id>yt:channel:abcdefghijklmnopqrstuv</id>
 <yt:channelId>abcdefghijklmnopqrstuv</yt:channelId>
 <title>Example Channel</title>
 <link rel="alternate" href="https://www.youtube.com/channel/UCabcdefghijklmnopqrstuv"/>
 <author>
  <name>Example Channel</name>
  <uri>https://www.youtube.com/channel/UCabcdefghijklmnopqrstuv</uri>
 </author>
 <published>2015-03-01T00:00:00+00:00</published>
 <entry>
  <id>yt:video:aBcDeFgHiJk</id>
  <yt:videoId>aBcDeFgHiJk</yt:videoId>
  <yt:channelId>UCabcdefghijklmnopqrstuv</yt:channelId>
  <title>Building a reader</title>
  <link rel="alternate" href="https://www.youtube.com/watch?v=aBcDeFgHiJk"/>
  <author>
   <name>Example Channel</name>
   <uri>https://www.youtube.com/channel/UCabcdefghijklmnopqrstuv</uri>
  </author>
  <published>2026-10-14T16:00:06+00:00</published>
  <updated>2026-10-14T16:05:00+00:00</updated>
  <media:group>
   <media:title>Building a reader</media:title>
   <media:content url="https://www.youtube.com/v/aBcDeFgHiJk?version=3" type="application/x-shockwave-flash" width="640" height="390"/>
   <media:thumbnail url="https://i2.ytimg.com/vi/aBcDeFgHiJk/hqdefault.jpg" width="480" height="360"/>
   <media:description>In this video we build a reader &amp; test it.
Second line

More at https://example.org/more.</media:description>
   <media:community>
    <media:starRating count="12" average="5.00" min="1" max="5"/>
    <media:statistics views="345"/>
   </media:community>
  </media:group>
 </entry>
</feed>"""

    private val mastodon = """<?xml version="1.0" encoding="UTF-8"?>
<rss version="2.0" xmlns:webfeeds="http://webfeeds.org/rss/1.0" xmlns:media="http://search.yahoo.com/mrss/">
  <channel>
    <title>Ada Example</title>
    <description>Public posts from @ada@social.example</description>
    <link>https://social.example/@ada</link>
    <language>en</language>
    <generator>Mastodon v4.3.0</generator>
    <item>
      <guid isPermaLink="true">https://social.example/@ada/113300000000000001</guid>
      <link>https://social.example/@ada/113300000000000001</link>
      <pubDate>Wed, 14 Oct 2026 14:20:00 +0000</pubDate>
      <description>&lt;p&gt;Look at this chart&lt;/p&gt;</description>
      <media:content url="https://files.social.example/media/chart.png" type="image/png" fileSize="48213" medium="image">
        <media:rating scheme="urn:simple">nonadult</media:rating>
        <media:description type="plain">A bar chart</media:description>
      </media:content>
      <category>charts</category>
    </item>
  </channel>
</rss>"""

    private fun style(images: Boolean = true) = RssPageStyle(RssTheme.DEFAULT, RssFont.SANS, 17, false, "#2F7CF6", images)

    private fun item(guid: String, title: String = "Post $guid", published: Long = now, url: String = "", author: String = "", content: String = "") =
        RssParsedItem(guid = guid, title = title, url = url, author = author, published = published, contentHtml = content)

    private fun fill(library: RssLibrary, feed: RssFeed, vararg items: RssParsedItem, at: Long = now) =
        library.applyFetch(feed.id, RssFetchOutcome.Fetched(feed.url, false, RssParsedFeed(items = items.toList())), at)

    fun testYouTubeMediaGroupGivesThumbnailAndDescription() {
        val feed = RssFeedParser.parse(youTube, "https://www.youtube.com/feeds/videos.xml?channel_id=UCabcdefghijklmnopqrstuv")
        assertEq("Atom 1.0", feed.format)
        val entry = feed.items.single()
        assertEq("Building a reader", entry.title)
        assertEq(listOf("https://i2.ytimg.com/vi/aBcDeFgHiJk/hqdefault.jpg"), entry.images)
        assertEq(
            "<p>In this video we build a reader &amp; test it.<br>Second line</p><p>More at <a href=\"https://example.org/more\">https://example.org/more</a>.</p>",
            entry.contentHtml,
        )
        expect(entry.enclosures.isEmpty())
        val library = RssLibrary()
        val stored = library.subscribe("https://www.youtube.com/feeds/videos.xml?channel_id=UCabcdefghijklmnopqrstuv", now = now)
        library.applyFetch(stored.id, RssFetchOutcome.Fetched(stored.url, false, feed), now)
        val article = library.articles(RssScope.Feed(stored.id), now).single()
        assertEq("https://i2.ytimg.com/vi/aBcDeFgHiJk/hqdefault.jpg", article.image)
        val body = RssArticlePage.body(article, false, true)
        expect(body.startsWith("<p><img src=\"https://i2.ytimg.com/vi/aBcDeFgHiJk/hqdefault.jpg\""), body)
        expect(body.contains("In this video we build a reader"))
        val offline = RssArticlePage.body(article, false, false)
        expect(!offline.contains("<img"))
        expect(offline.contains("<a href=\"https://i2.ytimg.com/vi/aBcDeFgHiJk/hqdefault.jpg\">Image</a>"))
    }

    fun testMastodonImageContent() {
        val feed = RssFeedParser.parse(mastodon, "https://social.example/@ada.rss")
        assertEq("RSS 2.0", feed.format)
        assertEq("en", feed.language)
        val entry = feed.items.single()
        assertEq(listOf("https://files.social.example/media/chart.png"), entry.images)
        assertEq("<p>Look at this chart</p>", entry.contentHtml)
        val library = RssLibrary()
        val stored = library.subscribe("https://social.example/@ada.rss", now = now)
        library.applyFetch(stored.id, RssFetchOutcome.Fetched(stored.url, false, feed), now)
        val article = library.articles(RssScope.Feed(stored.id), now).single()
        assertEq("https://files.social.example/media/chart.png", article.image)
        assertEq("en", library.feed(stored.id)!!.language)
        assertEq("", article.language)
        val page = RssArticlePage.html(article, "Ada Example", style(), feedLanguage = "en")
        expect(page.contains("<img src=\"https://files.social.example/media/chart.png\""))
        expect(page.contains("<html lang=\"en\" dir=\"auto\">"))
    }

    fun testMediaFallbacksAndImagePriority() {
        val rss = """<rss version="2.0" xmlns:media="http://search.yahoo.com/mrss/"><channel><title>T</title>
            <item><guid>a</guid><media:title>Media title</media:title><media:description type="html">&lt;p&gt;Rich&lt;/p&gt;</media:description>
              <enclosure url="https://cdn.example.org/e.jpg" type="image/jpeg" length="10"/>
              <enclosure url="https://cdn.example.org/a.mp3" type="audio/mpeg" length="10"/>
              <media:content url="https://cdn.example.org/c.jpg" type="image/jpeg"/>
              <media:content url="https://cdn.example.org/v.mp4" type="video/mp4"/>
              <media:thumbnail url="https://cdn.example.org/t.jpg"/></item>
            <item><guid>b</guid><title>Own</title><description>Desc</description><media:description>Ignored</media:description></item>
            </channel></rss>"""
        val items = RssFeedParser.parse(rss).items
        assertEq("Media title", items[0].title)
        assertEq("<p>Rich</p>", items[0].contentHtml)
        assertEq(listOf("https://cdn.example.org/t.jpg", "https://cdn.example.org/c.jpg", "https://cdn.example.org/e.jpg"), items[0].images)
        assertEq(listOf("https://cdn.example.org/e.jpg", "https://cdn.example.org/a.mp3", "https://cdn.example.org/v.mp4"), items[0].enclosures.map { it.url })
        assertEq("Desc", items[1].contentHtml)
        val library = RssLibrary()
        val feed = library.subscribe("https://cdn.example.org/rss", now = now)
        library.applyFetch(feed.id, RssFetchOutcome.Fetched(feed.url, false, RssParsedFeed(items = items)), now)
        val article = library.articles(RssScope.Feed(feed.id), now).first { it.guid == "a" }
        assertEq("https://cdn.example.org/t.jpg", article.image)
        val page = RssArticlePage.html(article, "T", style())
        expect(!page.contains("Download image"))
        expect(page.contains("<audio controls"))
        val json = """{"version":"https://jsonfeed.org/version/1.1","title":"J","language":"fr","items":[
            {"id":"1","content_html":"<p><img src=\"https://cdn.example.org/inline.png\"></p>","image":"https://cdn.example.org/j.png","banner_image":"https://cdn.example.org/b.png"},
            {"id":"2","content_html":"<p><img src=\"https://cdn.example.org/inline.png\"></p>","banner_image":"https://cdn.example.org/b.png","language":"ar"},
            {"id":"3","content_html":"<p><img src=\"https://cdn.example.org/inline.png\"></p>"}]}"""
        val parsed = RssFeedParser.parse(json, "https://cdn.example.org/feed.json")
        assertEq("JSON Feed 1.1", parsed.format)
        assertEq("fr", parsed.language)
        val jsonFeed = library.subscribe("https://cdn.example.org/feed.json", now = now)
        library.applyFetch(jsonFeed.id, RssFetchOutcome.Fetched(jsonFeed.url, false, parsed), now)
        val byGuid = library.articles(RssScope.Feed(jsonFeed.id), now).associateBy { it.guid }
        assertEq("https://cdn.example.org/j.png", byGuid.getValue("1").image)
        assertEq("https://cdn.example.org/b.png", byGuid.getValue("2").image)
        assertEq("https://cdn.example.org/inline.png", byGuid.getValue("3").image)
        assertEq("ar", byGuid.getValue("2").language)
        assertEq("", byGuid.getValue("1").language)
        assertEq("fr", library.feed(jsonFeed.id)!!.language)
        assertEq("RSS 1.0", RssFeedParser.parse("<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\" xmlns=\"http://purl.org/rss/1.0/\"><channel><title>R</title></channel></rdf:RDF>").format)
    }

    fun testPlainTextToHtml() {
        assertEq("", RssText.plainToHtml("  \n "))
        assertEq("<p>a &lt;b&gt;<br>c</p><p>d</p>", RssText.plainToHtml("a <b>\r\nc\n  \n\nd"))
        assertEq("<p>see <a href=\"https://x.example/a?b=1&amp;c=2\">https://x.example/a?b=1&amp;c=2</a>, ok</p>", RssText.plainToHtml("see https://x.example/a?b=1&c=2, ok"))
    }

    fun testInstapaperIgnoreIsDropped() {
        val html = "<p>Keep</p><div class=\"share instapaper_ignore\"><div>Share <b>this</b></div></div><p>After</p>" +
            "<img class=\"instapaper_ignore\" src=\"https://x.example/i.png\"><span class=\"instapaper_ignored\">stays</span>"
        assertEq("<p>Keep</p><p>After</p><span>stays</span>", RssSanitizer.sanitize(html))
    }

    fun testAltCaptions() {
        val html = RssSanitizer.sanitize(
            "<p><img src=\"https://x.example/a.png\" alt=\"A cat\"></p>" +
                "<figure><img src=\"https://x.example/b.png\" alt=\"Has caption\"><figcaption>Caption</figcaption></figure>" +
                "<figure><img src=\"https://x.example/c.png\" alt=\"No caption\"></figure>" +
                "<img src=\"https://x.example/d.png\" alt=\"\">",
        )
        val captioned = RssSanitizer.withAltCaptions(html)
        expect(captioned.contains("referrerpolicy=\"no-referrer\"><span class=\"alt-caption\">A cat</span></p>"), captioned)
        expect(!captioned.contains(">Has caption</span>"))
        expect(captioned.contains("<span class=\"alt-caption\">No caption</span></figure>"))
        assertEq(2, Regex("alt-caption").findAll(captioned).count())
        val article = RssArticle("f-1", "f", "1", contentHtml = html)
        expect(!RssArticlePage.body(article, false, false).contains("alt-caption"))
        expect(RssArticlePage.body(article, false, true).contains("alt-caption"))
    }

    fun testLanguageDirectionAndCodeStyle() {
        assertEq("rtl", RssRules.direction("ar"))
        assertEq("rtl", RssRules.direction("he-IL"))
        assertEq("rtl", RssRules.direction("ckb"))
        assertEq("auto", RssRules.direction("en-US"))
        assertEq("auto", RssRules.direction(""))
        assertEq("pt-BR", RssRules.language(" pt_BR "))
        val atom = """<feed xmlns="http://www.w3.org/2005/Atom" xml:lang="en"><title>A</title>
            <entry xml:lang="fa"><id>1</id><title>T</title><content type="html">&lt;p&gt;x&lt;/p&gt;</content></entry>
            <entry><id>2</id><title>U</title></entry></feed>"""
        val parsed = RssFeedParser.parse(atom)
        assertEq("en", parsed.language)
        assertEq(listOf("fa", ""), parsed.items.map { it.language })
        val article = RssArticle("f-1", "f", "1", title = "T", contentHtml = "<pre><code>x</code></pre>", language = "fa")
        val page = RssArticlePage.html(article, "A", style(), feedLanguage = "en")
        expect(page.contains("<html lang=\"fa\" dir=\"rtl\">"))
        expect(page.contains("pre { white-space: pre-wrap; overflow-x: hidden;"))
        expect(page.contains("touch-action: pan-y"))
        expect(page.contains("code { white-space: pre-wrap; }"))
        val plain = RssArticlePage.html(article.copy(language = ""), "A", style())
        expect(plain.contains("<html dir=\"auto\">"))
    }

    fun testReadingTime() {
        assertEq(0, RssRules.readingMinutes("   "))
        assertEq(1, RssRules.readingMinutes("one two"))
        assertEq(1, RssRules.readingMinutes(List(230) { "w" }.joinToString(" ")))
        assertEq(2, RssRules.readingMinutes(List(231) { "w" }.joinToString(" ")))
        assertEq(1, RssRules.readingMinutes("漢".repeat(500)))
        assertEq(2, RssRules.readingMinutes("漢".repeat(500) + " word"))
        assertEq(3, RssRules.readingMinutes("ひ".repeat(400) + " " + List(300) { "w" }.joinToString(" ")))
        val article = RssArticle("f-1", "f", "1", title = "T", contentHtml = "<p>${List(460) { "word" }.joinToString(" ")}</p>", published = now)
        val page = RssArticlePage.html(article, "A", style())
        expect(page.contains(" · 2 min read</div>"), page.substringAfter("class=\"date\"").take(80))
        expect(!RssArticlePage.html(article.copy(contentHtml = ""), "A", style()).contains("min read"))
    }

    fun testFiltersHideMarkReadAndStarredExemption() {
        val library = RssLibrary()
        val a = library.subscribe("https://a.example.org/rss", "Alpha", now = now)
        val b = library.subscribe("https://b.example.org/rss", "Beta", now = now)
        fill(library, a, item("1", "Sponsored: buy now"), item("2", "Real news", content = "<p>contains crypto words</p>"), item("3", "Weekly", author = "Bot Account"))
        fill(library, b, item("4", "Sponsored too"))
        assertEq(4, library.counts(now).unread)
        assertEq(RssMatcher.INVALID_REGEX, RssErrors.describe(assertFails<RssLibraryException> { library.saveFilter(RssFilter("", pattern = "([", regex = true)) }.code))
        expect(!RssMatcher.valid("([", true))
        expect(!RssMatcher("([", true).matches("(["))
        val hide = library.saveFilter(RssFilter("", feedId = a.id, field = RssFilterField.TITLE, pattern = "SPONSORED", action = RssFilterAction.HIDE))
        assertEq(listOf("Real news", "Weekly"), library.timeline(RssScope.Feed(a.id), now).map { it.title }.sorted())
        assertEq(3, library.counts(now).unread)
        assertEq(2, library.counts(now).byFeed[a.id])
        assertEq(1, library.timeline(RssScope.Feed(b.id), now).size)
        library.saveFilter(RssFilter("", field = RssFilterField.TITLE_OR_TEXT, pattern = "cry?pto", regex = true))
        assertEq(2, library.counts(now).unread)
        library.saveFilter(RssFilter("", field = RssFilterField.AUTHOR, pattern = "^bot", regex = true))
        assertEq(1, library.counts(now).unread)
        val hidden = library.articles(RssScope.Feed(b.id), now).single()
        val sponsored = library.article(RssLibrary.articleId(a.id, "1"))!!
        library.setStarred(listOf(sponsored.id), true)
        expect(library.timeline(RssScope.Feed(a.id), now).any { it.id == sponsored.id })
        library.deleteFilter(hide.id)
        assertEq(listOf(hidden.id), library.articles(RssScope.Feed(b.id), now).map { it.id })
        library.saveFilter(RssFilter("", pattern = "deal", action = RssFilterAction.MARK_READ))
        fill(library, b, item("5", "Big deal today"), item("6", "Other"))
        assertEq(true, library.article(RssLibrary.articleId(b.id, "5"))!!.read)
        assertEq(false, library.article(RssLibrary.articleId(b.id, "6"))!!.read)
        library.setRead(listOf(RssLibrary.articleId(b.id, "6")), true)
        library.saveFilter(RssFilter("", pattern = "Other", action = RssFilterAction.MARK_READ))
        library.setRead(listOf(RssLibrary.articleId(b.id, "6")), false)
        fill(library, b, item("5", "Big deal today"), item("6", "Other"))
        assertEq(false, library.article(RssLibrary.articleId(b.id, "6"))!!.read)
        repeat(RssLibrary.MAX_FILTERS - library.filters().size) { library.saveFilter(RssFilter("", pattern = "p$it")) }
        assertEq("too_many_filters", assertFails<RssLibraryException> { library.saveFilter(RssFilter("", pattern = "x")) }.code)
        library.unsubscribe(a.id, now)
        expect(library.filters().none { it.feedId == a.id })
    }

    fun testDuplicateLinksFold() {
        val library = RssLibrary()
        val a = library.subscribe("https://a.example.org/rss", now = now)
        val b = library.subscribe("https://b.example.org/rss", now = now)
        fill(library, a, item("1", url = "https://news.example.org/story?id=1#top"))
        fill(library, b, item("x", url = "http://NEWS.example.org/story?id=1"))
        assertEq(false, library.article(RssLibrary.articleId(b.id, "x"))!!.read)
        library.updateSettings { it.copy(foldDuplicates = true) }
        fill(library, b, item("y", url = "http://news.example.org/story?id=1#comments"), item("z", url = "https://news.example.org/story?id=2"))
        assertEq(true, library.article(RssLibrary.articleId(b.id, "y"))!!.read)
        assertEq(false, library.article(RssLibrary.articleId(b.id, "z"))!!.read)
        fill(library, a, item("2", url = "https://news.example.org/story?id=2"), item("3", url = "https://news.example.org/story?id=3"))
        assertEq(true, library.article(RssLibrary.articleId(a.id, "2"))!!.read)
        assertEq(false, library.article(RssLibrary.articleId(a.id, "3"))!!.read)
    }

    fun testSmartFeeds() {
        val library = RssLibrary()
        val folder = library.addFolder("Tech")
        val a = library.subscribe("https://a.example.org/rss", "Alpha", folder.id, now = now)
        val b = library.subscribe("https://b.example.org/rss", "Beta", now = now)
        fill(library, a, item("1", "Kotlin 3 released", published = now - 2 * day), item("2", "Swift news", published = now - 10 * day))
        fill(library, b, item("3", "Kotlin tips", content = "<p>about Swift</p>"), item("4", "Cooking"))
        val any = library.saveSmartFeed(RssSmartFeed("", "Everything"))
        val words = library.saveSmartFeed(RssSmartFeed("", "swift", words = "SWIFT"))
        val regex = library.saveSmartFeed(RssSmartFeed("", "Kotlin in Tech", words = "^kotlin", regex = true, scope = "folder:${folder.id}"))
        val recent = library.saveSmartFeed(RssSmartFeed("", "Recent Alpha", scope = "feed:${a.id}", maxAgeDays = 7))
        val starred = library.saveSmartFeed(RssSmartFeed("", "Starred unread", unreadOnly = true, starredOnly = true))
        assertEq(listOf("Everything", "Kotlin in Tech", "Recent Alpha", "Starred unread", "swift"), library.smartFeeds().map { it.name })
        assertEq(RssMatcher.INVALID_REGEX, RssErrors.describe(assertFails<RssLibraryException> { library.saveSmartFeed(RssSmartFeed("", "Bad", words = "(", regex = true)) }.code))
        assertEq("smart_feed_name_required", assertFails<RssLibraryException> { library.saveSmartFeed(RssSmartFeed("", " ")) }.code)
        assertEq(listOf("Kotlin tips", "Swift news"), library.timeline(RssScope.Smart(words.id), now).map { it.title }.sorted())
        assertEq(listOf("Kotlin 3 released"), library.timeline(RssScope.Smart(regex.id), now).map { it.title })
        assertEq(listOf("Kotlin 3 released"), library.timeline(RssScope.Smart(recent.id), now).map { it.title })
        assertEq(4, library.timeline(RssScope.Smart(any.id), now).size)
        val tips = RssLibrary.articleId(b.id, "3")
        library.setStarred(listOf(tips), true)
        assertEq(listOf(tips), library.timeline(RssScope.Smart(starred.id), now).map { it.id })
        var counts = library.counts(now)
        assertEq(4, counts.bySmartFeed[any.id])
        assertEq(2, counts.bySmartFeed[words.id])
        assertEq(2, counts.of(RssScope.Smart(words.id)))
        library.setRead(listOf(tips), true)
        assertEq(emptyList<RssArticle>(), library.timeline(RssScope.Smart(starred.id), now))
        library.saveFilter(RssFilter("", pattern = "Cooking"))
        assertEq(2, library.counts(now).bySmartFeed[any.id])
        library.setFeedSwitches(b.id, paused = false, excludeFromToday = false, excludeFromUnread = true)
        counts = library.counts(now)
        assertEq(1, counts.bySmartFeed[words.id])
        library.saveSmartFeed(words.copy(name = "Swift Things"))
        assertEq("Swift Things", library.smartFeed(words.id)!!.name)
        library.deleteSmartFeed(any.id)
        assertEq(null, library.smartFeed(any.id))
        assertEq(emptyList<RssArticle>(), library.timeline(RssScope.Smart(any.id), now))
        repeat(RssLibrary.MAX_SMART_FEEDS - library.smartFeeds().size) { library.saveSmartFeed(RssSmartFeed("", "S$it")) }
        assertEq("too_many_smart_feeds", assertFails<RssLibraryException> { library.saveSmartFeed(RssSmartFeed("", "One more")) }.code)
        assertEq(RssScope.Smart("x"), RssScope.of(RssScope.Smart("x").key))
        assertEq(RssScope.ReadLater, RssScope.of("readLater"))
    }

    fun testReadLaterIsKeptByPruning() {
        val library = RssLibrary()
        val feed = library.subscribe("https://a.example.org/rss", now = now)
        fill(library, feed, *Array(RssLibrary.MAX_ARTICLES_PER_FEED) { item("p$it", published = now - it * 60_000L - 1000) })
        val oldest = RssLibrary.articleId(feed.id, "p${RssLibrary.MAX_ARTICLES_PER_FEED - 1}")
        val other = RssLibrary.articleId(feed.id, "p${RssLibrary.MAX_ARTICLES_PER_FEED - 2}")
        assertEq(0, library.counts(now).readLaterTotal)
        library.setReadLater(listOf(oldest), true)
        library.setRead(listOf(oldest), true)
        var counts = library.counts(now)
        assertEq(0, counts.readLater)
        assertEq(1, counts.readLaterTotal)
        assertEq(listOf(oldest), library.timeline(RssScope.ReadLater, now).map { it.id })
        library.setRead(listOf(oldest), false)
        assertEq(1, library.counts(now).readLater)
        fill(library, feed, item("n1", published = now + 1), item("n2", published = now + 2))
        expect(library.article(oldest) != null)
        expect(library.article(other) == null)
        assertEq(true, library.article(oldest)!!.readLater)
        library.setReadLater(listOf(oldest), false)
        assertEq(0, library.counts(now).readLaterTotal)
    }

    fun testDiscoveryRewrites() {
        val feed = RssFeedFinder.YOUTUBE_FEED
        assertEq("$feed?channel_id=UCabcdefghijklmnopqrstuv", RssFeedFinder.youTubeFeed("https://www.youtube.com/channel/UCabcdefghijklmnopqrstuv/videos"))
        assertEq("$feed?channel_id=UCabcdefghijklmnopqrstuv", RssFeedFinder.youTubeFeed("https://m.youtube.com/channel/UCabcdefghijklmnopqrstuv"))
        assertEq("$feed?playlist_id=PL123-abc_X", RssFeedFinder.youTubeFeed("https://youtube.com/playlist?list=PL123-abc_X&si=zz"))
        assertEq("$feed?user=someone", RssFeedFinder.youTubeFeed("https://www.youtube.com/user/someone"))
        assertEq("", RssFeedFinder.youTubeFeed("https://www.youtube.com/@handle"))
        assertEq("", RssFeedFinder.youTubeFeed("https://www.youtube.com/channel/notachannel"))
        assertEq("", RssFeedFinder.youTubeFeed("https://notyoutube.com/channel/UCabcdefghijklmnopqrstuv"))
        for (page in listOf("https://www.youtube.com/@handle", "https://youtube.com/c/Name", "https://www.youtube.com/watch?v=aBcDeFgHiJk",
            "https://youtu.be/aBcDeFgHiJk", "https://www.youtube.com/shorts/aBcDeFgHiJk")) {
            expect(RssFeedFinder.youTubeNeedsPage(page), page)
        }
        expect(!RssFeedFinder.youTubeNeedsPage("https://www.youtube.com/"))
        expect(!RssFeedFinder.youTubeNeedsPage("https://www.youtube.com/watch"))
        expect(!RssFeedFinder.youTubeNeedsPage("https://social.example/@handle"))
        assertEq("https://social.example/@ada.rss", RssFeedFinder.mastodonFeed("https://social.example/@ada"))
        assertEq("https://social.example/@ada.rss", RssFeedFinder.mastodonFeed("https://social.example/@ada/"))
        assertEq("", RssFeedFinder.mastodonFeed("https://social.example/@ada/123"))
        assertEq("", RssFeedFinder.mastodonFeed("https://www.youtube.com/@ada"))
        assertEq("", RssFeedFinder.mastodonFeed("https://social.example/users/ada"))
        assertEq("https://bsky.app/profile/ada.example.org/rss", RssFeedFinder.blueskyFeed("https://bsky.app/profile/ada.example.org"))
        assertEq("https://bsky.app/profile/ada.example.org/rss", RssFeedFinder.blueskyFeed("https://bsky.app/profile/ada.example.org/"))
        assertEq("", RssFeedFinder.blueskyFeed("https://bsky.app/profile/ada.example.org/post/1"))
        expect(RssFetcher.GUESS_PATHS.last() == "/rss/")
    }

    fun testYouTubeChannelIdExtraction() {
        val id = "UCabcdefghijklmnopqrstuv"
        assertEq(id, RssFeedFinder.youTubeChannelId("""<script>var x = {"browseId":"x","channelId":"$id","other":1}</script>"""))
        assertEq(id, RssFeedFinder.youTubeChannelId("""<meta itemprop="name" content="n"><meta content="$id" itemprop="channelId">"""))
        assertEq(id, RssFeedFinder.youTubeChannelId("""<link href="https://www.youtube.com/channel/$id" rel="canonical">"""))
        assertEq("", RssFeedFinder.youTubeChannelId("""<link rel="alternate" href="https://www.youtube.com/channel/$id"><p>"channelId":"UCshort"</p>"""))
        assertEq(id, RssFeedFinder.youTubeChannelId("""<link rel="canonical" href="https://www.youtube.com/channel/$id"><x>"channelId":"$id"</x>"""))
    }

    fun testDiscoveryFlow() {
        val http = Http()
        val id = "UCabcdefghijklmnopqrstuv"
        http.serve("https://www.youtube.com/@example", "<html><head><link rel=\"canonical\" href=\"https://www.youtube.com/channel/$id\"></head></html>", "text/html")
        http.serve("https://www.youtube.com/feeds/videos.xml?channel_id=$id", youTube, "application/atom+xml")
        http.serve("https://social.example/@ada.rss", mastodon)
        http.serve("https://ghost.example.org/", "<html><head><title>Ghost</title></head></html>", "text/html")
        http.serve("https://ghost.example.org/rss/", mastodon)
        val fetcher = RssFetcher(http)
        assertEq("https://www.youtube.com/feeds/videos.xml?channel_id=$id", fetcher.discover("youtube.com/@example".let { "https://www.$it" }).outcome.url)
        assertEq("https://social.example/@ada.rss", fetcher.discover("https://social.example/@ada").outcome.url)
        assertEq("https://ghost.example.org/rss/", fetcher.discover("ghost.example.org/").outcome.url)
        assertEq("http_404", assertFails<RssFetchException> { fetcher.discover("https://bsky.app/profile/nobody.example") }.code)
        expect(http.requests.any { it.first == "https://bsky.app/profile/nobody.example/rss" })
    }

    fun testImportFromUrlAndHtmlUrl() {
        val http = Http()
        val opml = """<opml version="1.1"><body><outline text="A" xmlUrl="https://a.example.org/rss" htmlUrl="https://a.example.org/"/>
            <outline text="B" xmlUrl="https://b.example.org/rss"/></body></opml>"""
        http.serve("https://lists.example.org/subs.opml", opml, "text/x-opml")
        val fetcher = RssFetcher(http)
        val library = RssLibrary()
        val text = fetcher.opml("lists.example.org/subs.opml")
        assertEq(2, library.importOpml(text, now).imported)
        assertEq("https://a.example.org/", library.findFeed("https://a.example.org/rss")!!.siteUrl)
        val exported = library.exportOpml(now)
        expect(exported.contains("xmlUrl=\"https://a.example.org/rss\" htmlUrl=\"https://a.example.org/\"/>"))
        expect(exported.contains("xmlUrl=\"https://b.example.org/rss\"/>"))
        assertEq("http_404", assertFails<RssFetchException> { fetcher.opml("https://lists.example.org/missing.opml") }.code)
        assertEq("invalid_url", assertFails<RssFetchException> { fetcher.opml("  ") }.code)
    }

    fun testCatchUpAndMarkAllKeepsStarred() {
        val library = RssLibrary()
        val a = library.subscribe("https://a.example.org/rss", now = now)
        val b = library.subscribe("https://b.example.org/rss", now = now)
        fill(library, a, item("new", published = now - day / 2), item("old", published = now - 2 * day), item("older", published = now - 10 * day))
        fill(library, b, item("bold", published = now - 40 * day))
        val older = RssLibrary.articleId(a.id, "older")
        library.setStarred(listOf(older), true)
        assertEq(2, library.catchUpIds(RssScope.Feed(a.id), RssCatchUp.DAY.cutoff(now), now).size)
        library.updateSettings { it.copy(markAllKeepsStarred = true) }
        assertEq(1, library.catchUpIds(RssScope.Feed(a.id), RssCatchUp.DAY.cutoff(now), now).size)
        assertEq(1, library.catchUpIds(null, RssCatchUp.WEEK.cutoff(now), now).size)
        assertEq(1, library.catchUp(null, RssCatchUp.MONTH.cutoff(now), now))
        assertEq(true, library.article(RssLibrary.articleId(b.id, "bold"))!!.read)
        assertEq(0, library.catchUp(RssScope.Feed(a.id), RssCatchUp.THREE_DAYS.cutoff(now), now))
        assertEq(1, library.catchUp(RssScope.Feed(a.id), RssCatchUp.DAY.cutoff(now), now))
        assertEq(false, library.article(older)!!.read)
        assertEq(1, library.markAllRead(library.timeline(RssScope.Feed(a.id), now)))
        assertEq(false, library.article(older)!!.read)
        assertEq(true, library.article(RssLibrary.articleId(a.id, "new"))!!.read)
        library.updateSettings { it.copy(markAllKeepsStarred = false) }
        assertEq(1, library.markAllRead(library.timeline(RssScope.Feed(a.id), now)))
        assertEq(true, library.article(older)!!.read)
        assertEq(listOf(1, 3, 7, 14, 30), RssCatchUp.entries.map { it.days })
        assertEq("Older Than 1 Month", RssCatchUp.MONTH.label)
    }

    fun testHideReadArticles() {
        val library = RssLibrary()
        val feed = library.subscribe("https://a.example.org/rss", now = now)
        fill(library, feed, item("1"), item("2"), item("3"))
        val ids = library.timeline(RssScope.Feed(feed.id), now).map { it.id }
        library.setRead(listOf(ids[0]), true)
        library.setStarred(listOf(ids[0]), true)
        assertEq(3, library.timeline(RssScope.Feed(feed.id), now).size)
        expect(!library.hidesRead(RssScope.Feed(feed.id)))
        expect(library.hidesRead(RssScope.Unread))
        library.updateSettings { it.copy(hideReadArticles = true) }
        expect(library.hidesRead(RssScope.Feed(feed.id)))
        expect(!library.hidesRead(RssScope.Starred))
        assertEq(2, library.timeline(RssScope.Feed(feed.id), now).size)
        assertEq(1, library.timeline(RssScope.Starred, now).size)
        val keep = library.timeline(RssScope.Feed(feed.id), now).map { it.id }.toSet()
        library.setRead(listOf(ids[1]), true)
        assertEq(2, library.timeline(RssScope.Feed(feed.id), now, keep = keep).size)
        assertEq(1, library.timeline(RssScope.Feed(feed.id), now).size)
        assertEq(1, library.timeline(RssScope.Today, now).size)
        library.markAllRead(library.timeline(RssScope.Feed(feed.id), now, keep = keep))
        assertEq(0, library.timeline(RssScope.Feed(feed.id), now).size)
    }

    fun testFeedSwitchesPauseAndExclusions() {
        val library = RssLibrary()
        val folder = library.addFolder("F")
        val a = library.subscribe("https://a.example.org/rss", "Alpha", folder.id, now = now)
        val b = library.subscribe("https://b.example.org/rss", "Beta", folder.id, now = now)
        fill(library, a, item("1"), item("2"))
        fill(library, b, item("3"))
        library.setFeedSwitches(a.id, paused = true, excludeFromToday = false, excludeFromUnread = false)
        expect(library.due(now + RssLibrary.FETCH_INTERVAL_MS, force = true).none { it.id == a.id })
        expect(library.due(now + RssLibrary.FETCH_INTERVAL_MS).any { it.id == b.id })
        assertEq(2, library.articles(RssScope.Feed(a.id), now).size)
        library.setFeedSwitches(a.id, paused = false, excludeFromToday = true, excludeFromUnread = false)
        var counts = library.counts(now)
        assertEq(1, counts.today)
        assertEq(3, counts.unread)
        assertEq(listOf(b.id), library.timeline(RssScope.Today, now).map { it.feedId }.distinct())
        library.setFeedSwitches(a.id, paused = false, excludeFromToday = false, excludeFromUnread = true)
        counts = library.counts(now)
        assertEq(1, counts.today)
        assertEq(1, counts.unread)
        assertEq(2, counts.byFeed[a.id])
        assertEq(1, counts.byFolder[folder.id])
        assertEq(1, library.timeline(RssScope.Unread, now).size)
        assertEq(3, library.timeline(RssScope.Folder(folder.id), now).size)
    }

    fun testFeedAddressEdit() {
        val http = Http()
        val library = RssLibrary()
        val a = library.subscribe("https://a.example.org/rss", now = now)
        val b = library.subscribe("https://b.example.org/rss", now = now)
        library.applyFetch(a.id, RssFetchOutcome.Failed("timeout"), now)
        assertEq("invalid_url", assertFails<RssLibraryException> { library.changeFeedUrl(a.id, "not a url") }.code)
        assertEq("feed_exists", assertFails<RssLibraryException> { library.changeFeedUrl(a.id, "http://B.example.org/rss") }.code)
        val moved = library.changeFeedUrl(a.id, "feed://a.example.org/feed.xml")
        assertEq("https://a.example.org/feed.xml", moved.url)
        assertEq(listOf("https://a.example.org/rss"), moved.previousUrls)
        assertEq(0, moved.errorCount)
        assertEq("", moved.error)
        assertEq(0L, moved.nextCheck)
        assertEq("", moved.etag)
        assertEq(a.id, library.findFeed("https://a.example.org/rss")!!.id)
        assertEq("feed_exists", assertFails<RssLibraryException> { library.changeFeedUrl(b.id, "https://a.example.org/rss") }.code)
        val again = library.changeFeedUrl(a.id, "a.example.org/rss")
        assertEq("https://a.example.org/rss", again.url)
        assertEq(listOf("https://a.example.org/feed.xml"), again.previousUrls)
        http.serve("https://a.example.org/rss", mastodon, etag = "\"v1\"")
        val fetcher = RssFetcher(http)
        library.applyFetch(a.id, fetcher.fetch(library.feed(a.id)!!), now)
        assertEq("\"v1\"", library.feed(a.id)!!.etag)
        val outcome = fetcher.fetchNow(library.feed(a.id)!!)
        expect(outcome is RssFetchOutcome.Fetched)
        assertEq(null, http.requests.last().second["If-None-Match"])
        assertEq("RSS 2.0", (outcome as RssFetchOutcome.Fetched).feed.format)
        library.setFeedSwitches(a.id, paused = true, excludeFromToday = false, excludeFromUnread = false)
        expect(fetcher.fetchNow(library.feed(a.id)!!) is RssFetchOutcome.Fetched)
    }

    fun testSortByUnreadAndFindFeed() {
        val library = RssLibrary()
        val folder = library.addFolder("Zed Folder")
        val a = library.subscribe("https://alpha.example.org/rss", "Alpha", now = now)
        val b = library.subscribe("https://beta.example.org/rss", "Beta", folder.id, now = now)
        val c = library.subscribe("https://gamma.example.net/feed", "Gamma", folder.id, now = now)
        fill(library, a, item("1"))
        fill(library, b, item("2"), item("3"))
        fill(library, c, item("4"), item("5"), item("6"))
        var counts = library.counts(now)
        assertEq(listOf("Alpha", "Zed Folder"), library.sidebar(false, counts).map { it.name })
        library.updateSettings { it.copy(sortFeedsByUnread = true) }
        val sorted = library.sidebar(false, counts)
        assertEq(listOf("Zed Folder", "Alpha"), sorted.map { it.name })
        assertEq(listOf("Gamma", "Beta"), (sorted[0] as RssSidebarEntry.FolderEntry).feeds.map { it.displayTitle })
        library.setRead(library.articles(RssScope.Feed(c.id), now).map { it.id }, true)
        counts = library.counts(now)
        assertEq(listOf("Beta", "Gamma"), (library.sidebar(false, counts)[0] as RssSidebarEntry.FolderEntry).feeds.map { it.displayTitle })
        library.updateSettings { it.copy(sortFeedsByUnread = false) }
        assertEq(listOf("Zed Folder"), library.sidebar(false, counts, search = "GAMMA").map { it.name })
        assertEq(listOf("Gamma"), (library.sidebar(true, counts, search = "example.net")[0] as RssSidebarEntry.FolderEntry).feeds.map { it.displayTitle })
        assertEq(listOf("Alpha"), library.sidebar(false, counts, search = "alpha.example").map { it.name })
        assertEq(emptyList<String>(), library.sidebar(false, counts, search = "zed").map { it.name })
        assertEq(listOf("Alpha", "Zed Folder"), library.sidebar(false, counts, search = "/rss").map { it.name })
    }

    fun testNewFieldsPersistAndOldJsonLoads() {
        withTempDir("rss-roadmap") { dir ->
            File(dir, "articles").mkdirs()
            File(dir, "library.json").writeText(
                """{"version":1,"folders":[],"feeds":[{"id":"f1","url":"https://a.example.org/rss","title":"A"}],
                "settings":{"sortOldestFirst":true},"activity":[],"lastRefresh":5}""",
            )
            File(dir, "articles/f1.json").writeText("""{"articles":[{"id":"f1-aa","feedId":"f1","guid":"g","title":"Old","read":false}]}""")
            val store = RssStore(dir)
            val old = store.load()
            val feed = old.feed("f1")!!
            expect(!feed.paused && !feed.excludeFromToday && !feed.excludeFromUnread)
            assertEq("", feed.language)
            val settings = old.settings()
            expect(settings.sortOldestFirst)
            expect(!settings.showThumbnails && !settings.hideReadArticles && !settings.foldDuplicates && !settings.markAllKeepsStarred && !settings.sortFeedsByUnread)
            assertEq(emptyList<RssFilter>(), old.filters())
            assertEq(emptyList<RssSmartFeed>(), old.smartFeeds())
            val article = old.article("f1-aa")!!
            expect(!article.readLater)
            assertEq(emptyList<String>(), article.images)
            old.setFeedSwitches("f1", paused = true, excludeFromToday = true, excludeFromUnread = true)
            old.setReadLater(listOf("f1-aa"), true)
            old.updateSettings {
                it.copy(showThumbnails = true, hideReadArticles = true, foldDuplicates = true, markAllKeepsStarred = true, sortFeedsByUnread = true)
            }
            val filter = old.saveFilter(RssFilter("", feedId = "f1", field = RssFilterField.TITLE_OR_TEXT, pattern = "x+", regex = true, action = RssFilterAction.MARK_READ))
            val smart = old.saveSmartFeed(RssSmartFeed("", "Mine", words = "w", regex = false, scope = "feed:f1", unreadOnly = true, starredOnly = true, maxAgeDays = 3))
            old.applyFetch("f1", RssFetchOutcome.Fetched("https://a.example.org/rss", false, RssParsedFeed(language = "de", items = listOf(
                RssParsedItem(guid = "n", title = "New", images = listOf("https://a.example.org/i.png"), language = "fr", published = now),
            ))), now)
            store.save(old)
            val raw = Json.parseObject(File(dir, "library.json").readText())
            expect(raw.containsKey("filters") && raw.containsKey("smartFeeds"))
            val again = store.load()
            val loaded = again.feed("f1")!!
            expect(loaded.paused && loaded.excludeFromToday && loaded.excludeFromUnread)
            assertEq("de", loaded.language)
            val s = again.settings()
            expect(s.showThumbnails && s.hideReadArticles && s.foldDuplicates && s.markAllKeepsStarred && s.sortFeedsByUnread)
            assertEq(listOf(filter), again.filters())
            assertEq(listOf(smart), again.smartFeeds())
            expect(again.article("f1-aa")!!.readLater)
            val fresh = again.article(RssLibrary.articleId("f1", "n"))!!
            assertEq(listOf("https://a.example.org/i.png"), fresh.images)
            assertEq("fr", fresh.language)
            assertEq("https://a.example.org/i.png", fresh.image)
        }
    }
}
