import FMBase
import Foundation

public protocol RSSReaderDisk: AnyObject {
    func read(_ name: String) -> Data?
    func write(_ name: String, _ data: Data)
    func remove(_ name: String)
    func sync()
}

public final class RSSReaderMemoryDisk: RSSReaderDisk {
    public private(set) var files: [String: Data] = [:]
    public private(set) var writes = 0

    public init() {}

    public func read(_ name: String) -> Data? { files[name] }

    public func write(_ name: String, _ data: Data) {
        files[name] = data
        writes += 1
    }

    public func remove(_ name: String) { files[name] = nil }

    public func sync() {}
}

public final class RSSReaderFileDisk: RSSReaderDisk, @unchecked Sendable {
    public let directory: URL
    private let queue = DispatchQueue(label: "com.forkmesh.rss-reader.disk", qos: .utility)

    public init(directory: URL) {
        self.directory = directory
    }

    private func url(_ name: String) -> URL { directory.appendingPathComponent(name, isDirectory: false) }

    public func read(_ name: String) -> Data? {
        queue.sync { try? Data(contentsOf: url(name)) }
    }

    public func write(_ name: String, _ data: Data) {
        let target = url(name)
        queue.async {
            try? FileManager.default.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
            #if os(iOS)
            let options: Data.WritingOptions = [.atomic, .completeFileProtectionUntilFirstUserAuthentication]
            #else
            let options: Data.WritingOptions = [.atomic]
            #endif
            do {
                try data.write(to: target, options: options)
            } catch {
                Log.warning("RSSReader", "could not write \(target.lastPathComponent): \(error)")
            }
        }
    }

    public func remove(_ name: String) {
        let target = url(name)
        queue.async { _ = unlink(target.path) }
    }

    public func sync() { queue.sync {} }
}

public struct RSSPreparedArticle: Sendable {
    public var guid: String
    public var title: String
    public var url: String
    public var externalUrl: String
    public var author: String
    public var summary: String
    public var image: String
    public var published: Int64
    public var contentKey: String
    public var body: RSSArticleBody
    public var language: String = ""
}

public enum RSSFetchOutcome: Sendable {
    case notModified
    case updated(feed: RSSParsedFeed, items: [RSSPreparedArticle], finalUrl: String, permanent: Bool, etag: String, lastModified: String)
    case failed(String)
}

public struct RSSFetchResult: Sendable {
    public var feedId: String
    public var requestedUrl: String
    public var outcome: RSSFetchOutcome

    public init(feedId: String, requestedUrl: String, outcome: RSSFetchOutcome) {
        self.feedId = feedId
        self.requestedUrl = requestedUrl
        self.outcome = outcome
    }
}

public struct RSSApplied: Equatable, Sendable {
    public var added = 0
    public var updated = 0
    public var ok = true
}

public final class RSSReaderStore {
    public private(set) var library = RSSReaderLibrary()
    public private(set) var articles: [String: [RSSArticle]] = [:]
    private var bodies: [String: [String: RSSArticleBody]] = [:]
    private var bodyUse: [String] = []
    private var dirtyLibrary = false
    private var dirtyHeads: Set<String> = []
    private var dirtyBodies: Set<String> = []
    private var removed: Set<String> = []
    private var texts: [String: [String: String]] = [:]
    private var matchCache: [String: [String: Bool]] = [:]
    private var linkIndex: [String: Set<String>]?
    private let disk: RSSReaderDisk
    private let clock: () -> Int64
    private let newId: () -> String

    public static let libraryFile = "library.json"
    public static func headsFile(_ feedId: String) -> String { "articles-\(feedId).json" }
    public static func bodiesFile(_ feedId: String) -> String { "bodies-\(feedId).json" }
    public static func iconFile(_ feedId: String) -> String { "icon-\(feedId)" }

    public init(
        disk: RSSReaderDisk = RSSReaderMemoryDisk(), now: @escaping () -> Int64 = { SystemClock().nowMillis },
        newId: @escaping () -> String = { UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased() }
    ) {
        self.disk = disk
        self.clock = now
        self.newId = newId
        load()
    }

    public var now: Int64 { clock() }
    public var folders: [RSSFolder] { library.folders }
    public var feeds: [RSSFeed] { library.feeds }
    public var settings: RSSReaderSettings { library.settings }
    public var activity: [RSSActivity] { library.activity }
    public var isDirty: Bool { dirtyLibrary || !dirtyHeads.isEmpty || !dirtyBodies.isEmpty || !removed.isEmpty }

    private func load() {
        let decoder = JSONDecoder()
        if let data = disk.read(Self.libraryFile), let decoded = try? decoder.decode(RSSReaderLibrary.self, from: data) {
            library = decoded
        }
        for feed in library.feeds {
            if let data = disk.read(Self.headsFile(feed.id)), let list = try? decoder.decode([RSSArticle].self, from: data) {
                articles[feed.id] = list
            } else {
                articles[feed.id] = []
            }
        }
    }

    public func flush() {
        let encoder = JSONEncoder()
        if dirtyLibrary, let data = try? encoder.encode(library) {
            disk.write(Self.libraryFile, data)
        }
        dirtyLibrary = false
        for feedId in removed {
            disk.remove(Self.headsFile(feedId))
            disk.remove(Self.bodiesFile(feedId))
            disk.remove(Self.iconFile(feedId))
        }
        removed.removeAll()
        for feedId in dirtyHeads {
            if let data = try? encoder.encode(articles[feedId] ?? []) { disk.write(Self.headsFile(feedId), data) }
        }
        dirtyHeads.removeAll()
        for feedId in dirtyBodies {
            if let data = try? encoder.encode(bodies[feedId] ?? [:]) { disk.write(Self.bodiesFile(feedId), data) }
        }
        dirtyBodies.removeAll()
        let keep = Set(bodyUse.suffix(8))
        for key in bodies.keys where !keep.contains(key) { bodies[key] = nil }
        bodyUse = bodyUse.filter(keep.contains)
    }

