import FMBase
import Foundation

public enum MarketingKind: String, CaseIterable, Sendable {
    case content
    case campaign
    case ad
    case adResults
    case experiment
    case funnelWeek
    case keyword
    case redditThread
    case post
    case metric
    case channel
    case attribution
    case targets
    case goals
    case redditWatch
    case xConnection
    case xPost
    case link

    public var resource: String {
        switch self {
        case .content: return "content"
        case .campaign: return "campaigns"
        case .ad, .adResults: return "ads"
        case .experiment: return "experiments"
        case .funnelWeek: return "funnel"
        case .keyword: return "keywords"
        case .redditThread: return "reddit"
        case .post: return "posts"
        case .metric: return "metrics"
        case .channel, .attribution, .targets, .goals, .redditWatch: return "settings"
        case .xConnection: return "x-connection"
        case .xPost: return "x-post"
        case .link: return "links"
        }
    }

    public var replyKey: String {
        switch self {
        case .content: return "content"
        case .campaign: return "campaign"
        case .ad, .adResults: return "ad"
        case .experiment: return "experiment"
        case .keyword: return "keyword"
        case .redditThread: return "thread"
        case .post: return "post"
        case .metric: return "metric"
        default: return ""
        }
    }

    public var title: String {
        switch self {
        case .content: return "Content"
        case .campaign: return "Campaign"
        case .ad: return "Ad"
        case .adResults: return "Ad results"
        case .experiment: return "Experiment"
        case .funnelWeek: return "Funnel week"
        case .keyword: return "Keyword"
        case .redditThread: return "Reddit thread"
        case .post: return "Posted link"
        case .metric: return "Channel numbers"
        case .channel: return "Channel"
        case .attribution: return "Link tracking"
        case .targets: return "Funnel targets"
        case .goals: return "Conversion goals"
        case .redditWatch: return "Reddit watch list"
        case .xConnection: return "Connect X"
        case .xPost: return "Post to X"
        case .link: return "Tagged link"
        }
    }

    public var statuses: [String] {
        switch self {
        case .content: return MarketingContent.statuses
        case .campaign: return ["idea", "planned", "active", "paused", "completed", "archived"]
        case .ad: return MarketingCampaignAd.statuses
        case .experiment: return ["idea", "planned", "running", "review", "done", "dropped"]
        case .redditThread: return ["to_review", "drafting", "replied", "skipped"]
        default: return []
        }
    }

    public var deletable: Bool {
        [.content, .campaign, .ad, .experiment, .funnelWeek, .keyword, .redditThread, .post].contains(self)
    }

    public var managersOnly: Bool {
        [.channel, .attribution, .targets, .goals, .redditWatch, .xConnection, .xPost].contains(self)
    }
}

public enum MarketingText {
    public static func title(_ value: String) -> String {
        if value.isEmpty { return "None" }
        return value.split(whereSeparator: { $0 == "-" || $0 == "_" }).map { $0.prefix(1).uppercased() + $0.dropFirst() }
            .joined(separator: " ")
    }

    public static func count(_ value: Int64) -> String {
        let magnitude = abs(value)
        let sign = value < 0 ? "-" : ""
        if magnitude >= 1_000_000 { return sign + trimmed(Double(magnitude) / 1_000_000) + "M" }
        if magnitude >= 10_000 { return sign + trimmed(Double(magnitude) / 1_000) + "K" }
        return sign + String(magnitude)
    }

    public static func signed(_ value: Int64) -> String { (value >= 0 ? "+" : "") + count(value) }

    public static func percent(_ value: Double) -> String { trimmed(value) + "%" }

    public static func trimmed(_ value: Double) -> String {
        let rounded = (value * 10).rounded() / 10
        return rounded == rounded.rounded() ? String(Int64(rounded)) : String(format: "%.1f", rounded)
    }

    public static func plural(_ count: Int, _ one: String, _ many: String) -> String {
        "\(count) " + (count == 1 ? one : many)
    }
}

