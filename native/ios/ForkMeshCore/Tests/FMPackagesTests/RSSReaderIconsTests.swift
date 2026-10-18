import FMBase
import FMNet
import Foundation
import XCTest

@testable import FMPackages

final class RSSReaderIconsTests: XCTestCase {
    static let png = Data([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 0x49, 0x48, 0x44, 0x52])
    static let ico = Data([0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x10, 0x10, 0, 0, 1, 0, 32, 0])

    final class Site: @unchecked Sendable {
        let lock = NSLock()
        var pages: [String: HTTPResponse] = [:]
        var offline = false
        var asked: [String] = []

        func set(_ url: String, _ response: HTTPResponse) {
            lock.lock()
            pages[url] = response
            lock.unlock()
        }

        func response(_ request: HTTPRequest) throws -> HTTPResponse {
            lock.lock()
            defer { lock.unlock() }
            let url = request.url.absoluteString
            asked.append(url)
            if offline { throw HTTPError.transport("offline") }
            return pages[url] ?? HTTPResponse(status: 404, text: "<html>missing</html>")
        }
    }

    func fetcher(_ site: Site) -> RSSReaderFetcher {
        RSSReaderFetcher(http: FakeHTTPClient { request in try site.response(request) })
    }

    func testPicksTheBestPageIcon() {
        let page = """
        <html><head><base href="https://eng.example.org/static/">
        <link rel="mask-icon" href="/mask.svg" color="#000">
        <link rel="icon" type="image/svg+xml" href="/icon.svg">
        <link rel="shortcut icon" href="/favicon.ico">
        <link rel="icon" sizes="16x16" href="icons/favicon-16x16.png">
        <link rel="icon" href="icons/favicon-32x32.png">
        <link rel="icon" sizes="512x512" href="icons/big.png">
        <link rel="icon" sizes="300x100" href="icons/wide.png">
        <link rel="apple-touch-icon-precomposed" sizes="152x152" href="/touch-152.png">
        <link rel="icon" href="data:image/png;base64,AAAA">
        </head><body><link rel="apple-touch-icon" sizes="180x180" href="/touch-180.png"></body></html>
        """
        let links = RSSFeedIcons.links(page, base: "https://eng.example.org/blog/")
        XCTAssertFalse(links.contains { $0.url.hasSuffix(".svg") })
        XCTAssertEqual(links.first { $0.url.hasSuffix("favicon-32x32.png") }?.sizes, [32])
        XCTAssertEqual(RSSFeedIcons.ranked(links), [
            "https://eng.example.org/touch-180.png",
            "https://eng.example.org/touch-152.png",
            "https://eng.example.org/static/icons/favicon-32x32.png",
            "https://eng.example.org/favicon.ico",
            "https://eng.example.org/static/icons/big.png",
            "https://eng.example.org/static/icons/favicon-16x16.png",
            "https://eng.example.org/static/icons/wide.png",
        ])
        let plain = RSSFeedIcons.links("<link rel=\"icon\" href=\"/a.png\"><link rel=\"apple-touch-icon\" href=\"/t.png\">", base: "https://x.example.org/p")
        XCTAssertEqual(RSSFeedIcons.best(plain), "https://x.example.org/t.png")
    }

    func testCandidateOrderAndFeedProxies() {
        var feed = RSSFeed(id: "f", url: "https://feeds.example.net/eng.xml", siteUrl: "https://eng.example.org/blog/")
        feed.iconUrl = "https://cdn.example.org/logo.png"
        let page = [RSSIconLink(url: "https://eng.example.org/touch.png", kind: .touch)]
        XCTAssertEqual(RSSFeedIcons.pages(feed), ["https://eng.example.org/blog/", "https://eng.example.org/"])
        XCTAssertEqual(RSSFeedIcons.plan(feed, page: page), [
            "https://cdn.example.org/logo.png",
            "https://eng.example.org/touch.png",
            "https://eng.example.org/favicon.ico",
            "https://feeds.example.net/favicon.ico",
            "https://eng.example.org/apple-touch-icon.png",
            "https://feeds.example.net/apple-touch-icon.png",
        ])

        let proxied = RSSFeed(id: "p", url: "https://feeds.feedburner.com/Example", siteUrl: "https://www.example.com/news")
        XCTAssertTrue(RSSFeedIcons.isProxy("feeds.feedburner.com"))
        XCTAssertTrue(RSSFeedIcons.isProxy("feedproxy.google.com"))
        XCTAssertFalse(RSSFeedIcons.isProxy("example.com"))
        XCTAssertEqual(RSSFeedIcons.homePage(proxied), "https://www.example.com/news")
        XCTAssertEqual(RSSFeedIcons.fallbacks(proxied), ["https://www.example.com/favicon.ico", "https://www.example.com/apple-touch-icon.png"])

        let bare = RSSFeed(id: "b", url: "https://feeds.feedburner.com/Bare")
        XCTAssertEqual(RSSFeedIcons.homePage(bare), "")
        XCTAssertEqual(RSSFeedIcons.plan(bare), [])

        let unknownHome = RSSFeed(id: "u", url: "https://blog.example.org/feed.xml", siteUrl: "https://blog.example.org/feed.xml")
        XCTAssertEqual(RSSFeedIcons.homePage(unknownHome), "https://blog.example.org/")
        XCTAssertEqual(RSSFeedIcons.pages(unknownHome), ["https://blog.example.org/"])
        XCTAssertEqual(unknownHome.iconAddress, "https://blog.example.org/favicon.ico")
    }

