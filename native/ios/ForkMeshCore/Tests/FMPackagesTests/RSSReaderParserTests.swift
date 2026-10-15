import FMBase
import Foundation
import XCTest

@testable import FMPackages

final class RSSReaderParserTests: XCTestCase {
    func item(guid: String = "", url: String = "", title: String = "", published: Int64 = 0) -> RSSParsedItem {
        var item = RSSParsedItem()
        item.guid = guid
        item.url = url
        item.title = title
        item.published = published
        return item
    }

    func testGuidRuleMatchesTheContract() {
        XCTAssertEqual(item(guid: "  a \n b  ", url: "https://x").articleGuid, "a b")
        XCTAssertEqual(item(url: "https://example.org/p").articleGuid, "https://example.org/p")
        XCTAssertEqual(item(title: "Only  title", published: 5).articleGuid, "Only title|5")
        XCTAssertEqual(item(title: "No date").articleGuid, "No date")
        XCTAssertEqual(item().articleGuid, "")
        XCTAssertEqual(item(guid: String(repeating: "x", count: 900)).articleGuid.count, 600)
        XCTAssertEqual(item(title: "", published: 42).articleGuid, "42")
        XCTAssertEqual(item(guid: "\u{00A0}tab\there\u{2003}").articleGuid, "tab here")
        XCTAssertEqual(item(guid: String(repeating: "\u{00E9}", count: 700)).articleGuid.unicodeScalars.count, 600)
    }

    static let rss = """
    <?xml version="1.0" encoding="UTF-8"?>
    <rss version="2.0" xmlns:content="http://purl.org/rss/1.0/modules/content/" xmlns:dc="http://purl.org/dc/elements/1.1/">
      <channel>
        <title>Example Engineering</title>
        <link>https://eng.example.org/</link>
        <description>Notes from the &lt;b&gt;team&lt;/b&gt;</description>
        <image><url>/logo.png</url></image>
        <item>
          <title>First post</title>
          <link>/posts/first</link>
          <guid isPermaLink="false">  post-1  </guid>
          <pubDate>Wed, 14 Oct 2026 14:20:00 GMT</pubDate>
          <dc:creator>Ada Example</dc:creator>
          <description>Short summary</description>
          <content:encoded><![CDATA[<p>Hello <script>alert(1)</script><b>world</b> &amp; friends</p>]]></content:encoded>
          <comments>https://eng.example.org/posts/first#comments</comments>
          <enclosure url="https://cdn.example.org/ep1.mp3" type="audio/mpeg" length="1234"/>
        </item>
        <item>
          <title>Second &amp; last</title>
          <guid>https://eng.example.org/posts/second</guid>
          <pubDate>Tue, 13 Oct 2026 09:00:00 -0400</pubDate>
          <author>ada@example.org (Ada Example)</author>
          <description>Plain &lt;i&gt;description&lt;/i&gt;</description>
        </item>
        <item>
          <title>Untitled guidless</title>
          <dc:date>2026-10-12T08:30:00Z</dc:date>
        </item>
      </channel>
    </rss>
    """

    func testParsesRSS2() throws {
        let feed = try RSSFeedParser.parse(Self.rss, base: "https://eng.example.org/feed.xml")
        XCTAssertEqual(feed.title, "Example Engineering")
        XCTAssertEqual(feed.siteUrl, "https://eng.example.org/")
        XCTAssertEqual(feed.iconUrl, "https://eng.example.org/logo.png")
        XCTAssertEqual(feed.description, "Notes from the team")
        XCTAssertEqual(feed.items.count, 3)
        let first = feed.items[0]
        XCTAssertEqual(first.articleGuid, "post-1")
        XCTAssertEqual(first.url, "https://eng.example.org/posts/first")
        XCTAssertEqual(first.author, "Ada Example")
        XCTAssertEqual(first.published, 1_791_987_600_000)
        XCTAssertTrue(first.contentHtml.contains("<script>"))
        XCTAssertEqual(first.summaryHtml, "Short summary")
        XCTAssertEqual(first.commentsUrl, "https://eng.example.org/posts/first#comments")
        XCTAssertEqual(first.enclosures, [RSSEnclosure(url: "https://cdn.example.org/ep1.mp3", type: "audio/mpeg", length: 1234)])
        let second = feed.items[1]
        XCTAssertEqual(second.title, "Second & last")
        XCTAssertEqual(second.url, "https://eng.example.org/posts/second")
        XCTAssertEqual(second.articleGuid, "https://eng.example.org/posts/second")
        XCTAssertEqual(second.author, "Ada Example")
        XCTAssertEqual(second.published, 1_791_896_400_000)
        XCTAssertEqual(second.contentHtml, "Plain <i>description</i>")
        let third = feed.items[2]
        XCTAssertEqual(third.published, 1_791_793_800_000)
        XCTAssertEqual(third.articleGuid, "Untitled guidless|1791793800000")
    }

