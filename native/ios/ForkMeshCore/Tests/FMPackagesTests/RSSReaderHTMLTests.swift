import Foundation
import XCTest

@testable import FMPackages

final class RSSReaderHTMLTests: XCTestCase {
    let base = "https://eng.example.org/posts/first"

    func testSanitizerKeepsTheAllowlistOnly() {
        let dirty = """
        <div onclick="x()" class="c"><p style="color:red">Hi <b>there</b><script>alert(1)</script>
        <a href="javascript:alert(1)">bad</a> <a href="/rel" onmouseover="x">rel</a> <a href="mailto:ada@example.org">mail</a>
        <img src="data:image/png;base64,AAAA" data-src="/lazy.png" alt="A &quot;pic&quot;" width="640px" height="480">
        <iframe src="https://video.example.com/embed/1"></iframe><style>p{}</style><form><input value="x"></form>
        <svg><circle/></svg><unknown>kept text</unknown><video src="/v.mp4"></video><br/><table><tr><td colspan="2">c</td></tr></table>
        """
        let clean = RSSHTML.sanitize(dirty, base: base)
        XCTAssertFalse(clean.contains("script"))
        XCTAssertFalse(clean.contains("onclick"))
        XCTAssertFalse(clean.contains("style"))
        XCTAssertFalse(clean.contains("javascript"))
        XCTAssertFalse(clean.contains("svg"))
        XCTAssertFalse(clean.contains("class="))
        XCTAssertTrue(clean.contains("<div><p>Hi <b>there</b>"), clean)
        XCTAssertTrue(clean.contains("<a rel=\"noopener noreferrer\" target=\"_blank\">bad</a>"), clean)
        XCTAssertTrue(clean.contains("<a href=\"https://eng.example.org/rel\" rel=\"noopener noreferrer\" target=\"_blank\">rel</a>"), clean)
        XCTAssertTrue(clean.contains("href=\"mailto:ada@example.org\""), clean)
        XCTAssertTrue(
            clean.contains("<img src=\"https://eng.example.org/lazy.png\" alt=\"A &quot;pic&quot;\" height=\"480\" loading=\"lazy\" referrerpolicy=\"no-referrer\">"),
            clean)
        XCTAssertTrue(
            clean.contains("<p><a href=\"https://video.example.com/embed/1\" rel=\"noopener noreferrer\" target=\"_blank\">Open embedded content</a></p>"),
            clean)
        XCTAssertTrue(clean.contains("kept text"))
        XCTAssertTrue(clean.contains("<video src=\"https://eng.example.org/v.mp4\" controls></video>"), clean)
        XCTAssertTrue(clean.contains("<br>"))
        XCTAssertTrue(clean.contains("<td colspan=\"2\">c</td>"))
        XCTAssertTrue(clean.hasSuffix("</div>"), clean)
        XCTAssertEqual(RSSHTML.sanitize("   "), "")
        XCTAssertEqual(RSSHTML.sanitize("a < b & c"), "a &lt; b &amp; c")
        XCTAssertEqual(RSSHTML.sanitize("<p>Caf&eacute; &#8212; &#x2603; &nbsp</p>"), "<p>Caf\u{00E9} \u{2014} \u{2603} \u{00A0}</p>")
    }

    func testSanitizerReplacesMediaWhenImagesAreOff() {
        let html = "<p><img src=\"https://cdn.example.org/a.png\" alt=\"Chart\"><audio src=\"https://cdn.example.org/a.mp3\"><source src=\"https://cdn.example.org/a.ogg\"></audio></p>"
        let clean = RSSHTML.sanitize(html, base: base, remoteMedia: false)
        XCTAssertFalse(clean.contains("<img"))
        XCTAssertFalse(clean.contains("<audio"))
        XCTAssertFalse(clean.contains("<source"))
        XCTAssertTrue(clean.contains(">Image: Chart</a>"), clean)
        XCTAssertTrue(clean.contains(">Open audio</a>"), clean)
        let upgraded = RSSHTML.sanitize(
            "<img src=\"http://cdn.example.org/a.png\"><a href=\"http://eng.example.org/x\">x</a>", upgradeMedia: true)
        XCTAssertTrue(upgraded.contains("src=\"https://cdn.example.org/a.png\""), upgraded)
        XCTAssertTrue(upgraded.contains("href=\"http://eng.example.org/x\""), upgraded)
    }

