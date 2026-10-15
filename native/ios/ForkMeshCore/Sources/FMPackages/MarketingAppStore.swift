import FMBase
import Foundation

public struct MarketingAppStoreApp: Decodable, Equatable, Sendable {
    public var id: String
    public var name: String
    public var bundleId: String

    public init(id: String = "", name: String = "", bundleId: String = "") {
        self.id = id
        self.name = name
        self.bundleId = bundleId
    }

    enum CodingKeys: String, CodingKey { case id, name, bundleId }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = c.lenientString(.id)
        name = c.lenientString(.name)
        bundleId = c.lenientString(.bundleId)
    }
}

public struct MarketingAppStoreLookup: Decodable, Equatable, Sendable {
    public var appId = ""
    public var country = ""
    public var name = ""
    public var seller = ""
    public var version = ""
    public var releasedAt: Int64 = 0
    public var firstReleasedAt: Int64 = 0
    public var genre = ""
    public var price = ""
    public var minimumOs = ""
    public var rating: Double = 0
    public var ratings = 0
    public var versionRating: Double = 0
    public var versionRatings = 0
    public var url = ""
    public var lookedUpAt: Int64 = 0

    public init() {}

    enum CodingKeys: String, CodingKey {
        case appId, country, name, seller, version, releasedAt, firstReleasedAt, genre, price, minimumOs, rating, ratings
        case versionRating, versionRatings, url, lookedUpAt
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        appId = c.lenientString(.appId)
        country = c.lenientString(.country)
        name = c.lenientString(.name)
        seller = c.lenientString(.seller)
        version = c.lenientString(.version)
        releasedAt = c.lenientStamp(.releasedAt)
        firstReleasedAt = c.lenientStamp(.firstReleasedAt)
        genre = c.lenientString(.genre)
        price = c.lenientString(.price)
        minimumOs = c.lenientString(.minimumOs)
        rating = c.marketingDouble(.rating)
        ratings = max(0, c.lenientInt(.ratings))
        versionRating = c.marketingDouble(.versionRating)
        versionRatings = max(0, c.lenientInt(.versionRatings))
        url = c.lenientString(.url)
        lookedUpAt = c.lenientStamp(.lookedUpAt)
    }

    public var isEmpty: Bool { appId.isEmpty && name.isEmpty && version.isEmpty }

    public var link: URL? { url.hasPrefix("https://") ? URL(string: url) : nil }
}

public struct MarketingAppStoreVersion: Decodable, Equatable, Sendable, Identifiable {
    public var version: String
    public var build: String
    public var status: AppStoreState
    public var createdAt: Int64

    enum CodingKeys: String, CodingKey { case version, build, status, createdAt }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        version = c.lenientString(.version)
        build = c.lenientString(.build)
        status = c.lenientState(.status)
        createdAt = c.lenientStamp(.createdAt)
    }

    public var id: String { version + "(" + build + ")" }

    public var title: String { build.isEmpty ? version : version + " (" + build + ")" }
}

public struct MarketingAppStoreReview: Decodable, Equatable, Sendable {
    public var createdAt: Int64
    public var rating: Int
    public var title: String
    public var territory: String
    public var body: String

    enum CodingKeys: String, CodingKey { case createdAt, rating, title, territory, body }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        createdAt = c.lenientStamp(.createdAt)
        rating = min(max(c.lenientInt(.rating), 0), 5)
        title = c.lenientString(.title)
        territory = c.lenientString(.territory)
        body = c.lenientString(.body)
    }

    public var stars: String { AppStoreStatusText.stars(rating) }
}

public struct MarketingAppStoreCounts: Decodable, Equatable, Sendable {
    public var downloads: Int
    public var redownloads: Int
    public var updates: Int

    public init(downloads: Int = 0, redownloads: Int = 0, updates: Int = 0) {
        self.downloads = downloads
        self.redownloads = redownloads
        self.updates = updates
    }

    enum CodingKeys: String, CodingKey { case downloads, redownloads, updates }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        downloads = max(0, c.lenientInt(.downloads))
        redownloads = max(0, c.lenientInt(.redownloads))
        updates = max(0, c.lenientInt(.updates))
    }
}