    public func sync() { disk.sync() }

    private func touchLibrary() { dirtyLibrary = true }

    private func storedBodies(_ feedId: String) -> [String: RSSArticleBody] {
        if let cached = bodies[feedId] { return cached }
        if let data = disk.read(Self.bodiesFile(feedId)),
           let decoded = try? JSONDecoder().decode([String: RSSArticleBody].self, from: data) {
            return decoded
        }
        return [:]
    }

    public static func filterText(_ contentHtml: String, summary: String) -> String {
        let text = RSSHTML.plainText(contentHtml, limit: RSSReaderLimits.filterTextChars)
        return text.isEmpty ? summary : text
    }

    public func text(for article: RSSArticle) -> String {
        if let map = texts[article.feedId] { return map[article.id] ?? article.summary }
        let source = storedBodies(article.feedId)
        var map: [String: String] = [:]
        for item in articles[article.feedId] ?? [] {
            map[item.id] = Self.filterText(source[item.id]?.contentHtml ?? "", summary: item.summary)
        }
        texts[article.feedId] = map
        return map[article.id] ?? article.summary
    }

    private func cached(_ key: String, _ article: RSSArticle, _ evaluate: () -> Bool) -> Bool {
        if let hit = matchCache[article.id]?[key] { return hit }
        let value = evaluate()
        matchCache[article.id, default: [:]][key] = value
        return value
    }

    public func matches(_ filter: RSSFilter, _ article: RSSArticle) -> Bool {
        guard filter.applies(to: article.feedId), !filter.pattern.isEmpty else { return false }
        let key = "f\u{1}\(filter.field.rawValue)\u{1}\(filter.regex)\u{1}\(filter.pattern)"
        return cached(key, article) {
            RSSMatcher.matches(filter, title: article.title, author: article.author, text: { self.text(for: article) })
        }
    }

    public func wordsMatch(_ words: String, regex: Bool, _ article: RSSArticle) -> Bool {
        if words.isEmpty { return true }
        let key = "w\u{1}\(regex)\u{1}\(words)"
        return cached(key, article) {
            RSSMatcher.matches(article.title, pattern: words, regex: regex)
                || RSSMatcher.matches(text(for: article), pattern: words, regex: regex)
        }
    }

    private var hideFilters: [RSSFilter] { library.filters.filter { $0.action == .hide && !$0.pattern.isEmpty } }

    public func isHidden(_ article: RSSArticle) -> Bool {
        if article.starred { return false }
        return hideFilters.contains { matches($0, article) }
    }

    private func isHidden(_ article: RSSArticle, filters: [RSSFilter]) -> Bool {
        if article.starred || filters.isEmpty { return false }
        return filters.contains { matches($0, article) }
    }

    private func forget(_ ids: [String], feedId: String) {
        for id in ids {
            matchCache[id] = nil
            texts[feedId]?[id] = nil
        }
    }

    private func loadBodies(_ feedId: String) -> [String: RSSArticleBody] {
        bodyUse.removeAll { $0 == feedId }
        bodyUse.append(feedId)
        if let cached = bodies[feedId] { return cached }
        var loaded: [String: RSSArticleBody] = [:]
        if let data = disk.read(Self.bodiesFile(feedId)),
           let decoded = try? JSONDecoder().decode([String: RSSArticleBody].self, from: data) {
            loaded = decoded
        }
        bodies[feedId] = loaded
        return loaded
    }

    public func log(_ kind: String, _ text: String) {
        library.activity.insert(RSSActivity(at: now, kind: kind, text: text), at: 0)
        if library.activity.count > RSSReaderLimits.maxActivity {
            library.activity.removeLast(library.activity.count - RSSReaderLimits.maxActivity)
        }
        touchLibrary()
    }

    public func updateSettings(_ change: (inout RSSReaderSettings) -> Void) {
        var copy = library.settings
        change(&copy)
        copy.timelineLines = min(4, max(0, copy.timelineLines))
        copy.articleSize = min(RSSReaderSettings.textSizes.upperBound, max(RSSReaderSettings.textSizes.lowerBound, copy.articleSize))
        guard copy != library.settings else { return }
        library.settings = copy
        touchLibrary()
    }

    public var filters: [RSSFilter] { library.filters }

    public var smartFeeds: [RSSSmartFeed] { library.smartFeeds.sorted { Self.byName($0.name, $1.name) } }

    public func smartFeed(_ id: String) -> RSSSmartFeed? { library.smartFeeds.first { $0.id == id } }

    @discardableResult
    public func saveFilter(_ input: RSSFilter) throws -> RSSFilter {
        var filter = input
        filter.pattern = RSSText.strip(RSSText.prefix(filter.pattern, 500))
        guard !filter.pattern.isEmpty else { throw RSSReaderError.emptyPattern }
        if feed(filter.feedId) == nil { filter.feedId = "" }
        if let index = library.filters.firstIndex(where: { $0.id == filter.id }) {
            library.filters[index] = filter
        } else {
            guard library.filters.count < RSSReaderLimits.maxFilters else { throw RSSReaderError.tooManyFilters }
            if filter.id.isEmpty { filter.id = newId() }
            library.filters.append(filter)
        }
        matchCache.removeAll()
        touchLibrary()
        return filter
    }

    public func deleteFilter(_ id: String) {
        library.filters.removeAll { $0.id == id }
        matchCache.removeAll()
        touchLibrary()
    }

    @discardableResult
    public func saveSmartFeed(_ input: RSSSmartFeed) throws -> RSSSmartFeed {
        var smart = input
        smart.name = RSSText.clean(smart.name, 120)
        guard !smart.name.isEmpty else { throw RSSReaderError.emptyName }
        smart.words = RSSText.strip(RSSText.prefix(smart.words, 500))
        if smart.regex && !smart.words.isEmpty && !RSSMatcher.isValidRegex(smart.words) { throw RSSReaderError.invalidRegex }
        smart.maxAgeDays = max(0, min(smart.maxAgeDays, 3650))
        if let index = library.smartFeeds.firstIndex(where: { $0.id == smart.id }) {
            library.smartFeeds[index] = smart
        } else {
            guard library.smartFeeds.count < RSSReaderLimits.maxSmartFeeds else { throw RSSReaderError.tooManySmartFeeds }
            if smart.id.isEmpty { smart.id = newId() }
            library.smartFeeds.append(smart)
        }
        touchLibrary()
        return smart
    }