public struct MarketingRecord: Equatable, Sendable, Identifiable {
    public var kind: MarketingKind
    public var json: JSONObject

    public init(kind: MarketingKind, json: JSONObject) {
        self.kind = kind
        self.json = json
    }

    public var id: String { json.str("id") }
    public var status: String { json.str("status") }

    public var title: String {
        switch kind {
        case .campaign: return json.str("name")
        case .experiment: return json.str("title")
        case .keyword: return json.str("term")
        case .redditThread:
            let title = json.str("title")
            return title.isEmpty ? "r/\(json.str("subreddit")) thread" : title
        case .post:
            let title = json.str("title")
            return title.isEmpty ? json.str("url") : title
        case .funnelWeek: return "Week"
        default: return json.str("title")
        }
    }

    public var tasks: [MarketingTask] {
        json.maps("tasks").map { MarketingTask(title: $0.str("title"), done: $0.bool("done"), owner: $0.str("owner")) }
    }

    public func facts(_ overview: MarketingOverview) -> [String] {
        switch kind {
        case .campaign:
            let stats = json.obj("stats")
            var parts = [MarketingText.title(json.str("objective"))]
            let total = stats.int("tasksTotal")
            if total > 0 { parts.append("\(stats.int("tasksDone"))/\(total) tasks") }
            parts.append(MarketingText.plural(stats.int("content"), "post", "posts"))
            if stats.int("ads") > 0 { parts.append(MarketingText.plural(stats.int("ads"), "ad", "ads")) }
            if stats.int("daysLeft") > 0 { parts.append(MarketingText.plural(stats.int("daysLeft"), "day left", "days left")) }
            return parts
        case .experiment:
            var parts = [MarketingText.title(json.str("stage"))]
            let score = json.int("score")
            parts.append(score > 0 ? "ICE \(score)" : "Not scored")
            if !json.str("decision").isEmpty { parts.append(MarketingText.title(json.str("decision"))) }
            if json.bool("reviewDue") || json.bool("ended") { parts.append("Review due") }
            return parts
        case .keyword:
            var parts: [String] = []
            if let position = json["position"]?.doubleValue {
                var text = "Position " + MarketingText.trimmed(position)
                if let change = json["positionChange"]?.doubleValue, change != 0 {
                    text += " (" + (change > 0 ? "+" : "") + MarketingText.trimmed(change) + ")"
                }
                parts.append(text)
            } else {
                parts.append("No ranking yet")
            }
            if let impressions = json["impressions"]?.int64Value { parts.append(MarketingText.count(impressions) + " impressions") }
            if let clicks = json["clicks"]?.int64Value { parts.append(MarketingText.count(clicks) + " clicks") }
            if !json.str("intent").isEmpty { parts.append(MarketingText.title(json.str("intent"))) }
            if json.int("priority") > 0 { parts.append("Priority \(json.int("priority"))") }
            return parts
        case .redditThread:
            var parts = ["r/" + json.str("subreddit"), MarketingText.title(json.str("status"))]
            if let score = json["score"]?.int64Value { parts.append("\(MarketingText.count(score)) points") }
            if let comments = json["comments"]?.int64Value { parts.append("\(MarketingText.count(comments)) comments") }
            if json.bool("monitored") { parts.append("Monitored") }
            return parts
        case .post:
            var parts = [overview.channelName(json.str("channel"))]
            for (key, label) in [("views", "views"), ("likes", "likes"), ("replies", "replies"), ("reposts", "reposts")] {
                if let value = json[key]?.int64Value { parts.append(MarketingText.count(value) + " " + label) }
            }
            return parts
        case .funnelWeek:
            var parts: [String] = []
            for (key, label) in [("visits", "visits"), ("signups", "signups"), ("activated", "activated"), ("retained", "retained")] {
                if let value = json[key]?.int64Value { parts.append(MarketingText.count(value) + " " + label) }
            }
            return parts.isEmpty ? ["No numbers"] : parts
        default:
            return []
        }
    }

