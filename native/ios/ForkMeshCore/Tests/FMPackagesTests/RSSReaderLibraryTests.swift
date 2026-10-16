import FMBase
import FMNet
import Foundation
import XCTest

@testable import FMPackages

final class RSSReaderLibraryTests: XCTestCase {
    var clock = RSSReaderStoreTests.Clock()
    var disk = RSSReaderMemoryDisk()
    var counter = 0
    let hour: Int64 = 60 * 60 * 1000

    func makeStore() -> RSSReaderStore {
        let clock = self.clock
        return RSSReaderStore(disk: disk, now: { clock.now }, newId: { [unowned self] in
            self.counter += 1
            return "id\(self.counter)"
        })
    }

    struct Entry {
        var guid: String
        var title: String
        var age: Int64 = 0
        var author = ""
        var html = ""
        var link = ""
    }

    @discardableResult
    func fetch(_ store: RSSReaderStore, _ feed: RSSFeed, _ entries: [Entry]) -> RSSApplied {
        var parsed = RSSParsedFeed()
        parsed.title = feed.title.isEmpty ? "Feed" : feed.title
        parsed.items = entries.map { entry in
            var item = RSSParsedItem()
            item.guid = entry.guid
            item.title = entry.title
            item.url = entry.link.isEmpty ? "https://\(RSSURL.host(feed.url))/\(entry.guid)" : entry.link
            item.published = clock.now - entry.age
            item.author = entry.author
            item.contentHtml = entry.html.isEmpty ? "<p>\(entry.title)</p>" : entry.html
            return item
        }
        let items = RSSReaderStore.prepare(parsed, feedUrl: feed.url, now: clock.now)
        return store.apply(RSSFetchResult(
            feedId: feed.id, requestedUrl: feed.url,
            outcome: .updated(feed: parsed, items: items, finalUrl: feed.url, permanent: false, etag: "", lastModified: "")))
    }

    func ids(_ store: RSSReaderStore, _ scope: RSSScope, unreadOnly: Bool = false) -> [String] {
        store.timeline(scope, unreadOnly: unreadOnly).map(\.guid).sorted()
    }

    func article(_ store: RSSReaderStore, _ guid: String) -> RSSArticle? {
        store.articles.values.flatMap { $0 }.first { $0.guid == guid }
    }

    func testMatcherPlainRegexAndInvalidRegex() {
        XCTAssertTrue(RSSMatcher.matches("Apple Event Recap", pattern: "event", regex: false))
        XCTAssertFalse(RSSMatcher.matches("Apple", pattern: "", regex: false))
        XCTAssertTrue(RSSMatcher.matches("Sponsored: thing", pattern: "^SPONSOR(ED)?:", regex: true))
        XCTAssertFalse(RSSMatcher.matches("Not sponsored: thing", pattern: "^sponsor(ed)?:", regex: true))
        XCTAssertFalse(RSSMatcher.matches("anything [", pattern: "[", regex: true))
        XCTAssertFalse(RSSMatcher.isValidRegex("(unclosed"))
        XCTAssertTrue(RSSMatcher.isValidRegex("a|b"))
        XCTAssertTrue(RSSMatcher.matches("x [ y", pattern: "[", regex: false))
        let filter = RSSFilter(id: "f", field: .author, pattern: "bot", regex: false, action: .hide)
        XCTAssertTrue(RSSMatcher.matches(filter, title: "Hello", author: "News Bot", text: { "" }))
        XCTAssertFalse(RSSMatcher.matches(RSSFilter(id: "t", field: .title, pattern: "body"), title: "Hello", author: "", text: { "body" }))
        XCTAssertTrue(RSSMatcher.matches(RSSFilter(id: "t", field: .titleOrText, pattern: "body"), title: "Hello", author: "", text: { "some body" }))
    }