    func testRepairsBareAmpersandsAndHTMLEntities() throws {
        let broken = """
        <!DOCTYPE rss [<!ENTITY nbsp "&#160;">]>
        <rss version="2.0"><channel><title>Open Source Weekly &mdash; Q&A</title><link>https://weekly.example.net</link>
        <item><title>Tips&nbsp;&amp; tricks</title><link>https://weekly.example.net/a?x=1&y=2</link></item></channel></rss>
        """
        let feed = try RSSFeedParser.parse(broken, base: "https://weekly.example.net/rss")
        XCTAssertEqual(feed.title, "Open Source Weekly \u{2014} Q&A")
        XCTAssertEqual(feed.items[0].url, "https://weekly.example.net/a?x=1&y=2")
        XCTAssertEqual(feed.items[0].articleGuid, "https://weekly.example.net/a?x=1&y=2")
        XCTAssertEqual(feed.items[0].title, "Tips & tricks", "a no-break space collapses like any other space")
    }

    func testParsesAtom() throws {
        let atom = """
        <?xml version="1.0" encoding="utf-8"?>
        <feed xmlns="http://www.w3.org/2005/Atom">
          <title type="text">Open Source Weekly</title>
          <subtitle>Releases &amp; news</subtitle>
          <link rel="self" href="https://weekly.example.net/atom.xml"/>
          <link rel="alternate" type="text/html" href="https://weekly.example.net/"/>
          <icon>https://weekly.example.net/icon.png</icon>
          <author><name>The Editors</name></author>
          <entry>
            <id>tag:weekly.example.net,2026:1</id>
            <title type="html">Issue &lt;em&gt;42&lt;/em&gt;</title>
            <link rel="alternate" href="/issues/42"/>
            <link rel="enclosure" type="video/mp4" href="https://cdn.example.net/42.mp4" length="99"/>
            <link rel="replies" href="https://weekly.example.net/issues/42#c"/>
            <published>2026-10-14T14:20:00+02:00</published>
            <updated>2026-10-15T08:00:00Z</updated>
            <summary>Short</summary>
            <content type="xhtml"><div xmlns="http://www.w3.org/1999/xhtml"><p>Body <a href="/x">link</a></p></div></content>
          </entry>
          <entry>
            <title>No id</title>
            <link href="https://weekly.example.net/issues/41"/>
            <updated>2026-10-07T08:00:00.123456789Z</updated>
            <content type="html">&lt;p&gt;Escaped&lt;/p&gt;</content>
          </entry>
        </feed>
        """
        let feed = try RSSFeedParser.parse(atom, base: "https://weekly.example.net/atom.xml")
        XCTAssertEqual(feed.title, "Open Source Weekly")
        XCTAssertEqual(feed.siteUrl, "https://weekly.example.net/")
        XCTAssertEqual(feed.iconUrl, "https://weekly.example.net/icon.png")
        XCTAssertEqual(feed.description, "Releases & news")
        let first = feed.items[0]
        XCTAssertEqual(first.articleGuid, "tag:weekly.example.net,2026:1")
        XCTAssertEqual(first.title, "Issue 42")
        XCTAssertEqual(first.url, "https://weekly.example.net/issues/42")
        XCTAssertEqual(first.author, "The Editors")
        XCTAssertEqual(first.published, 1_791_980_400_000)
        XCTAssertTrue(first.contentHtml.contains("<p>Body <a href=\"/x\">link</a></p>"), first.contentHtml)
        XCTAssertEqual(first.summaryHtml, "Short")
        XCTAssertEqual(first.commentsUrl, "https://weekly.example.net/issues/42#c")
        XCTAssertEqual(first.enclosures.first?.type, "video/mp4")
        let second = feed.items[1]
        XCTAssertEqual(second.articleGuid, "https://weekly.example.net/issues/41")
        XCTAssertEqual(second.published, 0)
        XCTAssertEqual(second.updated, 1_791_360_000_123)
        XCTAssertEqual(second.contentHtml, "<p>Escaped</p>")
    }

