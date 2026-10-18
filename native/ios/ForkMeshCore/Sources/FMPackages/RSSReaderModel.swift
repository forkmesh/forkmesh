import FMBase
import FMNet
import Foundation
import Observation

@MainActor
@Observable
public final class RSSReaderModel {
    @ObservationIgnored public let store: RSSReaderStore
    @ObservationIgnored public let fetcher: RSSReaderFetcher
    @ObservationIgnored private var saveTask: Task<Void, Never>?
    @ObservationIgnored private let saveDelay: UInt64

    public private(set) var revision = 0
    public private(set) var refreshing = false
    public private(set) var refreshDone = 0
    public private(set) var refreshTotal = 0

    public init(store: RSSReaderStore, fetcher: RSSReaderFetcher, saveDelayMs: UInt64 = 800) {
        self.store = store
        self.fetcher = fetcher
        self.saveDelay = saveDelayMs * 1_000_000
    }

    public static func defaultDirectory(base: URL?) -> URL? {
        base?.appendingPathComponent("rss-reader", isDirectory: true)
    }

    private func changed(save: Bool = true) {
        revision &+= 1
        if save { scheduleSave() }
    }

    public func scheduleSave() {
        saveTask?.cancel()
        let delay = saveDelay
        saveTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: delay)
            guard !Task.isCancelled else { return }
            self?.store.flush()
        }
    }

    public func saveNow() {
        saveTask?.cancel()
        saveTask = nil
        store.flush()
    }

    public var settings: RSSReaderSettings {
        _ = revision
        return store.settings
    }

    public func updateSettings(_ change: (inout RSSReaderSettings) -> Void) {
        store.updateSettings(change)
        changed()
    }

    public var feeds: [RSSFeed] {
        _ = revision
        return store.feeds
    }

    public var folders: [RSSFolder] {
        _ = revision
        return store.folders.sorted { RSSReaderStore.byName($0.name, $1.name) }
    }

    public var counts: RSSCounts {
        _ = revision
        return store.counts()
    }

    public var activity: [RSSActivity] {
        _ = revision
        return store.activity
    }

    public var failingFeeds: [RSSFeed] {
        _ = revision
        return store.failingFeeds
    }

    public var stats: RSSStats {
        _ = revision
        return store.stats()
    }

    public var lastRefreshAt: Int64 {
        _ = revision
        return store.library.lastRefreshAt
    }

    public func isCollapsed(_ key: String) -> Bool {
        _ = revision
        return store.isCollapsed(key)
    }

    public func toggleCollapsed(_ key: String) {
        store.setCollapsed(key, !store.isCollapsed(key))
        changed()
    }

    public func sidebar(selected: RSSScope? = nil, search: String = "") -> [RSSSidebarEntry] {
        _ = revision
        return store.sidebar(unreadOnly: store.settings.feedsUnreadOnly, selected: selected, counts: store.counts(), search: search)
    }

    public var filters: [RSSFilter] {
        _ = revision
        return store.filters
    }

    public var smartFeeds: [RSSSmartFeed] {
        _ = revision
        return store.smartFeeds
    }

    public func smartFeed(_ id: String) -> RSSSmartFeed? {
        _ = revision
        return store.smartFeed(id)
    }

    public func saveFilter(_ filter: RSSFilter) throws {
        try store.saveFilter(filter)
        changed()
    }

    public func deleteFilter(_ id: String) {
        store.deleteFilter(id)
        changed()
    }

    @discardableResult
    public func saveSmartFeed(_ smart: RSSSmartFeed) throws -> RSSSmartFeed {
        let saved = try store.saveSmartFeed(smart)
        changed()
        return saved
    }

    public func deleteSmartFeed(_ id: String) {
        store.deleteSmartFeed(id)
        changed()
    }

    public func setFeedFlag(_ id: String, _ path: WritableKeyPath<RSSFeed, Bool>, _ value: Bool) {
        store.setFeedFlag(id, path, value)
        changed()
    }

    @discardableResult
    public func changeAddress(_ id: String, to input: String) throws -> Bool {
        guard try store.changeAddress(id, to: input) else { return false }
        changed()
        return true
    }

    public func checkNow(_ id: String) async -> String {
        guard let feed = store.feed(id) else { return "" }
        let stamp = store.now
        let result = await fetcher.fetch(feed, now: stamp, conditional: false)
        store.apply(result)
        changed()
        switch result.outcome {
        case let .updated(parsed, _, _, _, _, _):
            return RSSReaderText.checkResult(format: parsed.format, items: parsed.items.count, at: stamp)
        case .notModified:
            return RSSReaderText.notModified(at: stamp)
        case .failed(let code):
            return RSSReaderText.feedError(code)
        }
    }

    public func unread(_ scope: RSSScope) -> Int {
        _ = revision
        return store.unread(scope, counts: store.counts())
    }

    public func title(_ scope: RSSScope) -> String {
        _ = revision
        return store.title(scope)
    }

    public func feed(_ id: String) -> RSSFeed? {
        _ = revision
        return store.feed(id)
    }

    public func article(_ id: String) -> RSSArticle? {
        _ = revision
        return store.article(id)
    }

    public func timeline(_ scope: RSSScope, query: String = "", unreadOnly: Bool = false, keep: Set<String> = []) -> [RSSArticle] {
        _ = revision
        return store.timeline(scope, query: query, unreadOnly: unreadOnly, keep: keep)
    }

    public func status(now: Int64? = nil) -> String {
        _ = revision
        if refreshing {
            return refreshTotal > 1 ? "Updating \(min(refreshDone + 1, refreshTotal)) of \(refreshTotal)\u{2026}" : "Updating\u{2026}"
        }
        return RSSReaderText.updated(store.library.lastRefreshAt, now: now ?? store.now)
    }

    public func refresh(force: Bool = false) async {
        guard !refreshing else { return }
        let due = store.dueFeeds(force: force)
        guard !due.isEmpty else {
            if force {
                store.markRefreshed()
                changed()
            }
            return
        }
        refreshing = true
        refreshDone = 0
        refreshTotal = due.count
        changed(save: false)
        tally = (0, 0)
        let stamp = store.now
        await fetcher.refresh(due, now: stamp) { [weak self] result in
            await self?.receive(result)
        }
        store.markRefreshed()
        var text = "Refreshed \(RSSReaderText.plural(due.count, "feed")): \(RSSReaderText.plural(tally.added, "new article"))"
        if tally.failed > 0 { text += ", \(tally.failed) failed" }
        store.log("refresh", text)
        refreshing = false
        iconLookups.removeAll()
        changed()
    }

    @ObservationIgnored private var iconLookups: Set<String> = []

    public func iconData(_ feedId: String) -> Data? { store.iconData(feedId) }

    public func needsIcon(_ feedId: String, hasImage: Bool) -> Bool {
        guard let feed = store.feed(feedId) else { return false }
        if iconLookups.contains(feedId + "|" + RSSFeedIcons.source(feed)) { return false }
        return RSSFeedIcons.needsLookup(feed, now: store.now, hasImage: hasImage)
    }

    @discardableResult
    public func resolveIcon(_ feedId: String, decodes: @escaping @Sendable (Data) -> Bool = { _ in true }) async -> Data? {
        guard let feed = store.feed(feedId) else { return nil }
        let source = RSSFeedIcons.source(feed)
        guard iconLookups.insert(feedId + "|" + source).inserted else { return nil }
        let outcome = await fetcher.icon(for: feed, decodes: decodes)
        guard store.feed(feedId) != nil else { return nil }
        switch outcome {
        case let .found(url, data):
            store.setIcon(feedId, url: url, data: data, source: source, at: store.now)
            changed()
            return data
        case .missing:
            store.setIcon(feedId, url: "", data: nil, source: source, at: store.now)
            changed()
            return nil
        case .unreachable:
            return nil
        }
    }

    @ObservationIgnored private var tally = (added: 0, failed: 0)

    private func receive(_ result: RSSFetchResult) {
        let applied = store.apply(result)
        tally.added += applied.added
        if !applied.ok { tally.failed += 1 }
        refreshDone += 1
        changed(save: false)
    }

    public func addFeed(_ input: String, title: String = "", folderId: String = "") async throws -> RSSFeed {
        let address = RSSURL.normalizeFeedInput(input)
        guard !address.isEmpty else { throw RSSReaderError.invalidAddress }
        if let existing = store.feed(matching: address) { throw RSSReaderError.duplicate(existing.id) }
        guard store.feeds.count < RSSReaderLimits.maxFeeds else { throw RSSReaderError.tooManyFeeds }
        let found: RSSDiscovered
        switch await fetcher.discover(address) {
        case .failure(let error):
            if error == .noFeedFound { throw RSSReaderError.noFeedFound }
            throw RSSAddFeedError(code: error.code)
        case .success(let discovered):
            found = discovered
        }
        if let existing = store.feed(matching: found.url) { throw RSSReaderError.duplicate(existing.id) }
        let feed = try store.addFeed(url: found.url, title: "", folderId: folderId, siteUrl: found.feed.siteUrl)
        if !RSSText.clean(title, 200).isEmpty { store.renameFeed(feed.id, to: title) }
        let stamp = store.now
        let items = RSSReaderStore.prepare(found.feed, feedUrl: found.url, now: stamp)
        store.apply(RSSFetchResult(
            feedId: feed.id, requestedUrl: found.url,
            outcome: .updated(
                feed: found.feed, items: items, finalUrl: found.url, permanent: false, etag: found.answer.etag,
                lastModified: found.answer.lastModified)))
        let added = store.feed(feed.id) ?? feed
        store.log("add", "Subscribed to \(added.displayTitle)")
        changed()
        return added
    }

    public func addFolder(_ name: String) throws -> RSSFolder {
        let folder = try store.addFolder(name)
        changed()
        return folder
    }

    public func renameFolder(_ id: String, to name: String) throws {
        try store.renameFolder(id, to: name)
        changed()
    }

    public func deleteFolder(_ id: String) {
        store.deleteFolder(id)
        changed()
    }

    public func renameFeed(_ id: String, to title: String) {
        store.renameFeed(id, to: title)
        changed()
    }

    public func moveFeed(_ id: String, toFolder folderId: String) {
        store.moveFeed(id, toFolder: folderId)
        changed()
    }

    public func unsubscribe(_ id: String) {
        store.unsubscribe(id)
        changed()
    }

    public func setRead(_ ids: [String], _ read: Bool) {
        if store.setRead(ids, read) > 0 { changed() }
    }

    public func setStarred(_ ids: [String], _ starred: Bool) {
        if store.setStarred(ids, starred) > 0 { changed() }
    }

    public func toggleRead(_ id: String) {
        guard let article = store.article(id) else { return }
        setRead([id], !article.read)
    }

    public func toggleStar(_ id: String) {
        guard let article = store.article(id) else { return }
        setStarred([id], !article.starred)
    }

    public func toggleReadLater(_ id: String) {
        guard let article = store.article(id) else { return }
        if store.setReadLater([id], !article.readLater) > 0 { changed() }
    }

    public func olderUnreadCount(_ scope: RSSScope?, _ choice: RSSCatchUp) -> Int {
        _ = revision
        return store.olderUnread(scope, choice).count
    }

    @discardableResult
    public func markOlderRead(_ scope: RSSScope?, _ choice: RSSCatchUp) -> Int {
        let count = store.markOlderRead(scope, choice)
        if count > 0 { changed() }
        return count
    }

    public func open(_ id: String) {
        setRead([id], true)
    }

    @discardableResult
    public func markAllRead(_ scope: RSSScope) -> Int {
        let count = store.markAllRead(scope)
        if count > 0 { changed() }
        return count
    }

    @discardableResult
    public func markRead(_ articles: [RSSArticle]) -> Int {
        let keepStarred = store.settings.markAllKeepsStarred
        let count = store.setRead(articles.filter { !$0.read && !(keepStarred && $0.starred) }.map(\.id), true)
        if count > 0 { changed() }
        return count
    }

    public func nextUnread(after id: String?, in list: [RSSArticle], scope: RSSScope) -> (scope: RSSScope, article: RSSArticle)? {
        store.nextUnread(after: id, in: list, scope: scope)
    }

    public func body(for article: RSSArticle) -> RSSArticleBody {
        _ = revision
        return store.body(for: article)
    }

    public func page(for article: RSSArticle, reader: Bool) -> String {
        _ = revision
        return RSSArticlePage.html(
            article: article, body: store.body(for: article), feed: store.feed(article.feedId), settings: store.settings,
            reader: reader, icon: store.settings.loadRemoteImages ? store.iconData(article.feedId) : nil)
    }

    public func loadReader(_ id: String) async -> Bool {
        guard let article = store.article(id) else { return false }
        if !store.body(for: article).readerHtml.isEmpty { return true }
        let link = RSSURL.http(article.link)
        guard !link.isEmpty else { return false }
        switch await fetcher.readable(link) {
        case .success(let result):
            guard let current = store.article(id) else { return false }
            store.setReader(result.contentHtml, for: current)
            changed()
            return true
        case .failure:
            return false
        }
    }

    public func importOPML(_ data: Data) -> String {
        let text = RSSXML.decode(data)
        guard let entries = try? RSSOPML.parse(text) else { return RSSReaderError.unreadableOPML.message }
        let result = store.importOPML(entries)
        changed()
        return "Imported \(RSSReaderText.plural(result.imported, "subscription")), skipped \(result.skipped)."
    }

    public func importOPML(fromURL address: String) async -> String {
        switch await fetcher.opml(address) {
        case .failure(let error):
            return error == .invalidAddress ? RSSReaderError.invalidAddress.message : RSSReaderText.feedError(error.code)
        case .success(let data):
            return importOPML(data)
        }
    }

    public func exportOPML() -> String { store.exportOPML() }

    public func addForkMeshBlog() async throws -> RSSFeed {
        try await addFeed(RSSReaderLimits.forkMeshBlogFeed)
    }
}

public struct RSSAddFeedError: Error, Equatable, Sendable {
    public var code: String

    public init(code: String) {
        self.code = code
    }

    public var message: String { RSSReaderText.feedError(code) }
}

public enum RSSReaderErrors {
    public static func message(_ error: Error) -> String {
        if let reader = error as? RSSReaderError { return reader.message }
        if let add = error as? RSSAddFeedError { return add.message }
        return String(describing: error)
    }
}