    func testHideFiltersHideFromTimelinesAndCountsButNotStarred() throws {
        let store = makeStore()
        let a = try store.addFeed(url: "https://a.example.org/rss")
        let b = try store.addFeed(url: "https://b.example.org/rss")
        fetch(store, a, [
            Entry(guid: "a1", title: "Weekly sponsor message"), Entry(guid: "a2", title: "Real news"),
            Entry(guid: "a3", title: "Quiet", html: "<p>" + String(repeating: "filler ", count: 100) + "crypto giveaway</p>"),
        ])
        fetch(store, b, [Entry(guid: "b1", title: "Sponsor of the week")])
        XCTAssertEqual(store.counts().unread, 4)
        try store.saveFilter(RSSFilter(id: "", feedId: a.id, field: .title, pattern: "SPONSOR", action: .hide))
        XCTAssertEqual(ids(store, .unread), ["a2", "a3", "b1"])
        XCTAssertEqual(store.counts().unread, 3)
        XCTAssertEqual(store.counts().byFeed[a.id], 2)
        let text = try store.saveFilter(RSSFilter(id: "", field: .titleOrText, pattern: "crypto\\s+give", regex: true, action: .hide))
        XCTAssertEqual(ids(store, .feed(a.id)), ["a2"])
        XCTAssertEqual(store.articles[a.id]?.count, 3, "hidden articles stay stored")
        let a1 = try XCTUnwrap(article(store, "a1"))
        store.setStarred([a1.id], true)
        XCTAssertEqual(ids(store, .feed(a.id)), ["a1", "a2"])
        XCTAssertEqual(store.markAllRead(.feed(a.id)), 2)
        XCTAssertFalse(article(store, "a3")?.read ?? true, "hidden articles are not marked by Mark All as Read")
        store.deleteFilter(text.id)
        XCTAssertEqual(ids(store, .unread), ["a3", "b1"])
        try store.saveFilter(RSSFilter(id: "", field: .title, pattern: "[", regex: true, action: .hide))
        XCTAssertEqual(ids(store, .unread), ["a3", "b1"], "an invalid regular expression never matches")
        XCTAssertThrowsError(try store.saveFilter(RSSFilter(id: "", pattern: "  "))) {
            XCTAssertEqual($0 as? RSSReaderError, .emptyPattern)
        }
        XCTAssertEqual(RSSReaderError.invalidRegex.message, "Invalid regular expression")
    }

    func testMarkReadFiltersApplyOnlyOnArrivalAndFoldDuplicates() throws {
        let store = makeStore()
        let a = try store.addFeed(url: "https://a.example.org/rss")
        let b = try store.addFeed(url: "https://b.example.org/rss")
        fetch(store, a, [Entry(guid: "old", title: "Release notes")])
        try store.saveFilter(RSSFilter(id: "", field: .author, pattern: "dependabot", action: .markRead))
        try store.saveFilter(RSSFilter(id: "", field: .title, pattern: "release", action: .markRead))
        XCTAssertFalse(article(store, "old")?.read ?? true, "markRead filters do not touch stored articles")
        fetch(store, a, [Entry(guid: "new", title: "Release 2.0"), Entry(guid: "bot", title: "Bump x", author: "Dependabot"), Entry(guid: "keep", title: "Other")])
        XCTAssertTrue(article(store, "new")?.read ?? false)
        XCTAssertTrue(article(store, "bot")?.read ?? false)
        XCTAssertFalse(article(store, "keep")?.read ?? true)
        fetch(store, b, [Entry(guid: "dup", title: "Shared", link: "http://A.example.org/keep#frag")])
        XCTAssertFalse(article(store, "dup")?.read ?? true, "folding is off by default")
        store.updateSettings { $0.foldDuplicates = true }
        fetch(store, b, [
            Entry(guid: "dup2", title: "Shared again", link: "https://a.example.org/keep"),
            Entry(guid: "fresh", title: "Fresh", link: "https://b.example.org/fresh"),
        ])
        XCTAssertTrue(article(store, "dup2")?.read ?? false)
        XCTAssertFalse(article(store, "fresh")?.read ?? true)
        fetch(store, b, [Entry(guid: "same-feed", title: "Same feed twice", link: "https://b.example.org/fresh")])
        XCTAssertFalse(article(store, "same-feed")?.read ?? true, "a duplicate inside the same feed is not folded")
    }