    public func deleteSmartFeed(_ id: String) {
        library.smartFeeds.removeAll { $0.id == id }
        library.collapsed.removeAll { $0 == "smart:\(id)" }
        touchLibrary()
    }

    public func setFeedFlag(_ id: String, _ path: WritableKeyPath<RSSFeed, Bool>, _ value: Bool) {
        guard let current = feed(id), current[keyPath: path] != value else { return }
        updateFeed(id) { $0[keyPath: path] = value }
    }

    @discardableResult
    public func changeAddress(_ id: String, to input: String) throws -> Bool {
        guard let current = feed(id) else { return false }
        let url = RSSURL.normalizeFeedInput(input)
        guard !url.isEmpty, !url.contains(where: { $0.isWhitespace }), URL(string: url) != nil else { throw RSSReaderError.invalidAddress }
        let identity = RSSURL.identity(url)
        if let other = library.feeds.first(where: { $0.id != id && $0.identities.contains(identity) }) {
            throw RSSReaderError.duplicate(other.id)
        }
        if url == current.url { return false }
        let stamp = now
        updateFeed(id) { feed in
            if !feed.previousUrls.contains(feed.url) { feed.previousUrls.append(feed.url) }
            feed.previousUrls.removeAll { $0 == url }
            if feed.previousUrls.count > 10 { feed.previousUrls.removeFirst(feed.previousUrls.count - 10) }
            feed.url = url
            feed.etag = ""
            feed.lastModified = ""
            feed.error = ""
            feed.errorAt = 0
            feed.errorCount = 0
            feed.nextFetchAt = stamp
        }
        log("edit", "Changed the address of \(current.displayTitle) to \(url)")
        return true
    }

    public func isCollapsed(_ key: String) -> Bool { library.collapsed.contains(key) }

    public func setCollapsed(_ key: String, _ collapsed: Bool) {
        if collapsed {
            if !library.collapsed.contains(key) { library.collapsed.append(key) }
        } else {
            library.collapsed.removeAll { $0 == key }
        }
        touchLibrary()
    }

    public func markRefreshed() {
        library.lastRefreshAt = now
        touchLibrary()
    }

    public func feed(_ id: String) -> RSSFeed? { library.feeds.first { $0.id == id } }

    public func folder(_ id: String) -> RSSFolder? { library.folders.first { $0.id == id } }

    public func feed(matching url: String) -> RSSFeed? {
        let identity = RSSURL.identity(url)
        return library.feeds.first { $0.identities.contains(identity) }
    }

    public func folder(named name: String) -> RSSFolder? {
        let wanted = name.lowercased()
        return library.folders.first { $0.name.lowercased() == wanted }
    }

    private func updateFeed(_ id: String, _ change: (inout RSSFeed) -> Void) {
        guard let index = library.feeds.firstIndex(where: { $0.id == id }) else { return }
        change(&library.feeds[index])
        touchLibrary()
    }

    @discardableResult
    public func addFolder(_ rawName: String) throws -> RSSFolder {
        let name = RSSText.clean(rawName, 120)
        guard !name.isEmpty else { throw RSSReaderError.emptyName }
        if let existing = folder(named: name) { return existing }
        guard library.folders.count < RSSReaderLimits.maxFolders else { throw RSSReaderError.tooManyFolders }
        let folder = RSSFolder(id: newId(), name: name)
        library.folders.append(folder)
        touchLibrary()
        return folder
    }

    public func renameFolder(_ id: String, to rawName: String) throws {
        let name = RSSText.clean(rawName, 120)
        guard !name.isEmpty else { throw RSSReaderError.emptyName }
        if let other = folder(named: name), other.id != id { throw RSSReaderError.duplicate(name) }
        guard let index = library.folders.firstIndex(where: { $0.id == id }) else { return }
        library.folders[index].name = name
        touchLibrary()
    }

    public func deleteFolder(_ id: String) {
        guard let index = library.folders.firstIndex(where: { $0.id == id }) else { return }
        let name = library.folders[index].name
        library.folders.remove(at: index)
        for feedIndex in library.feeds.indices where library.feeds[feedIndex].folderId == id {
            library.feeds[feedIndex].folderId = ""
        }
        library.collapsed.removeAll { $0 == "folder:\(id)" }
        log("folder", "Deleted the folder \(name)")
        touchLibrary()
    }

    @discardableResult
    public func addFeed(url rawUrl: String, title: String = "", folderId: String = "", siteUrl: String = "") throws -> RSSFeed {
        let url = RSSURL.http(rawUrl)
        guard !url.isEmpty else { throw RSSReaderError.invalidAddress }
        if let existing = feed(matching: url) { throw RSSReaderError.duplicate(existing.id) }
        guard library.feeds.count < RSSReaderLimits.maxFeeds else { throw RSSReaderError.tooManyFeeds }
        var feed = RSSFeed(
            id: newId(), url: url, title: RSSText.clean(title, 200),
            folderId: folder(folderId) == nil ? "" : folderId, siteUrl: RSSURL.http(siteUrl), addedAt: now)
        feed.iconUrl = ""
        library.feeds.append(feed)
        articles[feed.id] = []
        bodies[feed.id] = [:]
        touchLibrary()
        return feed
    }

    public func iconData(_ feedId: String) -> Data? {
        guard let feed = feed(feedId), !feed.iconFound.isEmpty else { return nil }
        return disk.read(Self.iconFile(feedId))
    }