    public var dates: [MarketingDate] {
        let pairs: [(String, String)]
        switch kind {
        case .campaign: pairs = [("Starts", "startsAt"), ("Ends", "endsAt")]
        case .experiment: pairs = [("Starts", "startsAt"), ("Ends", "endsAt"), ("Review", "reviewAt")]
        case .post: pairs = [("Posted", "postedAt"), ("Checked", "checkedAt")]
        case .redditThread: pairs = [("Logged", "createdAt")]
        case .keyword: pairs = [("Read", "observedAt")]
        case .funnelWeek: pairs = [("Week of", "weekStart")]
        default: pairs = []
        }
        return pairs.map { MarketingDate(label: $0.0, ms: json.int64($0.1)) }.filter { $0.ms > 0 }
    }

    public var link: String {
        switch kind {
        case .campaign: return json.obj("stats").str("taggedLink")
        case .redditThread, .post: return json.str("url")
        case .keyword: return json.str("targetUrl")
        default: return ""
        }
    }

    public var needsAttention: Bool {
        switch kind {
        case .experiment: return json.bool("reviewDue") || json.bool("ended")
        case .redditThread: return ["to_review", "drafting"].contains(status)
        default: return false
        }
    }

    public var redditReplyPrompt: String {
        guard kind == .redditThread else { return "" }
        let text: String = json.str("body").isEmpty ? json.str("text") : json.str("body")
        return """
        Draft one helpful, genuine Reddit reply to the post below. Help the author first and mention ForkMesh only when it directly solves their problem. Do not use hype, unverifiable claims, or a salesy tone. Respect r/\(json.str("subreddit"))'s rules, disclose that I work on or contribute to ForkMesh if recommending it, and use at most one relevant link. Return only a concise draft for me to review and rewrite before posting.

        Reddit post
        Subreddit: r/\(json.str("subreddit"))
        Title: \(title)
        URL: \(json.str("url"))
        Text: \(text)

        ForkMesh links
        https://forkmesh.com/
        https://forkmesh.com/docs/
        https://github.com/forkmesh/forkmesh
        """
    }

    public func matches(_ query: String) -> Bool {
        PackagesText.matches(
            query, title, json.str("campaign"), json.str("owner"), json.str("subreddit"), json.str("url"),
            json.str("note"), json.str("notes"), json.str("hypothesis"))
    }
}

public struct MarketingDate: Equatable, Sendable, Identifiable {
    public var label: String
    public var ms: Int64

    public var id: String { label }
}

public struct MarketingTask: Equatable, Sendable {
    public var title: String
    public var done: Bool
    public var owner: String

    public init(title: String, done: Bool = false, owner: String = "") {
        self.title = title
        self.done = done
        self.owner = owner
    }

    public var json: JSONValue {
        .object(["title": .string(title), "done": .bool(done), "owner": .string(owner)])
    }
}

public struct MarketingChannelSetting: Equatable, Sendable, Identifiable {
    public var id: String
    public var enabled: Bool
    public var displayName: String
    public var profileUrl: String
    public var hasAccessToken: Bool
    public var lastReadAt: Int64
    public var lastError: String

    public init(json: JSONObject) {
        id = json.str("id")
        enabled = json["enabled"]?.boolValue ?? true
        displayName = json.str("displayName")
        profileUrl = json.str("profileUrl")
        hasAccessToken = json.bool("hasAccessToken")
        let feed = json.obj("feed")
        lastReadAt = feed.int64("lastReadAt")
        lastError = feed.str("lastError")
    }

    public var payload: JSONObject {
        ["id": .string(id), "enabled": .bool(enabled), "displayName": .string(displayName), "profileUrl": .string(profileUrl)]
    }

    public static let tokenChannels: Set<String> = ["x", "github"]

    public var takesToken: Bool { Self.tokenChannels.contains(id) }
}

public struct MarketingSuggestion: Equatable, Sendable, Identifiable {
    public var level: String
    public var code: String
    public var title: String
    public var detail: String
    public var campaign: String
    public var action: JSONObject