public struct MarketingAppStoreDay: Decodable, Equatable, Sendable, Identifiable {
    public var day: String
    public var downloads: Int
    public var redownloads: Int
    public var updates: Int

    public init(day: String, downloads: Int = 0, redownloads: Int = 0, updates: Int = 0) {
        self.day = day
        self.downloads = downloads
        self.redownloads = redownloads
        self.updates = updates
    }

    enum CodingKeys: String, CodingKey { case day, downloads, redownloads, updates }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        day = c.lenientString(.day)
        downloads = max(0, c.lenientInt(.downloads))
        redownloads = max(0, c.lenientInt(.redownloads))
        updates = max(0, c.lenientInt(.updates))
    }

    public var id: String { day }

    public var date: Date? { MarketingAppStore.date(day) }
}

public struct MarketingAppStoreBestDay: Decodable, Equatable, Sendable {
    public var day: String
    public var downloads: Int

    enum CodingKeys: String, CodingKey { case day, downloads }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        day = c.lenientString(.day)
        downloads = max(0, c.lenientInt(.downloads))
    }
}

public struct MarketingAppStoreShare: Decodable, Equatable, Sendable, Identifiable {
    public var name: String
    public var downloads: Int

    enum CodingKeys: String, CodingKey { case code, name, downloads }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let code = c.lenientString(.code)
        name = code.isEmpty ? c.lenientString(.name) : code
        downloads = max(0, c.lenientInt(.downloads))
    }

    public var id: String { name }

    public var countryName: String {
        guard name.count == 2 else { return name }
        return Locale(identifier: "en_US").localizedString(forRegionCode: name) ?? name
    }
}

public struct MarketingAppStoreWindow: Decodable, Equatable, Sendable, Identifiable {
    public var days: Int
    public var from: String
    public var to: String
    public var downloads: Int
    public var redownloads: Int
    public var updates: Int
    public var previous: MarketingAppStoreCounts
    public var comparable: Bool
    public var territories: [MarketingAppStoreShare]
    public var territoryCount: Int
    public var devices: [MarketingAppStoreShare]

    enum CodingKeys: String, CodingKey {
        case days, from, to, downloads, redownloads, updates, previous, comparable, territories, territoryCount, devices
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        days = c.lenientInt(.days)
        from = c.lenientString(.from)
        to = c.lenientString(.to)
        downloads = max(0, c.lenientInt(.downloads))
        redownloads = max(0, c.lenientInt(.redownloads))
        updates = max(0, c.lenientInt(.updates))
        previous = (try? c.decodeIfPresent(MarketingAppStoreCounts.self, forKey: .previous)) ?? MarketingAppStoreCounts()
        comparable = c.lenientBool(.comparable)
        territories = c.lenientList(.territories)
        territoryCount = max(c.lenientInt(.territoryCount), territories.count)
        devices = c.lenientList(.devices)
    }

    public var id: Int { days }

    public var downloadsChange: Double? {
        MarketingAppStore.percentChange(downloads, previous.downloads, comparable: comparable)
    }

    public var downloadsChangeText: String {
        guard let change = downloadsChange else { return "" }
        return (change >= 0 ? "+" : "") + MarketingText.percent(change)
    }
}

public struct MarketingAppStore: Decodable, Equatable, Sendable {
    public static let ranges = [30, 90, 365]
    public static let emptyText =
        "No downloads yet. Run native/ios/tools/asc_store_report.py --publish on the computer that holds the App Store Connect key; it sends each day's downloads here."

    public var app = MarketingAppStoreApp()
    public var lookup = MarketingAppStoreLookup()
    public var versions: [MarketingAppStoreVersion] = []
    public var reviews: [MarketingAppStoreReview] = []
    public var publishedAt: Int64 = 0
    public var publishedBy = ""
    public var fresh = false
    public var firstDay = ""
    public var lastDay = ""
    public var days: [MarketingAppStoreDay] = []
    public var total = MarketingAppStoreCounts()
    public var bestDay: MarketingAppStoreBestDay?
    public var windows: [MarketingAppStoreWindow] = []

    public init() {}