    public func setIcon(_ feedId: String, url: String, data: Data?, source: String, at stamp: Int64) {
        guard feed(feedId) != nil else { return }
        let found = data == nil ? "" : RSSURL.http(url)
        updateFeed(feedId) { feed in
            feed.iconFound = found
            feed.iconSource = source
            feed.iconCheckedAt = stamp
        }
        if let data, !found.isEmpty {
            disk.write(Self.iconFile(feedId), data)
        } else {
            disk.remove(Self.iconFile(feedId))
        }
    }

    public func renameFeed(_ id: String, to rawTitle: String) {
        let title = RSSText.clean(rawTitle, 200)
        updateFeed(id) { feed in
            feed.customTitle = title == feed.title ? "" : title
        }
    }

    public func moveFeed(_ id: String, toFolder folderId: String) {
        let target = folder(folderId) == nil ? "" : folderId
        updateFeed(id) { $0.folderId = target }
    }

    public func unsubscribe(_ id: String) {
        guard let feed = feed(id) else { return }
        library.feeds.removeAll { $0.id == id }
        library.filters.removeAll { $0.feedId == id }
        articles[id] = nil
        bodies[id] = nil
        texts[id] = nil
        linkIndex = nil
        bodyUse.removeAll { $0 == id }
        dirtyHeads.remove(id)
        dirtyBodies.remove(id)
        removed.insert(id)
        log("remove", "Unsubscribed from \(feed.displayTitle)")
        touchLibrary()
    }

    public func syncLibrary() -> RSSReaderLibrary {
        var copy = library
        copy.activity = []
        copy.lastRefreshAt = 0
        copy.feeds = library.feeds.map { feed in
            var clean = RSSFeed(id: feed.id, url: feed.url, title: feed.title, folderId: feed.folderId, siteUrl: feed.siteUrl, addedAt: feed.addedAt)
            clean.customTitle = feed.customTitle
            clean.iconUrl = feed.iconUrl
            clean.summary = feed.summary
            clean.previousUrls = feed.previousUrls
            clean.paused = feed.paused
            clean.excludeFromToday = feed.excludeFromToday
            clean.excludeFromUnread = feed.excludeFromUnread
            clean.language = feed.language
            return clean
        }
        return copy
    }

    @discardableResult
    public func restoreLibrary(_ incoming: RSSReaderLibrary) -> Int {
        let local = Dictionary(library.feeds.map { ($0.url, $0) }, uniquingKeysWith: { first, _ in first })
        let kept = Set(incoming.feeds.map(\.id))
        for feed in library.feeds where !kept.contains(feed.id) && !incoming.feeds.contains(where: { $0.url == feed.url }) {
            unsubscribe(feed.id)
        }
        var feeds: [RSSFeed] = []
        for var feed in incoming.feeds {
            if let mine = local[feed.url] {
                feed.id = mine.id
                feed.etag = mine.etag
                feed.lastModified = mine.lastModified
                feed.fetchedAt = mine.fetchedAt
                feed.nextFetchAt = mine.nextFetchAt
                feed.iconFound = mine.iconFound
                feed.iconSource = mine.iconSource
                feed.iconCheckedAt = mine.iconCheckedAt
            } else {
                feed.nextFetchAt = 0
                articles[feed.id] = articles[feed.id] ?? []
            }
            if !feeds.contains(where: { $0.url == feed.url }) { feeds.append(feed) }
        }
        let renamed = Dictionary(incoming.feeds.compactMap { feed in local[feed.url].map { (feed.id, $0.id) } }, uniquingKeysWith: { first, _ in first })
        library.feeds = feeds
        library.folders = incoming.folders
        library.settings = incoming.settings
        library.collapsed = incoming.collapsed
        library.filters = incoming.filters.map { filter in
            var copy = filter
            if let id = renamed[filter.feedId] { copy.feedId = id }
            return copy
        }
        library.smartFeeds = incoming.smartFeeds
        linkIndex = nil
        matchCache = [:]
        log("restore", "Restored \(feeds.count) subscriptions saved on ForkMesh")
        touchLibrary()
        return feeds.count
    }

    public static func prepare(_ parsed: RSSParsedFeed, feedUrl: String, now: Int64) -> [RSSPreparedArticle] {
        var seen: Set<String> = []
        var prepared: [RSSPreparedArticle] = []
        for item in parsed.items.prefix(RSSReaderLimits.maxItemsPerFetch) {
            let guid = item.articleGuid
            if guid.isEmpty || !seen.insert(guid).inserted { continue }
            let content = RSSHTML.sanitize(item.contentHtml, base: item.url.isEmpty ? feedUrl : item.url)
            let summarySource = item.summaryHtml.isEmpty ? item.contentHtml : item.summaryHtml
            var published = item.published != 0 ? item.published : item.updated
            if published <= 0 || published > now + RSSReaderLimits.futureSlackMs { published = now }
            var title = RSSText.clean(item.title, 1000)
            if title.isEmpty { title = RSSHTML.plainText(summarySource, limit: 120) }
            let image = item.images.first ?? RSSHTML.firstImage(content)
            let media = item.images.isEmpty ? "" : "\n" + item.images.joined(separator: "\n")
            prepared.append(RSSPreparedArticle(
                guid: guid, title: title, url: item.url, externalUrl: item.externalUrl,
                author: RSSText.clean(item.author, 200), summary: RSSHTML.plainText(summarySource, limit: RSSHTML.maxSummaryChars),
                image: image, published: published, contentKey: RSSText.fnv(title + "\n" + item.url + "\n" + content + media),
                body: RSSArticleBody(contentHtml: content, commentsUrl: item.commentsUrl, enclosures: item.enclosures, images: item.images),
                language: item.language == parsed.language ? "" : item.language))
        }
        return prepared
    }