    func testSmartFeedsMatchAndCount() throws {
        let store = makeStore()
        let folder = try store.addFolder("Tech")
        let a = try store.addFeed(url: "https://a.example.org/rss", folderId: folder.id)
        let b = try store.addFeed(url: "https://b.example.org/rss")
        fetch(store, a, [
            Entry(guid: "swift", title: "Swift 7 released", age: hour), Entry(guid: "old", title: "Swift history", age: 10 * 24 * hour),
            Entry(guid: "rust", title: "Rust news", html: "<p>Compared with Swift</p>"),
        ])
        fetch(store, b, [Entry(guid: "b-swift", title: "Swift birds")])
        let smart = try store.saveSmartFeed(RSSSmartFeed(id: "", name: "Swift", words: "swift"))
        XCTAssertEqual(ids(store, .smart(smart.id)), ["b-swift", "old", "rust", "swift"])
        XCTAssertEqual(store.counts().bySmartFeed[smart.id], 4)
        var edited = smart
        edited.scope = "folder:\(folder.id)"
        edited.maxAgeDays = 7
        try store.saveSmartFeed(edited)
        XCTAssertEqual(ids(store, .smart(smart.id)), ["rust", "swift"])
        edited.scope = "feed:\(b.id)"
        edited.maxAgeDays = 0
        try store.saveSmartFeed(edited)
        XCTAssertEqual(ids(store, .smart(smart.id)), ["b-swift"])
        let starred = try store.saveSmartFeed(RSSSmartFeed(id: "", name: "Starred Swift", words: "^swift", regex: true, starredOnly: true))
        XCTAssertEqual(ids(store, .smart(starred.id)), [])
        let swiftArticle = try XCTUnwrap(article(store, "swift"))
        store.setStarred([swiftArticle.id], true)
        XCTAssertEqual(ids(store, .smart(starred.id)), ["swift"])
        let unread = try store.saveSmartFeed(RSSSmartFeed(id: "", name: "Any Unread", unreadOnly: true))
        store.setRead([swiftArticle.id], true)
        XCTAssertFalse(ids(store, .smart(unread.id)).contains("swift"))
        XCTAssertTrue(store.timeline(.smart(unread.id), keep: [swiftArticle.id]).contains { $0.guid == "swift" })
        XCTAssertEqual(store.smartFeeds.map(\.name), ["Any Unread", "Starred Swift", "Swift"])
        XCTAssertEqual(store.title(.smart(smart.id)), "Swift")
        XCTAssertThrowsError(try store.saveSmartFeed(RSSSmartFeed(id: "", name: "Bad", words: "(", regex: true))) {
            XCTAssertEqual($0 as? RSSReaderError, .invalidRegex)
        }
        XCTAssertThrowsError(try store.saveSmartFeed(RSSSmartFeed(id: "", name: " "))) { XCTAssertEqual($0 as? RSSReaderError, .emptyName) }
        store.deleteSmartFeed(unread.id)
        XCTAssertNil(store.smartFeed(unread.id))
        XCTAssertFalse(store.exists(.smart(unread.id)))
        for index in store.smartFeeds.count..<RSSReaderLimits.maxSmartFeeds { try store.saveSmartFeed(RSSSmartFeed(id: "", name: "S\(index)")) }
        XCTAssertThrowsError(try store.saveSmartFeed(RSSSmartFeed(id: "", name: "One more"))) {
            XCTAssertEqual($0 as? RSSReaderError, .tooManySmartFeeds)
        }
    }

    func testReadLaterIsKeptByPruning() throws {
        let store = makeStore()
        let feed = try store.addFeed(url: "https://a.example.org/rss")
        fetch(store, feed, [Entry(guid: "later", title: "Long read", age: 1000 * hour), Entry(guid: "now", title: "Now", age: hour)])
        let later = try XCTUnwrap(article(store, "later"))
        XCTAssertEqual(store.counts().readLaterTotal, 0)
        XCTAssertEqual(store.setReadLater([later.id], true), 1)
        store.setRead([later.id], true)
        XCTAssertTrue(article(store, "later")?.readLater ?? false, "reading does not remove it")
        XCTAssertEqual(ids(store, .readLater), ["later"])
        XCTAssertEqual(store.counts().readLater, 0)
        XCTAssertEqual(store.counts().readLaterTotal, 1)
        store.setRead([later.id], false)
        XCTAssertEqual(store.unread(.readLater, counts: store.counts()), 1)
        XCTAssertEqual(store.title(.readLater), "Read Later")
        let half = RSSReaderLimits.maxArticlesPerFeed / 2
        fetch(store, feed, (0..<half).map { Entry(guid: "n\($0)", title: "N\($0)") })
        fetch(store, feed, (0..<half).map { Entry(guid: "m\($0)", title: "M\($0)") })
        XCTAssertNotNil(article(store, "later"))
        XCTAssertNil(article(store, "now"))
        XCTAssertEqual(store.articles[feed.id]?.count, RSSReaderLimits.maxArticlesPerFeed + 1)
    }

    func testFeedSwitchesAffectRefreshTodayAndCounts() throws {
        let store = makeStore()
        let folder = try store.addFolder("News")
        let a = try store.addFeed(url: "https://a.example.org/rss", folderId: folder.id)
        let b = try store.addFeed(url: "https://b.example.org/rss", folderId: folder.id)
        fetch(store, a, [Entry(guid: "a1", title: "A1"), Entry(guid: "a2", title: "A2")])
        fetch(store, b, [Entry(guid: "b1", title: "B1")])
        clock.now += RSSReaderLimits.fetchIntervalMs
        store.setFeedFlag(a.id, \.paused, true)
        XCTAssertEqual(store.dueFeeds().map(\.id), [b.id])
        clock.now += RSSReaderLimits.minForceGapMs
        XCTAssertEqual(store.dueFeeds(force: true).map(\.id), [b.id])
        XCTAssertEqual(store.articles[a.id]?.count, 2)
        store.setFeedFlag(b.id, \.excludeFromToday, true)
        var counts = store.counts()
        XCTAssertEqual(counts.today, 2)
        XCTAssertEqual(counts.unread, 3)
        XCTAssertEqual(ids(store, .today), ["a1", "a2"])
        store.setFeedFlag(a.id, \.excludeFromUnread, true)
        counts = store.counts()
        XCTAssertEqual(counts.today, 0)
        XCTAssertEqual(counts.unread, 1)
        XCTAssertEqual(counts.byFeed[a.id], 2)
        XCTAssertEqual(store.unread(.folder(folder.id), counts: counts), 1)
        XCTAssertEqual(ids(store, .unread), ["b1"])
        XCTAssertEqual(ids(store, .folder(folder.id)), ["a1", "a2", "b1"])
        XCTAssertEqual(store.sidebar(unreadOnly: false, counts: counts).first, .folder(try XCTUnwrap(store.folder(folder.id)), unread: 1, collapsed: false))
    }

