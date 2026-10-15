import Foundation
import XCTest

@testable import FMPackages

final class RSSReaderStoreTests: XCTestCase {
    final class Clock: @unchecked Sendable {
        var now: Int64 = 1_791_987_600_000
    }

    var clock = Clock()
    var disk = RSSReaderMemoryDisk()
    var counter = 0

    func makeStore() -> RSSReaderStore {
        let clock = self.clock
        return RSSReaderStore(disk: disk, now: { clock.now }, newId: { [unowned self] in
            self.counter += 1
            return "id\(self.counter)"
        })
    }

    func parsed(_ items: [(guid: String, title: String, published: Int64)], title: String = "Example Engineering") -> RSSParsedFeed {
        var feed = RSSParsedFeed()
        feed.title = title
        feed.siteUrl = "https://eng.example.org/"
        feed.items = items.map { entry in
            var item = RSSParsedItem()
            item.guid = entry.guid
            item.title = entry.title
            item.url = "https://eng.example.org/\(entry.guid)"
            item.published = entry.published
            item.contentHtml = "<p>\(entry.title)<script>x</script></p>"
            return item
        }
        return feed
    }

    func fetched(_ store: RSSReaderStore, _ feed: RSSFeed, _ parsedFeed: RSSParsedFeed, final: String? = nil, permanent: Bool = false) -> RSSApplied {
        let items = RSSReaderStore.prepare(parsedFeed, feedUrl: feed.url, now: clock.now)
        return store.apply(RSSFetchResult(
            feedId: feed.id, requestedUrl: feed.url,
            outcome: .updated(feed: parsedFeed, items: items, finalUrl: final ?? feed.url, permanent: permanent, etag: "\"e1\"", lastModified: "")))
    }

    func testAddFeedRejectsDuplicatesByHostPortPathAndQuery() throws {
        let store = makeStore()
        let feed = try store.addFeed(url: "https://eng.example.org/feed.xml")
        XCTAssertThrowsError(try store.addFeed(url: "http://ENG.example.org/feed.xml#x")) {
            XCTAssertEqual($0 as? RSSReaderError, .duplicate(feed.id))
        }
        XCTAssertNoThrow(try store.addFeed(url: "https://eng.example.org/feed.xml?lang=en"))
        XCTAssertThrowsError(try store.addFeed(url: "nonsense")) { XCTAssertEqual($0 as? RSSReaderError, .invalidAddress) }
    }

    func testLimitsOnFeedsAndFolders() throws {
        let store = makeStore()
        for index in 0..<RSSReaderLimits.maxFeeds { _ = try store.addFeed(url: "https://f\(index).example.com/rss") }
        XCTAssertThrowsError(try store.addFeed(url: "https://one-more.example.com/rss")) {
            XCTAssertEqual($0 as? RSSReaderError, .tooManyFeeds)
        }
        for index in 0..<RSSReaderLimits.maxFolders { _ = try store.addFolder("Folder \(index)") }
        XCTAssertThrowsError(try store.addFolder("Extra")) { XCTAssertEqual($0 as? RSSReaderError, .tooManyFolders) }
        XCTAssertEqual(try store.addFolder("folder 3").name, "Folder 3")
    }