    public init(json: JSONObject) {
        level = json.str("level", "info")
        code = json.str("code")
        title = json.str("title")
        detail = json.str("detail")
        campaign = json.str("campaign")
        action = json.obj("action")
    }

    public var id: String { code + ":" + campaign + ":" + title }
    public var actionType: String { action.str("type") }
}

public struct MarketingAdTotals: Equatable, Sendable, Identifiable {
    public var currency: String
    public var ads: Int
    public var active: Int
    public var spendCents: Int64
    public var budgetCents: Int64
    public var revenueCents: Int64
    public var impressions: Int64
    public var clicks: Int64
    public var conversions: Int64
    public var ctr: Double
    public var roas: Double

    public init(json: JSONObject) {
        currency = json.str("currency")
        ads = json.int("ads")
        active = json.int("active")
        spendCents = json.int64("spendCents")
        budgetCents = json.int64("budgetCents")
        revenueCents = json.int64("revenueCents")
        impressions = json.int64("impressions")
        clicks = json.int64("clicks")
        conversions = json.int64("conversions")
        ctr = json.double("ctr")
        roas = json.double("roas")
    }

    public var id: String { currency }

    public var facts: [String] {
        var parts = [MoneyText.cents(spendCents, currency) + " spent of " + MoneyText.cents(budgetCents, currency)]
        parts.append(MarketingText.count(impressions) + " impressions")
        parts.append(MarketingText.count(clicks) + " clicks")
        if ctr > 0 { parts.append(MarketingText.percent(ctr) + " CTR") }
        if conversions > 0 { parts.append(MarketingText.count(conversions) + " conversions") }
        if roas > 0 { parts.append(MarketingText.trimmed(roas) + "x ROAS") }
        return parts
    }
}

public struct MarketingFunnelRate: Equatable, Sendable, Identifiable {
    public var id: String
    public var label: String
    public var value: Double?
    public var change: Double?
    public var target: Double
    public var small: Bool

    public init(json: JSONObject) {
        id = json.str("key")
        label = json.str("label")
        value = json["value"]?.doubleValue
        change = json["change"]?.doubleValue
        target = json.double("target")
        small = json.bool("small")
    }

    public var text: String {
        guard let value else { return "No data" }
        var parts = [MarketingText.percent(value)]
        if target > 0 { parts.append("target " + MarketingText.percent(target)) }
        if let change, change != 0 { parts.append((change > 0 ? "+" : "") + MarketingText.trimmed(change) + " pts") }
        if small { parts.append("small sample") }
        return parts.joined(separator: " · ")
    }

    public var belowTarget: Bool {
        guard let value, target > 0 else { return false }
        return value < target
    }
}

public struct MarketingFunnelReport: Equatable, Sendable {
    public var weekStart: Int64
    public var rates: [MarketingFunnelRate]
    public var bottleneck: String
    public var bottleneckReason: String

    public init(json: JSONObject) {
        weekStart = json.int64("weekStart")
        rates = json.maps("rates").map(MarketingFunnelRate.init(json:))
        bottleneck = json.str("bottleneck")
        bottleneckReason = json.str("bottleneckReason")
    }
}

public struct MarketingXStatus: Equatable, Sendable {
    public var connected: Bool
    public var username: String
    public var connectedBy: String
    public var lastPostedAt: Int64
    public var lastPostUrl: String

    public init(json: JSONObject) {
        connected = json.bool("connected")
        username = json.str("username")
        connectedBy = json.str("connectedBy")
        lastPostedAt = json.int64("lastPostedAt")
        lastPostUrl = json.str("lastPostUrl")
    }
}

public struct MarketingReadyPost: Equatable, Sendable, Identifiable {
    public static let limit = 280
    public static let imagePrefixes = ["data:image/png;base64,", "data:image/jpeg;base64,", "data:image/webp;base64,"]
    public static let closeStatuses = ["posted", "skipped"]
    public static let notPostingActivities: Set<String> = [
        "com.apple.UIKit.activity.CopyToPasteboard", "com.apple.UIKit.activity.SaveToCameraRoll",
        "com.apple.UIKit.activity.Print", "com.apple.UIKit.activity.AssignToContact",
        "com.apple.UIKit.activity.AddToReadingList", "com.apple.UIKit.activity.MarkupAsPDF",
        "com.apple.DocumentManagerUICore.SaveToFiles",
    ]

