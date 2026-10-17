import FMBase
import Foundation

public struct MarketingChannel: Equatable, Sendable, Identifiable {
    public var id: String
    public var name: String
    public var category: String

    public init(id: String, name: String, category: String = "") {
        self.id = id
        self.name = name
        self.category = category
    }
}

public struct MarketingContent: Equatable, Sendable, Identifiable {
    public var id: String
    public var channel: String
    public var status: String
    public var title: String
    public var body: String
    public var campaign: String
    public var owner: String
    public var url: String
    public var tags: [String]
    public var updatedAt: Int64
    public var scheduledAt: Int64
    public var publishedAt: Int64
    public var json: JSONObject

    public init(json: JSONObject) {
        self.json = json
        id = json.str("id")
        channel = json.str("channel", "blog")
        status = json.str("status", "draft")
        title = json.str("title")
        body = json.str("body")
        campaign = json.str("campaign")
        owner = json.str("owner")
        url = json.str("url")
        tags = json.strings("tags")
        updatedAt = json.int64("updatedAt")
        scheduledAt = json.int64("scheduledAt")
        publishedAt = json.int64("publishedAt")
    }

    public static let statuses = ["draft", "review", "queued", "scheduled", "published", "archived"]

    public var nextStatuses: [String] {
        MarketingContent.statuses.filter { $0 != status }
    }
}

public struct MarketingCampaignAd: Equatable, Sendable, Identifiable {
    public var id: String
    public var channel: String
    public var status: String
    public var name: String
    public var campaign: String
    public var headline: String
    public var platform: String
    public var currency: String
    public var budgetCents: Int64
    public var spendCents: Int64
    public var impressions: Int64
    public var clicks: Int64
    public var conversions: Int64
    public var pacing: String
    public var startsAt: Int64
    public var endsAt: Int64
    public var managerUrl: String
    public var platformUrl: String
    public var linkChecks: [String]
    public var json: JSONObject

    public init(json: JSONObject) {
        self.json = json
        id = json.str("id")
        channel = json.str("channel")
        status = json.str("status", "draft")
        name = json.str("name")
        campaign = json.str("campaign")
        headline = json.str("headline")
        platform = json.str("platform")
        currency = json.str("currency")
        budgetCents = json.int64("budgetCents")
        spendCents = json.int64("spendCents")
        impressions = json.int64("impressions")
        clicks = json.int64("clicks")
        conversions = json.int64("conversions")
        pacing = json.obj("pacing").str("state")
        startsAt = json.int64("startsAt")
        endsAt = json.int64("endsAt")
        managerUrl = json.str("managerUrl")
        platformUrl = json.str("platformUrl")
        linkChecks = json.maps("linkChecks").map { $0.str("message") }.filter { !$0.isEmpty }
    }

    public static let statuses = ["draft", "review", "scheduled", "active", "paused", "completed", "archived"]

    public var consoleUrl: String { managerUrl.isEmpty ? platformUrl : managerUrl }

    public var needsAttention: Bool { ["over", "under", "overspent"].contains(pacing) }

    public var clickRate: Double { impressions > 0 ? Double(clicks) / Double(impressions) : 0 }

    public var spendLabel: String {
        MoneyText.cents(spendCents, currency) + " of " + MoneyText.cents(budgetCents, currency)
    }
}

public struct MarketingHealthCheck: Equatable, Sendable, Identifiable {
    public var id: String
    public var label: String
    public var ok: Bool
}

public struct MarketingChannelStats: Equatable, Sendable, Identifiable {
    public var id: String
    public var reach: Int64
    public var followers: Int64
    public var impressions: Int64
    public var engagements: Int64
    public var clicks: Int64
    public var posts: Int64 = 0
    public var views: Int64 = 0
    public var reviews: Int64 = 0
    public var visits: Int64 = 0
    public var indexedPages: Int64 = 0
    public var outbound: Int64 = 0
    public var replies: Int64 = 0
    public var closedDeals: Int64 = 0
    public var closedValue: Int64 = 0
    public var stars: Int64 = 0
    public var forks: Int64 = 0
    public var downloads: Int64 = 0
    public var redownloads: Int64 = 0
    public var updates: Int64 = 0
    public var ratings: Int64 = 0
    public var rating: Double = 0
    public var health: String = ""
    public var checks: [MarketingHealthCheck] = []
    public var observedAt: Int64 = 0
    public var source: String = ""
    public var followersChange: Int64?
    public var starsChange: Int64?
    public var engagementRate: Double?
    public var postsLast30d: Int64 = 0
    public var lastPostAt: Int64 = 0
    public var profileUrl: String = ""