    func testApplyStoresSanitizedArticlesAndKeepsReadState() throws {
        let store = makeStore()
        let feed = try store.addFeed(url: "https://eng.example.org/feed.xml")
        let first = fetched(store, feed, parsed([("a", "Alpha", clock.now - 1000), ("b", "Beta", clock.now - 2000), ("a", "Dup", 0)]))
        XCTAssertEqual(first.added, 2)
        XCTAssertEqual(store.feed(feed.id)?.title, "Example Engineering")
        XCTAssertEqual(store.feed(feed.id)?.etag, "\"e1\"")
        XCTAssertEqual(store.feed(feed.id)?.nextFetchAt, clock.now + RSSReaderLimits.fetchIntervalMs)
        let alpha = try XCTUnwrap(store.articles[feed.id]?.first { $0.guid == "a" })
        XCTAssertEqual(store.body(for: alpha).contentHtml, "<p>Alpha</p>")
        XCTAssertEqual(alpha.summary, "Alpha")
        store.setRead([alpha.id], true)
        store.setStarred([alpha.id], true)
        clock.now += 60_000
        let again = fetched(store, feed, parsed([("a", "Alpha edited", clock.now - 1000), ("c", "Gamma", clock.now + 10 * RSSReaderLimits.todayMs)]))
        XCTAssertEqual(again.added, 1)
        XCTAssertEqual(again.updated, 1)
        let edited = try XCTUnwrap(store.article(alpha.id))
        XCTAssertEqual(edited.title, "Alpha edited")
        XCTAssertTrue(edited.read)
        XCTAssertTrue(edited.starred)
        XCTAssertEqual(store.articles[feed.id]?.first { $0.guid == "c" }?.published, clock.now, "a date far in the future becomes first-seen time")
    }

    func testTrimsToTheNewest400PlusStarred() throws {
        let store = makeStore()
        let feed = try store.addFeed(url: "https://eng.example.org/feed.xml")
        let old = fetched(store, feed, parsed([("old", "Old", 1000)]))
        XCTAssertEqual(old.added, 1)
        let oldId = try XCTUnwrap(store.articles[feed.id]?.first?.id)
        store.setStarred([oldId], true)
        _ = fetched(store, feed, parsed([("older", "Older", 500)]))
        for batch in 0..<3 {
            let items = (0..<150).map { index -> (guid: String, title: String, published: Int64) in
                let number = batch * 150 + index
                return ("n\(number)", "N\(number)", clock.now - Int64(number) * 1000)
            }
            _ = fetched(store, feed, parsed(items))
        }
        let list = store.articles[feed.id] ?? []
        XCTAssertEqual(list.filter { !$0.starred }.count, RSSReaderLimits.maxArticlesPerFeed)
        XCTAssertTrue(list.contains { $0.id == oldId })
        XCTAssertFalse(list.contains { $0.guid == "older" })
        XCTAssertFalse(list.contains { $0.guid == "n449" })
    }

    func testFailuresBackOffAndNotModifiedClearsThem() throws {
        let store = makeStore()
        let feed = try store.addFeed(url: "https://eng.example.org/feed.xml")
        store.apply(RSSFetchResult(feedId: feed.id, requestedUrl: feed.url, outcome: .failed("http_500")))
        XCTAssertEqual(store.feed(feed.id)?.nextFetchAt, clock.now + RSSReaderLimits.fetchIntervalMs)
        store.apply(RSSFetchResult(feedId: feed.id, requestedUrl: feed.url, outcome: .failed("http_500")))
        XCTAssertEqual(store.feed(feed.id)?.nextFetchAt, clock.now + 2 * RSSReaderLimits.fetchIntervalMs)
        XCTAssertEqual(store.failingFeeds.map(\.id), [feed.id])
        XCTAssertEqual(store.stats().failingFeeds, 1)
        XCTAssertEqual(RSSReaderLimits.backoff(errors: 20), RSSReaderLimits.maxBackoffMs)
        store.apply(RSSFetchResult(feedId: feed.id, requestedUrl: feed.url, outcome: .notModified))
        XCTAssertEqual(store.feed(feed.id)?.errorCount, 0)
        XCTAssertEqual(store.feed(feed.id)?.error, "")
        XCTAssertTrue(store.dueFeeds().isEmpty)
        XCTAssertTrue(store.dueFeeds(force: true).isEmpty, "a feed checked under a minute ago is skipped")
        clock.now += RSSReaderLimits.minForceGapMs
        XCTAssertEqual(store.dueFeeds(force: true).map(\.id), [feed.id])
    }