    public var id: String
    public var text: String
    public var length: Int
    public var imageDataUrl: String
    public var hasImage: Bool
    public var imageAlt: String
    public var why: String
    public var link: String
    public var model: String
    public var author: String
    public var createdAt: Int64
    public var intentUrl: String

    public init?(json: JSONObject) {
        id = json.str("id")
        text = json.str("text")
        guard PackagesText.isHexId(id), !text.isEmpty else { return nil }
        length = json.contains("length") ? json.int("length") : text.count
        imageDataUrl = json.str("imageDataUrl")
        hasImage = json.bool("hasImage") || !imageDataUrl.isEmpty
        imageAlt = json.str("imageAlt")
        why = json.str("why")
        link = json.str("link")
        model = json.str("model")
        author = json.str("author")
        createdAt = json.int64("createdAt")
        intentUrl = json.str("intentUrl")
    }

    public var lengthLabel: String { "\(length)/\(Self.limit)" }

    public var tooLong: Bool { length > Self.limit }

    public var nearLimit: Bool { !tooLong && length > Self.limit - 20 }

    public var writer: String { model.isEmpty ? author : model }

    public var imageData: Data? {
        guard let prefix = Self.imagePrefixes.first(where: { imageDataUrl.hasPrefix($0) }) else { return nil }
        return Data(base64Encoded: String(imageDataUrl.dropFirst(prefix.count)))
    }

    public var composerURL: URL? {
        guard intentUrl.hasPrefix("https://x.com/intent/") else { return nil }
        return URL(string: intentUrl)
    }

    public static func sharePosted(activity: String?, completed: Bool) -> Bool {
        guard completed, let activity, !activity.isEmpty else { return false }
        return !notPostingActivities.contains(activity)
    }
}

public struct MarketingReadyClosed: Equatable, Sendable, Identifiable {
    public var id: String
    public var text: String
    public var why: String
    public var status: String
    public var createdAt: Int64
    public var closedAt: Int64
    public var closedBy: String
    public var url: String

    public init(json: JSONObject) {
        id = json.str("id")
        text = json.str("text")
        why = json.str("why")
        status = json.str("status", "replaced")
        createdAt = json.int64("createdAt")
        closedAt = json.int64("closedAt")
        closedBy = json.str("closedBy")
        url = json.str("url")
    }

    public var statusTitle: String { MarketingText.title(status) }

    public var link: URL? { url.hasPrefix("https://") ? URL(string: url) : nil }
}

public struct MarketingReadyState: Equatable, Sendable {
    public var available: Bool
    public var channel: String
    public var needsPost: Bool
    public var post: MarketingReadyPost?
    public var history: [MarketingReadyClosed]

    public init(json: JSONObject) {
        available = !json.isEmpty
        channel = json.str("channel", "x")
        post = MarketingReadyPost(json: json.obj("post"))
        needsPost = post == nil
        history = json.maps("history").map(MarketingReadyClosed.init(json:))
    }

    public var lastClosed: MarketingReadyClosed? { history.first }

    public func merged(with loaded: MarketingReadyState?) -> MarketingReadyState {
        guard let loaded, let id = post?.id, loaded.post?.id == id else { return self }
        return loaded
    }

    public func needsImage(loaded: MarketingReadyState?) -> Bool {
        guard let post, post.hasImage else { return false }
        return loaded?.post?.id != post.id
    }

    public static let emptyText = "No post ready yet. Herald (the X special agent) writes the next one."
}

public struct MarketingRedditSubreddit: Equatable, Sendable, Identifiable {
    public var name: String
    public var url: String
    public var rulesUrl: String
    public var rulesRead: Bool
    public var rules: [String]