    func testRecognisesImagesAndRejectsPages() {
        XCTAssertTrue(RSSFeedIcons.isImage(Self.png))
        XCTAssertTrue(RSSFeedIcons.isImage(Self.ico))
        XCTAssertTrue(RSSFeedIcons.isImage(Data([0xFF, 0xD8, 0xFF, 0xE0, 0, 0x10])))
        XCTAssertTrue(RSSFeedIcons.isImage(Data("GIF89a\u{1}\u{0}".utf8)))
        XCTAssertTrue(RSSFeedIcons.isImage(Data("RIFF\u{0}\u{0}\u{0}\u{0}WEBPVP8 ".utf8)))
        XCTAssertFalse(RSSFeedIcons.isImage(Data("<!DOCTYPE html><html>".utf8)))
        XCTAssertFalse(RSSFeedIcons.isImage(Data("<svg xmlns=\"http://www.w3.org/2000/svg\"/>".utf8)))
        XCTAssertFalse(RSSFeedIcons.isImage(Data()))
        XCTAssertEqual(RSSFeedIcons.mime(Self.png), "image/png")
        XCTAssertEqual(RSSFeedIcons.mime(Self.ico), "image/x-icon")
        XCTAssertEqual(RSSFeedIcons.mime(Data([0, 0, 0, 0x1C] + Array("ftypavif".utf8))), "image/avif")
        XCTAssertEqual(RSSFeedIcons.inline(Self.png), "data:image/png;base64," + Self.png.base64EncodedString())
        XCTAssertEqual(RSSFeedIcons.inline(Data("<html>".utf8)), "")
        XCTAssertEqual(RSSFeedIcons.inline(Self.png + Data(count: RSSFeedIcons.maxInlineBytes)), "")
    }

    func testArticlePageInlinesTheCachedIcon() {
        var feed = RSSFeed(id: "f", url: "https://eng.example.org/feed.xml", title: "Eng", siteUrl: "https://eng.example.org/")
        feed.iconFound = "https://eng.example.org/favicon.ico"
        let article = RSSArticle(feedId: "f", guid: "g")
        let inlined = RSSArticlePage.html(article: article, body: RSSArticleBody(), feed: feed, settings: RSSReaderSettings(), reader: false, icon: Self.png)
        XCTAssertTrue(inlined.contains("<img src=\"data:image/png;base64,"), inlined)
        let remote = RSSArticlePage.html(article: article, body: RSSArticleBody(), feed: feed, settings: RSSReaderSettings(), reader: false)
        XCTAssertTrue(remote.contains("<img src=\"https://eng.example.org/favicon.ico\""), remote)
        feed.iconFound = ""
        feed.iconCheckedAt = 5
        feed.iconSource = RSSFeedIcons.source(feed)
        let letter = RSSArticlePage.html(article: article, body: RSSArticleBody(), feed: feed, settings: RSSReaderSettings(), reader: false)
        XCTAssertTrue(letter.contains("<div class=\"tile\">E</div>"), letter)
    }