    public init(id: String, reach: Int64, followers: Int64, impressions: Int64, engagements: Int64, clicks: Int64) {
        self.id = id
        self.reach = reach
        self.followers = followers
        self.impressions = impressions
        self.engagements = engagements
        self.clicks = clicks
    }

    public init(json: JSONObject, settings: JSONObject) {
        self.init(
            id: json.str("channel"), reach: json.int64("reach"), followers: json.int64("followers"),
            impressions: json.int64("impressions"), engagements: json.int64("engagements"), clicks: json.int64("clicks"))
        posts = json.int64("posts")
        views = json.int64("views")
        reviews = json.int64("reviews")
        visits = json.int64("visits")
        indexedPages = json.int64("indexedPages")
        outbound = json.int64("outbound")
        replies = json.int64("replies")
        closedDeals = json.int64("closedDeals")
        closedValue = json.int64("closedValue")
        stars = json.int64("stars")
        forks = json.int64("forks")
        downloads = json.int64("downloads")
        redownloads = json.int64("redownloads")
        updates = json.int64("updates")
        ratings = json.int64("ratings")
        rating = json.double("rating")
        let health = json.obj("health")
        self.health = health.str("state")
        checks = health.maps("checks").map { MarketingHealthCheck(id: $0.str("id"), label: $0.str("label"), ok: $0.bool("ok")) }
        observedAt = health.int64("observedAt")
        source = health.str("source")
        followersChange = health["followersChange"]?.int64Value
        starsChange = health["starsChange"]?.int64Value
        engagementRate = health["engagementRate"]?.doubleValue
        postsLast30d = health.int64("postsLast30d")
        lastPostAt = health.int64("lastPostAt")
        profileUrl = settings.maps("channels").first { $0.str("id") == id }?.str("profileUrl") ?? ""
    }

    public static let healthTitles = [
        "good": "On track", "attention": "Needs attention", "stale": "Stale numbers", "setup": "Not set up", "paused": "Paused",
    ]

    public var healthTitle: String { Self.healthTitles[health] ?? "" }

    public var canCapture: Bool { profileUrl.hasPrefix("https://") && !isEmail }

    public var isGoogle: Bool { id == "google-search" }

    public var isEmail: Bool { id == "email" }

    public var isGitHub: Bool { id == "github" }

    public var isAppStore: Bool { id == MarketingChannelStats.appStoreChannel }

    public static let appStoreChannel = "app-store"

    public var missingChecks: [String] { checks.filter { !$0.ok }.map(\.label) }

    public func captureURL(organization: String) -> URL? {
        var components = URLComponents(string: "https://app.forkmesh.com/dashboard/marketing")
        components?.queryItems = [
            URLQueryItem(name: "org", value: organization), URLQueryItem(name: "page", value: "channels"),
            URLQueryItem(name: "capture", value: id),
        ]
        return components?.url
    }
}

public struct MarketingOverview: Equatable, Sendable {
    public var organization: String
    public var role: String
    public var content: [MarketingContent]
    public var ads: [MarketingCampaignAd]
    public var catalog: [MarketingChannel]
    public var channels: [MarketingChannelStats]
    public var published: Int
    public var queued: Int
    public var reach: Int64 = 0
    public var followers: Int64 = 0
    public var engagements: Int64 = 0
    public var healthCounts: [String: Int] = [:]
    public var canManageSettings = false
    public var assistantAvailable = false
    public var settings: JSONObject = [:]
    public var channelSettings: [MarketingChannelSetting] = []
    public var refreshable: Set<String> = []
    public var playbooks: JSONObject = [:]
    public var campaigns: [MarketingRecord] = []
    public var campaignTemplates: [JSONObject] = []
    public var experiments: [MarketingRecord] = []
    public var experimentsRunning = 0
    public var experimentsRunningLimit = 3
    public var funnel: [MarketingRecord] = []
    public var funnelReport = MarketingFunnelReport(json: [:])
    public var keywords: [MarketingRecord] = []
    public var redditThreads: [MarketingRecord] = []
    public var redditSubreddits: [MarketingRedditSubreddit] = []
    public var redditQueries: [MarketingNamedLink] = []
    public var redditAccounts: [MarketingNamedLink] = []
    public var redditSummary: JSONObject = [:]
    public var posts: [MarketingRecord] = []
    public var adTotals: [MarketingAdTotals] = []
    public var adsNeedingAttention = 0
    public var adPlatforms: [MarketingChannel] = []
    public var suggestions: [MarketingSuggestion] = []
    public var x = MarketingXStatus(json: [:])
    public var readyX = MarketingReadyState(json: [:])

