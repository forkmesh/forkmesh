import FMBase
import FMNet
import Foundation
import XCTest

@testable import FMPackages

final class RSSReaderFetcherTests: XCTestCase {
    static func rss(_ title: String, items: [String]) -> String {
        let entries = items.map { "<item><title>\($0)</title><guid>\($0)</guid><pubDate>Wed, 14 Oct 2026 14:00:00 GMT</pubDate></item>" }
        return "<rss version=\"2.0\"><channel><title>\(title)</title><link>https://eng.example.org/</link>\(entries.joined())</channel></rss>"
    }

    final class Site: @unchecked Sendable {
        let lock = NSLock()
        var pages: [String: HTTPResponse] = [:]
        var failing: Set<String> = []

        func set(_ url: String, _ response: HTTPResponse) {
            lock.lock()
            pages[url] = response
            lock.unlock()
        }

        func response(_ request: HTTPRequest) throws -> HTTPResponse {
            lock.lock()
            defer { lock.unlock() }
            let url = request.url.absoluteString
            if failing.contains(url) { throw HTTPError.transport("TLS handshake failed") }
            return pages[url] ?? HTTPResponse(status: 404, text: "missing")
        }
    }

    func client(_ site: Site) -> FakeHTTPClient {
        FakeHTTPClient { request in try site.response(request) }
    }

    func testConditionalGetAndNotModified() async {
        let site = Site()
        site.set("https://eng.example.org/feed.xml", HTTPResponse(status: 304))
        let http = client(site)
        let fetcher = RSSReaderFetcher(http: http)
        var feed = RSSFeed(id: "f", url: "https://eng.example.org/feed.xml")
        feed.etag = "\"abc\""
        feed.lastModified = "Wed, 14 Oct 2026 14:00:00 GMT"
        let result = await fetcher.fetch(feed, now: 0)
        guard case .notModified = result.outcome else { return XCTFail("expected not modified") }
        let request = http.lastRequest
        XCTAssertEqual(request?.headers["If-None-Match"], "\"abc\"")
        XCTAssertEqual(request?.headers["If-Modified-Since"], "Wed, 14 Oct 2026 14:00:00 GMT")
        XCTAssertEqual(request?.followRedirects, false)
        XCTAssertEqual(request?.maxResponseBytes, RSSReaderLimits.maxFetchBytes)
        XCTAssertNil(request?.headers["Authorization"])
        XCTAssertNil(request?.headers["Cookie"])
    }

    func testFollowsRedirectsAndTracksPermanence() async {
        let site = Site()
        site.set("https://old.example.org/rss", HTTPResponse(status: 301, headers: ["Location": "https://new.example.org/rss"]))
        site.set("https://new.example.org/rss", HTTPResponse(status: 200, headers: ["ETag": "\"v2\""], text: Self.rss("Example Engineering", items: ["one"])))
        site.set("https://temp.example.org/rss", HTTPResponse(status: 302, headers: ["Location": "/elsewhere"]))
        site.set("https://temp.example.org/elsewhere", HTTPResponse(status: 308, headers: ["Location": "https://new.example.org/rss"]))
        site.set("https://loop.example.org/a", HTTPResponse(status: 301, headers: ["Location": "/a"]))
        let fetcher = RSSReaderFetcher(http: client(site))
        let moved = await fetcher.get("https://old.example.org/rss")
        XCTAssertEqual(try? moved.get().finalUrl, "https://new.example.org/rss")
        XCTAssertEqual(try? moved.get().permanent, true)
        XCTAssertEqual(try? moved.get().etag, "\"v2\"")
        let temporary = await fetcher.get("https://temp.example.org/rss")
        XCTAssertEqual(try? temporary.get().finalUrl, "https://new.example.org/rss")
        XCTAssertEqual(try? temporary.get().permanent, false)
        let loop = await fetcher.get("https://loop.example.org/a")
        guard case .failure(.tooManyRedirects) = loop else { return XCTFail("expected a redirect loop error") }
        let result = await fetcher.fetch(RSSFeed(id: "f", url: "https://old.example.org/rss"), now: 1_791_987_600_000)
        guard case let .updated(feed, items, finalUrl, permanent, etag, _) = result.outcome else { return XCTFail("expected an update") }
        XCTAssertEqual(feed.title, "Example Engineering")
        XCTAssertEqual(items.map(\.guid), ["one"])
        XCTAssertEqual(finalUrl, "https://new.example.org/rss")
        XCTAssertTrue(permanent)
        XCTAssertEqual(etag, "\"v2\"")
    }

