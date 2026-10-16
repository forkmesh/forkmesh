import FMBase
import Foundation
import XCTest

@testable import FMPackages

final class RSSReaderMediaTests: XCTestCase {
    static let youTube = """
    <?xml version="1.0" encoding="UTF-8"?>
    <feed xmlns:yt="http://www.youtube.com/xml/schemas/2015" xmlns:media="http://search.yahoo.com/mrss/" xmlns="http://www.w3.org/2005/Atom">
     <link rel="self" href="http://www.youtube.com/feeds/videos.xml?channel_id=UCXuqSBlHAE6Xw-yeJA0Tunw"/>
     <id>yt:channel:XuqSBlHAE6Xw-yeJA0Tunw</id>
     <yt:channelId>XuqSBlHAE6Xw-yeJA0Tunw</yt:channelId>
     <title>Linus Tech Tips</title>
     <link rel="alternate" href="https://www.youtube.com/channel/UCXuqSBlHAE6Xw-yeJA0Tunw"/>
     <author>
      <name>Linus Tech Tips</name>
      <uri>https://www.youtube.com/channel/UCXuqSBlHAE6Xw-yeJA0Tunw</uri>
     </author>
     <published>2008-11-25T00:46:52+00:00</published>
     <entry>
      <id>yt:video:k1Dq7HzHn2Q</id>
      <yt:videoId>k1Dq7HzHn2Q</yt:videoId>
      <yt:channelId>UCXuqSBlHAE6Xw-yeJA0Tunw</yt:channelId>
      <title>I Tried Every Keyboard Switch</title>
      <link rel="alternate" href="https://www.youtube.com/watch?v=k1Dq7HzHn2Q"/>
      <author>
       <name>Linus Tech Tips</name>
       <uri>https://www.youtube.com/channel/UCXuqSBlHAE6Xw-yeJA0Tunw</uri>
      </author>
      <published>2026-10-14T17:00:06+00:00</published>
      <updated>2026-10-15T02:11:40+00:00</updated>
      <media:group>
       <media:title>I Tried Every Keyboard Switch</media:title>
       <media:content url="https://www.youtube.com/v/k1Dq7HzHn2Q?version=3" type="application/x-shockwave-flash" width="640" height="390"/>
       <media:thumbnail url="https://i2.ytimg.com/vi/k1Dq7HzHn2Q/hqdefault.jpg" width="480" height="360"/>
       <media:description>Thanks to our sponsor for this video.
    Check them out at https://example.com/ltt.

    Chapters:
    0:00 Intro &amp; setup</media:description>
       <media:community>
        <media:starRating count="12345" average="5.00" min="1" max="5"/>
        <media:statistics views="678901"/>
       </media:community>
      </media:group>
     </entry>
    </feed>
    """

    static let mastodon = """
    <?xml version="1.0" encoding="UTF-8"?>
    <rss version="2.0" xmlns:webfeeds="http://webfeeds.org/rss/1.0" xmlns:media="http://search.yahoo.com/mrss/">
      <channel>
        <title>Ada Example</title>
        <description>Public posts from @ada@mastodon.social</description>
        <link>https://mastodon.social/@ada</link>
        <image>
          <url>https://files.mastodon.social/accounts/avatars/000/000/001/original/ada.png</url>
          <title>Ada Example</title>
          <link>https://mastodon.social/@ada</link>
        </image>
        <lastBuildDate>Wed, 14 Oct 2026 10:00:00 +0000</lastBuildDate>
        <generator>Mastodon v4.3.0</generator>
        <item>
          <guid isPermaLink="true">https://mastodon.social/@ada/113300000000000001</guid>
          <link>https://mastodon.social/@ada/113300000000000001</link>
          <pubDate>Wed, 14 Oct 2026 09:58:12 +0000</pubDate>
          <description>&lt;p&gt;New keyboard day&lt;/p&gt;</description>
          <media:content url="https://files.mastodon.social/media_attachments/files/113/300/original/abc.jpg" type="image/jpeg" fileSize="183456" medium="image">
            <media:rating scheme="urn:simple">nonadult</media:rating>
            <media:description type="plain">A mechanical keyboard on a desk</media:description>
          </media:content>
          <category>keyboards</category>
        </item>
      </channel>
    </rss>
    """