    func testParsesRDF() throws {
        let rdf = """
        <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#" xmlns="http://purl.org/rss/1.0/" xmlns:dc="http://purl.org/dc/elements/1.1/">
          <channel rdf:about="https://rdf.example.com/"><title>Example RDF</title><link>https://rdf.example.com/</link><description>d</description></channel>
          <item rdf:about="https://rdf.example.com/one"><title>One</title><link>https://rdf.example.com/one</link><dc:date>2026-10-01</dc:date></item>
          <item rdf:about="https://rdf.example.com/two"><title>Two</title></item>
        </rdf:RDF>
        """
        let feed = try RSSFeedParser.parse(rdf, base: "https://rdf.example.com/index.rdf")
        XCTAssertEqual(feed.title, "Example RDF")
        XCTAssertEqual(feed.items.map(\.articleGuid), ["https://rdf.example.com/one", "https://rdf.example.com/two"])
        XCTAssertEqual(feed.items[1].url, "https://rdf.example.com/two")
        XCTAssertEqual(feed.items[0].published, 1_790_812_800_000)
    }

    func testParsesJSONFeed() throws {
        let json = """
        {"version": "https://jsonfeed.org/version/1.1", "title": "Example Engineering", "home_page_url": "https://eng.example.org/",
         "icon": "https://eng.example.org/icon.png", "authors": [{"name": "Team"}],
         "items": [
          {"id": 123, "url": "/p/1", "title": "Numbered", "content_text": "Line one\\nline two\\n\\nPara two", "date_published": "2026-10-14T14:20:00Z",
           "summary": "S", "external_url": "https://elsewhere.example.com/x",
           "attachments": [{"url": "https://cdn.example.org/a.m4a", "mime_type": "audio/mp4", "size_in_bytes": 10}]},
          {"url": "https://eng.example.org/p/2", "content_html": "<p>Two</p>", "authors": [{"name": "Ada"}, {"name": "Bo"}]},
          {"id": "", "title": "Nothing else"}
         ]}
        """
        let feed = try RSSFeedParser.parse(json, base: "https://eng.example.org/feed.json")
        XCTAssertEqual(feed.title, "Example Engineering")
        XCTAssertEqual(feed.iconUrl, "https://eng.example.org/icon.png")
        XCTAssertEqual(feed.items.map(\.articleGuid), ["123", "https://eng.example.org/p/2", "Nothing else"])
        XCTAssertEqual(feed.items[0].url, "https://eng.example.org/p/1")
        XCTAssertEqual(feed.items[0].contentHtml, "<p>Line one<br>line two</p><p>Para two</p>")
        XCTAssertEqual(feed.items[0].summaryHtml, "S")
        XCTAssertEqual(feed.items[0].author, "Team")
        XCTAssertEqual(feed.items[0].externalUrl, "https://elsewhere.example.com/x")
        XCTAssertEqual(feed.items[0].enclosures.first?.length, 10)
        XCTAssertEqual(feed.items[1].author, "Ada, Bo")
        XCTAssertThrowsError(try RSSFeedParser.parse("{\"version\": \"other\"}"))
    }

