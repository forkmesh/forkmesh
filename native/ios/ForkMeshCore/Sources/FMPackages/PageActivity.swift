import FMBase
import Foundation

public struct PageVisit: Equatable, Hashable, Sendable {
    public var kind: String
    public var type: String
    public var detail: String
    public var label: String
    public var visitor: String
    public var at: Int64

    public init(kind: String = "", type: String = "", detail: String = "", label: String = "", visitor: String = "", at: Int64 = 0) {
        self.kind = kind
        self.type = type
        self.detail = detail
        self.label = label
        self.visitor = visitor
        self.at = at
    }

    public init(json: JSONObject) {
        self.init(
            kind: json.str("kind"), type: json.str("type"), detail: json.str("detail"), label: json.str("label"),
            visitor: json.str("visitor"), at: max(0, json.int64("at")))
    }

    public func when(now: Int64) -> String { at > 0 ? Format.relativeTimeFrom(at, nowMillis: now) : "" }
}

public struct PageActivity: Equatable, Sendable {
    public static let shownVisits = 5

    public var days = 30
    public var views = 0
    public var visitors = 0
    public var interactions = 0
    public var starts = 0
    public var submits = 0
    public var leaves = 0
    public var lastActivityAt: Int64 = 0
    public var recent: [PageVisit] = []

    public init() {}

    public init(json: JSONObject) {
        days = max(1, json.int("days", 30))
        views = max(0, json.int("views"))
        visitors = max(0, json.int("visitors"))
        interactions = max(0, json.int("interactions"))
        starts = max(0, json.int("starts"))
        submits = max(0, json.int("submits"))
        leaves = max(0, json.int("leaves"))
        lastActivityAt = max(0, json.int64("lastActivityAt"))
        recent = json.maps("recent").map(PageVisit.init(json:)).filter { !$0.label.isEmpty }
    }

    public var isEmpty: Bool { views == 0 && interactions == 0 }

    public var period: String { days == 1 ? "day" : "\(days) days" }

    public func summary(now: Int64) -> String {
        if isEmpty { return "No visits in the last \(period)" }
        var parts = [
            SurveyText.plural(views, "view"), SurveyText.plural(visitors, "visitor"), "\(starts) started", "\(submits) sent",
        ]
        if lastActivityAt > 0 { parts.append("last activity " + Format.relativeTimeFrom(lastActivityAt, nowMillis: now)) }
        return "Last \(period): " + parts.joined(separator: " · ")
    }

    public var newest: [PageVisit] { Array(recent.prefix(Self.shownVisits)) }
}