    public init(json: JSONObject) {
        name = json.str("name")
        url = json.str("url")
        rulesUrl = json.str("rulesUrl")
        rulesRead = json.bool("rulesRead")
        rules = json.maps("rules").map { $0.str("title") }.filter { !$0.isEmpty }
    }

    public var id: String { name }
}

public struct MarketingAdvice: Equatable, Sendable {
    public var summary: String
    public var campaignId: String
    public var suggestions: [MarketingIdea]

    public init(json: JSONObject) {
        summary = json.str("summary")
        campaignId = json.str("campaignId")
        suggestions = json.maps("suggestions").map(MarketingIdea.init(json:))
    }
}

public struct MarketingIdea: Equatable, Sendable, Identifiable {
    public var kind: String
    public var title: String
    public var detail: String
    public var channel: String
    public var copy: String
    public var inDays: Int

    public init(json: JSONObject) {
        kind = json.str("kind")
        title = json.str("title")
        detail = json.str("detail")
        channel = json.str("channel")
        copy = json.str("copy")
        inDays = json.int("inDays")
    }

    public var id: String { kind + ":" + title }

    public var actionTitle: String {
        switch kind {
        case "content": return "Add to plan"
        case "task": return "Add as task"
        case "keyword": return "Track keyword"
        case "campaign": return "Start campaign"
        default: return ""
        }
    }
}

public struct MarketingPerformance: Equatable, Sendable {
    public var status: String
    public var from: Int64
    public var to: Int64
    public var sessions: Int64
    public var previousSessions: Int64
    public var conversions: Int64?
    public var goals: [String]
    public var insights: [MarketingSuggestion]
    public var campaigns: [MarketingPerformanceRow]
    public var channels: [MarketingPerformanceRow]
    public var landing: [MarketingPerformanceRow]
    public var projects: [MarketingNamedLink]
    public var project: String

    public init(json: JSONObject) {
        status = json.str("status", "unavailable")
        from = json.int64("from")
        to = json.int64("to")
        let totals = json.obj("totals")
        sessions = totals.int64("sessions")
        previousSessions = json.obj("previous").int64("sessions")
        conversions = totals["conversions"]?.int64Value
        goals = json.strings("goals")
        insights = json.maps("insights").map(MarketingSuggestion.init(json:))
        campaigns = json.maps("campaigns").map { row in
            let campaign = row.str("campaign")
            let label = !campaign.isEmpty && campaign != "(none)"
                ? "\(campaign) (\(row.str("source")) / \(row.str("medium")))" : "\(row.str("source")) / \(row.str("medium"))"
            return MarketingPerformanceRow(json: row, label: label)
        }
        channels = json.maps("channels").map { MarketingPerformanceRow(json: $0, label: $0.str("label")) }
        landing = json.maps("landing").map { MarketingPerformanceRow(json: $0, label: $0.str("landing", $0.str("value"))) }
        projects = json.maps("projects").map { MarketingNamedLink(id: $0.str("id"), name: $0.str("name")) }
        project = json.str("project")
    }

    public var ok: Bool { status == "ok" }

    public var sessionsChange: Int64 { sessions - previousSessions }
}

public struct MarketingNamedLink: Equatable, Sendable, Identifiable {
    public var id: String
    public var name: String
    public var url: String

    public init(id: String, name: String, url: String = "") {
        self.id = id
        self.name = name
        self.url = url
    }
}

public struct MarketingPerformanceRow: Equatable, Sendable, Identifiable {
    public var label: String
    public var sessions: Int64
    public var conversions: Int64?
    public var bounceRate: Double

    public init(json: JSONObject, label: String) {
        self.label = label
        sessions = json.int64("sessions")
        conversions = json["conversions"]?.int64Value
        bounceRate = json.double("bounceRate")
    }

    public var id: String { label }

    public var facts: String {
        var parts = [MarketingText.count(sessions) + " visits"]
        if let conversions { parts.append(MarketingText.count(conversions) + " conversions") }
        if bounceRate > 0 { parts.append(MarketingText.percent(bounceRate * 100) + " bounce") }
        return parts.joined(separator: " · ")
    }
}