    func testPlainTextSummaries() {
        XCTAssertEqual(RSSHTML.plainText("<p>One</p><p>Two <script>x</script>three</p>"), "One Two three")
        XCTAssertEqual(RSSHTML.plainText("Fish &amp; chips"), "Fish & chips")
        XCTAssertEqual(RSSHTML.plainText("alpha beta gamma delta", limit: 12), "alpha beta\u{2026}")
        XCTAssertEqual(RSSHTML.plainText("abcdefghijkl", limit: 5), "abcde\u{2026}")
    }

    func testDiscoversFeedLinks() {
        let page = """
        <html><head><base href="https://eng.example.org/blog/"><title> Example
        Engineering </title>
        <link rel="icon" href="/favicon.png"><link rel="apple-touch-icon" href="touch.png">
        <link rel="alternate" type="application/feed+json" href="feed.json" title="JSON">
        <link rel="alternate" type="application/atom+xml" href="atom.xml">
        <link rel="Alternate" type="application/rss+xml; charset=utf-8" href="/rss.xml" title="RSS">
        <link rel="alternate" type="text/html" href="/other">
        </head></html>
        """
        let found = RSSFeedTypes.discover(page, base: "https://eng.example.org/")
        XCTAssertEqual(found.feeds.map(\.url), [
            "https://eng.example.org/rss.xml", "https://eng.example.org/blog/atom.xml", "https://eng.example.org/blog/feed.json",
        ])
        XCTAssertEqual(found.icon, "https://eng.example.org/blog/touch.png")
        XCTAssertEqual(found.title, "Example Engineering")
    }

    func testReadableExtraction() {
        let paragraph = String(repeating: "This sentence carries the article text along. ", count: 6)
        let page = """
        <html><head><title>Fallback</title><meta property="og:title" content="A &amp; B"></head><body>
        <nav><p>\(paragraph)</p></nav><header><h1>Site</h1></header>
        <div class="sidebar"><p>Short</p></div>
        <div id="content"><h2>Heading</h2><p>\(paragraph)</p><p>\(paragraph)<img src="/i.png"></p><script>x()</script>
        <p>Third <a href="javascript:x">link</a></p></div>
        <footer><p>\(paragraph)</p></footer></body></html>
        """
        let result = RSSReadable.extract(page, base: "https://eng.example.org/posts/first")
        XCTAssertEqual(result.title, "A & B")
        XCTAssertTrue(result.contentHtml.hasPrefix("<h2>Heading</h2><p>This sentence"), result.contentHtml)
        XCTAssertTrue(result.contentHtml.contains("src=\"https://eng.example.org/i.png\""))
        XCTAssertFalse(result.contentHtml.contains("script"))
        XCTAssertFalse(result.contentHtml.contains("javascript"))
        XCTAssertFalse(result.contentHtml.contains("Site"))
        let article = "<html><body><div><p>\(paragraph)</p></div><article><p>\(paragraph)</p><p>\(paragraph)</p></article></body></html>"
        let preferred = RSSReadable.extract(article, base: "https://eng.example.org/")
        XCTAssertTrue(preferred.contentHtml.hasPrefix("<p>"), preferred.contentHtml)
        XCTAssertEqual(preferred.contentHtml.components(separatedBy: "<p>").count - 1, 2)
        XCTAssertEqual(RSSReadable.extract("<html><body><p>Too short</p></body></html>", base: "").contentHtml, "")
    }