    @discardableResult
    public func apply(_ result: RSSFetchResult) -> RSSApplied {
        guard let current = feed(result.feedId) else { return RSSApplied(added: 0, updated: 0, ok: false) }
        let stamp = now
        switch result.outcome {
        case .notModified:
            updateFeed(current.id) { feed in
                feed.error = ""
                feed.errorAt = 0
                feed.errorCount = 0
                feed.fetchedAt = stamp
                feed.nextFetchAt = stamp + RSSReaderLimits.fetchIntervalMs
            }
            return RSSApplied()
        case .failed(let error):
            updateFeed(current.id) { feed in
                feed.errorCount += 1
                feed.error = error
                feed.errorAt = stamp
                feed.fetchedAt = stamp
                feed.nextFetchAt = stamp + RSSReaderLimits.backoff(errors: feed.errorCount)
            }
            return RSSApplied(added: 0, updated: 0, ok: false)
        case let .updated(parsed, items, finalUrl, permanent, etag, lastModified):
            var target = current.url
            if permanent, !finalUrl.isEmpty, RSSURL.identity(finalUrl) != RSSURL.identity(current.url) {
                let identity = RSSURL.identity(finalUrl)
                let clash = library.feeds.contains { $0.id != current.id && $0.identities.contains(identity) }
                if !clash { target = finalUrl }
            } else if permanent, !finalUrl.isEmpty, finalUrl != current.url, RSSURL.identity(finalUrl) == RSSURL.identity(current.url) {
                target = finalUrl
            }
            updateFeed(current.id) { feed in
                if target != feed.url {
                    if RSSURL.identity(target) != RSSURL.identity(feed.url), !feed.previousUrls.contains(feed.url) {
                        feed.previousUrls.append(feed.url)
                        if feed.previousUrls.count > 10 { feed.previousUrls.removeFirst(feed.previousUrls.count - 10) }
                    }
                    feed.url = target
                }
                if !parsed.title.isEmpty { feed.title = RSSText.clean(parsed.title, 200) }
                if !parsed.siteUrl.isEmpty { feed.siteUrl = parsed.siteUrl }
                if !parsed.description.isEmpty { feed.summary = parsed.description }
                if !parsed.iconUrl.isEmpty { feed.iconUrl = parsed.iconUrl }
                feed.language = parsed.language
                feed.etag = RSSText.prefix(etag, 200)
                feed.lastModified = RSSText.prefix(lastModified, 200)
                feed.error = ""
                feed.errorAt = 0
                feed.errorCount = 0
                feed.fetchedAt = stamp
                feed.nextFetchAt = stamp + RSSReaderLimits.fetchIntervalMs
            }
            return store(items, feedId: current.id, now: stamp)
        }
    }

    private func linkOwners() -> [String: Set<String>] {
        if let linkIndex { return linkIndex }
        var built: [String: Set<String>] = [:]
        for (feedId, list) in articles {
            for article in list where !RSSURL.http(article.link).isEmpty {
                built[RSSURL.identity(article.link), default: []].insert(feedId)
            }
        }
        linkIndex = built
        return built
    }

    @discardableResult
    public func store(_ items: [RSSPreparedArticle], feedId: String, now stamp: Int64) -> RSSApplied {
        var list = articles[feedId] ?? []
        var bodyMap = loadBodies(feedId)
        var index: [String: Int] = [:]
        for (position, article) in list.enumerated() { index[article.guid] = position }
        var applied = RSSApplied()
        let arrivalFilters = library.filters.filter { $0.action == .markRead && !$0.pattern.isEmpty && $0.applies(to: feedId) }
        let folding = library.settings.foldDuplicates
        var owners = folding ? linkOwners() : [:]
        var changedIds: [String] = []
        var newTexts: [String: String] = [:]
        for item in items {
            if let position = index[item.guid] {
                if list[position].contentKey != item.contentKey {
                    list[position].title = item.title
                    list[position].url = item.url
                    list[position].externalUrl = item.externalUrl
                    list[position].author = item.author
                    list[position].summary = item.summary
                    list[position].image = item.image
                    list[position].language = item.language
                    list[position].contentKey = item.contentKey
                    var body = item.body
                    body.readerHtml = bodyMap[list[position].id]?.readerHtml ?? ""
                    bodyMap[list[position].id] = body
                    changedIds.append(list[position].id)
                    newTexts[list[position].id] = Self.filterText(item.body.contentHtml, summary: item.summary)
                    applied.updated += 1
                }
                continue
            }
            var article = RSSArticle(feedId: feedId, guid: item.guid)
            article.title = item.title
            article.url = item.url
            article.externalUrl = item.externalUrl
            article.author = item.author
            article.summary = item.summary
            article.image = item.image
            article.language = item.language
            article.published = item.published
            article.arrivedAt = stamp
            article.contentKey = item.contentKey
            let text = Self.filterText(item.body.contentHtml, summary: item.summary)
            if arrivalFilters.contains(where: { RSSMatcher.matches($0, title: article.title, author: article.author, text: { text }) }) {
                article.read = true
            }
            if folding && !RSSURL.http(article.link).isEmpty {
                let identity = RSSURL.identity(article.link)
                if owners[identity, default: []].contains(where: { $0 != feedId }) { article.read = true }
                owners[identity, default: []].insert(feedId)
            }
            index[item.guid] = list.count
            list.append(article)
            bodyMap[article.id] = item.body
            changedIds.append(article.id)
            newTexts[article.id] = text
            applied.added += 1
        }
        let ordered = list.enumerated().sorted { lhs, rhs in
            lhs.element.published == rhs.element.published ? lhs.offset < rhs.offset : lhs.element.published > rhs.element.published
        }.map(\.element)
        var kept: [RSSArticle] = []
        var unstarred = 0
        for article in ordered {
            if article.starred || article.readLater {
                kept.append(article)
            } else if unstarred < RSSReaderLimits.maxArticlesPerFeed {
                kept.append(article)
                unstarred += 1
            }
        }
        let keptIds = Set(kept.map(\.id))
        for key in bodyMap.keys where !keptIds.contains(key) { bodyMap[key] = nil }
        let dropped = list.map(\.id).filter { !keptIds.contains($0) }
        forget(changedIds + dropped, feedId: feedId)
        if texts[feedId] != nil {
            for (id, text) in newTexts where keptIds.contains(id) { texts[feedId]?[id] = text }
        }
        if folding && dropped.isEmpty {
            linkIndex = owners
        } else {
            linkIndex = nil
        }
        articles[feedId] = kept
        bodies[feedId] = bodyMap
        if applied.added > 0 || applied.updated > 0 || kept.count != list.count {
            dirtyHeads.insert(feedId)
            dirtyBodies.insert(feedId)
        }
        return applied
    }

