import Foundation

public enum RSSReaderLimits {
    public static let maxFeeds = 500
    public static let maxFolders = 100
    public static let maxArticlesPerFeed = 400
    public static let maxItemsPerFetch = 200
    public static let maxFetchBytes = 2 * 1024 * 1024
    public static let fetchIntervalMs: Int64 = 30 * 60 * 1000
    public static let maxBackoffMs: Int64 = 24 * 60 * 60 * 1000
    public static let todayMs: Int64 = 24 * 60 * 60 * 1000
    public static let futureSlackMs: Int64 = 24 * 60 * 60 * 1000
    public static let minForceGapMs: Int64 = 60 * 1000
    public static let concurrency = 6
    public static let maxActivity = 200
    public static let maxRedirects = 5
    public static let maxFilters = 100
    public static let maxSmartFeeds = 50
    public static let filterTextChars = 20_000
    public static let dayMs: Int64 = 24 * 60 * 60 * 1000
    public static let forkMeshBlogFeed = "https://forkmesh.com/blog/rss.xml"

    public static func backoff(errors: Int) -> Int64 {
        let steps = max(0, min(errors, 10) - 1)
        return min(maxBackoffMs, fetchIntervalMs * (Int64(1) << Int64(steps)))
    }
}

public struct RSSFolder: Codable, Equatable, Hashable, Sendable, Identifiable {
    public var id: String
    public var name: String

    public init(id: String, name: String) {
        self.id = id
        self.name = name
    }
}

public struct RSSFeed: Codable, Equatable, Hashable, Sendable, Identifiable {
    public var id: String
    public var url: String
    public var title: String = ""
    public var customTitle: String = ""
    public var folderId: String = ""
    public var siteUrl: String = ""
    public var iconUrl: String = ""
    public var summary: String = ""
    public var etag: String = ""
    public var lastModified: String = ""
    public var previousUrls: [String] = []
    public var addedAt: Int64 = 0
    public var fetchedAt: Int64 = 0
    public var nextFetchAt: Int64 = 0
    public var errorCount: Int = 0
    public var error: String = ""
    public var errorAt: Int64 = 0
    public var paused = false
    public var excludeFromToday = false
    public var excludeFromUnread = false
    public var language: String = ""

    public init(id: String, url: String, title: String = "", folderId: String = "", siteUrl: String = "", addedAt: Int64 = 0) {
        self.id = id
        self.url = url
        self.title = title
        self.folderId = folderId
        self.siteUrl = siteUrl
        self.addedAt = addedAt
    }

    enum CodingKeys: String, CodingKey {
        case id, url, title, customTitle, folderId, siteUrl, iconUrl, summary, etag, lastModified, previousUrls
        case addedAt, fetchedAt, nextFetchAt, errorCount, error, errorAt
        case paused, excludeFromToday, excludeFromUnread, language
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = try c.decode(String.self, forKey: .id)
        url = try c.decode(String.self, forKey: .url)
        title = try c.decodeIfPresent(String.self, forKey: .title) ?? ""
        customTitle = try c.decodeIfPresent(String.self, forKey: .customTitle) ?? ""
        folderId = try c.decodeIfPresent(String.self, forKey: .folderId) ?? ""
        siteUrl = try c.decodeIfPresent(String.self, forKey: .siteUrl) ?? ""
        iconUrl = try c.decodeIfPresent(String.self, forKey: .iconUrl) ?? ""
        summary = try c.decodeIfPresent(String.self, forKey: .summary) ?? ""
        etag = try c.decodeIfPresent(String.self, forKey: .etag) ?? ""
        lastModified = try c.decodeIfPresent(String.self, forKey: .lastModified) ?? ""
        previousUrls = try c.decodeIfPresent([String].self, forKey: .previousUrls) ?? []
        addedAt = try c.decodeIfPresent(Int64.self, forKey: .addedAt) ?? 0
        fetchedAt = try c.decodeIfPresent(Int64.self, forKey: .fetchedAt) ?? 0
        nextFetchAt = try c.decodeIfPresent(Int64.self, forKey: .nextFetchAt) ?? 0
        errorCount = try c.decodeIfPresent(Int.self, forKey: .errorCount) ?? 0
        error = try c.decodeIfPresent(String.self, forKey: .error) ?? ""
        errorAt = try c.decodeIfPresent(Int64.self, forKey: .errorAt) ?? 0
        paused = (try? c.decodeIfPresent(Bool.self, forKey: .paused)) ?? false
        excludeFromToday = (try? c.decodeIfPresent(Bool.self, forKey: .excludeFromToday)) ?? false
        excludeFromUnread = (try? c.decodeIfPresent(Bool.self, forKey: .excludeFromUnread)) ?? false
        language = (try? c.decodeIfPresent(String.self, forKey: .language)) ?? ""
    }