    func testTokenizerHandlesCommentsAndRawText() {
        let tokens = RSSHTMLTokenizer.tokens("<!-- <p>no</p> --><p a=1 b='two' c>x &lt; y</p><style>p>a{}</style>< 3")
        XCTAssertEqual(tokens, [
            .start(name: "p", attributes: [RSSHTMLAttribute("a", "1"), RSSHTMLAttribute("b", "two"), RSSHTMLAttribute("c", "")], selfClosing: false),
            .text("x < y"), .end(name: "p"), .start(name: "style", attributes: [], selfClosing: false), .text("p>a{}"),
            .end(name: "style"), .text("< 3"),
        ])
    }

    func testArticlePageDisablesScriptsAndCarriesTheHeader() {
        var article = RSSArticle(feedId: "f", guid: "g")
        article.title = "Hello <world>"
        article.author = "Ada Example"
        article.url = "https://eng.example.org/p"
        article.published = 1_791_987_600_000
        var feed = RSSFeed(id: "f", url: "https://eng.example.org/feed.xml", title: "Example Engineering", siteUrl: "https://eng.example.org/")
        feed.iconUrl = "https://eng.example.org/icon.png"
        let body = RSSArticleBody(
            contentHtml: "<p onclick=\"x\">Body<script>x()</script></p>", commentsUrl: "https://eng.example.org/p#c",
            enclosures: [RSSEnclosure(url: "https://cdn.example.org/a.mp3", type: "audio/mpeg")])
        var settings = RSSReaderSettings()
        settings.articleTheme = .sepia
        settings.articleSize = 20
        let page = RSSArticlePage.html(
            article: article, body: body, feed: feed, settings: settings, reader: false, timeZone: TimeZone(identifier: "UTC")!,
            locale: Locale(identifier: "en_US"))
        XCTAssertTrue(page.contains("script-src 'none'"))
        XCTAssertFalse(page.contains("<script"))
        XCTAssertFalse(page.contains("onclick"))
        XCTAssertTrue(page.contains("<p>Body</p>"))
        XCTAssertTrue(page.contains("Hello &lt;world&gt;"))
        XCTAssertTrue(page.contains("class=\"feed\" href=\"https://eng.example.org/\">Example Engineering</a>"))
        XCTAssertTrue(page.contains("<span class=\"author\">Ada Example</span>"))
        XCTAssertTrue(page.contains("<img src=\"https://eng.example.org/icon.png\""))
        XCTAssertTrue(page.contains("OCT 14, 2026"), page)
        XCTAssertTrue(page.contains("<audio controls"))
        XCTAssertTrue(page.contains(">Comments</a>"))
        XCTAssertTrue(page.contains("#F4ECD8"))
        XCTAssertTrue(page.contains("font-size:20px"))
        settings.loadRemoteImages = false
        let plain = RSSArticlePage.html(article: article, body: body, feed: feed, settings: settings, reader: false)
        XCTAssertTrue(plain.contains("img-src data:;"))
        XCTAssertFalse(plain.contains("<audio"))
        XCTAssertTrue(plain.contains("<div class=\"tile\">E</div>"))
        var withReader = body
        withReader.readerHtml = "<p>Reader text</p>"
        XCTAssertTrue(RSSArticlePage.html(article: article, body: withReader, feed: feed, settings: settings, reader: true).contains("Reader text"))
    }

    func testAboutCreditsNetNewsWire() {
        XCTAssertTrue(RSSReaderText.credit.contains("Inspired by NetNewsWire"))
        XCTAssertTrue(RSSReaderText.credit.contains("Brent Simmons"))
        XCTAssertTrue(RSSReaderText.credit.contains("Ranchero Software"))
        XCTAssertTrue(RSSReaderText.credit.contains("not affiliated with or endorsed by it"))
        XCTAssertEqual(RSSReaderText.netNewsWireURL, "https://netnewswire.com/")
        XCTAssertEqual(RSSReaderText.netNewsWireSourceURL, "https://github.com/Ranchero-Software/NetNewsWire")
        XCTAssertTrue(RSSReaderText.feedError("insecure_http").contains("plain HTTP"))
        XCTAssertEqual(RSSReaderText.feedError("http_404"), "The feed was not found (404).")
    }
}