    public func body(for article: RSSArticle) -> RSSArticleBody {
        loadBodies(article.feedId)[article.id] ?? RSSArticleBody()
    }

    public func setReader(_ html: String, for article: RSSArticle) {
        var map = loadBodies(article.feedId)
        var body = map[article.id] ?? RSSArticleBody()
        body.readerHtml = html
        map[article.id] = body
        bodies[article.feedId] = map
        dirtyBodies.insert(article.feedId)
    }

    public func article(_ id: String) -> RSSArticle? {
        for list in articles.values {
            if let found = list.first(where: { $0.id == id }) { return found }
        }
        return nil
    }

    private func update(_ ids: Set<String>, _ change: (inout RSSArticle) -> Bool) -> Int {
        var changed = 0
        for (feedId, list) in articles {
            var copy = list
            var touched = false
            for position in copy.indices where ids.contains(copy[position].id) {
                if change(&copy[position]) {
                    touched = true
                    changed += 1
                }
            }
            if touched {
                articles[feedId] = copy
                dirtyHeads.insert(feedId)
            }
        }
        return changed
    }

    @discardableResult
    public func setRead(_ ids: [String], _ read: Bool) -> Int {
        update(Set(ids)) { article in
            guard article.read != read else { return false }
            article.read = read
            return true
        }
    }

    @discardableResult
    public func setStarred(_ ids: [String], _ starred: Bool) -> Int {
        update(Set(ids)) { article in
            guard article.starred != starred else { return false }
            article.starred = starred
            return true
        }
    }

    @discardableResult
    public func setReadLater(_ ids: [String], _ readLater: Bool) -> Int {
        update(Set(ids)) { article in
            guard article.readLater != readLater else { return false }
            article.readLater = readLater
            return true
        }
    }

    public func feedIds(in scope: RSSScope) -> Set<String>? {
        switch scope {
        case .feed(let id): return [id]
        case .folder(let id): return Set(library.feeds.filter { $0.folderId == id }.map(\.id))
        default: return nil
        }
    }

    public func smartMatches(_ smart: RSSSmartFeed, _ article: RSSArticle, feed: RSSFeed?, now stamp: Int64) -> Bool {
        if smart.starredOnly && !article.starred { return false }
        if smart.maxAgeDays > 0 && article.date < stamp - Int64(smart.maxAgeDays) * RSSReaderLimits.dayMs { return false }
        if smart.scope.hasPrefix("feed:") {
            if article.feedId != String(smart.scope.dropFirst(5)) { return false }
        } else if smart.scope.hasPrefix("folder:") {
            if feed?.folderId != String(smart.scope.dropFirst(7)) { return false }
        }
        return wordsMatch(smart.words, regex: smart.regex, article)
    }

    public func inScope(_ article: RSSArticle, _ scope: RSSScope, feed: RSSFeed?, now stamp: Int64) -> Bool {
        switch scope {
        case .today:
            if feed?.excludeFromToday ?? false || feed?.excludeFromUnread ?? false { return false }
            return article.published >= stamp - RSSReaderLimits.todayMs
        case .unread: return !(feed?.excludeFromUnread ?? false)
        case .starred: return article.starred
        case .readLater: return article.readLater
        case .smart(let id):
            guard let smart = smartFeed(id) else { return false }
            return smartMatches(smart, article, feed: feed, now: stamp)
        case .feed(let id): return article.feedId == id
        case .folder(let id): return feed?.folderId == id
        }
    }

    public func matches(_ article: RSSArticle, _ scope: RSSScope, now stamp: Int64) -> Bool {
        switch scope {
        case .today, .unread: return !article.read && inScope(article, scope, feed: feed(article.feedId), now: stamp)
        default: return inScope(article, scope, feed: feed(article.feedId), now: stamp)
        }
    }

    private var feedsById: [String: RSSFeed] {
        Dictionary(library.feeds.map { ($0.id, $0) }, uniquingKeysWith: { first, _ in first })
    }

    @discardableResult
    public func markAllRead(_ scope: RSSScope, asOf: Int64? = nil) -> Int {
        let stamp = now
        let limit = asOf ?? stamp
        let keepStarred = library.settings.markAllKeepsStarred
        let filters = hideFilters
        let byId = feedsById
        var ids: [String] = []
        let feedFilter = feedIds(in: scope)
        for (feedId, list) in articles where feedFilter?.contains(feedId) ?? true {
            let feed = byId[feedId]
            for article in list where !article.read && article.arrivedAt <= limit {
                if keepStarred && article.starred { continue }
                guard inScope(article, scope, feed: feed, now: stamp), !isHidden(article, filters: filters) else { continue }
                ids.append(article.id)
            }
        }
        return setRead(ids, true)
    }

    public func olderUnread(_ scope: RSSScope?, _ choice: RSSCatchUp) -> [String] {
        let stamp = now
        let cutoff = stamp - Int64(choice.days) * RSSReaderLimits.dayMs
        let keepStarred = library.settings.markAllKeepsStarred
        let filters = hideFilters
        let byId = feedsById
        let feedFilter = scope.flatMap { feedIds(in: $0) }
        var ids: [String] = []
        for (feedId, list) in articles where feedFilter?.contains(feedId) ?? true {
            let feed = byId[feedId]
            for article in list where !article.read && article.date < cutoff {
                if keepStarred && article.starred { continue }
                if let scope, !inScope(article, scope, feed: feed, now: stamp) { continue }
                if isHidden(article, filters: filters) { continue }
                ids.append(article.id)
            }
        }
        return ids
    }

    @discardableResult
    public func markOlderRead(_ scope: RSSScope?, _ choice: RSSCatchUp) -> Int {
        setRead(olderUnread(scope, choice), true)
    }

