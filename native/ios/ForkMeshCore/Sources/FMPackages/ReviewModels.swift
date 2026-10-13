import FMBase
import Foundation

public struct ReviewRules: Equatable, Sendable {
    public var reviewRequiredBelow: Int
    public var minChars: Int
    public var minWords: Int
    public var maxChars: Int
    public var categories: [(id: String, label: String)]

    public init(
        reviewRequiredBelow: Int = 5, minChars: Int = 40, minWords: Int = 8, maxChars: Int = 4000,
        categories: [(id: String, label: String)] = ReviewRules.defaultCategories
    ) {
        self.reviewRequiredBelow = reviewRequiredBelow
        self.minChars = minChars
        self.minWords = minWords
        self.maxChars = maxChars
        self.categories = categories
    }

    public init(json: JSONObject) {
        let categories = json.maps("categories").map { (id: $0.str("id"), label: $0.str("label")) }
        self.init(
            reviewRequiredBelow: json.int("reviewRequiredBelow", 5), minChars: json.int("minChars", 40),
            minWords: json.int("minWords", 8), maxChars: json.int("maxChars", 4000),
            categories: categories.isEmpty ? ReviewRules.defaultCategories : categories)
    }

    public static let defaultCategories: [(id: String, label: String)] = [
        ("bug", "Bug"), ("missing-feature", "Missing feature"), ("docs", "Docs"), ("performance", "Performance"),
        ("ux", "UX"), ("other", "Other"),
    ]

    public static func == (lhs: ReviewRules, rhs: ReviewRules) -> Bool {
        lhs.reviewRequiredBelow == rhs.reviewRequiredBelow && lhs.minChars == rhs.minChars
            && lhs.minWords == rhs.minWords && lhs.maxChars == rhs.maxChars
            && lhs.categories.map(\.id) == rhs.categories.map(\.id)
    }
}

public struct MarketReviewReply: Equatable, Sendable {
    public var text: String
    public var by: String
    public var at: Int64
}

public struct MarketReview: Equatable, Sendable, Identifiable {
    public var id: String
    public var stars: Int
    public var reviewerName: String
    public var reviewerDisplayName: String
    public var problem: String
    public var improvement: String
    public var context: String
    public var comment: String
    public var category: String
    public var version: String
    public var verifiedUse: Bool
    public var helpful: Int
    public var viewerFoundHelpful: Bool
    public var createdAt: Int64
    public var updatedAt: Int64
    public var edited: Bool
    public var reply: MarketReviewReply?
    public var addressedIn: String

    public init(json: JSONObject) {
        id = json.str("id")
        stars = min(5, max(1, json.int("stars", 5)))
        let reviewer = json.obj("reviewer")
        reviewerName = reviewer.str("name")
        reviewerDisplayName = reviewer.str("displayName")
        problem = json.str("problem")
        improvement = json.str("improvement")
        context = json.str("context")
        comment = json.str("comment")
        category = json.str("category")
        version = json.str("version")
        verifiedUse = json.bool("verifiedUse")
        helpful = max(0, json.int("helpful"))
        viewerFoundHelpful = json.bool("viewerFoundHelpful")
        createdAt = json.int64("createdAt")
        updatedAt = json.int64("updatedAt")
        edited = json.bool("edited")
        let reply = json.obj("reply")
        self.reply = reply.str("text").isEmpty
            ? nil : MarketReviewReply(text: reply.str("text"), by: reply.str("by"), at: reply.int64("at"))
        addressedIn = json.str("addressedIn")
    }

    public var reviewerLabel: String { reviewerDisplayName.isEmpty ? reviewerName : reviewerDisplayName }
}

public struct ReviewViewer: Equatable, Sendable {
    public var signedIn: Bool
    public var name: String
    public var isMaintainer: Bool
    public var isAdmin: Bool
    public var canRate: Bool
    public var reason: String
    public var review: MarketReview?
    public var views: Int
    public var opens: Int

    public init(json: JSONObject) {
        signedIn = json.bool("signedIn")
        name = json.str("name")
        isMaintainer = json.bool("isMaintainer")
        isAdmin = json.bool("isAdmin")
        canRate = json.bool("canRate")
        reason = json.str("reason")
        let own = json.obj("review")
        review = own.str("id").isEmpty ? nil : MarketReview(json: own)
        views = max(0, json.int("views"))
        opens = max(0, json.int("opens"))
    }

    public var canSeePeople: Bool { isMaintainer || isAdmin }

    public var cannotRateReason: String {
        canRate ? "" : PackagesError.sentence(reason.isEmpty ? "sign_in_required" : reason)
    }
}

public struct ReviewPage: Equatable, Sendable {
    public var summary: RatingSummary
    public var reviews: [MarketReview]
    public var total: Int
    public var hasMore: Bool
    public var viewer: ReviewViewer
    public var rules: ReviewRules
    public var activity: PackageActivityTotals

    public init(json: JSONObject) {
        summary = RatingSummary(json: json.obj("summary"))
        reviews = json.maps("reviews").map(MarketReview.init(json:))
        total = json.int("total", reviews.count)
        hasMore = json.bool("hasMore")
        viewer = ReviewViewer(json: json.obj("viewer"))
        rules = ReviewRules(json: json.obj("rules"))
        activity = PackageActivityTotals(json: json.obj("activity"))
    }
}

public struct PackageActivityTotals: Equatable, Sendable {
    public var views: Int
    public var opens: Int
    public var people: Int