    func testChangeAddressValidatesAndResetsState() throws {
        let store = makeStore()
        let a = try store.addFeed(url: "https://a.example.org/rss")
        let b = try store.addFeed(url: "https://b.example.org/rss")
        store.apply(RSSFetchResult(feedId: a.id, requestedUrl: a.url, outcome: .failed("http_500")))
        XCTAssertThrowsError(try store.changeAddress(a.id, to: "not a url at all")) { XCTAssertEqual($0 as? RSSReaderError, .invalidAddress) }
        XCTAssertThrowsError(try store.changeAddress(a.id, to: "file:///etc/passwd")) { XCTAssertEqual($0 as? RSSReaderError, .invalidAddress) }
        XCTAssertThrowsError(try store.changeAddress(a.id, to: "feed://B.example.org/rss")) { XCTAssertEqual($0 as? RSSReaderError, .duplicate(b.id)) }
        XCTAssertFalse(try store.changeAddress(a.id, to: "https://a.example.org/rss"))
        clock.now += 5000
        XCTAssertTrue(try store.changeAddress(a.id, to: "a.example.org/feed.xml"))
        let changed = try XCTUnwrap(store.feed(a.id))
        XCTAssertEqual(changed.url, "https://a.example.org/feed.xml")
        XCTAssertEqual(changed.previousUrls, ["https://a.example.org/rss"])
        XCTAssertEqual(changed.errorCount, 0)
        XCTAssertEqual(changed.error, "")
        XCTAssertEqual(changed.etag, "")
        XCTAssertEqual(changed.nextFetchAt, clock.now)
        XCTAssertTrue(store.dueFeeds().contains { $0.id == a.id })
        XCTAssertThrowsError(try store.changeAddress(b.id, to: "https://a.example.org/rss")) { XCTAssertEqual($0 as? RSSReaderError, .duplicate(a.id)) }
    }

    func testCatchUpAndMarkAllKeepsStarred() throws {
        let store = makeStore()
        let a = try store.addFeed(url: "https://a.example.org/rss")
        let b = try store.addFeed(url: "https://b.example.org/rss")
        fetch(store, a, [
            Entry(guid: "fresh", title: "Fresh", age: hour), Entry(guid: "two-days", title: "Two", age: 49 * hour),
            Entry(guid: "ten-days", title: "Ten", age: 240 * hour), Entry(guid: "starred-old", title: "Old star", age: 240 * hour),
        ])
        fetch(store, b, [Entry(guid: "b-old", title: "B old", age: 100 * hour)])
        store.setStarred([try XCTUnwrap(article(store, "starred-old")).id], true)
        store.updateSettings { $0.markAllKeepsStarred = true }
        XCTAssertEqual(store.olderUnread(.feed(a.id), .week).count, 1)
        XCTAssertEqual(store.olderUnread(nil, .day).count, 3)
        XCTAssertEqual(store.markOlderRead(.feed(a.id), .day), 2)
        XCTAssertFalse(article(store, "b-old")?.read ?? true)
        XCTAssertFalse(article(store, "starred-old")?.read ?? true)
        XCTAssertEqual(store.markOlderRead(nil, .threeDays), 1)
        XCTAssertTrue(article(store, "b-old")?.read ?? false)
        XCTAssertEqual(store.markAllRead(.unread), 1)
        XCTAssertFalse(article(store, "starred-old")?.read ?? true)
        store.updateSettings { $0.markAllKeepsStarred = false }
        XCTAssertEqual(store.markOlderRead(nil, .month), 0)
        XCTAssertEqual(store.markAllRead(.starred), 1)
        XCTAssertEqual(RSSCatchUp.allCases.map(\.days), [1, 3, 7, 14, 30])
        XCTAssertEqual(RSSCatchUp.twoWeeks.title, "Older Than 2 Weeks")
        XCTAssertEqual(RSSReaderText.markOlderPrompt(3, .week, scope: nil), "Mark 3 articles older than 1 week as read?")
    }