    public func counts(now stamp: Int64? = nil) -> RSSCounts {
        let moment = stamp ?? now
        var counts = RSSCounts()
        let filters = hideFilters
        let smarts = library.smartFeeds
        for smart in smarts { counts.bySmartFeed[smart.id] = 0 }
        for feed in library.feeds {
            var unread = 0
            for article in articles[feed.id] ?? [] {
                if article.read {
                    if article.readLater && !isHidden(article, filters: filters) { counts.readLaterTotal += 1 }
                    continue
                }
                if isHidden(article, filters: filters) { continue }
                if article.readLater { counts.readLaterTotal += 1 }
                unread += 1
                if feed.excludeFromUnread { continue }
                if !feed.excludeFromToday && article.published >= moment - RSSReaderLimits.todayMs { counts.today += 1 }
                if article.starred { counts.starred += 1 }
                if article.readLater { counts.readLater += 1 }
                for smart in smarts where smartMatches(smart, article, feed: feed, now: moment) {
                    counts.bySmartFeed[smart.id, default: 0] += 1
                }
            }
            counts.byFeed[feed.id] = unread
            if !feed.excludeFromUnread { counts.unread += unread }
        }
        return counts
    }

    public func folderUnread(_ id: String, counts: RSSCounts) -> Int {
        library.feeds.filter { $0.folderId == id && !$0.excludeFromUnread }.reduce(0) { $0 + (counts.byFeed[$1.id] ?? 0) }
    }

    public func unread(_ scope: RSSScope, counts: RSSCounts) -> Int {
        switch scope {
        case .today: return counts.today
        case .unread: return counts.unread
        case .starred: return counts.starred
        case .readLater: return counts.readLater
        case .smart(let id): return counts.bySmartFeed[id] ?? 0
        case .feed(let id): return counts.byFeed[id] ?? 0
        case .folder(let id): return folderUnread(id, counts: counts)
        }
    }

    public func title(_ scope: RSSScope) -> String {
        switch scope {
        case .today: return "Today"
        case .unread: return "All Unread"
        case .starred: return "Starred"
        case .readLater: return "Read Later"
        case .smart(let id): return smartFeed(id)?.name ?? "Smart Feed"
        case .feed(let id): return feed(id)?.displayTitle ?? "Feed"
        case .folder(let id): return folder(id)?.name ?? "Folder"
        }
    }

    public func exists(_ scope: RSSScope) -> Bool {
        switch scope {
        case .today, .unread, .starred, .readLater: return true
        case .smart(let id): return smartFeed(id) != nil
        case .feed(let id): return feed(id) != nil
        case .folder(let id): return folder(id) != nil
        }
    }

    static func byName(_ lhs: String, _ rhs: String) -> Bool {
        let order = lhs.compare(rhs, options: [.caseInsensitive, .diacriticInsensitive])
        return order == .orderedSame ? lhs < rhs : order == .orderedAscending
    }

    public static func feedMatches(_ feed: RSSFeed, query: String) -> Bool {
        let needle = RSSText.clean(query, 200).lowercased()
        if needle.isEmpty { return true }
        return feed.displayTitle.lowercased().contains(needle) || RSSURL.host(feed.url).contains(needle)
            || feed.url.lowercased().contains(needle)
    }

    public func sidebar(unreadOnly: Bool, selected: RSSScope? = nil, counts: RSSCounts, search: String = "") -> [RSSSidebarEntry] {
        let folderIds = Set(library.folders.map(\.id))
        let searching = !RSSText.clean(search, 200).isEmpty
        let byUnread = library.settings.sortFeedsByUnread
        let shows: (RSSFeed) -> Bool = { feed in
            if searching { return Self.feedMatches(feed, query: search) }
            return !unreadOnly || (counts.byFeed[feed.id] ?? 0) > 0 || selected == .feed(feed.id)
        }
        let order: (String, Int, String, Int) -> Bool = { leftName, leftCount, rightName, rightCount in
            if byUnread && leftCount != rightCount { return leftCount > rightCount }
            return Self.byName(leftName, rightName)
        }
        enum Node {
            case feed(RSSFeed, Int)
            case folder(RSSFolder, Int)
            var name: String {
                switch self {
                case .feed(let feed, _): return feed.displayTitle
                case .folder(let folder, _): return folder.name
                }
            }
            var count: Int {
                switch self {
                case .feed(_, let count), .folder(_, let count): return count
                }
            }
        }
        var nodes: [Node] = library.folders.map { Node.folder($0, folderUnread($0.id, counts: counts)) }
        nodes += library.feeds.filter { $0.folderId.isEmpty || !folderIds.contains($0.folderId) }.filter(shows)
            .map { Node.feed($0, counts.byFeed[$0.id] ?? 0) }
        nodes.sort { order($0.name, $0.count, $1.name, $1.count) }
        var entries: [RSSSidebarEntry] = []
        for node in nodes {
            switch node {
            case .feed(let feed, let unread):
                entries.append(.feed(feed, unread: unread, indented: false))
            case .folder(let folder, let unread):
                let children = library.feeds.filter { $0.folderId == folder.id && shows($0) }
                    .sorted { order($0.displayTitle, counts.byFeed[$0.id] ?? 0, $1.displayTitle, counts.byFeed[$1.id] ?? 0) }
                if searching && children.isEmpty { continue }
                if !searching && unreadOnly && children.isEmpty && selected != .folder(folder.id) { continue }
                let collapsed = !searching && isCollapsed("folder:\(folder.id)")
                entries.append(.folder(folder, unread: unread, collapsed: collapsed))
                if !collapsed {
                    for feed in children { entries.append(.feed(feed, unread: counts.byFeed[feed.id] ?? 0, indented: true)) }
                }
            }
        }
        return entries
    }

