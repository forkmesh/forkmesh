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
    private let disk: RSSReaderDisk
    private let clock: () -> Int64
    private let newId: () -> String

    public static let libraryFile = "library.json"
    public static func headsFile(_ feedId: String) -> String { "articles-\(feedId).json" }
    public static func bodiesFile(_ feedId: String) -> String { "bodies-\(feedId).json" }

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
        articles[id] = nil
        bodies[id] = nil
        bodyUse.removeAll { $0 == id }
        dirtyHeads.remove(id)
        dirtyBodies.remove(id)
        removed.insert(id)
        log("remove", "Unsubscribed from \(feed.displayTitle)")
        touchLibrary()
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
            let image = RSSHTML.firstImage(content)
            prepared.append(RSSPreparedArticle(
                guid: guid, title: title, url: item.url, externalUrl: item.externalUrl,
                author: RSSText.clean(item.author, 200), summary: RSSHTML.plainText(summarySource, limit: RSSHTML.maxSummaryChars),
                image: image, published: published, contentKey: RSSText.fnv(title + "\n" + item.url + "\n" + content),
                body: RSSArticleBody(contentHtml: content, commentsUrl: item.commentsUrl, enclosures: item.enclosures)))
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

    @discardableResult
    public func store(_ items: [RSSPreparedArticle], feedId: String, now stamp: Int64) -> RSSApplied {
        var list = articles[feedId] ?? []
        var bodyMap = loadBodies(feedId)
        var index: [String: Int] = [:]
        for (position, article) in list.enumerated() { index[article.guid] = position }
        var applied = RSSApplied()
        for item in items {
            if let position = index[item.guid] {
                if list[position].contentKey != item.contentKey {
                    list[position].title = item.title
                    list[position].url = item.url
                    list[position].externalUrl = item.externalUrl
                    list[position].author = item.author
                    list[position].summary = item.summary
                    list[position].image = item.image
                    list[position].contentKey = item.contentKey
                    var body = item.body
                    body.readerHtml = bodyMap[list[position].id]?.readerHtml ?? ""
                    bodyMap[list[position].id] = body
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
            article.published = item.published
            article.arrivedAt = stamp
            article.contentKey = item.contentKey
            index[item.guid] = list.count
            list.append(article)
            bodyMap[article.id] = item.body
            applied.added += 1
        }
        let ordered = list.enumerated().sorted { lhs, rhs in
            lhs.element.published == rhs.element.published ? lhs.offset < rhs.offset : lhs.element.published > rhs.element.published
        }.map(\.element)
        var kept: [RSSArticle] = []
        var unstarred = 0
        for article in ordered {
            if article.starred {
                kept.append(article)
            } else if unstarred < RSSReaderLimits.maxArticlesPerFeed {
                kept.append(article)
                unstarred += 1
            }
        }
        let keptIds = Set(kept.map(\.id))
        for key in bodyMap.keys where !keptIds.contains(key) { bodyMap[key] = nil }
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

    public func feedIds(in scope: RSSScope) -> Set<String>? {
        switch scope {
        case .feed(let id): return [id]
        case .folder(let id): return Set(library.feeds.filter { $0.folderId == id }.map(\.id))
        default: return nil
        }
    }

    public func matches(_ article: RSSArticle, _ scope: RSSScope, now stamp: Int64) -> Bool {
        switch scope {
        case .today: return !article.read && article.published >= stamp - RSSReaderLimits.todayMs
        case .unread: return !article.read
        case .starred: return article.starred
        case .feed(let id): return article.feedId == id
        case .folder(let id): return feed(article.feedId)?.folderId == id
        }
    }

    @discardableResult
    public func markAllRead(_ scope: RSSScope, asOf: Int64? = nil) -> Int {
        let stamp = now
        let limit = asOf ?? stamp
        var ids: [String] = []
        let feedFilter = feedIds(in: scope)
        for (feedId, list) in articles where feedFilter?.contains(feedId) ?? true {
            for article in list where !article.read && article.arrivedAt <= limit && matches(article, scope, now: stamp) {
                ids.append(article.id)
            }
        }
        return setRead(ids, true)
    }

    public func counts(now stamp: Int64? = nil) -> RSSCounts {
        let moment = stamp ?? now
        var counts = RSSCounts()
        for feed in library.feeds {
            var unread = 0
            for article in articles[feed.id] ?? [] where !article.read {
                unread += 1
                if article.published >= moment - RSSReaderLimits.todayMs { counts.today += 1 }
                if article.starred { counts.starred += 1 }
            }
            counts.byFeed[feed.id] = unread
            counts.unread += unread
        }
        return counts
    }

    public func unread(_ scope: RSSScope, counts: RSSCounts) -> Int {
        switch scope {
        case .today: return counts.today
        case .unread: return counts.unread
        case .starred: return counts.starred
        case .feed(let id): return counts.byFeed[id] ?? 0
        case .folder(let id): return library.feeds.filter { $0.folderId == id }.reduce(0) { $0 + (counts.byFeed[$1.id] ?? 0) }
        }
    }

    public func title(_ scope: RSSScope) -> String {
        switch scope {
        case .today: return "Today"
        case .unread: return "All Unread"
        case .starred: return "Starred"
        case .feed(let id): return feed(id)?.displayTitle ?? "Feed"
        case .folder(let id): return folder(id)?.name ?? "Folder"
        }
    }

    public func exists(_ scope: RSSScope) -> Bool {
        switch scope {
        case .today, .unread, .starred: return true
        case .feed(let id): return feed(id) != nil
        case .folder(let id): return folder(id) != nil
        }
    }

    static func byName(_ lhs: String, _ rhs: String) -> Bool {
        let order = lhs.compare(rhs, options: [.caseInsensitive, .diacriticInsensitive])
        return order == .orderedSame ? lhs < rhs : order == .orderedAscending
    }

    public func sidebar(unreadOnly: Bool, selected: RSSScope? = nil, counts: RSSCounts) -> [RSSSidebarEntry] {
        let folderIds = Set(library.folders.map(\.id))
        let shows: (RSSFeed) -> Bool = { feed in
            !unreadOnly || (counts.byFeed[feed.id] ?? 0) > 0 || selected == .feed(feed.id)
        }
        enum Node {
            case feed(RSSFeed)
            case folder(RSSFolder)
            var name: String {
                switch self {
                case .feed(let feed): return feed.displayTitle
                case .folder(let folder): return folder.name
                }
            }
        }
        var nodes: [Node] = library.folders.map { Node.folder($0) }
        nodes += library.feeds.filter { $0.folderId.isEmpty || !folderIds.contains($0.folderId) }.filter(shows).map { Node.feed($0) }
        nodes.sort { Self.byName($0.name, $1.name) }
        var entries: [RSSSidebarEntry] = []
        for node in nodes {
            switch node {
            case .feed(let feed):
                entries.append(.feed(feed, unread: counts.byFeed[feed.id] ?? 0, indented: false))
            case .folder(let folder):
                let children = library.feeds.filter { $0.folderId == folder.id && shows($0) }
                    .sorted { Self.byName($0.displayTitle, $1.displayTitle) }
                if unreadOnly && children.isEmpty && selected != .folder(folder.id) { continue }
                let collapsed = isCollapsed("folder:\(folder.id)")
                let unread = children.reduce(0) { $0 + (counts.byFeed[$1.id] ?? 0) }
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
        var titles: [String: String] = [:]
        for feed in library.feeds { titles[feed.id] = feed.displayTitle }
        var list: [RSSArticle] = []
        for (feedId, items) in articles where feedFilter?.contains(feedId) ?? true {
            for article in items {
                let kept = keep.contains(article.id)
                switch scope {
                case .today:
                    guard article.published >= moment - RSSReaderLimits.todayMs, !article.read || kept else { continue }
                case .unread:
                    guard !article.read || kept else { continue }
                case .starred:
                    guard article.starred else { continue }
                default:
                    break
                }
                if unreadOnly && article.read && !kept { continue }
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