    func testRejectsNonFeeds() {
        XCTAssertThrowsError(try RSSFeedParser.parse("")) { XCTAssertEqual($0 as? RSSFeedError, .empty) }
        XCTAssertThrowsError(try RSSFeedParser.parse("<!doctype html><html><body>hi</body></html>")) {
            XCTAssertEqual($0 as? RSSFeedError, .notAFeed)
        }
        XCTAssertThrowsError(try RSSFeedParser.parse("<rss><channel><title>x</title>")) {
            XCTAssertEqual($0 as? RSSFeedError, .unreadable)
        }
        XCTAssertThrowsError(try RSSFeedParser.parse("<opml/>")) { XCTAssertEqual($0 as? RSSFeedError, .notAFeed) }
    }

    func testDates() {
        XCTAssertEqual(RSSDate.parse("Wed, 14 Oct 2026 14:20:00 GMT"), 1_791_987_600_000)
        XCTAssertEqual(RSSDate.parse("14 Oct 2026 14:20 +0000"), 1_791_987_600_000)
        XCTAssertEqual(RSSDate.parse("Wed, 14 Oct 26 10:20:00 EDT"), 1_791_987_600_000)
        XCTAssertEqual(RSSDate.parse("Wednesday, 14 October 2026 16:20:00 +0200"), 1_791_987_600_000)
        XCTAssertEqual(RSSDate.parse("2026-10-14T14:20:00Z"), 1_791_987_600_000)
        XCTAssertEqual(RSSDate.parse("2026-10-14T16:20:00+0200"), 1_791_987_600_000)
        XCTAssertEqual(RSSDate.parse("2026-10-14 14:20:00.5"), 1_791_987_600_500)
        XCTAssertEqual(RSSDate.parse("2026-10-14"), 1_791_936_000_000)
        XCTAssertEqual(RSSDate.parse("2026-10-14 sometime"), 1_791_936_000_000)
        XCTAssertEqual(RSSDate.parse("31 Feb 2026 10:00:00 GMT"), 0)
        XCTAssertEqual(RSSDate.parse("not a date"), 0)
        XCTAssertEqual(RSSDate.parse(""), 0)
    }

    func testURLRules() {
        XCTAssertEqual(RSSURL.normalizeFeedInput("feed://example.org/rss"), "https://example.org/rss")
        XCTAssertEqual(RSSURL.normalizeFeedInput("feed:https://example.org/rss"), "https://example.org/rss")
        XCTAssertEqual(RSSURL.normalizeFeedInput("example.org"), "https://example.org")
        XCTAssertEqual(RSSURL.normalizeFeedInput("  //example.org/a "), "https://example.org/a")
        XCTAssertEqual(RSSURL.normalizeFeedInput("http://example.org/a"), "http://example.org/a")
        XCTAssertEqual(RSSURL.normalizeFeedInput("ftp://example.org"), "")
        XCTAssertEqual(RSSURL.normalizeFeedInput("https://user:pw@example.org"), "")
        XCTAssertEqual(RSSURL.normalizeFeedInput("https://example.org:99999/"), "")
        XCTAssertEqual(RSSURL.normalizeFeedInput(""), "")
        XCTAssertEqual(RSSURL.identity("https://Example.org/feed?x=1#top"), "example.org/feed?x=1")
        XCTAssertEqual(RSSURL.identity("http://example.org:80"), "example.org/")
        XCTAssertEqual(RSSURL.identity("https://example.org:8443/a"), "example.org:8443/a")
        XCTAssertEqual(RSSURL.identity("http://example.org/a"), RSSURL.identity("https://example.org/a"))
        XCTAssertEqual(RSSURL.resolve("../c?q", against: "https://example.org/a/b/d"), "https://example.org/a/c?q")
        XCTAssertEqual(RSSURL.resolve("/x", against: "https://example.org/a/b"), "https://example.org/x")
        XCTAssertEqual(RSSURL.resolve("//cdn.example.org/i.png", against: "https://example.org/"), "https://cdn.example.org/i.png")
        XCTAssertEqual(RSSURL.resolve("#f", against: "https://example.org/a?b"), "https://example.org/a?b#f")
        XCTAssertEqual(RSSURL.resolve("p", against: "https://example.org"), "https://example.org/p")
        XCTAssertEqual(RSSURL.http("javascript:alert(1)", base: "https://example.org/"), "")
        XCTAssertEqual(RSSURL.favicon("https://example.org/blog/"), "https://example.org/favicon.ico")
        XCTAssertEqual(RSSURL.upgraded("http://example.org:80/feed"), "https://example.org/feed")
        XCTAssertEqual(RSSURL.upgraded("https://example.org/feed"), "https://example.org/feed")
    }

