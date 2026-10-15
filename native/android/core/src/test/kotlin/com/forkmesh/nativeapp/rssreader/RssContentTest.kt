package com.forkmesh.nativeapp.rssreader

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class RssContentTest {
    fun testSanitizerKeepsAllowlistOnly() {
        val dirty = """<div onclick="x()"><p style="color:red">Hi <b>there</b><script>alert(1)</script></p>
            <a href="javascript:alert(1)">bad</a><a href="/rel" target="_self">good</a><a href="mailto:me@example.org">mail</a>
            <img src="data:image/png;base64,AAA" data-src="/lazy.png" alt="Lazy" width="100px" height="80">
            <iframe src="https://video.example.org/embed/1"></iframe><style>p{}</style><nav><a href="/x">menu</a></nav>
            <video src="https://cdn.example.org/v.mp4"></video><unknown>kept text</unknown><svg><text>gone</text></svg></div>"""
        val clean = RssSanitizer.sanitize(dirty, "https://blog.example.org/post")
        expect(!clean.contains("onclick"))
        expect(!clean.contains("style"))
        expect(!clean.contains("script"))
        expect(!clean.contains("javascript:"))
        expect(!clean.contains("menu"))
        expect(!clean.contains("gone"))
        expect(clean.contains("<a rel=\"noopener noreferrer\" target=\"_blank\">bad</a>"))
        expect(clean.contains("<a href=\"https://blog.example.org/rel\" rel=\"noopener noreferrer\" target=\"_blank\">good</a>"))
        expect(clean.contains("href=\"mailto:me@example.org\""))
        expect(clean.contains("<img src=\"https://blog.example.org/lazy.png\" alt=\"Lazy\" height=\"80\" loading=\"lazy\" referrerpolicy=\"no-referrer\">"))
        expect(clean.contains("<p><a href=\"https://video.example.org/embed/1\" rel=\"noopener noreferrer\" target=\"_blank\">Open embedded content</a></p>"))
        expect(clean.contains("<video src=\"https://cdn.example.org/v.mp4\" controls></video>"))
        expect(clean.contains("kept text"))
        expect(clean.startsWith("<div><p>Hi <b>there</b></p>"))
        expect(clean.endsWith("</div>"))
    }

    fun testSanitizerEscapesTextAndClosesTags() {
        assertEq("<p>a &lt;b&gt; &amp; c</p>", RssSanitizer.sanitize("<p>a &lt;b&gt; &amp; c"))
        assertEq("<ul><li>one</li></ul>", RssSanitizer.sanitize("<ul><li>one</ul>"))
        assertEq("", RssSanitizer.sanitize("   "))
        assertEq("<p>x</p>after", RssSanitizer.sanitize("<p>x</p><embed src=\"a\">after"))
    }

    fun testRemoteMediaBecomesLinks() {
        val clean = RssSanitizer.sanitize("<p>A<img src=\"https://cdn.example.org/a.png\" alt=\"Chart\"></p><video controls><source src=\"https://cdn.example.org/v.mp4\" type=\"video/mp4\"></video>")
        val offline = RssSanitizer.withoutRemoteMedia(clean)
        expect(!offline.contains("<img"))
        expect(!offline.contains("<video"))
        expect(offline.contains("<a href=\"https://cdn.example.org/a.png\">Image: Chart</a>"))
        expect(offline.contains("<a href=\"https://cdn.example.org/v.mp4\">Open video</a>"))
    }

    fun testPlainText() {
        assertEq("Hello world", RssSanitizer.plainText("<p>Hello</p><p>world</p><script>x</script>"))
        assertEq("a & b", RssSanitizer.plainText("a &amp; b"))
        assertEq("one two…", RssSanitizer.plainText("one two three", 10))
    }

    fun testDiscoveryPrefersRssThenAtom() {
        val page = """<html><head><title>Example Engineering</title><base href="https://blog.example.org/">
            <link rel="alternate" type="application/atom+xml" href="atom.xml" title="Atom">
            <link rel="alternate" type="application/rss+xml" href="/rss.xml" title="RSS">
            <link rel="icon" href="/favicon.png"><link rel="apple-touch-icon" href="/touch.png"></head><body></body></html>"""
        val found = RssPages.discover(page, "https://blog.example.org/home")
        assertEq(listOf("https://blog.example.org/rss.xml", "https://blog.example.org/atom.xml"), found.feeds.map { it.url })
        assertEq("https://blog.example.org/touch.png", found.icon)
        assertEq("Example Engineering", found.title)
    }

    fun testReadableExtraction() {
        val body = "This paragraph carries enough words to count as the main body of the article page. ".repeat(3)
        val page = """<html><head><title>Fallback</title><meta property="og:title" content="Example Story"></head><body>
            <header><p>Site header text that is not part of the story at all and is long enough to matter maybe.</p></header>
            <nav><ul><li>Home</li><li>About</li></ul></nav>
            <div class="content"><article><h2>Heading</h2><p>$body</p><p>$body</p><script>evil()</script></article></div>
            <aside><p>Related links</p></aside><footer>Footer</footer></body></html>"""
        val readable = RssPages.readable(page, "https://blog.example.org/story")
        assertEq("Example Story", readable.title)
        expect(readable.contentHtml.contains("<h2>Heading</h2>"))
        expect(readable.contentHtml.contains("main body"))
        expect(!readable.contentHtml.contains("Site header"))
        expect(!readable.contentHtml.contains("Related"))
        expect(!readable.contentHtml.contains("evil"))
        assertEq("", RssPages.readable("<html><body><p>Too short</p></body></html>", "https://example.org/").contentHtml)
        val noArticle = "<html><body><div id=a><p>$body</p><p>$body</p></div><div id=b><p>short</p></div></body></html>"
        expect(RssPages.readable(noArticle, "https://example.org/").contentHtml.contains("main body"))
    }

    fun testArticlePageIsThemedAndSafe() {
        val article = RssArticle(
            id = "f-1", feedId = "f", guid = "g", title = "A <title>", url = "https://blog.example.org/a", author = "Ada",
            contentHtml = "<p>Body <img src=\"https://cdn.example.org/i.png\" alt=\"\"></p>", published = 1791987600000L,
            commentsUrl = "https://blog.example.org/a#c", enclosures = listOf(RssEnclosure("https://cdn.example.org/a.mp3", "audio/mpeg")),
        )
        val style = RssPageStyle(RssTheme.SEPIA, RssFont.SERIF, 18, false, "#3478f6", true)
        val page = RssArticlePage.html(article, "Example Engineering", style)
        expect(page.contains("A &lt;title&gt;"))
        expect(page.contains("font-size: 18px"))
        expect(page.contains("#f4ecd8"))
        expect(page.contains("<audio controls"))
        expect(page.contains(">Comments</a>"))
        expect(page.contains("class=\"icon letter\">E<"))
        expect(page.contains("default-src 'none'"))
        val offline = RssArticlePage.html(article, "Example Engineering", style.copy(loadRemoteImages = false), "data:image/png;base64,AAAA")
        expect(!offline.contains("<img src=\"https://cdn.example.org/i.png\""))
        expect(!offline.contains("<audio"))
        expect(offline.contains("img-src data:;"))
        expect(offline.contains("<img class=\"icon\" src=\"data:image/png;base64,AAAA\""))
    }

    fun testArticleDateFormat() {
        assertEq("OCT 14, 2026 AT 2:20 PM", RssFormat.articleDate(1791987600000L, java.time.ZoneOffset.UTC))
        assertEq("Updating…", RssFormat.status(true, 0))
        assertEq("Updated 2:20 PM", RssFormat.status(false, 1791987600000L, java.time.ZoneOffset.UTC))
    }

    fun testOpmlRoundTripFlattensNestedFolders() {
        val opml = """<?xml version="1.0"?><opml version="2.0"><head><title>x</title></head><body>
            <outline text="Top" type="rss" xmlUrl="feed://top.example.org/rss" htmlUrl="https://top.example.org/"/>
            <outline text="Tech"><outline text="Example Engineering" xmlUrl="https://blog.example.org/feed"/>
              <outline text="Nested"><outline title="Deep Example" xmlUrl="https://deep.example.org/atom.xml"/></outline></outline>
            <outline text="bad" xmlUrl="javascript:alert(1)"/></body></opml>"""
        val entries = RssOpml.parse(opml)
        assertEq(
            listOf(
                RssOpmlEntry("https://top.example.org/rss", "Top", "", "https://top.example.org/"),
                RssOpmlEntry("https://blog.example.org/feed", "Example Engineering", "Tech", ""),
                RssOpmlEntry("https://deep.example.org/atom.xml", "Deep Example", "Tech", ""),
            ),
            entries,
        )
        val library = RssLibrary()
        val result = library.importOpml(opml, 1000)
        assertEq(RssImportResult(3, 0, 1), result)
        assertEq(RssImportResult(0, 3, 0), library.importOpml(opml, 2000))
        val exported = library.exportOpml(3000)
        val again = RssOpml.parse(exported)
        assertEq(entries.map { it.url to it.folder }.toSet(), again.map { it.url to it.folder }.toSet())
        expect(exported.contains("<outline text=\"Tech\" title=\"Tech\">"))
    }
}