    func testPermanentRedirectRenamesAndRemembersTheOldAddress() throws {
        let store = makeStore()
        let feed = try store.addFeed(url: "https://old.example.org/rss")
        let other = try store.addFeed(url: "https://taken.example.org/rss")
        _ = fetched(store, feed, parsed([]), final: "https://new.example.org/feed", permanent: true)
        XCTAssertEqual(store.feed(feed.id)?.url, "https://new.example.org/feed")
        XCTAssertEqual(store.feed(feed.id)?.previousUrls, ["https://old.example.org/rss"])
        XCTAssertEqual(store.feed(matching: "http://old.example.org/rss")?.id, feed.id)
        _ = fetched(store, store.feed(feed.id)!, parsed([]), final: "https://elsewhere.example.org/tmp", permanent: false)
        XCTAssertEqual(store.feed(feed.id)?.url, "https://new.example.org/feed")
        _ = fetched(store, store.feed(feed.id)!, parsed([]), final: "https://taken.example.org/rss", permanent: true)
        XCTAssertEqual(store.feed(feed.id)?.url, "https://new.example.org/feed", "never takes another feed's address")
        XCTAssertEqual(store.feed(other.id)?.url, "https://taken.example.org/rss")
    }

    func testCountsTimelineAndMarkAll() throws {
        let store = makeStore()
        let folder = try store.addFolder("Tech")
        let a = try store.addFeed(url: "https://a.example.org/rss", folderId: folder.id)
        let b = try store.addFeed(url: "https://b.example.org/rss")
        _ = fetched(store, a, parsed([("a1", "A one", clock.now - 1000), ("a2", "A two", clock.now - 2 * RSSReaderLimits.todayMs)], title: "Zeta Feed"))
        _ = fetched(store, b, parsed([("b1", "B one", clock.now - 500)], title: "Alpha Feed"))
        var counts = store.counts()
        XCTAssertEqual(counts.unread, 3)
        XCTAssertEqual(counts.today, 2)
        XCTAssertEqual(counts.byFeed[a.id], 2)
        XCTAssertEqual(store.unread(.folder(folder.id), counts: counts), 2)
        XCTAssertEqual(store.timeline(.unread).map(\.guid), ["b1", "a1", "a2"])
        XCTAssertEqual(store.timeline(.today).map(\.guid), ["b1", "a1"])
        XCTAssertEqual(store.timeline(.folder(folder.id)).map(\.guid), ["a1", "a2"])
        XCTAssertEqual(store.timeline(.unread, query: "two").map(\.guid), ["a2"])
        XCTAssertEqual(store.timeline(.unread, query: "alpha feed").map(\.guid), ["b1"])
        store.updateSettings { $0.sortOldestFirst = true }
        XCTAssertEqual(store.timeline(.unread).map(\.guid), ["a2", "a1", "b1"])
        store.updateSettings {
            $0.sortOldestFirst = false
            $0.groupByFeed = true
        }
        XCTAssertEqual(store.timeline(.unread).map(\.guid), ["b1", "a1", "a2"])
        let a1 = try XCTUnwrap(store.articles[a.id]?.first { $0.guid == "a1" })
        store.setStarred([a1.id], true)
        counts = store.counts()
        XCTAssertEqual(counts.starred, 1)
        XCTAssertEqual(store.timeline(.starred).map(\.guid), ["a1"])
        store.setRead([a1.id], true)
        XCTAssertEqual(store.timeline(.unread).map(\.guid), ["b1", "a2"])
        XCTAssertEqual(store.timeline(.unread, keep: [a1.id]).map(\.guid), ["b1", "a1", "a2"])
        XCTAssertEqual(store.timeline(.feed(a.id), unreadOnly: true).map(\.guid), ["a2"])
        XCTAssertEqual(store.markAllRead(.folder(folder.id)), 1)
        XCTAssertEqual(store.counts().unread, 1)
        clock.now += 1
        XCTAssertEqual(store.markAllRead(.unread, asOf: clock.now - 10_000_000), 0)
        XCTAssertEqual(store.markAllRead(.unread), 1)
        XCTAssertEqual(store.counts().unread, 0)
    }