    func testUpgradesPlainHTTPForAppTransportSecurity() async {
        let site = Site()
        site.set("https://secure.example.org/rss", HTTPResponse(status: 200, text: Self.rss("Secure", items: ["a"])))
        site.failing = ["https://plain.example.org/rss"]
        let http = client(site)
        let fetcher = RSSReaderFetcher(http: http)
        let upgraded = await fetcher.fetch(RSSFeed(id: "s", url: "http://secure.example.org/rss"), now: 0)
        XCTAssertEqual(http.requests.first?.url.absoluteString, "https://secure.example.org/rss")
        guard case let .updated(_, _, finalUrl, permanent, _, _) = upgraded.outcome else { return XCTFail("expected an update") }
        XCTAssertEqual(finalUrl, "https://secure.example.org/rss")
        XCTAssertTrue(permanent, "the https address replaces the stored http one")
        let plain = await fetcher.fetch(RSSFeed(id: "p", url: "http://plain.example.org/rss"), now: 0)
        guard case .failed(let code) = plain.outcome else { return XCTFail("expected a failure") }
        XCTAssertEqual(code, "insecure_http")
        let lenient = RSSReaderFetcher(http: client(site), upgradesInsecure: false)
        let direct = await lenient.get("http://plain.example.org/rss")
        guard case .failure(.http(404)) = direct else { return XCTFail("expected the plain address to be asked as is") }
    }

    func testRefusesLargeBodiesAndReportsHTTPErrors() async {
        let site = Site()
        site.set("https://big.example.org/rss", HTTPResponse(status: 200, body: Data(count: RSSReaderLimits.maxFetchBytes + 1)))
        site.set("https://gone.example.org/rss", HTTPResponse(status: 410))
        site.set("https://page.example.org/rss", HTTPResponse(status: 200, text: "<html><body>Not a feed</body></html>"))
        let fetcher = RSSReaderFetcher(http: client(site))
        let big = await fetcher.fetch(RSSFeed(id: "b", url: "https://big.example.org/rss"), now: 0)
        guard case .failed("too_large") = big.outcome else { return XCTFail("expected too_large") }
        let gone = await fetcher.fetch(RSSFeed(id: "g", url: "https://gone.example.org/rss"), now: 0)
        guard case .failed("http_410") = gone.outcome else { return XCTFail("expected http_410") }
        let page = await fetcher.fetch(RSSFeed(id: "p", url: "https://page.example.org/rss"), now: 0)
        guard case .failed("not_a_feed") = page.outcome else { return XCTFail("expected not_a_feed") }
    }

    func testDiscoversFromLinksThenGuessesPaths() async {
        let site = Site()
        site.set("https://eng.example.org", HTTPResponse(status: 200, text: """
            <html><head><link rel="icon" href="/fav.png"><link rel="alternate" type="application/atom+xml" href="/atom.xml"></head></html>
            """))
        site.set("https://eng.example.org/atom.xml", HTTPResponse(status: 200, text: """
            <feed xmlns="http://www.w3.org/2005/Atom"><title>Example Engineering</title><link rel="self" href="https://eng.example.org/atom.xml"/>
            <entry><id>e1</id><title>One</title></entry></feed>
            """))
        site.set("https://weekly.example.net", HTTPResponse(status: 200, text: "<html><head><title>Weekly</title></head></html>"))
        site.set("https://weekly.example.net/feed.xml", HTTPResponse(status: 200, text: Self.rss("Open Source Weekly", items: ["x"])))
        site.set("https://nothing.example.com", HTTPResponse(status: 200, text: "<html></html>"))
        let fetcher = RSSReaderFetcher(http: client(site))
        let linked = await fetcher.discover("eng.example.org")
        XCTAssertEqual(try? linked.get().url, "https://eng.example.org/atom.xml")
        XCTAssertEqual(try? linked.get().feed.siteUrl, "https://eng.example.org")
        XCTAssertEqual(try? linked.get().feed.iconUrl, "https://eng.example.org/fav.png")
        let guessed = await fetcher.discover("feed://weekly.example.net")
        XCTAssertEqual(try? guessed.get().url, "https://weekly.example.net/feed.xml")
        let nothing = await fetcher.discover("nothing.example.com")
        guard case .failure(.noFeedFound) = nothing else { return XCTFail("expected no feed") }
        let invalid = await fetcher.discover("   ")
        guard case .failure(.invalidAddress) = invalid else { return XCTFail("expected an invalid address") }
    }