    public var displayTitle: String {
        if !customTitle.isEmpty { return customTitle }
        if !title.isEmpty { return title }
        let host = RSSURL.host(url)
        return host.isEmpty ? url : host
    }

    public var homePage: String { siteUrl.isEmpty ? RSSURL.origin(url) : siteUrl }

    public var iconAddress: String { iconUrl.isEmpty ? RSSURL.favicon(siteUrl.isEmpty ? url : siteUrl) : iconUrl }

    public var identities: Set<String> { Set(([url] + previousUrls).map(RSSURL.identity)) }
}

public struct RSSArticle: Codable, Equatable, Hashable, Sendable, Identifiable {
    public var id: String
    public var feedId: String
    public var guid: String
    public var title: String = ""
    public var url: String = ""
    public var externalUrl: String = ""
    public var author: String = ""
    public var summary: String = ""
    public var image: String = ""
    public var published: Int64 = 0
    public var arrivedAt: Int64 = 0
    public var read: Bool = false
    public var starred: Bool = false
    public var readLater: Bool = false
    public var language: String = ""
    public var contentKey: String = ""

    public init(feedId: String, guid: String) {
        self.id = RSSArticle.makeId(feedId: feedId, guid: guid)
        self.feedId = feedId
        self.guid = guid
    }

    public static func makeId(feedId: String, guid: String) -> String { "\(feedId)-\(RSSText.fnv(guid))" }

    enum CodingKeys: String, CodingKey {
        case id, feedId, guid, title, url, externalUrl, author, summary, image, published, arrivedAt, read, starred, contentKey
        case readLater, language
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        feedId = try c.decode(String.self, forKey: .feedId)
        guid = try c.decode(String.self, forKey: .guid)
        id = try c.decodeIfPresent(String.self, forKey: .id) ?? RSSArticle.makeId(feedId: feedId, guid: guid)
        title = try c.decodeIfPresent(String.self, forKey: .title) ?? ""
        url = try c.decodeIfPresent(String.self, forKey: .url) ?? ""
        externalUrl = try c.decodeIfPresent(String.self, forKey: .externalUrl) ?? ""
        author = try c.decodeIfPresent(String.self, forKey: .author) ?? ""
        summary = try c.decodeIfPresent(String.self, forKey: .summary) ?? ""
        image = try c.decodeIfPresent(String.self, forKey: .image) ?? ""
        published = try c.decodeIfPresent(Int64.self, forKey: .published) ?? 0
        arrivedAt = try c.decodeIfPresent(Int64.self, forKey: .arrivedAt) ?? 0
        read = try c.decodeIfPresent(Bool.self, forKey: .read) ?? false
        starred = try c.decodeIfPresent(Bool.self, forKey: .starred) ?? false
        contentKey = try c.decodeIfPresent(String.self, forKey: .contentKey) ?? ""
        readLater = (try? c.decodeIfPresent(Bool.self, forKey: .readLater)) ?? false
        language = (try? c.decodeIfPresent(String.self, forKey: .language)) ?? ""
    }

    public var link: String { externalUrl.isEmpty ? url : externalUrl }

    public var date: Int64 { published > 0 ? published : arrivedAt }
}

public struct RSSArticleBody: Codable, Equatable, Sendable {
    public var contentHtml: String = ""
    public var commentsUrl: String = ""
    public var enclosures: [RSSEnclosure] = []
    public var readerHtml: String = ""
    public var images: [String] = []

    public init(
        contentHtml: String = "", commentsUrl: String = "", enclosures: [RSSEnclosure] = [], readerHtml: String = "", images: [String] = []
    ) {
        self.contentHtml = contentHtml
        self.commentsUrl = commentsUrl
        self.enclosures = enclosures
        self.readerHtml = readerHtml
        self.images = images
    }

