import FMBase
import Foundation

extension PackagesClient {
    func marketingPath(_ organization: String, _ suffix: String = "") throws -> String {
        "/api/marketing/" + (try Self.segment(organization)) + (suffix.isEmpty ? "" : "/" + suffix)
    }

    @discardableResult
    public func saveMarketing(_ draft: MarketingDraft, organization: String, overview: MarketingOverview, now: Int64,
                              zone: TimeZone = .current) async throws -> JSONObject
    {
        let body: JSONObject
        do {
            body = try draft.body(overview: overview, now: now, zone: zone)
        } catch let error as MarketingFormError {
            throw PackagesError("form_invalid", fields: ["message": error.message])
        }
        return try await sendMarketing(draft.kind, id: draft.id, organization: organization, body: body)
    }

    @discardableResult
    public func sendMarketing(_ kind: MarketingKind, id: String = "", organization: String, body: JSONObject) async throws
        -> JSONObject
    {
        switch kind {
        case .channel, .attribution, .targets, .goals, .redditWatch:
            return try await call("PUT", marketingPath(organization, "settings"), body: body)
        case .xConnection, .xPost, .link, .metric, .funnelWeek:
            return try await call("POST", marketingPath(organization, kind.resource), body: body)
        case .adResults:
            return try await call("PATCH", marketingPath(organization, "ads/" + Self.item(id)), body: body)
        default:
            if id.isEmpty { return try await call("POST", marketingPath(organization, kind.resource), body: body) }
            return try await call("PATCH", marketingPath(organization, kind.resource + "/" + Self.item(id)), body: body)
        }
    }

    public func setMarketingStatus(_ kind: MarketingKind, id: String, status: String, organization: String) async throws
        -> JSONObject
    {
        guard kind.statuses.contains(status) else { throw PackagesError("invalid_status") }
        return try await sendMarketing(kind, id: id, organization: organization, body: ["status": .string(status)])
    }

    public func deleteMarketing(_ kind: MarketingKind, id: String, organization: String) async throws {
        guard kind.deletable else { throw PackagesError("method_not_allowed") }
        try await call("DELETE", marketingPath(organization, kind.resource + "/" + Self.item(id)), body: [:])
    }

    public func addMarketingChannels(_ ids: [String], organization: String, overview: MarketingOverview) async throws {
        try await call("PUT", marketingPath(organization, "settings"), body: MarketingForms.channelsBody(overview, adding: ids))
    }

    public func refreshMarketingChannel(_ channel: String, organization: String) async throws -> (MarketingOverview, String) {
        let reply = try await call("POST", marketingPath(organization, "refresh"), body: ["channel": .string(channel)])
        let refresh = reply.obj("refresh")
        let items = refresh.int("items")
        var message = "\(items) post\(items == 1 ? "" : "s") read · \(refresh.int("imported")) new"
        let metrics = refresh.obj("metrics")
        if metrics.contains("followers") && channel != "email" {
            message += " · \(MarketingText.count(metrics.int64("followers"))) followers"
        }
        return (MarketingOverview(json: reply), message)
    }

    public func marketingReadyPost(organization: String, channel: String = "x") async throws -> MarketingReadyState {
        MarketingReadyState(json: try await call("GET", marketingPath(organization, "ready-post"), query: [("channel", channel)])
            .obj("readyPost"))
    }

    public func closeMarketingReadyPost(organization: String, channel: String = "x", id: String, status: String, url: String = "")
        async throws -> MarketingReadyState
    {
        guard MarketingReadyPost.closeStatuses.contains(status) else { throw PackagesError("invalid_status") }
        var body: JSONObject = ["channel": .string(channel), "id": .string(try Self.item(id)), "status": .string(status)]
        if !url.isEmpty { body["url"] = .string(url) }
        return MarketingReadyState(json: try await call("PATCH", marketingPath(organization, "ready-post"), body: body).obj("readyPost"))
    }

    public func disconnectX(organization: String) async throws {
        try await call("DELETE", marketingPath(organization, "x-connection"), body: [:])
    }

    public func marketingAdvice(organization: String, campaignId: String = "", question: String = "") async throws -> MarketingAdvice {
        var body: JSONObject = ["question": .string(String(question.prefix(600)))]
        if !campaignId.isEmpty { body["campaignId"] = .string(try Self.item(campaignId)) }
        return MarketingAdvice(json: try await call("POST", marketingPath(organization, "assistant"), body: body))
    }

    public func marketingReport(organization: String) async throws -> MarketingReport {
        MarketingReport(json: try await call("GET", marketingPath(organization, "report")))
    }

    public func marketingPerformance(organization: String, days: Int, project: String = "", now: Int64) async throws
        -> MarketingPerformance
    {
        var query = [("from", String(now - Int64(max(1, days)) * 86_400_000)), ("to", String(now))]
        if PackagesText.isHexId(project) { query.append(("project", project)) }
        return MarketingPerformance(json: try await call("GET", marketingPath(organization, "performance"), query: query).obj("performance"))
    }

    public func buildMarketingLink(_ draft: MarketingDraft, organization: String, overview: MarketingOverview, now: Int64) async throws
        -> MarketingLink
    {
        MarketingLink(json: try await saveMarketing(draft, organization: organization, overview: overview, now: now))
    }

    public func importMarketingKeywords(_ rows: [JSONObject], organization: String, source: String, observedAt: Int64) async throws
        -> (created: Int, updated: Int, skipped: Int)
    {
        guard !rows.isEmpty else { throw PackagesError("invalid_keyword_rows") }
        let reply = try await call(
            "POST", marketingPath(organization, "keywords"),
            body: ["rows": .array(rows.prefix(500).map { .object($0) }), "source": .string(String(source.prefix(60))),
                   "observedAt": .int(observedAt)])
        return (reply.int("created"), reply.int("updated"), reply.int("skipped"))
    }
}