    func testHideReadArticlesSetting() {
        var settings = RSSReaderSettings()
        XCTAssertTrue(settings.hidesRead(.today))
        XCTAssertFalse(settings.hidesRead(.feed("x")))
        settings.hideReadArticles = true
        XCTAssertTrue(settings.hidesRead(.feed("x")))
        XCTAssertTrue(settings.hidesRead(.readLater))
        XCTAssertTrue(settings.hidesRead(.smart("s")))
        XCTAssertFalse(settings.hidesRead(.starred))
    }

    func testSidebarSortsByUnreadAndFindsFeeds() throws {
        let store = makeStore()
        let folder = try store.addFolder("Alpha Folder")
        let quiet = try store.addFeed(url: "https://quiet.example.org/rss", title: "Aardvark")
        let busy = try store.addFeed(url: "https://busy.example.org/rss", title: "Zebra")
        let inside = try store.addFeed(url: "https://inside.example.net/atom", title: "Middle", folderId: folder.id)
        let other = try store.addFeed(url: "https://other.example.net/feed", title: "Bee", folderId: folder.id)
        fetch(store, busy, [Entry(guid: "z1", title: "1"), Entry(guid: "z2", title: "2"), Entry(guid: "z3", title: "3")])
        fetch(store, other, [Entry(guid: "o1", title: "1")])
        fetch(store, inside, [Entry(guid: "i1", title: "1"), Entry(guid: "i2", title: "2")])
        var counts = store.counts()
        XCTAssertEqual(store.sidebar(unreadOnly: false, counts: counts).map(\.id), [
            "feed:\(quiet.id)", "folder:\(folder.id)", "feed:\(other.id)", "feed:\(inside.id)", "feed:\(busy.id)",
        ])
        store.updateSettings { $0.sortFeedsByUnread = true }
        counts = store.counts()
        XCTAssertEqual(store.sidebar(unreadOnly: false, counts: counts).map(\.id), [
            "folder:\(folder.id)", "feed:\(inside.id)", "feed:\(other.id)", "feed:\(busy.id)", "feed:\(quiet.id)",
        ])
        store.setCollapsed("folder:\(folder.id)", true)
        XCTAssertEqual(store.sidebar(unreadOnly: false, counts: counts, search: "EXAMPLE.NET").map(\.id), [
            "folder:\(folder.id)", "feed:\(inside.id)", "feed:\(other.id)",
        ])
        XCTAssertEqual(store.sidebar(unreadOnly: true, counts: counts, search: "aard").map(\.id), ["feed:\(quiet.id)"])
        XCTAssertEqual(store.sidebar(unreadOnly: false, counts: counts, search: "/atom").map(\.id), ["folder:\(folder.id)", "feed:\(inside.id)"])
        XCTAssertEqual(store.sidebar(unreadOnly: false, counts: counts, search: "nothing").count, 0)
        XCTAssertEqual(store.sidebar(unreadOnly: false, counts: counts).count, 3)
    }