    public init(json: JSONObject) {
        organization = json.str("organization")
        role = json.str("role", "member")
        content = json.maps("content").map(MarketingContent.init(json:)).filter { PackagesText.isHexId($0.id) }
        ads = json.maps("ads").map(MarketingCampaignAd.init(json:)).filter { PackagesText.isHexId($0.id) }
        catalog = json.maps("channelCatalog").map {
            MarketingChannel(id: $0.str("id"), name: $0.str("name"), category: $0.str("category"))
        }
        let summary = json.obj("summary")
        let settings = json.obj("settings")
        channels = summary.maps("channels").map { MarketingChannelStats(json: $0, settings: settings) }
        published = summary.int("published")
        queued = summary.int("queued")
        reach = summary.int64("reach")
        followers = summary.int64("followers")
        engagements = summary.int64("engagements")
        for (state, value) in summary.obj("health") { healthCounts[state] = value.int() }
        canManageSettings = json.bool("canManageSettings")
        assistantAvailable = json.bool("assistantAvailable")
        self.settings = settings
        channelSettings = settings.maps("channels").map(MarketingChannelSetting.init(json:)).filter { !$0.id.isEmpty }
        refreshable = Set(json.strings("refreshableChannels"))
        playbooks = json.obj("channelPlaybooks")
        func records(_ key: String, _ kind: MarketingKind) -> [MarketingRecord] {
            json.maps(key).map { MarketingRecord(kind: kind, json: $0) }.filter { PackagesText.isHexId($0.id) }
        }
        campaigns = records("campaigns", .campaign)
        campaignTemplates = json.maps("campaignTemplates")
        experiments = records("experiments", .experiment)
        let experimentsSummary = json.obj("experimentsSummary")
        experimentsRunning = experimentsSummary.int("running")
        experimentsRunningLimit = experimentsSummary.int("runningLimit", 3)
        funnel = records("funnel", .funnelWeek)
        funnelReport = MarketingFunnelReport(json: json.obj("funnelReport"))
        keywords = records("keywords", .keyword)
        redditThreads = records("redditThreads", .redditThread)
        let desk = json.obj("redditDesk")
        redditSubreddits = desk.maps("subreddits").map(MarketingRedditSubreddit.init(json:))
        redditQueries = desk.maps("queries").map { MarketingNamedLink(id: $0.str("query"), name: $0.str("query"), url: $0.str("url")) }
        redditAccounts = desk.maps("accounts").map { MarketingNamedLink(id: $0.str("name"), name: $0.str("name"), url: $0.str("url")) }
            .filter { !$0.name.isEmpty }
        redditSummary = desk.obj("summary")
        posts = records("posts", .post)
        let adsSummary = json.obj("adsSummary")
        adTotals = adsSummary.maps("currencies").map(MarketingAdTotals.init(json:))
        adsNeedingAttention = adsSummary.int("attention")
        adPlatforms = json.maps("adPlatforms").map { MarketingChannel(id: $0.str("channel"), name: $0.str("name")) }
        suggestions = json.maps("suggestions").map(MarketingSuggestion.init(json:))
        x = MarketingXStatus(json: settings.obj("x"))
        readyX = MarketingReadyState(json: json.obj("readyPosts").obj("x"))
    }