    enum CodingKeys: String, CodingKey {
        case app, lookup, versions, reviews, publishedAt, publishedBy, fresh, firstDay, lastDay, days, total, bestDay, windows
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        app = (try? c.decodeIfPresent(MarketingAppStoreApp.self, forKey: .app)) ?? MarketingAppStoreApp()
        lookup = (try? c.decodeIfPresent(MarketingAppStoreLookup.self, forKey: .lookup)) ?? MarketingAppStoreLookup()
        versions = c.lenientList(.versions)
        reviews = c.lenientList(.reviews)
        publishedAt = c.lenientStamp(.publishedAt)
        publishedBy = c.lenientString(.publishedBy)
        fresh = c.lenientBool(.fresh)
        firstDay = c.lenientString(.firstDay)
        lastDay = c.lenientString(.lastDay)
        days = (c.lenientList(.days) as [MarketingAppStoreDay]).filter { Self.date($0.day) != nil }.sorted { $0.day < $1.day }
        total = (try? c.decodeIfPresent(MarketingAppStoreCounts.self, forKey: .total)) ?? MarketingAppStoreCounts()
        bestDay = (try? c.decodeIfPresent(MarketingAppStoreBestDay.self, forKey: .bestDay)).flatMap { $0.day.isEmpty ? nil : $0 }
        windows = c.lenientList(.windows)
    }

    public static func decode(_ object: JSONObject) throws -> MarketingAppStore {
        try JSONDecoder().decode(MarketingAppStore.self, from: JSON.encodeData(.object(object)))
    }

    public var isEmpty: Bool { lastDay.isEmpty }

    public func window(days: Int) -> MarketingAppStoreWindow? { windows.first { $0.days == days } }

    public func chartDays(range: Int) -> [MarketingAppStoreDay] {
        guard !lastDay.isEmpty, range > 0, let start = Self.shift(lastDay, by: 1 - range) else { return [] }
        return days.filter { $0.day >= start && $0.day <= lastDay }
    }

    public var ratingText: String { Self.ratingText(lookup.rating) }

    public var appName: String { lookup.name.isEmpty ? app.name : lookup.name }

    public func sentLine(now: Int64) -> String {
        guard publishedAt > 0 else { return "" }
        let by = publishedBy.isEmpty ? "" : " by " + publishedBy
        return "Sent from App Store Connect " + Format.relativeTimeFrom(publishedAt, nowMillis: now) + by
    }

    public static func ratingText(_ rating: Double) -> String {
        guard rating > 0 else { return "" }
        return String(format: "%.1f\u{2605}", rating)
    }

    public static func percentChange(_ current: Int, _ previous: Int, comparable: Bool) -> Double? {
        guard comparable, previous > 0 else { return nil }
        return (Double(current) - Double(previous)) / Double(previous) * 100
    }

    public static func rangeTitle(_ days: Int) -> String { "\(days)D" }

    static let dayFormatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.timeZone = TimeZone(identifier: "UTC")
        formatter.calendar = Calendar(identifier: .gregorian)
        formatter.dateFormat = "yyyy-MM-dd"
        return formatter
    }()

    public static func date(_ day: String) -> Date? {
        guard day.count == 10 else { return nil }
        return dayFormatter.date(from: day)
    }

    public static func shift(_ day: String, by delta: Int) -> String? {
        guard let date = date(day) else { return nil }
        return dayFormatter.string(from: date.addingTimeInterval(TimeInterval(delta) * 86_400))
    }
}

public struct MarketingAppStoreReply: Equatable, Sendable {
    public var organization: String
    public var canPublish: Bool
    public var appStore: MarketingAppStore

    public init(organization: String, json: JSONObject) {
        self.organization = organization
        canPublish = json.bool("canPublish")
        appStore = (try? MarketingAppStore.decode(json.obj("appStore"))) ?? MarketingAppStore()
    }
}

extension KeyedDecodingContainer {
    func marketingDouble(_ key: Key) -> Double {
        if let number = try? decodeIfPresent(Double.self, forKey: key), number.isFinite { return number }
        if let text = try? decodeIfPresent(String.self, forKey: key), let number = Double(text), number.isFinite { return number }
        return 0
    }
}