    enum CodingKeys: String, CodingKey { case contentHtml, commentsUrl, enclosures, readerHtml, images }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        contentHtml = try c.decodeIfPresent(String.self, forKey: .contentHtml) ?? ""
        commentsUrl = try c.decodeIfPresent(String.self, forKey: .commentsUrl) ?? ""
        enclosures = try c.decodeIfPresent([RSSEnclosure].self, forKey: .enclosures) ?? []
        readerHtml = try c.decodeIfPresent(String.self, forKey: .readerHtml) ?? ""
        images = (try? c.decodeIfPresent([String].self, forKey: .images)) ?? []
    }
}

public enum RSSArticleTheme: String, Codable, CaseIterable, Sendable {
    case standard = "default"
    case sepia
    case paper
    case contrast

    public var title: String {
        switch self {
        case .standard: return "Default"
        case .sepia: return "Sepia"
        case .paper: return "Paper"
        case .contrast: return "High contrast"
        }
    }
}

public enum RSSArticleFont: String, Codable, CaseIterable, Sendable {
    case sans
    case serif
    case mono

    public var title: String {
        switch self {
        case .sans: return "Sans Serif"
        case .serif: return "Serif"
        case .mono: return "Monospaced"
        }
    }

    public var css: String {
        switch self {
        case .sans: return "-apple-system, system-ui, sans-serif"
        case .serif: return "ui-serif, 'New York', Georgia, serif"
        case .mono: return "ui-monospace, 'SF Mono', Menlo, monospace"
        }
    }
}

public enum RSSColorPalette: String, Codable, CaseIterable, Sendable {
    case automatic
    case light
    case dark

    public var title: String {
        switch self {
        case .automatic: return "Automatic"
        case .light: return "Light"
        case .dark: return "Dark"
        }
    }
}

public struct RSSReaderSettings: Codable, Equatable, Sendable {
    public static let textSizes = 12...28

    public var sortOldestFirst = false
    public var groupByFeed = false
    public var refreshClearsRead = false
    public var confirmMarkAllRead = true
    public var timelineLines = 2
    public var showFeedIcons = false
    public var feedsUnreadOnly = false
    public var articleTheme: RSSArticleTheme = .standard
    public var articleFont: RSSArticleFont = .sans
    public var articleSize = 17
    public var openLinksInApp = true
    public var loadRemoteImages = true
    public var palette: RSSColorPalette = .automatic
    public var showThumbnails = false
    public var hideReadArticles = false
    public var foldDuplicates = false
    public var markAllKeepsStarred = false
    public var sortFeedsByUnread = false

    public init() {}

    enum CodingKeys: String, CodingKey {
        case sortOldestFirst, groupByFeed, refreshClearsRead, confirmMarkAllRead, timelineLines, showFeedIcons
        case feedsUnreadOnly, articleTheme, articleFont, articleSize, openLinksInApp, loadRemoteImages, palette
        case showThumbnails, hideReadArticles, foldDuplicates, markAllKeepsStarred, sortFeedsByUnread
    }