    func testYouTubeEntryShowsThumbnailAndDescription() throws {
        let feed = try RSSFeedParser.parse(Self.youTube, base: "https://www.youtube.com/feeds/videos.xml?channel_id=UCXuqSBlHAE6Xw-yeJA0Tunw")
        XCTAssertEqual(feed.format, "Atom 1.0")
        let item = try XCTUnwrap(feed.items.first)
        XCTAssertEqual(item.title, "I Tried Every Keyboard Switch")
        XCTAssertEqual(item.images, ["https://i2.ytimg.com/vi/k1Dq7HzHn2Q/hqdefault.jpg"])
        XCTAssertTrue(item.enclosures.isEmpty)
        XCTAssertEqual(
            item.contentHtml,
            "<p>Thanks to our sponsor for this video.<br>Check them out at <a href=\"https://example.com/ltt\">https://example.com/ltt</a>.</p><p>Chapters:<br>0:00 Intro &amp; setup</p>")
        let prepared = try XCTUnwrap(RSSReaderStore.prepare(feed, feedUrl: "https://www.youtube.com/feeds/videos.xml", now: 1_800_000_000_000).first)
        XCTAssertEqual(prepared.image, "https://i2.ytimg.com/vi/k1Dq7HzHn2Q/hqdefault.jpg")
        XCTAssertEqual(prepared.body.images, ["https://i2.ytimg.com/vi/k1Dq7HzHn2Q/hqdefault.jpg"])
        XCTAssertTrue(prepared.summary.hasPrefix("Thanks to our sponsor"))
        var article = RSSArticle(feedId: "y", guid: prepared.guid)
        article.title = prepared.title
        let page = RSSArticlePage.html(article: article, body: prepared.body, feed: nil, settings: RSSReaderSettings(), reader: false)
        XCTAssertTrue(page.contains("<p><img src=\"https://i2.ytimg.com/vi/k1Dq7HzHn2Q/hqdefault.jpg\" alt=\"\""), page)
        XCTAssertTrue(page.contains("Thanks to our sponsor"))
    }

    func testMastodonItemShowsItsImage() throws {
        let feed = try RSSFeedParser.parse(Self.mastodon, base: "https://mastodon.social/@ada.rss")
        XCTAssertEqual(feed.format, "RSS 2.0")
        let item = try XCTUnwrap(feed.items.first)
        XCTAssertEqual(item.images, ["https://files.mastodon.social/media_attachments/files/113/300/original/abc.jpg"])
        XCTAssertEqual(item.contentHtml, "<p>New keyboard day</p>")
        XCTAssertTrue(item.enclosures.isEmpty)
        let prepared = try XCTUnwrap(RSSReaderStore.prepare(feed, feedUrl: "https://mastodon.social/@ada.rss", now: 1_800_000_000_000).first)
        XCTAssertEqual(prepared.image, "https://files.mastodon.social/media_attachments/files/113/300/original/abc.jpg")
        XCTAssertEqual(prepared.title, "New keyboard day")
        let page = RSSArticlePage.html(
            article: RSSArticle(feedId: "m", guid: prepared.guid), body: prepared.body, feed: nil, settings: RSSReaderSettings(), reader: false)
        XCTAssertTrue(page.contains("<img src=\"https://files.mastodon.social/media_attachments/files/113/300/original/abc.jpg\""), page)
        var off = RSSReaderSettings()
        off.loadRemoteImages = false
        let linked = RSSArticlePage.html(
            article: RSSArticle(feedId: "m", guid: prepared.guid), body: prepared.body, feed: nil, settings: off, reader: false)
        XCTAssertFalse(linked.contains("<img src=\"https://files.mastodon"))
        XCTAssertTrue(linked.contains(">Image</a>"), linked)
    }

    func testImageOrderTitleFallbackAndImageEnclosures() throws {
        let document = """
        <rss version="2.0" xmlns:media="http://search.yahoo.com/mrss/"><channel><title>T</title><link>https://ex.org/</link>
        <item><guid>a</guid>
          <enclosure url="https://ex.org/enc.png" type="image/png" length="5"/>
          <enclosure url="https://ex.org/ep.mp3" type="audio/mpeg" length="9"/>
          <media:content url="https://ex.org/content.jpg" medium="image"/>
          <media:content url="https://ex.org/typed.webp" type="image/webp"/>
          <media:group><media:title>Group title</media:title><media:thumbnail url="/thumb.jpg"/>
          <media:description type="html">&lt;p&gt;From &lt;b&gt;media&lt;/b&gt;&lt;/p&gt;</media:description></media:group>
        </item>
        <item><guid>b</guid><title>Has title</title><description>Real description</description>
          <media:description>Ignored</media:description></item>
        </channel></rss>
        """
        let feed = try RSSFeedParser.parse(document, base: "https://ex.org/feed")
        let first = feed.items[0]
        XCTAssertEqual(first.title, "Group title")
        XCTAssertEqual(first.images, ["https://ex.org/thumb.jpg", "https://ex.org/content.jpg", "https://ex.org/typed.webp", "https://ex.org/enc.png"])
        XCTAssertEqual(first.contentHtml, "<p>From <b>media</b></p>")
        XCTAssertEqual(first.enclosures.count, 2)
        XCTAssertEqual(feed.items[1].contentHtml, "Real description")
        let page = RSSArticlePage.enclosures(first.enclosures, remote: true)
        XCTAssertFalse(page.contains("enc.png"))
        XCTAssertTrue(page.contains("ep.mp3"))
        XCTAssertEqual(RSSArticlePage.enclosures([RSSEnclosure(url: "https://ex.org/a.png", type: "image/png")], remote: true), "")
    }