    func testSidebarSortsFoldersAndFeedsTogetherAndFilters() throws {
        let store = makeStore()
        let tech = try store.addFolder("Tech")
        let alpha = try store.addFeed(url: "https://alpha.example.org/rss", title: "alpha news")
        let open = try store.addFeed(url: "https://open.example.net/rss", title: "Open Source Weekly", folderId: tech.id)
        let zed = try store.addFeed(url: "https://zed.example.org/rss", title: "Zed")
        _ = fetched(store, zed, parsed([("z", "Z", clock.now)], title: "Zed"))
        var counts = store.counts()
        XCTAssertEqual(store.sidebar(unreadOnly: false, counts: counts).map(\.id), [
            "feed:\(alpha.id)", "folder:\(tech.id)", "feed:\(open.id)", "feed:\(zed.id)",
        ])
        store.setCollapsed("folder:\(tech.id)", true)
        XCTAssertEqual(store.sidebar(unreadOnly: false, counts: counts).count, 3)
        XCTAssertEqual(store.sidebar(unreadOnly: true, counts: counts).map(\.id), ["feed:\(zed.id)"])
        XCTAssertEqual(store.sidebar(unreadOnly: true, selected: .feed(alpha.id), counts: counts).map(\.id), ["feed:\(alpha.id)", "feed:\(zed.id)"])
        store.deleteFolder(tech.id)
        XCTAssertEqual(store.feed(open.id)?.folderId, "")
        counts = store.counts()
        XCTAssertEqual(store.sidebar(unreadOnly: false, counts: counts).map(\.id), ["feed:\(alpha.id)", "feed:\(open.id)", "feed:\(zed.id)"])
        store.renameFeed(alpha.id, to: "Beta")
        XCTAssertEqual(store.feed(alpha.id)?.displayTitle, "Beta")
        store.renameFeed(alpha.id, to: "alpha news")
        XCTAssertEqual(store.feed(alpha.id)?.customTitle, "")
    }

    func testNextUnreadMovesToTheNextFeedWithUnread() throws {
        let store = makeStore()
        let a = try store.addFeed(url: "https://a.example.org/rss", title: "A")
        let b = try store.addFeed(url: "https://b.example.org/rss", title: "B")
        let c = try store.addFeed(url: "https://c.example.org/rss", title: "C")
        _ = fetched(store, a, parsed([("a1", "A1", clock.now - 1), ("a2", "A2", clock.now - 2)], title: "A"))
        _ = fetched(store, c, parsed([("c1", "C1", clock.now - 3)], title: "C"))
        let listA = store.timeline(.feed(a.id))
        let first = try XCTUnwrap(store.nextUnread(after: nil, in: listA, scope: .feed(a.id)))
        XCTAssertEqual(first.article.guid, "a1")
        store.setRead([first.article.id], true)
        let second = try XCTUnwrap(store.nextUnread(after: first.article.id, in: listA, scope: .feed(a.id)))
        XCTAssertEqual(second.article.guid, "a2")
        store.setRead([second.article.id], true)
        let jump = try XCTUnwrap(store.nextUnread(after: second.article.id, in: listA, scope: .feed(a.id)))
        XCTAssertEqual(jump.scope, .feed(c.id))
        XCTAssertEqual(jump.article.guid, "c1")
        store.setRead([jump.article.id], true)
        XCTAssertNil(store.nextUnread(after: jump.article.id, in: store.timeline(.feed(c.id)), scope: .feed(c.id)))
        XCTAssertEqual(store.feedOrder(), [a.id, b.id, c.id])
    }