    func testReaderViewExtractsTheOriginalPage() async {
        let paragraph = String(repeating: "Readable article text for the reader view. ", count: 5)
        let site = Site()
        site.set("https://eng.example.org/p/1", HTTPResponse(status: 200, text: "<html><body><main><p>\(paragraph)</p></main></body></html>"))
        site.set("https://eng.example.org/p/2", HTTPResponse(status: 200, text: "<html><body><p>tiny</p></body></html>"))
        let fetcher = RSSReaderFetcher(http: client(site))
        let good = await fetcher.readable("https://eng.example.org/p/1")
        XCTAssertTrue((try? good.get().contentHtml)?.hasPrefix("<p>Readable") ?? false)
        let bad = await fetcher.readable("https://eng.example.org/p/2")
        guard case .failure(.noReadableText) = bad else { return XCTFail("expected no readable text") }
    }

    @MainActor
    func testModelAddsRefreshesAndReadsLocally() async throws {
        let site = Site()
        site.set("https://eng.example.org/feed.xml", HTTPResponse(status: 200, headers: ["ETag": "\"1\""], text: Self.rss("Example Engineering", items: ["one", "two"])))
        site.set("https://weekly.example.net/rss", HTTPResponse(status: 200, text: Self.rss("Open Source Weekly", items: ["w"])))
        let http = client(site)
        let disk = RSSReaderMemoryDisk()
        let clock = RSSReaderStoreTests.Clock()
        let store = RSSReaderStore(disk: disk, now: { clock.now })
        let model = RSSReaderModel(store: store, fetcher: RSSReaderFetcher(http: http), saveDelayMs: 1)
        let feed = try await model.addFeed("https://eng.example.org/feed.xml", title: "Eng")
        XCTAssertEqual(feed.displayTitle, "Eng")
        XCTAssertEqual(model.counts.unread, 2)
        XCTAssertEqual(model.activity.first?.text, "Subscribed to Eng")
        do {
            _ = try await model.addFeed("http://eng.example.org/feed.xml")
            XCTFail("expected a duplicate")
        } catch {
            XCTAssertEqual(error as? RSSReaderError, .duplicate(feed.id))
        }
        let requestsBefore = http.requests.count
        let folder = try model.addFolder("Tech")
        _ = try await model.addFeed("weekly.example.net/rss", folderId: folder.id)
        XCTAssertEqual(http.requests.count, requestsBefore + 1)
        XCTAssertEqual(model.sidebar().map(\.id).first, "feed:\(feed.id)")
        await model.refresh()
        XCTAssertEqual(http.requests.count, requestsBefore + 1, "nothing is due right after adding")
        site.set("https://eng.example.org/feed.xml", HTTPResponse(status: 304))
        clock.now += 2 * RSSReaderLimits.minForceGapMs
        await model.refresh(force: true)
        XCTAssertFalse(model.refreshing)
        XCTAssertEqual(model.activity.first?.kind, "refresh")
        let list = model.timeline(.feed(feed.id))
        let first = try XCTUnwrap(list.first)
        model.open(first.id)
        XCTAssertTrue(model.article(first.id)?.read ?? false)
        model.toggleStar(first.id)
        XCTAssertEqual(model.counts.starred, 0)
        XCTAssertEqual(model.timeline(.starred).count, 1)
        let next = try XCTUnwrap(model.nextUnread(after: first.id, in: list, scope: .feed(feed.id)))
        XCTAssertNotEqual(next.article.id, first.id)
        XCTAssertEqual(next.scope, .feed(feed.id))
        XCTAssertEqual(model.markAllRead(.unread), 2)
        XCTAssertTrue(model.exportOPML().contains("https://weekly.example.net/rss"))
        let imported = model.importOPML(Data("<opml><body><outline text=\"X\" xmlUrl=\"https://x.example.com/rss\"/></body></opml>".utf8))
        XCTAssertEqual(imported, "Imported 1 subscription, skipped 0.")
        XCTAssertEqual(model.importOPML(Data("nope".utf8)), RSSReaderError.unreadableOPML.message)
        model.saveNow()
        XCTAssertNotNil(disk.files[RSSReaderStore.libraryFile])
        model.unsubscribe(feed.id)
        XCTAssertNil(model.feed(feed.id))
    }
}