    func testLookupFallsThroughBrokenCandidates() async {
        let site = Site()
        var feed = RSSFeed(id: "f", url: "https://eng.example.org/feed.xml", siteUrl: "https://eng.example.org/")
        feed.iconUrl = "https://eng.example.org/broken.png"
        site.set("https://eng.example.org/broken.png", HTTPResponse(status: 200, headers: ["Content-Type": "image/png"], text: "<html>login</html>"))
        site.set("https://eng.example.org/", HTTPResponse(
            status: 200, headers: ["Content-Type": "text/html"],
            text: "<html><head><link rel=\"apple-touch-icon\" href=\"/gone.png\"><link rel=\"icon\" href=\"/undecodable.png\"></head></html>"))
        site.set("https://eng.example.org/undecodable.png", HTTPResponse(status: 200, body: Self.png + Data([1])))
        site.set("https://eng.example.org/favicon.ico", HTTPResponse(status: 200, headers: ["Content-Type": "text/html"], text: "<html></html>"))
        site.set("https://eng.example.org/apple-touch-icon.png", HTTPResponse(status: 200, body: Self.png))
        let result = await fetcher(site).icon(for: feed, decodes: { $0 == Self.png })
        XCTAssertEqual(result, .found(url: "https://eng.example.org/apple-touch-icon.png", data: Self.png))
        XCTAssertEqual(site.asked, [
            "https://eng.example.org/broken.png", "https://eng.example.org/", "https://eng.example.org/gone.png",
            "https://eng.example.org/undecodable.png", "https://eng.example.org/favicon.ico",
            "https://eng.example.org/apple-touch-icon.png",
        ])

        let none = Site()
        let missing = await fetcher(none).icon(for: RSSFeed(id: "n", url: "https://none.example.org/rss"))
        XCTAssertEqual(missing, .missing)
        let offline = Site()
        offline.offline = true
        let unreachable = await fetcher(offline).icon(for: RSSFeed(id: "o", url: "https://off.example.org/rss"))
        XCTAssertEqual(unreachable, .unreachable)
    }

    func testRetryPolicy() {
        var feed = RSSFeed(id: "f", url: "https://eng.example.org/feed.xml")
        let day = RSSReaderLimits.dayMs
        XCTAssertTrue(RSSFeedIcons.needsLookup(feed, now: 1_000, hasImage: false))
        feed.iconCheckedAt = 1_000
        feed.iconSource = RSSFeedIcons.source(feed)
        XCTAssertFalse(RSSFeedIcons.needsLookup(feed, now: 1_000 + day - 1, hasImage: false))
        XCTAssertTrue(RSSFeedIcons.needsLookup(feed, now: 1_000 + day, hasImage: false))
        XCTAssertEqual(feed.iconAddress, "")
        feed.iconFound = "https://eng.example.org/favicon.ico"
        XCTAssertEqual(feed.iconAddress, "https://eng.example.org/favicon.ico")
        XCTAssertFalse(RSSFeedIcons.needsLookup(feed, now: 1_000 + day, hasImage: true))
        XCTAssertTrue(RSSFeedIcons.needsLookup(feed, now: 1_000 + day, hasImage: false))
        XCTAssertTrue(RSSFeedIcons.needsLookup(feed, now: 1_000 + RSSFeedIcons.refreshMs, hasImage: true))
        feed.iconUrl = "https://eng.example.org/new.png"
        XCTAssertTrue(RSSFeedIcons.needsLookup(feed, now: 1_001, hasImage: true))
    }

    @MainActor
    func testModelCachesTheIconAndAFailure() async throws {
        let site = Site()
        site.set("https://eng.example.org/favicon.ico", HTTPResponse(status: 200, body: Self.ico))
        var clock: Int64 = 1_000
        let disk = RSSReaderMemoryDisk()
        let store = RSSReaderStore(disk: disk, now: { clock })
        let model = RSSReaderModel(store: store, fetcher: fetcher(site), saveDelayMs: 0)
        let feed = try store.addFeed(url: "https://eng.example.org/feed.xml", siteUrl: "https://eng.example.org/")
        let other = try store.addFeed(url: "https://quiet.example.org/feed.xml")
        XCTAssertTrue(model.needsIcon(feed.id, hasImage: false))
        let data = await model.resolveIcon(feed.id)
        XCTAssertEqual(data, Self.ico)
        XCTAssertEqual(model.iconData(feed.id), Self.ico)
        XCTAssertEqual(model.feed(feed.id)?.iconFound, "https://eng.example.org/favicon.ico")
        XCTAssertFalse(model.needsIcon(feed.id, hasImage: true))

        let missing = await model.resolveIcon(other.id)
        XCTAssertNil(missing)
        XCTAssertEqual(model.feed(other.id)?.iconCheckedAt, 1_000)
        XCTAssertEqual(model.feed(other.id)?.iconAddress, "")
        let asked = site.asked.count
        XCTAssertFalse(model.needsIcon(other.id, hasImage: false))
        let again = await model.resolveIcon(other.id)
        XCTAssertNil(again)
        XCTAssertEqual(site.asked.count, asked)
        clock += RSSReaderLimits.dayMs
        await model.refresh(force: true)
        XCTAssertTrue(model.needsIcon(other.id, hasImage: false))

        model.unsubscribe(feed.id)
        model.saveNow()
        XCTAssertNil(disk.files[RSSReaderStore.iconFile(feed.id)])
    }
}