    public func hidesRead(_ scope: RSSScope) -> Bool {
        if scope.unreadOnly { return true }
        return hideReadArticles && scope != .starred
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let base = RSSReaderSettings()
        sortOldestFirst = (try? c.decodeIfPresent(Bool.self, forKey: .sortOldestFirst)) ?? base.sortOldestFirst
        groupByFeed = (try? c.decodeIfPresent(Bool.self, forKey: .groupByFeed)) ?? base.groupByFeed
        refreshClearsRead = (try? c.decodeIfPresent(Bool.self, forKey: .refreshClearsRead)) ?? base.refreshClearsRead
        confirmMarkAllRead = (try? c.decodeIfPresent(Bool.self, forKey: .confirmMarkAllRead)) ?? base.confirmMarkAllRead
        timelineLines = min(4, max(0, (try? c.decodeIfPresent(Int.self, forKey: .timelineLines)) ?? base.timelineLines))
        showFeedIcons = (try? c.decodeIfPresent(Bool.self, forKey: .showFeedIcons)) ?? base.showFeedIcons
        feedsUnreadOnly = (try? c.decodeIfPresent(Bool.self, forKey: .feedsUnreadOnly)) ?? base.feedsUnreadOnly
        articleTheme = (try? c.decodeIfPresent(RSSArticleTheme.self, forKey: .articleTheme)) ?? base.articleTheme
        articleFont = (try? c.decodeIfPresent(RSSArticleFont.self, forKey: .articleFont)) ?? base.articleFont
        let size = (try? c.decodeIfPresent(Int.self, forKey: .articleSize)) ?? base.articleSize
        articleSize = min(RSSReaderSettings.textSizes.upperBound, max(RSSReaderSettings.textSizes.lowerBound, size))
        openLinksInApp = (try? c.decodeIfPresent(Bool.self, forKey: .openLinksInApp)) ?? base.openLinksInApp
        loadRemoteImages = (try? c.decodeIfPresent(Bool.self, forKey: .loadRemoteImages)) ?? base.loadRemoteImages
        palette = (try? c.decodeIfPresent(RSSColorPalette.self, forKey: .palette)) ?? base.palette
        showThumbnails = (try? c.decodeIfPresent(Bool.self, forKey: .showThumbnails)) ?? base.showThumbnails
        hideReadArticles = (try? c.decodeIfPresent(Bool.self, forKey: .hideReadArticles)) ?? base.hideReadArticles
        foldDuplicates = (try? c.decodeIfPresent(Bool.self, forKey: .foldDuplicates)) ?? base.foldDuplicates
        markAllKeepsStarred = (try? c.decodeIfPresent(Bool.self, forKey: .markAllKeepsStarred)) ?? base.markAllKeepsStarred
        sortFeedsByUnread = (try? c.decodeIfPresent(Bool.self, forKey: .sortFeedsByUnread)) ?? base.sortFeedsByUnread
    }
}

public struct RSSActivity: Codable, Equatable, Sendable, Identifiable {
    public var at: Int64
    public var kind: String
    public var text: String

    public var id: String { "\(at)-\(kind)-\(RSSText.fnv(text))" }

    public init(at: Int64, kind: String, text: String) {
        self.at = at
        self.kind = kind
        self.text = text
    }
}

public struct RSSReaderLibrary: Codable, Equatable, Sendable {
    public var version = 1
    public var folders: [RSSFolder] = []
    public var feeds: [RSSFeed] = []
    public var settings = RSSReaderSettings()
    public var activity: [RSSActivity] = []
    public var collapsed: [String] = []
    public var lastRefreshAt: Int64 = 0
    public var filters: [RSSFilter] = []
    public var smartFeeds: [RSSSmartFeed] = []

    public init() {}

    enum CodingKeys: String, CodingKey { case version, folders, feeds, settings, activity, collapsed, lastRefreshAt, filters, smartFeeds }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        version = (try? c.decodeIfPresent(Int.self, forKey: .version)) ?? 1
        folders = (try? c.decodeIfPresent([RSSFolder].self, forKey: .folders)) ?? []
        feeds = (try? c.decodeIfPresent([RSSFeed].self, forKey: .feeds)) ?? []
        settings = (try? c.decodeIfPresent(RSSReaderSettings.self, forKey: .settings)) ?? RSSReaderSettings()
        activity = (try? c.decodeIfPresent([RSSActivity].self, forKey: .activity)) ?? []
        collapsed = (try? c.decodeIfPresent([String].self, forKey: .collapsed)) ?? []
        lastRefreshAt = (try? c.decodeIfPresent(Int64.self, forKey: .lastRefreshAt)) ?? 0
        filters = (try? c.decodeIfPresent([RSSFilter].self, forKey: .filters)) ?? []
        smartFeeds = (try? c.decodeIfPresent([RSSSmartFeed].self, forKey: .smartFeeds)) ?? []
    }
}

public enum RSSFilterField: String, Codable, CaseIterable, Sendable {
    case title
    case titleOrText
    case author

    public var title: String {
        switch self {
        case .title: return "Title"
        case .titleOrText: return "Title or Text"
        case .author: return "Author"
        }
    }
}

public enum RSSFilterAction: String, Codable, CaseIterable, Sendable {
    case hide
    case markRead

    public var title: String {
        switch self {
        case .hide: return "Hide"
        case .markRead: return "Mark as Read"
        }
    }
}

public struct RSSFilter: Codable, Equatable, Hashable, Sendable, Identifiable {
    public var id: String
    public var feedId: String = ""
    public var field: RSSFilterField = .title
    public var pattern: String = ""
    public var regex = false
    public var action: RSSFilterAction = .hide