public struct MarketingReport: Equatable, Sendable {
    public var generatedAt: Int64
    public var markdown: String

    public init(json: JSONObject) {
        generatedAt = json.int64("generatedAt")
        markdown = json.str("markdown")
    }
}

public struct MarketingLink: Equatable, Sendable {
    public var url: String
    public var checks: [String]

    public init(json: JSONObject) {
        url = json.str("url")
        checks = json.maps("checks").map { $0.str("message") }.filter { !$0.isEmpty }
    }
}

public enum MarketingKeywordImport {
    public static func rows(_ source: String) -> [JSONObject] {
        let lines = source.split(whereSeparator: \.isNewline).map { $0.trimmingCharacters(in: .whitespaces) }
            .filter { !$0.isEmpty }
        guard let first = lines.first else { return [] }
        let separator: Character = first.contains("\t") ? "\t" : first.contains(";") && !first.contains(",") ? ";" : ","
        let head = cells(first, separator).map { $0.lowercased() }
        func find(_ names: [String]) -> Int? {
            head.firstIndex { cell in names.contains { cell.contains($0) } }
        }
        let term = find(["quer", "keyword", "search term", "term"])
        let clicks = find(["click"])
        let impressions = find(["impression"])
        let position = find(["position", "rank"])
        let header = term != nil && (clicks != nil || impressions != nil || position != nil || head.count == 1)
        var rows: [JSONObject] = []
        for line in lines.dropFirst(header ? 1 : 0) {
            var row = JSONObject()
            if header, let term {
                let values = cells(line, separator)
                func cell(_ index: Int?) -> String { index.flatMap { $0 < values.count ? values[$0] : nil } ?? "" }
                row["term"] = .string(cell(term))
                row["clicks"] = clicks == nil ? .null : number(cell(clicks), whole: true)
                row["impressions"] = impressions == nil ? .null : number(cell(impressions), whole: true)
                row["position"] = position == nil ? .null : number(cell(position), whole: false)
            } else {
                var value = String(line.split(separator: separator, maxSplits: 1, omittingEmptySubsequences: false).first ?? "")
                if value.hasPrefix("\"") { value.removeFirst() }
                if value.hasSuffix("\"") { value.removeLast() }
                row["term"] = .string(value)
            }
            if !row.str("term").isEmpty { rows.append(row) }
        }
        return rows
    }

    public static func top(_ rows: [JSONObject], limit: Int) -> [JSONObject] {
        let ordered = rows.enumerated().sorted { a, b in
            let left = (a.element.int64("clicks"), a.element.int64("impressions"))
            let right = (b.element.int64("clicks"), b.element.int64("impressions"))
            if left.0 != right.0 { return left.0 > right.0 }
            if left.1 != right.1 { return left.1 > right.1 }
            return a.offset < b.offset
        }.map(\.element)
        return Array(ordered.prefix(max(1, limit)))
    }

    static func cells(_ line: String, _ separator: Character) -> [String] {
        var result: [String] = []
        var current = ""
        var quoted = false
        var characters = Array(line)[...]
        while let character = characters.popFirst() {
            if character == "\"" {
                if quoted, characters.first == "\"" {
                    current.append("\"")
                    characters.removeFirst()
                } else {
                    quoted.toggle()
                }
            } else if character == separator && !quoted {
                result.append(current.trimmingCharacters(in: .whitespaces))
                current = ""
            } else {
                current.append(character)
            }
        }
        result.append(current.trimmingCharacters(in: .whitespaces))
        return result
    }

    static func number(_ value: String, whole: Bool) -> JSONValue {
        var clean = value.filter { $0 != "%" && !$0.isWhitespace }
        clean = clean.replacingOccurrences(of: #",(?=\d{3}\b)"#, with: "", options: .regularExpression)
        guard !clean.isEmpty, let number = Double(clean.replacingOccurrences(of: ",", with: ".")) else { return .null }
        return whole ? .int(Int64(number.rounded())) : .double(number)
    }
}
