import FMBase
import Foundation

public struct AnalyticsProject: Equatable, Sendable, Identifiable {
    public var id: String
    public var name: String
    public var siteDefault: Bool
    public var eventsToday: Int64

    public init(json: JSONObject) {
        id = json.str("id")
        let settings = json.obj("settings")
        name = settings.str("name").isEmpty ? (json.bool("siteDefault") ? "Website" : "Project") : settings.str("name")
        siteDefault = json.bool("siteDefault")
        eventsToday = json.obj("usage").int64("events")
    }
}

public struct AnalyticsTotals: Equatable, Sendable {
    public var visitors: Int64
    public var pageviews: Int64
    public var sessions: Int64
    public var bounceRate: Double
    public var averageSessionMs: Int64
    public var events: Int64

    public init(json: JSONObject) {
        visitors = json.int64("visitors")
        pageviews = json.int64("pageviews")
        sessions = json.int64("sessions")
        bounceRate = json.double("bounceRate")
        averageSessionMs = json.int64("averageSessionMs")
        events = json.int64("events")
    }

    public static func change(_ current: Int64, _ previous: Int64) -> Double? {
        previous > 0 ? Double(current - previous) / Double(previous) : nil
    }
}

public struct AnalyticsSeries: Equatable, Sendable, Identifiable {
    public var name: String
    public var data: [Double]
    public var total: Double

    public var id: String { name }
}

public struct AnalyticsTopRow: Equatable, Sendable, Identifiable {
    public var value: String
    public var count: Int64

    public var id: String { value }
}

public enum AnalyticsRange: String, CaseIterable, Sendable {
    case day = "24h"
    case week = "7d"
    case month = "30d"

    public var millis: Int64 {
        switch self {
        case .day: return 86_400_000
        case .week: return 7 * 86_400_000
        case .month: return 30 * 86_400_000
        }
    }

    public func query(nowMs: Int64) -> JSONObject {
        ["kind": "overview", "from": .int(nowMs - millis), "to": .int(nowMs)]
    }
}

public struct AnalyticsOverview: Equatable, Sendable {
    public var totals: AnalyticsTotals
    public var previous: AnalyticsTotals
    public var labels: [Int64]
    public var series: [AnalyticsSeries]
    public var pages: [AnalyticsTopRow]
    public var referrers: [AnalyticsTopRow]
    public var countries: [AnalyticsTopRow]
    public var devices: [AnalyticsTopRow]
    public var events: [AnalyticsTopRow]
    public var truncated: Bool

    public init(json: JSONObject) {
        let result = json.obj("result")
        totals = AnalyticsTotals(json: result.obj("totals"))
        previous = AnalyticsTotals(json: result.obj("previous"))
        let timeseries = result.obj("timeseries")
        labels = timeseries.list("labels").map { $0.int64() }
        series = timeseries.maps("series").prefix(10).map {
            AnalyticsSeries(name: $0.str("name"), data: $0.list("data").map { $0.double() }, total: $0.double("total"))
        }
        let breakdowns = result.obj("breakdowns")
        func top(_ key: String) -> [AnalyticsTopRow] {
            breakdowns.maps(key).prefix(12).map { AnalyticsTopRow(value: $0.str("value"), count: $0.int64("count")) }
        }
        pages = top("pages")
        referrers = top("referrers")
        countries = top("countries")
        devices = top("devices")
        events = top("events")
        truncated = json.bool("truncated")
    }
}