    func testOPMLFlattensNestedFolders() throws {
        let opml = """
        <?xml version="1.0"?>
        <opml version="2.0"><head><title>Mine</title></head><body>
          <outline text="Example Engineering" xmlUrl="https://eng.example.org/feed.xml" htmlUrl="https://eng.example.org/"/>
          <outline text="Tech">
            <outline title="Open Source Weekly" text="ignored" xmlUrl="feed://weekly.example.net/rss"/>
            <outline text="Deeper"><outline text="Deep feed" xmlUrl="https://deep.example.com/atom"/></outline>
          </outline>
          <outline text=""><outline text="Orphan" xmlUrl="https://orphan.example.com/rss"/></outline>
          <outline text="Bad" xmlUrl="ftp://bad.example.com"/>
        </body></opml>
        """
        let entries = try RSSOPML.parse(opml)
        XCTAssertEqual(entries.map(\.url), [
            "https://eng.example.org/feed.xml", "https://weekly.example.net/rss", "https://deep.example.com/atom",
            "https://orphan.example.com/rss",
        ])
        XCTAssertEqual(entries.map(\.folder), ["", "Tech", "Tech", "Imported"])
        XCTAssertEqual(entries[1].title, "Open Source Weekly")
        XCTAssertEqual(entries[0].siteUrl, "https://eng.example.org/")
        XCTAssertThrowsError(try RSSOPML.parse("<rss/>"))
    }

    func testOPMLExportRoundTrips() throws {
        let folders = [RSSFolder(id: "f1", name: "Tech & Code")]
        var a = RSSFeed(id: "a", url: "https://eng.example.org/feed.xml", title: "Example Engineering", siteUrl: "https://eng.example.org/")
        a.customTitle = "Eng \"quoted\""
        let b = RSSFeed(id: "b", url: "https://weekly.example.net/rss?x=1&y=2", title: "Open Source Weekly", folderId: "f1")
        let text = RSSOPML.build(folders: folders, feeds: [a, b], now: 1_791_987_600_000)
        XCTAssertTrue(text.contains("<dateCreated>Wed, 14 Oct 2026 14:20:00 GMT</dateCreated>"))
        let entries = try RSSOPML.parse(text)
        XCTAssertEqual(entries.count, 2)
        XCTAssertEqual(entries[0].title, "Eng \"quoted\"")
        XCTAssertEqual(entries[1].url, "https://weekly.example.net/rss?x=1&y=2")
        XCTAssertEqual(entries[1].folder, "Tech & Code")
    }

    func testDecodesDeclaredCharsets() {
        let latin = Data([0x3C, 0x61, 0x3E, 0xE9, 0x3C, 0x2F, 0x61, 0x3E])
        XCTAssertEqual(RSSXML.decode(latin, contentType: "text/xml; charset=ISO-8859-1"), "<a>\u{00E9}</a>")
        let declared = Data("<?xml version=\"1.0\" encoding=\"windows-1252\"?><a>".utf8) + Data([0x93, 0x94]) + Data("</a>".utf8)
        XCTAssertTrue(RSSXML.decode(declared).hasSuffix("<a>\u{201C}\u{201D}</a>"))
        let bom = Data([0xEF, 0xBB, 0xBF]) + Data("<rss/>".utf8)
        XCTAssertEqual(RSSXML.decode(bom), "<rss/>")
        let feed = "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?><rss version=\"2.0\"><channel><title>Caf\u{00E9}</title></channel></rss>"
        let data = feed.data(using: .isoLatin1)!
        XCTAssertEqual(try RSSFeedParser.parse(RSSXML.decode(data)).title, "Caf\u{00E9}")
    }
}