    func testJSONFeedImagesComeFirstAndContentImagesLast() throws {
        let json = """
        {"version": "https://jsonfeed.org/version/1.1", "title": "J", "language": "de-DE", "home_page_url": "https://j.example/",
         "items": [{"id": "1", "content_html": "<p><img src=\\"/inline.png\\"> hi</p>", "image": "/main.png", "banner_image": "https://j.example/banner.png", "language": "fr"},
                   {"id": "2", "content_html": "<p><img src=\\"/inline.png\\"> hi</p>"}]}
        """
        let feed = try RSSFeedParser.parse(json, base: "https://j.example/feed.json")
        XCTAssertEqual(feed.format, "JSON Feed 1.1")
        XCTAssertEqual(feed.language, "de-DE")
        XCTAssertEqual(feed.items[0].images, ["https://j.example/main.png", "https://j.example/banner.png"])
        let prepared = RSSReaderStore.prepare(feed, feedUrl: "https://j.example/feed.json", now: 1_800_000_000_000)
        XCTAssertEqual(prepared[0].image, "https://j.example/main.png")
        XCTAssertEqual(prepared[0].language, "fr")
        XCTAssertEqual(prepared[1].image, "https://j.example/inline.png")
        XCTAssertEqual(prepared[1].language, "")
        let lead = RSSArticlePage.leadImages(["https://j.example/inline.png", "https://j.example/new.png"], content: prepared[1].body.contentHtml)
        XCTAssertEqual(lead, "<p><img src=\"https://j.example/new.png\" alt=\"\"></p>")
        XCTAssertEqual(try RSSFeedParser.parse(#"{"version": "https://jsonfeed.org/version/1", "items": []}"#).format, "JSON Feed 1.0")
    }

    func testLanguagesAndFormats() throws {
        let rss = try RSSFeedParser.parse("<rss version=\"0.91\"><channel><title>R</title><language>he-IL</language><item><guid>x</guid></item></channel></rss>")
        XCTAssertEqual(rss.language, "he-IL")
        XCTAssertEqual(rss.format, "RSS 0.91")
        let atom = try RSSFeedParser.parse("""
            <feed xmlns="http://www.w3.org/2005/Atom" xml:lang="en-US"><title>A</title>
            <entry xml:lang="ar"><id>1</id><title>One</title></entry><entry><id>2</id><title>Two</title></entry></feed>
            """)
        XCTAssertEqual(atom.language, "en-US")
        XCTAssertEqual(atom.items.map(\.language), ["ar", ""])
        let rdf = try RSSFeedParser.parse("""
            <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#" xmlns="http://purl.org/rss/1.0/">
            <channel><title>D</title></channel><item><title>i</title><link>https://d.example/1</link></item></rdf:RDF>
            """)
        XCTAssertEqual(rdf.format, "RSS 1.0")
        XCTAssertEqual(RSSLanguage.direction("ar"), "rtl")
        XCTAssertEqual(RSSLanguage.direction("he-IL"), "rtl")
        XCTAssertEqual(RSSLanguage.direction("ckb_IQ"), "rtl")
        XCTAssertEqual(RSSLanguage.direction("en-US"), "auto")
        XCTAssertEqual(RSSLanguage.direction(""), "auto")
        XCTAssertEqual(RSSLanguage.tag("en_US"), "en-US")
        XCTAssertEqual(RSSLanguage.tag("x\" onload=\"y"), "")
    }

    func testPlainTextBecomesParagraphsWithLinks() {
        XCTAssertEqual(
            RSSHTML.plainToHTML("One <b>\nhttp://a.example/x?y=1&z=2, then\r\n\r\n\n  two  "),
            "<p>One &lt;b&gt;<br><a href=\"http://a.example/x?y=1&amp;z=2\">http://a.example/x?y=1&amp;z=2</a>, then</p><p>two</p>")
        XCTAssertEqual(RSSHTML.linkify("see https:// nothing"), "see https:// nothing")
        XCTAssertEqual(RSSHTML.plainToHTML("   "), "")
    }

    func testSanitizerDropsInstapaperIgnore() {
        let html = """
        <p>Keep</p><div class="share instapaper_ignore"><p>Share this <b>post</b></p><div>nested</div></div>
        <img class="instapaper_ignore" src="https://x.example/i.png"><p class="instapaper_ignored">Also keep</p>
        """
        let clean = RSSHTML.sanitize(html, base: "https://x.example/")
        XCTAssertEqual(clean, "<p>Keep</p>\n<p>Also keep</p>")
    }

    func testAltCaptionsOnlyOutsideCaptionedFigures() {
        let html = """
        <p><img src="/a.png" alt="A cat"></p><figure><img src="/b.png" alt="Dog"><figcaption>Our dog</figcaption></figure>
        <figure><img src="/c.png" alt="Bird"></figure><img src="/d.png" alt="">
        """
        let shown = RSSHTML.sanitize(html, base: "https://x.example/", altCaptions: true)
        XCTAssertTrue(shown.contains("referrerpolicy=\"no-referrer\"><span class=\"alt-caption\">A cat</span></p>"), shown)
        XCTAssertFalse(shown.contains(">Dog</span>"))
        XCTAssertTrue(shown.contains("<span class=\"alt-caption\">Bird</span></figure>"), shown)
        XCTAssertEqual(shown.components(separatedBy: "alt-caption").count, 3)
        let hidden = RSSHTML.sanitize(html, base: "https://x.example/", remoteMedia: false, altCaptions: true)
        XCTAssertFalse(hidden.contains("alt-caption"))
        XCTAssertFalse(RSSHTML.sanitize(html, base: "https://x.example/").contains("alt-caption"))
    }

    func testReadingTime() {
        XCTAssertEqual(RSSReaderText.readingMinutes(""), 0)
        XCTAssertEqual(RSSReaderText.readingMinutes("   "), 0)
        XCTAssertEqual(RSSReaderText.readingMinutes("one"), 1)
        XCTAssertEqual(RSSReaderText.readingMinutes(Array(repeating: "word", count: 230).joined(separator: " ")), 1)
        XCTAssertEqual(RSSReaderText.readingMinutes(Array(repeating: "word", count: 231).joined(separator: " ")), 2)
        XCTAssertEqual(RSSReaderText.readingMinutes(String(repeating: "\u{6F22}", count: 500)), 1)
        XCTAssertEqual(RSSReaderText.readingMinutes(String(repeating: "\u{6F22}", count: 501)), 2)
        XCTAssertEqual(RSSReaderText.readingMinutes(String(repeating: "\u{3042}", count: 250) + " " + Array(repeating: "w", count: 115).joined(separator: " ")), 1)
        XCTAssertEqual(RSSReaderText.readingMinutes(String(repeating: "\u{AC00}", count: 251) + " " + Array(repeating: "w", count: 116).joined(separator: " ")), 2)
        XCTAssertEqual(RSSReaderText.readingTime("a b c"), "1 min read")
        XCTAssertEqual(RSSReaderText.readingTime(""), "")
    }

    func testArticlePageLanguageReadingTimeAndCode() {
        var article = RSSArticle(feedId: "f", guid: "g")
        article.published = 1_791_987_600_000
        var feed = RSSFeed(id: "f", url: "https://x.example/feed")
        feed.language = "fa-IR"
        let body = RSSArticleBody(contentHtml: "<p>" + Array(repeating: "word", count: 500).joined(separator: " ") + "</p><pre><code>let x = 1</code></pre>")
        let page = RSSArticlePage.html(
            article: article, body: body, feed: feed, settings: RSSReaderSettings(), reader: false, timeZone: TimeZone(identifier: "UTC")!,
            locale: Locale(identifier: "en_US"))
        XCTAssertTrue(page.contains("<html lang=\"fa-IR\" dir=\"rtl\">"), page)
        XCTAssertTrue(page.contains("\u{00B7} 3 min read</p>"), page)
        XCTAssertTrue(page.contains("pre{padding:12px;overflow-x:auto;white-space:pre;"))
        article.language = "en"
        let english = RSSArticlePage.html(article: article, body: RSSArticleBody(), feed: feed, settings: RSSReaderSettings(), reader: false)
        XCTAssertTrue(english.contains("<html lang=\"en\" dir=\"auto\">"))
        XCTAssertFalse(english.contains("min read"))
        XCTAssertTrue(RSSArticlePage.html(article: RSSArticle(feedId: "f", guid: "g"), body: body, feed: nil, settings: RSSReaderSettings(), reader: false)
            .contains("<html dir=\"auto\">"))
    }

    func testCheckResultLine() {
        let line = RSSReaderText.checkResult(
            format: "Atom 1.0", items: 15, at: 1_791_987_600_000, timeZone: TimeZone(identifier: "UTC")!, locale: Locale(identifier: "en_US_POSIX"))
        XCTAssertTrue(line.hasPrefix("Atom 1.0 \u{00B7} 15 items \u{00B7} checked "), line)
        XCTAssertEqual(RSSReaderText.lastCheckFailed("http_404"), "Last check failed: The feed was not found (404).")
    }
}