    public init(
        id: String, feedId: String = "", field: RSSFilterField = .title, pattern: String = "", regex: Bool = false,
        action: RSSFilterAction = .hide
    ) {
        self.id = id
        self.feedId = feedId
        self.field = field
        self.pattern = pattern
        self.regex = regex
        self.action = action
    }

    enum CodingKeys: String, CodingKey { case id, feedId, field, pattern, regex, action }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = try c.decode(String.self, forKey: .id)
        feedId = (try? c.decodeIfPresent(String.self, forKey: .feedId)) ?? ""
        field = (try? c.decodeIfPresent(RSSFilterField.self, forKey: .field)) ?? .title
        pattern = (try? c.decodeIfPresent(String.self, forKey: .pattern)) ?? ""
        regex = (try? c.decodeIfPresent(Bool.self, forKey: .regex)) ?? false
        action = (try? c.decodeIfPresent(RSSFilterAction.self, forKey: .action)) ?? .hide
    }

    public func applies(to feedId: String) -> Bool { self.feedId.isEmpty || self.feedId == feedId }
}

public struct RSSSmartFeed: Codable, Equatable, Hashable, Sendable, Identifiable {
    public var id: String
    public var name: String = ""
    public var words: String = ""
    public var regex = false
    public var scope: String = ""
    public var unreadOnly = false
    public var starredOnly = false
    public var maxAgeDays = 0

    public init(
        id: String, name: String = "", words: String = "", regex: Bool = false, scope: String = "", unreadOnly: Bool = false,
        starredOnly: Bool = false, maxAgeDays: Int = 0
    ) {
        self.id = id
        self.name = name
        self.words = words
        self.regex = regex
        self.scope = scope
        self.unreadOnly = unreadOnly
        self.starredOnly = starredOnly
        self.maxAgeDays = maxAgeDays
    }

    enum CodingKeys: String, CodingKey { case id, name, words, regex, scope, unreadOnly, starredOnly, maxAgeDays }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = try c.decode(String.self, forKey: .id)
        name = (try? c.decodeIfPresent(String.self, forKey: .name)) ?? ""
        words = (try? c.decodeIfPresent(String.self, forKey: .words)) ?? ""
        regex = (try? c.decodeIfPresent(Bool.self, forKey: .regex)) ?? false
        scope = (try? c.decodeIfPresent(String.self, forKey: .scope)) ?? ""
        unreadOnly = (try? c.decodeIfPresent(Bool.self, forKey: .unreadOnly)) ?? false
        starredOnly = (try? c.decodeIfPresent(Bool.self, forKey: .starredOnly)) ?? false
        maxAgeDays = max(0, (try? c.decodeIfPresent(Int.self, forKey: .maxAgeDays)) ?? 0)
    }
}

public enum RSSMatcher {
    private static let cache = RSSRegexCache()

    public static func expression(_ pattern: String) -> NSRegularExpression? {
        cache.expression(pattern)
    }

    public static func isValidRegex(_ pattern: String) -> Bool { expression(pattern) != nil }

    public static func matches(_ value: String, pattern: String, regex: Bool) -> Bool {
        guard !pattern.isEmpty, !value.isEmpty else { return false }
        if regex {
            guard let expression = expression(pattern) else { return false }
            return expression.firstMatch(in: value, range: NSRange(value.startIndex..., in: value)) != nil
        }
        return value.range(of: pattern, options: [.caseInsensitive]) != nil
    }

    public static func matches(_ filter: RSSFilter, title: String, author: String, text: () -> String) -> Bool {
        switch filter.field {
        case .title: return matches(title, pattern: filter.pattern, regex: filter.regex)
        case .author: return matches(author, pattern: filter.pattern, regex: filter.regex)
        case .titleOrText:
            return matches(title, pattern: filter.pattern, regex: filter.regex) || matches(text(), pattern: filter.pattern, regex: filter.regex)
        }
    }
}

final class RSSRegexCache: @unchecked Sendable {
    private let lock = NSLock()
    private var compiled: [String: NSRegularExpression] = [:]
    private var invalid: Set<String> = []

