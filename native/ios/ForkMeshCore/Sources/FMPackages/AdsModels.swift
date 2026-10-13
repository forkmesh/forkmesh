import FMBase
import Foundation

public struct OwnedAd: Equatable, Sendable, Identifiable {
    public var id: String
    public var headline: String
    public var body: String
    public var cta: String
    public var sponsor: String
    public var url: String
    public var category: String
    public var placements: [String]
    public var status: String
    public var delivery: String
    public var paused: Bool
    public var reviewReason: String
    public var reviewCodes: [String]
    public var screenFlags: [String]
    public var contentHash: String
    public var clicks: Int64
    public var clickWindowDays: Int
    public var submittedAt: Int64
    public var updatedAt: Int64
    public var owner: String
    public var openReports: Int

    public init(json: JSONObject) {
        id = json.str("id")
        headline = json.str("headline")
        body = json.str("body")
        cta = json.str("cta")
        sponsor = json.str("sponsor")
        url = json.str("url")
        category = json.str("categoryLabel", json.str("category"))
        placements = json.strings("placements")
        status = json.str("status", "draft")
        delivery = json.str("delivery", json.str("status"))
        paused = json.bool("paused")
        reviewReason = json.str("reviewReason")
        reviewCodes = json.strings("reviewCodes")
        screenFlags = json.strings("screenFlags")
        contentHash = json.str("contentHash")
        let clicks = json.obj("clicks")
        self.clicks = clicks.int64("total")
        clickWindowDays = clicks.int("windowDays", 30)
        submittedAt = json.int64("submittedAt")
        updatedAt = json.int64("updatedAt")
        owner = json.str("owner")
        openReports = json.int("openReports")
    }

    public var reviewActions: [String] {
        switch status {
        case "pending": return ["approve", "reject"]
        case "approved": return ["suspend"]
        case "suspended": return ["reinstate"]
        default: return []
        }
    }

    public var deliveryText: String {
        switch delivery {
        case "running": return "Running"
        case "pending": return "Waiting for review"
        case "review-expired": return "Approval expired"
        case "paused": return "Paused"
        case "scheduled": return "Scheduled"
        case "ended": return "Ended"
        case "needs-membership", "over-allowance": return "Not running"
        case "rejected": return "Rejected"
        case "suspended": return "Suspended"
        case "withdrawn": return "Withdrawn"
        case "draft": return "Draft"
        default: return CrmStageLabel.title(delivery)
        }
    }
}

public struct AdReviewReason: Equatable, Sendable, Identifiable {
    public var id: String
    public var label: String
}

public struct AdReviewQueue: Equatable, Sendable {
    public var status: String
    public var ads: [OwnedAd]
    public var counts: [String: Int]
    public var prohibited: [AdReviewReason]

    public init(json: JSONObject) {
        status = json.str("status", "pending")
        ads = json.maps("ads").map(OwnedAd.init(json:)).filter { !$0.id.isEmpty }
        var counts: [String: Int] = [:]
        for (key, value) in json.obj("counts") { counts[key] = value.int() }
        self.counts = counts
        prohibited = json.maps("prohibited").map { AdReviewReason(id: $0.str("id"), label: $0.str("label")) }
    }

    public static let filters = ["pending", "approved", "suspended", "rejected", "reported"]
}

public struct AdReviewDecision: Equatable, Sendable {
    public var action: String
    public var reason: String
    public var codes: [String]

    public init(action: String, reason: String = "", codes: [String] = []) {
        self.action = action
        self.reason = reason
        self.codes = codes
    }

    public var needsReason: Bool { action == "reject" || action == "suspend" }

    public var isComplete: Bool {
        !needsReason || !reason.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || !codes.isEmpty
    }

    public func body(contentHash: String) -> JSONObject {
        var body: JSONObject = ["action": .string(action)]
        let trimmed = reason.trimmingCharacters(in: .whitespacesAndNewlines)
        if !trimmed.isEmpty { body["reason"] = .string(String(trimmed.prefix(500))) }
        if !codes.isEmpty { body["codes"] = .array(Array(Set(codes)).sorted().map { .string($0) }) }
        if action == "approve" { body["contentHash"] = .string(contentHash) }
        return body
    }
}
