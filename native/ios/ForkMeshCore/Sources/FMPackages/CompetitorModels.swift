import FMBase
import Foundation

public struct Competitor: Equatable, Sendable, Identifiable {
    public var id: String
    public var status: String
    public var name: String
    public var website: String
    public var category: String
    public var positioning: String
    public var strengths: String
    public var weaknesses: String
    public var notes: String
    public var updatedAt: Int64
    public var lastReviewedAt: Int64

    public init(json: JSONObject) {
        id = json.str("id")
        status = json.str("status", "watch")
        name = json.str("name")
        website = json.str("website")
        category = json.str("category")
        positioning = json.str("positioning")
        strengths = json.str("strengths")
        weaknesses = json.str("weaknesses")
        notes = json.str("notes")
        updatedAt = json.int64("updatedAt")
        lastReviewedAt = json.int64("lastReviewedAt")
    }

    public init(name: String = "") {
        id = ""
        status = "watch"
        self.name = name
        website = ""
        category = ""
        positioning = ""
        strengths = ""
        weaknesses = ""
        notes = ""
        updatedAt = 0
        lastReviewedAt = 0
    }

    public static let statuses = ["primary", "watch", "archived"]

    public func needsReview(nowMs: Int64, days: Int64 = 90) -> Bool {
        status != "archived" && (lastReviewedAt == 0 || nowMs - lastReviewedAt > days * 86_400_000)
    }

    public var body: JSONObject {
        [
            "name": .string(name.trimmingCharacters(in: .whitespacesAndNewlines)),
            "status": .string(Competitor.statuses.contains(status) ? status : "watch"),
            "website": .string(website.trimmingCharacters(in: .whitespacesAndNewlines)),
            "category": .string(category), "positioning": .string(positioning), "strengths": .string(strengths),
            "weaknesses": .string(weaknesses), "notes": .string(notes),
        ]
    }

    public var websiteIsValid: Bool {
        let trimmed = website.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty || (trimmed.lowercased().hasPrefix("https://") && !trimmed.contains(" "))
    }

    public func matches(_ query: String) -> Bool {
        PackagesText.matches(query, name, category, positioning, website)
    }
}

public struct CompetitorBoard: Equatable, Sendable {
    public var organization: String
    public var role: String
    public var competitors: [Competitor]

    public init(json: JSONObject) {
        organization = json.str("organization")
        role = json.str("role", "member")
        competitors = json.maps("competitors").map(Competitor.init(json:)).filter { PackagesText.isHexId($0.id) }
    }

    public func grouped(query: String = "") -> [(status: String, items: [Competitor])] {
        Competitor.statuses.map { status in
            (status: status, items: competitors.filter { $0.status == status && $0.matches(query) })
        }.filter { !$0.items.isEmpty }
    }
}
