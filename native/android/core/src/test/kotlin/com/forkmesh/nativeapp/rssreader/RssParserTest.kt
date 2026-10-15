package com.forkmesh.nativeapp.rssreader

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class RssParserTest {
    private val rss = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE rss [<!ENTITY xxe SYSTEM "file:///etc/passwd">]>
<rss version="2.0" xmlns:content="http://purl.org/rss/1.0/modules/content/" xmlns:dc="http://purl.org/dc/elements/1.1/">
  <channel>
    <title>Example Engineering</title>
    <link>https://blog.example.org/</link>
    <description>Notes &amp; news</description>
    <image><url>/icon.png</url></image>
    <item>
      <title>Shipping   the &lt;b&gt;reader&lt;/b&gt;</title>
      <link>/posts/one</link>
      <guid isPermaLink="false">  post-1
      </guid>
      <pubDate>Wed, 14 Oct 2026 14:20:00 GMT</pubDate>
      <dc:creator>Ada Example</dc:creator>
      <description>Short summary</description>
      <content:encoded><![CDATA[<p>Hello <script>alert(1)</script><b>world</b> &xxe;</p>]]></content:encoded>
      <comments>https://blog.example.org/posts/one#comments</comments>
      <enclosure url="https://cdn.example.org/one.mp3" type="audio/mpeg" length="1234"/>
    </item>
    <item>
      <title>Second post</title>
      <link>https://blog.example.org/posts/two</link>
      <pubDate>14 Oct 2026 10:00 +0200</pubDate>
      <author>ada@example.org (Ada Example)</author>
      <description>&lt;p&gt;Escaped &amp;amp; html&lt;/p&gt;</description>
    </item>
    <item>
      <title>Untitled  draft</title>
      <pubDate>Wed, 14 Oct 2026 14:20:00 GMT</pubDate>
    </item>
    <item>
      <title>No date</title>
    </item>
  </channel>
</rss>"""

    fun testRssItemsFieldsAndGuids() {
        val feed = RssFeedParser.parse(rss, "https://blog.example.org/feed.xml")
        assertEq("Example Engineering", feed.title)
        assertEq("https://blog.example.org/", feed.siteUrl)
        assertEq("https://blog.example.org/icon.png", feed.iconUrl)
        assertEq("Notes & news", feed.description)
        assertEq(4, feed.items.size)
        val first = feed.items[0]
        assertEq("post-1", RssGuid.of(first))
        assertEq("Shipping the reader", first.title)
        assertEq("https://blog.example.org/posts/one", first.url)
        assertEq("Ada Example", first.author)
        assertEq(1791987600000L, first.published)
        assertEq("Short summary", first.summaryHtml)
        expect(first.contentHtml.contains("<script>"))
        assertEq("https://blog.example.org/posts/one#comments", first.commentsUrl)
        assertEq(listOf(RssEnclosure("https://cdn.example.org/one.mp3", "audio/mpeg", 1234)), first.enclosures)
        val second = feed.items[1]
        assertEq("https://blog.example.org/posts/two", RssGuid.of(second))
        assertEq("Ada Example", second.author)
        assertEq(1791964800000L, second.published)
        assertEq("<p>Escaped &amp; html</p>", second.contentHtml)
        assertEq("Untitled draft|1791987600000", RssGuid.of(feed.items[2]))
        assertEq("No date", RssGuid.of(feed.items[3]))
    }

    fun testGuidRuleMatchesTheContract() {
        assertEq("a b", RssGuid.of(RssParsedItem(guid = "  a \n b  ", url = "https://x")))
        assertEq("https://example.org/p", RssGuid.of(RssParsedItem(url = "https://example.org/p")))
        assertEq("Only title|5", RssGuid.of(RssParsedItem(title = "Only  title", published = 5)))
        assertEq("No date", RssGuid.of(RssParsedItem(title = "No date")))
        assertEq("", RssGuid.of(RssParsedItem()))
        assertEq(600, RssGuid.of(RssParsedItem(guid = "x".repeat(900))).length)
        assertEq("a b".let { RssText.collapse(it) }, "a b")
        assertEq(600, RssGuid.of(RssParsedItem(guid = "😀".repeat(700))).codePointCount(0, 1200))
    }

    fun testAtomFeed() {
        val atom = """<?xml version="1.0"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <title type="text">Example Engineering</title>
  <link rel="alternate" type="text/html" href="https://blog.example.org/"/>
  <link rel="self" href="https://blog.example.org/atom.xml"/>
  <icon>https://blog.example.org/favicon.png</icon>
  <author><name>Team Example</name></author>
  <entry>
    <id>urn:uuid:1234</id>
    <title type="html">Atom &amp;amp; friends</title>
    <link rel="alternate" href="https://blog.example.org/a1"/>
    <link rel="enclosure" href="https://cdn.example.org/v.mp4" type="video/mp4" length="99"/>
    <link rel="replies" href="https://blog.example.org/a1/comments"/>
    <published>2026-10-14T12:00:00Z</published>
    <updated>2026-10-14T13:00:00+01:00</updated>
    <summary>Plain &lt;summary&gt;</summary>
    <content type="xhtml"><div xmlns="http://www.w3.org/1999/xhtml"><p>Rich <em>text</em></p></div></content>
  </entry>
  <entry>
    <title>No id</title>
    <link href="https://blog.example.org/a2"/>
    <updated>2026-10-13T12:00:00.123456789Z</updated>
    <content type="html">&lt;p&gt;Escaped&lt;/p&gt;</content>
  </entry>
</feed>"""
        val feed = RssFeedParser.parse(atom, "https://blog.example.org/atom.xml")
        assertEq("Example Engineering", feed.title)
        assertEq("https://blog.example.org/", feed.siteUrl)
        assertEq("https://blog.example.org/favicon.png", feed.iconUrl)
        val first = feed.items[0]
        assertEq("urn:uuid:1234", RssGuid.of(first))
        assertEq("Atom & friends", first.title)
        assertEq("Team Example", first.author)
        assertEq("<p>Rich <em>text</em></p>", first.contentHtml)
        assertEq("Plain &lt;summary&gt;", first.summaryHtml)
        assertEq(1791979200000L, first.published)
        assertEq(1791979200000L, first.updated)
        assertEq("https://blog.example.org/a1/comments", first.commentsUrl)
        assertEq("video/mp4", first.enclosures.single().type)
        val second = feed.items[1]
        assertEq("https://blog.example.org/a2", RssGuid.of(second))
        assertEq("<p>Escaped</p>", second.contentHtml)
        assertEq(0L, second.published)
        assertEq(1791892800123L, second.updated)
    }

    fun testRdfFeed() {
        val rdf = """<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#" xmlns="http://purl.org/rss/1.0/" xmlns:dc="http://purl.org/dc/elements/1.1/">
  <channel rdf:about="https://example.net/"><title>Example RDF</title><link>https://example.net/</link></channel>
  <item rdf:about="https://example.net/item-1"><title>Item one</title><dc:date>2026-10-14T08:00:00-04:00</dc:date><dc:creator>Example Author</dc:creator></item>
</rdf:RDF>"""
        val feed = RssFeedParser.parse(rdf, "https://example.net/rss")
        assertEq("Example RDF", feed.title)
        val item = feed.items.single()
        assertEq("https://example.net/item-1", RssGuid.of(item))
        assertEq("https://example.net/item-1", item.url)
        assertEq("Example Author", item.author)
        assertEq(1791979200000L, item.published)
    }

    fun testJsonFeed() {
        val json = """{"version":"https://jsonfeed.org/version/1.1","title":"Example JSON","home_page_url":"https://json.example.net/",
            "icon":"https://json.example.net/icon.png","authors":[{"name":"Feed Author"}],
            "items":[{"id":"j1","url":"/p/1","title":"First","content_text":"Line one\nline two\n\nPara two","summary":"Sum",
            "date_published":"2026-10-14T12:00:00Z","attachments":[{"url":"https://json.example.net/a.mp3","mime_type":"audio/mpeg","size_in_bytes":10}]},
            {"id":42,"title":"Numeric id","content_html":"<p>x</p>"},{"url":"https://json.example.net/p/3","title":"Url only"}]}"""
        val feed = RssFeedParser.parse(json, "https://json.example.net/feed.json")
        assertEq("Example JSON", feed.title)
        val first = feed.items[0]
        assertEq("j1", RssGuid.of(first))
        assertEq("https://json.example.net/p/1", first.url)
        assertEq("<p>Line one<br>line two</p><p>Para two</p>", first.contentHtml)
        assertEq("Sum", first.summaryHtml)
        assertEq("Feed Author", first.author)
        assertEq("42", RssGuid.of(feed.items[1]))
        assertEq("https://json.example.net/p/3", RssGuid.of(feed.items[2]))
    }

    fun testRejectsNonFeeds() {
        assertEq("not_a_feed", assertFails<RssFeedException> { RssFeedParser.parse("<html><body>hi</body></html>") }.code)
        assertEq("empty_feed", assertFails<RssFeedException> { RssFeedParser.parse("   ") }.code)
        assertEq("not_a_feed", assertFails<RssFeedException> { RssFeedParser.parse("{\"version\":\"1\"}") }.code)
        assertEq("not_a_feed", assertFails<RssFeedException> { RssFeedParser.parse("<opml><body/></opml>") }.code)
    }

    fun testLenientXmlRepairs() {
        val broken = "<rss><channel><title>Fish & Chips &mdash; Example</title><item><title>A &nbsp;B</title><link>https://example.com/a?x=1&y=2</link></item></channel></rss>"
        val feed = RssFeedParser.parse(broken)
        assertEq("Fish & Chips — Example", feed.title)
        assertEq("A B", feed.items.single().title)
        assertEq("https://example.com/a?x=1&y=2", feed.items.single().url)
    }

    fun testDates() {
        assertEq(1791987600000L, RssDates.parse("Wed, 14 Oct 2026 14:20:00 GMT"))
        assertEq(1792005600000L, RssDates.parse("Wed, 14 Oct 26 14:20:00 EST"))
        assertEq(1791987600000L, RssDates.parse("Oct 14 2026 14:20:00"))
        assertEq(1791987600000L, RssDates.parse("2026-10-14T14:20:00Z"))
        assertEq(1791980400000L, RssDates.parse("2026-10-14T14:20:00+0200"))
        assertEq(1791936000000L, RssDates.parse("2026-10-14"))
        assertEq(0L, RssDates.parse("someday"))
        assertEq(0L, RssDates.parse(""))
    }

    fun testUrls() {
        assertEq("https://example.org/feed", RssUrls.normalizeFeedInput("feed://example.org/feed"))
        assertEq("https://example.org/feed", RssUrls.normalizeFeedInput("feed:https://example.org/feed"))
        assertEq("https://example.org", RssUrls.normalizeFeedInput("example.org"))
        assertEq("", RssUrls.normalizeFeedInput("javascript:alert(1)"))
        assertEq("", RssUrls.normalizeFeedInput("https://user:pw@example.org/"))
        assertEq("https://b.com/p", RssUrls.httpUrl("HTTPS://b.com/p?", "https://a.com/x/"))
        assertEq("https://b.com/a/../p", RssUrls.httpUrl("https://b.com/a/../p#", "https://a.com/x/"))
        assertEq("http://b.com/a/../p?", RssUrls.httpUrl("http://b.com/a/../p?", "https://a.com/x/"))
        assertEq("https://a.com/z", RssUrls.httpUrl("../z", "https://a.com/x/y"))
        assertEq("https://c.com/q", RssUrls.httpUrl("//c.com/q", "https://a.com/x/y"))
        assertEq("https://a.com/p", RssUrls.httpUrl("p", "https://a.com"))
        assertEq(RssUrls.identity("http://Example.org:80/feed?x=1"), RssUrls.identity("https://example.org/feed?x=1"))
        expect(RssUrls.identity("https://example.org:8443/feed") != RssUrls.identity("https://example.org/feed"))
        assertEq("example.org/", RssUrls.identity("https://example.org"))
        assertEq("https://example.org/favicon.ico", RssUrls.favicon("https://example.org/blog/"))
    }
}