    func expression(_ pattern: String) -> NSRegularExpression? {
        lock.lock()
        defer { lock.unlock() }
        if let found = compiled[pattern] { return found }
        if invalid.contains(pattern) { return nil }
        guard let made = try? NSRegularExpression(pattern: pattern, options: [.caseInsensitive]) else {
            if invalid.count > 500 { invalid.removeAll() }
            invalid.insert(pattern)
            return nil
        }
        if compiled.count > 500 { compiled.removeAll() }
        compiled[pattern] = made
        return made
    }
}

public enum RSSCatchUp: Int, CaseIterable, Sendable, Identifiable {
    case day = 1
    case threeDays = 3
    case week = 7
    case twoWeeks = 14
    case month = 30

    public var id: Int { rawValue }

    public var days: Int { rawValue }

    public var title: String {
        switch self {
        case .day: return "Older Than 1 Day"
        case .threeDays: return "Older Than 3 Days"
        case .week: return "Older Than 1 Week"
        case .twoWeeks: return "Older Than 2 Weeks"
        case .month: return "Older Than 1 Month"
        }
    }

    public var phrase: String {
        switch self {
        case .day: return "older than 1 day"
        case .threeDays: return "older than 3 days"
        case .week: return "older than 1 week"
        case .twoWeeks: return "older than 2 weeks"
        case .month: return "older than 1 month"
        }
    }
}

public enum RSSScope: Hashable, Sendable, Codable {
    case today
    case unread
    case starred
    case readLater
    case smart(String)
    case feed(String)
    case folder(String)

    public var key: String {
        switch self {
        case .today: return "today"
        case .unread: return "unread"
        case .starred: return "starred"
        case .readLater: return "readLater"
        case .smart(let id): return "smart:\(id)"
        case .feed(let id): return "feed:\(id)"
        case .folder(let id): return "folder:\(id)"
        }
    }

    public var isSmart: Bool {
        switch self {
        case .today, .unread, .starred, .readLater, .smart: return true
        default: return false
        }
    }

    public var unreadOnly: Bool { self == .today || self == .unread }
}

public struct RSSCounts: Equatable, Sendable {
    public var today = 0
    public var unread = 0
    public var starred = 0
    public var readLater = 0
    public var readLaterTotal = 0
    public var byFeed: [String: Int] = [:]
    public var bySmartFeed: [String: Int] = [:]

    public init() {}
}

public struct RSSStats: Equatable, Sendable {
    public var feeds = 0
    public var folders = 0
    public var articles = 0
    public var unread = 0
    public var starred = 0
    public var failingFeeds = 0
    public var oldestArticle: Int64 = 0
    public var lastRefresh: Int64 = 0
}

public enum RSSSidebarEntry: Equatable, Sendable, Identifiable {
    case feed(RSSFeed, unread: Int, indented: Bool)
    case folder(RSSFolder, unread: Int, collapsed: Bool)

    public var id: String {
        switch self {
        case .feed(let feed, _, _): return "feed:\(feed.id)"
        case .folder(let folder, _, _): return "folder:\(folder.id)"
        }
    }

    public var scope: RSSScope {
        switch self {
        case .feed(let feed, _, _): return .feed(feed.id)
        case .folder(let folder, _, _): return .folder(folder.id)
        }
    }
}

public enum RSSReaderError: Error, Equatable, Sendable {
    case invalidAddress
    case tooManyFeeds
    case tooManyFolders
    case duplicate(String)
    case noFeedFound
    case emptyName
    case unreadableOPML
    case tooManyFilters
    case tooManySmartFeeds
    case invalidRegex
    case emptyPattern

    public var message: String {
        switch self {
        case .invalidAddress: return "Enter a web or feed address, such as example.org or https://example.org/feed.xml."
        case .tooManyFeeds: return "You can follow up to \(RSSReaderLimits.maxFeeds) feeds."
        case .tooManyFolders: return "You can have up to \(RSSReaderLimits.maxFolders) folders."
        case .duplicate: return "You already follow this feed."
        case .noFeedFound: return "No feed was found at that address."
        case .emptyName: return "Enter a name."
        case .unreadableOPML: return "That file is not an OPML subscription list."
        case .tooManyFilters: return "You can have up to \(RSSReaderLimits.maxFilters) filters."
        case .tooManySmartFeeds: return "You can have up to \(RSSReaderLimits.maxSmartFeeds) smart feeds."
        case .invalidRegex: return "Invalid regular expression"
        case .emptyPattern: return "Enter the text to match."
        }
    }
}