    public static func == (lhs: MarketingOverview, rhs: MarketingOverview) -> Bool {
        lhs.organization == rhs.organization && lhs.role == rhs.role && lhs.content == rhs.content && lhs.ads == rhs.ads
            && lhs.channels == rhs.channels && lhs.settings == rhs.settings && lhs.campaigns == rhs.campaigns
            && lhs.experiments == rhs.experiments && lhs.funnel == rhs.funnel && lhs.keywords == rhs.keywords
            && lhs.redditThreads == rhs.redditThreads && lhs.posts == rhs.posts && lhs.suggestions == rhs.suggestions
            && lhs.redditAccounts == rhs.redditAccounts && lhs.readyX == rhs.readyX
    }

    public var canCloseReadyPost: Bool { ["owner", "admin", "maintainer", "member"].contains(role) }

    public func channelName(_ id: String) -> String {
        if let setting = channelSettings.first(where: { $0.id == id }), !setting.displayName.isEmpty { return setting.displayName }
        return catalog.first { $0.id == id }?.name ?? id
    }

    public func content(status: String?) -> [MarketingContent] {
        guard let status, !status.isEmpty else { return content }
        return content.filter { $0.status == status }
    }

    public func content(query: String, status: String?, channel: String?) -> [MarketingContent] {
        content.filter { item in
            (status ?? "").isEmpty || item.status == status
        }.filter { item in
            (channel ?? "").isEmpty || item.channel == channel
        }.filter { PackagesText.matches(query, $0.title, $0.campaign, $0.owner, $0.body, $0.tags.joined(separator: " ")) }
    }

    public var plan: [(status: String, items: [MarketingContent])] {
        MarketingContent.statuses.filter { $0 != "archived" }.map { status in
            (status: status, items: content.filter { $0.status == status })
        }.filter { !$0.items.isEmpty }
    }

    public var activeAds: [MarketingCampaignAd] {
        ads.filter { $0.status == "active" || $0.status == "scheduled" || $0.status == "paused" }
    }

    public func ads(status: String?, query: String) -> [MarketingCampaignAd] {
        ads.filter { (status ?? "").isEmpty || $0.status == status }
            .filter { PackagesText.matches(query, $0.name, $0.headline, $0.campaign, $0.platform) }
    }

    public var campaignNames: [String] {
        campaigns.filter { !["completed", "archived"].contains($0.status) }.map(\.title).filter { !$0.isEmpty }
    }

    public func campaign(_ id: String) -> MarketingRecord? { campaigns.first { $0.id == id } }

    public func setting(_ id: String) -> MarketingChannelSetting? { channelSettings.first { $0.id == id } }

    public func stats(_ id: String) -> MarketingChannelStats? { channels.first { $0.id == id } }

    public func category(_ id: String) -> String { catalog.first { $0.id == id }?.category ?? "" }

    public func playbook(_ id: String) -> (goal: String, kpis: [String], tips: [String])? {
        let entry = playbooks.obj(category(id))
        if entry.isEmpty { return nil }
        return (entry.str("goal"), entry.strings("kpis"), entry.strings("tips"))
    }

    public var addableChannels: [MarketingChannel] {
        let configured = Set(channelSettings.map(\.id))
        return catalog.filter { !configured.contains($0.id) }
    }

    public var staleChannels: [String] {
        channels.filter { ["stale", "attention"].contains($0.health) && refreshable.contains($0.id) }
            .filter { setting($0.id)?.enabled ?? true }.map(\.id)
    }

    public func posts(channel: String) -> [MarketingRecord] { posts.filter { $0.json.str("channel") == channel } }

    public var openRedditThreads: [MarketingRecord] { redditThreads.filter(\.needsAttention) }

    public var redditQueriesText: String { redditQueries.map(\.name).joined(separator: "\n") }

    public mutating func replace(_ item: MarketingContent) {
        if let index = content.firstIndex(where: { $0.id == item.id }) { content[index] = item }
    }

    public mutating func replace(_ ad: MarketingCampaignAd) {
        if let index = ads.firstIndex(where: { $0.id == ad.id }) { ads[index] = ad }
    }
}

public enum MoneyText {
    public static func cents(_ cents: Int64, _ currency: String) -> String {
        let whole = cents / 100
        let part = abs(cents % 100)
        let amount = "\(whole).\(part < 10 ? "0" : "")\(part)"
        return currency.isEmpty ? amount : amount + " " + currency
    }
}