    func testNewFieldsPersistAndOldJSONStillLoads() throws {
        var store = makeStore()
        let feed = try store.addFeed(url: "https://a.example.org/rss")
        fetch(store, feed, [Entry(guid: "a", title: "A")])
        let stored = try XCTUnwrap(article(store, "a"))
        store.setReadLater([stored.id], true)
        store.setFeedFlag(feed.id, \.paused, true)
        store.setFeedFlag(feed.id, \.excludeFromToday, true)
        store.setFeedFlag(feed.id, \.excludeFromUnread, true)
        store.updateSettings {
            $0.showThumbnails = true
            $0.hideReadArticles = true
            $0.foldDuplicates = true
            $0.markAllKeepsStarred = true
            $0.sortFeedsByUnread = true
        }
        try store.saveFilter(RSSFilter(id: "", feedId: feed.id, field: .titleOrText, pattern: "x", regex: true, action: .markRead))
        try store.saveSmartFeed(RSSSmartFeed(id: "", name: "S", words: "w", scope: "feed:\(feed.id)", unreadOnly: true, maxAgeDays: 3))
        store.flush()
        let library = try XCTUnwrap(disk.files[RSSReaderStore.libraryFile])
        let json = try XCTUnwrap(JSONSerialization.jsonObject(with: library) as? [String: Any])
        let filter = try XCTUnwrap((json["filters"] as? [[String: Any]])?.first)
        XCTAssertEqual(filter["field"] as? String, "titleOrText")
        XCTAssertEqual(filter["action"] as? String, "markRead")
        XCTAssertEqual(filter["feedId"] as? String, feed.id)
        let smart = try XCTUnwrap((json["smartFeeds"] as? [[String: Any]])?.first)
        XCTAssertEqual(smart["scope"] as? String, "feed:\(feed.id)")
        XCTAssertEqual(smart["maxAgeDays"] as? Int, 3)
        store = makeStore()
        XCTAssertTrue(store.settings.showThumbnails && store.settings.hideReadArticles && store.settings.foldDuplicates)
        XCTAssertTrue(store.settings.markAllKeepsStarred && store.settings.sortFeedsByUnread)
        let reloaded = try XCTUnwrap(store.feed(feed.id))
        XCTAssertTrue(reloaded.paused && reloaded.excludeFromToday && reloaded.excludeFromUnread)
        XCTAssertTrue(article(store, "a")?.readLater ?? false)
        XCTAssertEqual(store.filters.first?.pattern, "x")
        XCTAssertEqual(store.smartFeeds.first?.maxAgeDays, 3)
        let old = #"{"version":1,"folders":[],"feeds":[{"id":"f","url":"https://a.example.org/rss","title":"Old"}],"settings":{"sortOldestFirst":true}}"#
        let decoded = try JSONDecoder().decode(RSSReaderLibrary.self, from: Data(old.utf8))
        XCTAssertEqual(decoded.filters, [])
        XCTAssertEqual(decoded.smartFeeds, [])
        XCTAssertFalse(decoded.feeds[0].paused || decoded.feeds[0].excludeFromToday || decoded.feeds[0].excludeFromUnread)
        XCTAssertEqual(decoded.feeds[0].language, "")
        XCTAssertFalse(decoded.settings.showThumbnails || decoded.settings.hideReadArticles || decoded.settings.foldDuplicates)
        XCTAssertFalse(decoded.settings.markAllKeepsStarred || decoded.settings.sortFeedsByUnread)
        let oldArticle = #"{"feedId":"f","guid":"g","read":true}"#
        let decodedArticle = try JSONDecoder().decode(RSSArticle.self, from: Data(oldArticle.utf8))
        XCTAssertFalse(decodedArticle.readLater)
        XCTAssertEqual(decodedArticle.language, "")
        let oldBody = try JSONDecoder().decode(RSSArticleBody.self, from: Data(#"{"contentHtml":"<p>x</p>"}"#.utf8))
        XCTAssertEqual(oldBody.images, [])
        let partial = try JSONDecoder().decode(RSSFilter.self, from: Data(#"{"id":"q","field":"bogus","action":"nope"}"#.utf8))
        XCTAssertEqual(partial, RSSFilter(id: "q"))
    }

    func testOPMLExportWritesHomePageOnlyWhenKnown() throws {
        let store = makeStore()
        _ = try store.addFeed(url: "https://a.example.org/rss", siteUrl: "https://a.example.org/")
        _ = try store.addFeed(url: "https://b.example.org/rss")
        let opml = store.exportOPML()
        XCTAssertTrue(opml.contains("xmlUrl=\"https://a.example.org/rss\" htmlUrl=\"https://a.example.org/\"/>"), opml)
        XCTAssertTrue(opml.contains("xmlUrl=\"https://b.example.org/rss\"/>"), opml)
        let entries = try RSSOPML.parse(opml)
        XCTAssertEqual(entries.first { $0.url.contains("a.example") }?.siteUrl, "https://a.example.org/")
    }
}

final class RSSReaderDiscoveryTests: XCTestCase {
    func testYouTubeRewrites() {
        let feeds = RSSDiscoveryRules.youTubeFeeds
        XCTAssertEqual(RSSDiscoveryRules.youTube("https://www.youtube.com/channel/UCXuqSBlHAE6Xw-yeJA0Tunw"), .feed("\(feeds)?channel_id=UCXuqSBlHAE6Xw-yeJA0Tunw"))
        XCTAssertEqual(RSSDiscoveryRules.youTube("https://m.youtube.com/channel/UCXuqSBlHAE6Xw-yeJA0Tunw/videos"), .feed("\(feeds)?channel_id=UCXuqSBlHAE6Xw-yeJA0Tunw"))
        XCTAssertEqual(RSSDiscoveryRules.youTube("https://youtube.com/playlist?list=PL590L5WQmH8fJ54F369BLDSqIwcs-TCfs"), .feed("\(feeds)?playlist_id=PL590L5WQmH8fJ54F369BLDSqIwcs-TCfs"))
        XCTAssertEqual(RSSDiscoveryRules.youTube("https://www.youtube.com/user/LinusTechTips"), .feed("\(feeds)?user=LinusTechTips"))
        XCTAssertEqual(RSSDiscoveryRules.youTube("https://www.youtube.com/@LinusTechTips"), .page)
        XCTAssertEqual(RSSDiscoveryRules.youTube("https://www.youtube.com/c/LinusTechTips"), .page)
        XCTAssertEqual(RSSDiscoveryRules.youTube("https://www.youtube.com/watch?v=k1Dq7HzHn2Q"), .page)
        XCTAssertEqual(RSSDiscoveryRules.youTube("https://youtu.be/k1Dq7HzHn2Q"), .page)
        XCTAssertEqual(RSSDiscoveryRules.youTube("https://www.youtube.com/shorts/k1Dq7HzHn2Q"), .page)
        XCTAssertEqual(RSSDiscoveryRules.youTube("https://www.youtube.com/"), RSSYouTubeRule.none)
        XCTAssertEqual(RSSDiscoveryRules.youTube("https://www.youtube.com/watch"), RSSYouTubeRule.none)
        XCTAssertEqual(RSSDiscoveryRules.youTube("https://notyoutube.com/@x"), RSSYouTubeRule.none)
        XCTAssertEqual(RSSDiscoveryRules.youTube("https://www.youtube.com/feeds/videos.xml?channel_id=UCx"), RSSYouTubeRule.none)
    }

    func testChannelIdExtraction() {
        let id = "UCXuqSBlHAE6Xw-yeJA0Tunw"
        XCTAssertEqual(RSSDiscoveryRules.youTubeChannelId("var ytInitialData = {\"channelId\":\"\(id)\",\"title\":\"x\"}"), id)
        XCTAssertEqual(RSSDiscoveryRules.youTubeChannelId("{\"channelId\":\"UCshort\"} <meta itemprop=\"channelId\" content=\"\(id)\">"), id)
        XCTAssertEqual(RSSDiscoveryRules.youTubeChannelId("<link rel=\"canonical\" href=\"https://www.youtube.com/channel/\(id)\">"), id)
        XCTAssertNil(RSSDiscoveryRules.youTubeChannelId("<link rel=\"canonical\" href=\"https://www.youtube.com/@x\">"))
        XCTAssertNil(RSSDiscoveryRules.youTubeChannelId("\"channelId\":\"\(id)toolong\""))
        XCTAssertEqual(RSSDiscoveryRules.channelFeed(id), "https://www.youtube.com/feeds/videos.xml?channel_id=\(id)")
    }

    func testMastodonBlueskyAndGhost() {
        XCTAssertEqual(RSSDiscoveryRules.mastodon("https://mastodon.social/@ada"), "https://mastodon.social/@ada.rss")
        XCTAssertEqual(RSSDiscoveryRules.mastodon("https://mastodon.social/@ada/"), "https://mastodon.social/@ada.rss")
        XCTAssertNil(RSSDiscoveryRules.mastodon("https://mastodon.social/@ada/113300000000000001"))
        XCTAssertNil(RSSDiscoveryRules.mastodon("https://mastodon.social/@"))
        XCTAssertNil(RSSDiscoveryRules.mastodon("https://mastodon.social/users/ada"))
        XCTAssertNil(RSSDiscoveryRules.mastodon("https://www.youtube.com/@LinusTechTips"))
        XCTAssertNil(RSSDiscoveryRules.mastodon("https://mastodon.social/@ada.rss"))
        XCTAssertEqual(RSSDiscoveryRules.bluesky("https://bsky.app/profile/ada.bsky.social"), "https://bsky.app/profile/ada.bsky.social/rss")
        XCTAssertEqual(RSSDiscoveryRules.bluesky("https://bsky.app/profile/did:plc:abc/"), "https://bsky.app/profile/did:plc:abc/rss")
        XCTAssertNil(RSSDiscoveryRules.bluesky("https://bsky.app/profile/ada.bsky.social/post/1"))
        XCTAssertNil(RSSDiscoveryRules.bluesky("https://example.org/profile/ada"))
        XCTAssertEqual(RSSDiscoveryRules.rewrites("https://mastodon.social/@ada"), ["https://mastodon.social/@ada.rss"])
        XCTAssertEqual(RSSDiscoveryRules.rewrites("https://example.org/blog"), [])
        XCTAssertEqual(RSSFeedTypes.guessPaths.last, "/rss/")
    }

    final class Site: @unchecked Sendable {
        let lock = NSLock()
        var pages: [String: HTTPResponse] = [:]

        func set(_ url: String, _ text: String, status: Int = 200, headers: HTTPHeaders = [:]) {
            lock.lock()
            pages[url] = HTTPResponse(status: status, headers: headers, text: text)
            lock.unlock()
        }

        func response(_ request: HTTPRequest) -> HTTPResponse {
            lock.lock()
            defer { lock.unlock() }
            return pages[request.url.absoluteString] ?? HTTPResponse(status: 404, text: "missing")
        }
    }

    func testFetcherUsesTheRewritesBeforeDiscovery() async {
        let site = Site()
        let id = "UCXuqSBlHAE6Xw-yeJA0Tunw"
        site.set("https://www.youtube.com/@LinusTechTips", "<html><script>var d={\"channelId\":\"\(id)\"}</script></html>")
        site.set(RSSDiscoveryRules.channelFeed(id), RSSReaderMediaTests.youTube)
        site.set("https://mastodon.social/@ada.rss", RSSReaderMediaTests.mastodon)
        site.set("https://bsky.app/profile/ada.bsky.social/rss", RSSReaderFetcherTests.rss("Ada on Bluesky", items: ["p1"]))
        site.set("https://ghost.example.org", "<html><head><title>Ghost</title></head></html>")
        site.set("https://ghost.example.org/rss/", RSSReaderFetcherTests.rss("Ghost blog", items: ["g1"]))
        site.set("https://lists.example.org/opml", "<opml><body><outline text=\"Ghost\" xmlUrl=\"https://ghost.example.org/rss/\" htmlUrl=\"https://ghost.example.org/\"/></body></opml>")
        let http = FakeHTTPClient { request in site.response(request) }
        let fetcher = RSSReaderFetcher(http: http)
        let youTube = await fetcher.discover("www.youtube.com/@LinusTechTips")
        XCTAssertEqual(try? youTube.get().url, RSSDiscoveryRules.channelFeed(id))
        XCTAssertEqual(try? youTube.get().feed.title, "Linus Tech Tips")
        let mastodon = await fetcher.discover("https://mastodon.social/@ada")
        XCTAssertEqual(try? mastodon.get().url, "https://mastodon.social/@ada.rss")
        let bluesky = await fetcher.discover("https://bsky.app/profile/ada.bsky.social")
        XCTAssertEqual(try? bluesky.get().url, "https://bsky.app/profile/ada.bsky.social/rss")
        let ghost = await fetcher.discover("ghost.example.org")
        XCTAssertEqual(try? ghost.get().url, "https://ghost.example.org/rss/")
        let opml = await fetcher.opml("lists.example.org/opml")
        XCTAssertNotNil(try? opml.get())
        let missing = await fetcher.opml("lists.example.org/none")
        guard case .failure(.http(404)) = missing else { return XCTFail("expected a 404") }
    }

    @MainActor
    func testModelCheckNowIgnoresPauseAndConditionalHeaders() async throws {
        let site = Site()
        site.set("https://eng.example.org/feed.xml", RSSReaderFetcherTests.rss("Example Engineering", items: ["one", "two"]), headers: ["ETag": "\"1\""])
        site.set("https://lists.example.org/subs.opml", "<opml><body><outline text=\"X\" xmlUrl=\"https://x.example.com/rss\"/></body></opml>")
        let http = FakeHTTPClient { request in site.response(request) }
        let clock = RSSReaderStoreTests.Clock()
        let store = RSSReaderStore(disk: RSSReaderMemoryDisk(), now: { clock.now })
        let model = RSSReaderModel(store: store, fetcher: RSSReaderFetcher(http: http), saveDelayMs: 1)
        let feed = try await model.addFeed("https://eng.example.org/feed.xml")
        model.setFeedFlag(feed.id, \.paused, true)
        store.apply(RSSFetchResult(feedId: feed.id, requestedUrl: feed.url, outcome: .failed("http_503")))
        var stored = try XCTUnwrap(model.feed(feed.id))
        XCTAssertGreaterThan(stored.errorCount, 0)
        XCTAssertEqual(stored.etag, "\"1\"")
        let line = await model.checkNow(feed.id)
        XCTAssertTrue(line.hasPrefix("RSS 2.0 \u{00B7} 2 items \u{00B7} checked "), line)
        XCTAssertNil(http.lastRequest?.headers["If-None-Match"])
        stored = try XCTUnwrap(model.feed(feed.id))
        XCTAssertEqual(stored.errorCount, 0)
        XCTAssertTrue(stored.paused)
        site.set("https://eng.example.org/feed.xml", "gone", status: 410)
        let failed = await model.checkNow(feed.id)
        XCTAssertEqual(failed, "The feed is gone (410).")
        site.set("https://eng.example.org/next.xml", RSSReaderFetcherTests.rss("Moved", items: ["n"]))
        XCTAssertTrue(try model.changeAddress(feed.id, to: "eng.example.org/next.xml"))
        let moved = await model.checkNow(feed.id)
        XCTAssertTrue(moved.hasPrefix("RSS 2.0 \u{00B7} 1 item \u{00B7}"), moved)
        XCTAssertEqual(model.feed(feed.id)?.url, "https://eng.example.org/next.xml")
        let imported = await model.importOPML(fromURL: "https://lists.example.org/subs.opml")
        XCTAssertEqual(imported, "Imported 1 subscription, skipped 0.")
        let list = model.timeline(.feed(feed.id))
        let first = try XCTUnwrap(list.first)
        model.toggleReadLater(first.id)
        XCTAssertTrue(model.article(first.id)?.readLater ?? false)
        XCTAssertEqual(model.olderUnreadCount(nil, .day), 0)
    }
}