    func testPersistsAndReloads() throws {
        var store = makeStore()
        let folder = try store.addFolder("Tech")
        let feed = try store.addFeed(url: "https://eng.example.org/feed.xml", folderId: folder.id)
        _ = fetched(store, feed, parsed([("a", "Alpha", clock.now)]))
        let article = try XCTUnwrap(store.articles[feed.id]?.first)
        store.setStarred([article.id], true)
        store.setReader("<p>Reader</p>", for: article)
        store.updateSettings { $0.timelineLines = 9 }
        store.log("test", "Hello")
        XCTAssertTrue(store.isDirty)
        store.flush()
        XCTAssertFalse(store.isDirty)
        XCTAssertNotNil(disk.files[RSSReaderStore.headsFile(feed.id)])
        XCTAssertNotNil(disk.files[RSSReaderStore.bodiesFile(feed.id)])
        store = makeStore()
        XCTAssertEqual(store.folders.map(\.name), ["Tech"])
        XCTAssertEqual(store.feed(feed.id)?.folderId, folder.id)
        XCTAssertEqual(store.settings.timelineLines, 4)
        XCTAssertEqual(store.activity.first?.text, "Hello")
        let reloaded = try XCTUnwrap(store.article(article.id))
        XCTAssertTrue(reloaded.starred)
        XCTAssertEqual(store.body(for: reloaded).readerHtml, "<p>Reader</p>")
        XCTAssertEqual(store.body(for: reloaded).contentHtml, "<p>Alpha</p>")
        store.unsubscribe(feed.id)
        store.flush()
        XCTAssertNil(disk.files[RSSReaderStore.headsFile(feed.id)])
        XCTAssertNil(disk.files[RSSReaderStore.bodiesFile(feed.id)])
        XCTAssertEqual(makeStore().feeds.count, 0)
    }

    func testFileDiskRoundTrip() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("rss-reader-test-\(UUID().uuidString)")
        let disk = RSSReaderFileDisk(directory: directory)
        disk.write("library.json", Data("{}".utf8))
        XCTAssertEqual(disk.read("library.json"), Data("{}".utf8))
        disk.remove("library.json")
        disk.sync()
        XCTAssertNil(disk.read("library.json"))
        _ = rmdir(directory.path)
    }

    func testImportsOPMLWithFoldersAndSkipsDuplicates() throws {
        let store = makeStore()
        _ = try store.addFeed(url: "https://eng.example.org/feed.xml")
        let entries = [
            RSSOPMLEntry(url: "https://eng.example.org/feed.xml", title: "Dup", folder: "", siteUrl: ""),
            RSSOPMLEntry(url: "https://weekly.example.net/rss", title: "Open Source Weekly", folder: "Tech", siteUrl: "https://weekly.example.net/"),
            RSSOPMLEntry(url: "https://deep.example.com/atom", title: "", folder: "tech", siteUrl: ""),
        ]
        let result = store.importOPML(entries)
        XCTAssertEqual(result.imported, 2)
        XCTAssertEqual(result.skipped, 1)
        XCTAssertEqual(store.folders.count, 1)
        XCTAssertEqual(store.feeds.filter { !$0.folderId.isEmpty }.count, 2)
        XCTAssertEqual(store.dueFeeds().count, 3)
        XCTAssertTrue(store.activity.first?.text.contains("Imported 2 subscriptions") ?? false)
        XCTAssertTrue(store.exportOPML().contains("xmlUrl=\"https://weekly.example.net/rss\""))
    }

    func testSettingsDecodeLeniently() throws {
        let data = Data(#"{"timelineLines": 7, "articleTheme": "unknown", "articleSize": 3, "confirmMarkAllRead": false}"#.utf8)
        let settings = try JSONDecoder().decode(RSSReaderSettings.self, from: data)
        XCTAssertEqual(settings.timelineLines, 4)
        XCTAssertEqual(settings.articleTheme, .standard)
        XCTAssertEqual(settings.articleSize, 12)
        XCTAssertFalse(settings.confirmMarkAllRead)
        XCTAssertTrue(settings.loadRemoteImages)
        XCTAssertTrue(settings.openLinksInApp)
    }
}