    public init(views: Int = 0, opens: Int = 0, people: Int = 0) {
        self.views = views
        self.opens = opens
        self.people = people
    }

    public init(json: JSONObject) {
        self.init(views: max(0, json.int("views")), opens: max(0, json.int("opens")), people: max(0, json.int("people")))
    }
}

public struct PackageActivityPerson: Equatable, Sendable, Identifiable {
    public var name: String
    public var views: Int
    public var opens: Int
    public var firstAt: Int64
    public var lastAt: Int64

    public var id: String { name }

    public init(json: JSONObject) {
        name = json.str("name")
        views = max(0, json.int("views"))
        opens = max(0, json.int("opens"))
        firstAt = json.int64("firstAt")
        lastAt = json.int64("lastAt")
    }
}

public struct PackageActivityPage: Equatable, Sendable {
    public var totals: PackageActivityTotals
    public var people: [PackageActivityPerson]
    public var hasMore: Bool

    public init(json: JSONObject) {
        totals = PackageActivityTotals(json: json.obj("totals"))
        people = json.maps("people").map(PackageActivityPerson.init(json:))
        hasMore = json.bool("hasMore")
    }
}

public enum PackageActivityKind: String, Sendable {
    case view
    case open
}

@MainActor
public final class PackageActivity {
    public static let shared = PackageActivity()
    public static let window: Int64 = 60_000

    private var last: [String: Int64] = [:]
    private let clock: () -> Int64

    public init(clock: @escaping () -> Int64 = { Int64(Date().timeIntervalSince1970 * 1000) }) {
        self.clock = clock
    }

    public func shouldReport(_ packageId: String, kind: PackageActivityKind) -> Bool {
        let key = kind.rawValue + ":" + packageId
        let now = clock()
        if let previous = last[key], now - previous < PackageActivity.window { return false }
        last[key] = now
        return true
    }

    public func report(_ packageId: String, kind: PackageActivityKind, client: PackagesClient?) {
        guard let client, PackagesText.isPackageId(packageId), shouldReport(packageId, kind: kind) else { return }
        Task { try? await client.reportActivity(packageId, kind: kind) }
    }
}

public struct ReviewDraft: Equatable, Sendable {
    public var stars: Int
    public var problem: String
    public var improvement: String
    public var context: String
    public var comment: String
    public var category: String
    public var version: String

    public init(
        stars: Int = 5, problem: String = "", improvement: String = "", context: String = "", comment: String = "",
        category: String = "", version: String = ""
    ) {
        self.stars = stars
        self.problem = problem
        self.improvement = improvement
        self.context = context
        self.comment = comment
        self.category = category
        self.version = version
    }

    public init(review: MarketReview) {
        self.init(
            stars: review.stars, problem: review.problem, improvement: review.improvement, context: review.context,
            comment: review.comment, category: review.category, version: review.version)
    }

    public func needsReview(_ rules: ReviewRules) -> Bool { stars < rules.reviewRequiredBelow }

    public func issues(_ rules: ReviewRules) -> [String: String] {
        var out: [String: String] = [:]
        let fields = ["problem": problem, "improvement": improvement, "context": context, "comment": comment]
        for (key, value) in fields where value.count > rules.maxChars { out[key] = "too_long" }
        guard needsReview(rules) else { return out }
        for key in ["problem", "improvement"] where out[key] == nil {
            let text = ReviewDraft.clean(fields[key] ?? "")
            if text.isEmpty {
                out[key] = "required"
            } else if text.count < rules.minChars {
                out[key] = "too_short"
            } else if ReviewDraft.words(text).count < rules.minWords {
                out[key] = "too_few_words"
            }
        }
        if out["improvement"] == nil, !improvement.isEmpty,
            ReviewDraft.clean(improvement).lowercased() == ReviewDraft.clean(problem).lowercased()
        {
            out["improvement"] = "same_as_problem"
        }
        return out
    }

    public var body: JSONObject {
        var body: JSONObject = ["stars": .int(Int64(min(5, max(1, stars))))]
        for (key, value) in [
            ("problem", problem), ("improvement", improvement), ("context", context), ("comment", comment),
            ("category", category), ("version", version),
        ] {
            let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
            if !trimmed.isEmpty { body[key] = .string(trimmed) }
        }
        return body
    }

    public static func clean(_ text: String) -> String {
        text.split(separator: "\n", omittingEmptySubsequences: false)
            .map { $0.split(whereSeparator: { $0 == " " || $0 == "\t" }).joined(separator: " ") }
            .joined(separator: "\n").trimmingCharacters(in: .whitespacesAndNewlines)
    }

    public static func words(_ text: String) -> [String] {
        text.lowercased().split { !$0.isLetter && !$0.isNumber }.map(String.init).filter {
            $0.filter(\.isLetter).count >= 2
        }
    }

    public static func fieldMessage(_ code: String) -> String {
        switch code {
        case "required": return "Required below five stars."
        case "too_short": return "Write at least a couple of sentences."
        case "too_few_words": return "Use a few more words."
        case "repeated_characters": return "Too many repeated characters."
        case "repetitive": return "Too repetitive."
        case "same_as_problem": return "Say how to improve, not the problem again."
        case "too_long": return "Too long."
        case "invalid": return "Not a valid choice."
        default: return code.replacingOccurrences(of: "_", with: " ")
        }
    }
}