    public func feedOrder() -> [String] {
        let folderIds = Set(library.folders.map(\.id))
        var nodes: [(String, [RSSFeed])] = library.folders.map { folder in
            (folder.name, library.feeds.filter { $0.folderId == folder.id }.sorted { Self.byName($0.displayTitle, $1.displayTitle) })
        }
        nodes += library.feeds.filter { $0.folderId.isEmpty || !folderIds.contains($0.folderId) }.map { ($0.displayTitle, [$0]) }
        nodes.sort { Self.byName($0.0, $1.0) }
        return nodes.flatMap { $0.1.map(\.id) }
    }

    public func timeline(
        _ scope: RSSScope, query: String = "", unreadOnly: Bool = false, keep: Set<String> = [], now stamp: Int64? = nil
    ) -> [RSSArticle] {
        let moment = stamp ?? now
        let needle = RSSText.clean(query, 200).lowercased()
        let feedFilter = feedIds(in: scope)
        let filters = hideFilters
        let byId = feedsById
        var titles: [String: String] = [:]
        for feed in library.feeds { titles[feed.id] = feed.displayTitle }
        var needsUnread = unreadOnly || scope.unreadOnly
        if case .smart(let id) = scope, smartFeed(id)?.unreadOnly ?? false { needsUnread = true }
        var list: [RSSArticle] = []
        for (feedId, items) in articles where feedFilter?.contains(feedId) ?? true {
            let feed = byId[feedId]
            for article in items {
                if needsUnread && article.read && !keep.contains(article.id) { continue }
                guard inScope(article, scope, feed: feed, now: moment), !isHidden(article, filters: filters) else { continue }
                if !needle.isEmpty {
                    let haystack = [article.title, article.summary, article.author, titles[feedId] ?? ""].joined(separator: "\n").lowercased()
                    if !haystack.contains(needle) { continue }
                }
                list.append(article)
            }
        }
        let oldest = library.settings.sortOldestFirst
        list.sort { lhs, rhs in
            if lhs.published != rhs.published { return oldest ? lhs.published < rhs.published : lhs.published > rhs.published }
            return lhs.id < rhs.id
        }
        if library.settings.groupByFeed, case .feed = scope {
            return list
        }
        if library.settings.groupByFeed {
            let positions = Dictionary(list.enumerated().map { ($0.element.id, $0.offset) }, uniquingKeysWith: { first, _ in first })
            list.sort { lhs, rhs in
                let left = titles[lhs.feedId] ?? ""
                let right = titles[rhs.feedId] ?? ""
                if left != right { return Self.byName(left, right) }
                if lhs.feedId != rhs.feedId { return lhs.feedId < rhs.feedId }
                return (positions[lhs.id] ?? 0) < (positions[rhs.id] ?? 0)
            }
        }
        return list
    }

    public func nextUnread(after currentId: String?, in list: [RSSArticle], scope: RSSScope) -> (scope: RSSScope, article: RSSArticle)? {
        let fresh = Dictionary(list.compactMap { article -> (String, RSSArticle)? in
            guard let current = self.article(article.id) else { return nil }
            return (article.id, current)
        }, uniquingKeysWith: { first, _ in first })
        let index = currentId.flatMap { id in list.firstIndex { $0.id == id } } ?? -1
        let after = list.suffix(from: min(list.count, index + 1)).first { !(fresh[$0.id]?.read ?? true) }
        let before = list.prefix(max(0, index)).first { !(fresh[$0.id]?.read ?? true) }
        if let found = after ?? before, let current = fresh[found.id] { return (scope, current) }
        let counts = self.counts()
        let order = feedOrder()
        var start = -1
        if case .feed(let id) = scope { start = order.firstIndex(of: id) ?? -1 }
        let rotated = Array(order.suffix(from: start + 1)) + Array(order.prefix(max(0, start)))
        for feedId in rotated where (counts.byFeed[feedId] ?? 0) > 0 {
            let next = RSSScope.feed(feedId)
            if let first = timeline(next).first(where: { !$0.read }) { return (next, first) }
        }
        return nil
    }

    public func dueFeeds(now stamp: Int64? = nil, force: Bool = false) -> [RSSFeed] {
        let moment = stamp ?? now
        return library.feeds.filter { feed in
            if feed.paused { return false }
            if force { return feed.fetchedAt <= moment - RSSReaderLimits.minForceGapMs }
            return feed.nextFetchAt <= moment
        }.sorted { $0.nextFetchAt < $1.nextFetchAt }
    }

    public func importOPML(_ entries: [RSSOPMLEntry]) -> (imported: Int, skipped: Int) {
        var imported = 0
        var skipped = 0
        for entry in entries {
            if library.feeds.count >= RSSReaderLimits.maxFeeds || feed(matching: entry.url) != nil {
                skipped += 1
                continue
            }
            var folderId = ""
            if !entry.folder.isEmpty { folderId = (try? addFolder(entry.folder))?.id ?? "" }
            if (try? addFeed(url: entry.url, title: entry.title, folderId: folderId, siteUrl: entry.siteUrl)) != nil {
                imported += 1
            } else {
                skipped += 1
            }
        }
        log("import", "Imported \(imported) subscription\(imported == 1 ? "" : "s"), skipped \(skipped)")
        return (imported, skipped)
    }

    public func exportOPML() -> String { RSSOPML.build(folders: library.folders, feeds: library.feeds, now: now) }

    public func stats() -> RSSStats {
        var stats = RSSStats()
        stats.feeds = library.feeds.count
        stats.folders = library.folders.count
        var oldest: Int64 = 0
        for list in articles.values {
            stats.articles += list.count
            for article in list {
                if !article.read { stats.unread += 1 }
                if article.starred { stats.starred += 1 }
                if oldest == 0 || article.published < oldest { oldest = article.published }
            }
        }
        stats.oldestArticle = oldest
        stats.failingFeeds = library.feeds.filter { $0.errorCount > 0 }.count
        stats.lastRefresh = library.lastRefreshAt
        return stats
    }

    public var failingFeeds: [RSSFeed] {
        library.feeds.filter { $0.errorCount > 0 }.sorted { $0.errorAt > $1.errorAt }
    }
}
