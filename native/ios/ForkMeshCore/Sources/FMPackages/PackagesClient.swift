import FMBase
import Foundation

@MainActor
public final class PackagesClient {
    public let transport: PackagesTransport

    public init(transport: PackagesTransport) {
        self.transport = transport
    }

    @discardableResult
    func call(_ method: String, _ path: String, query: [(String, String)] = [], body: JSONObject? = nil) async throws
        -> JSONObject
    {
        let reply = try await transport.send(method, path, query: query, body: body)
        if reply.ok { return reply.json }
        throw PackagesError.from(reply)
    }

    static func segment(_ value: String) throws -> String {
        guard PackagesText.isOrgName(value) else { throw PackagesError("organization_not_found") }
        return value
    }

    static func item(_ value: String) throws -> String {
        guard PackagesText.isHexId(value) else { throw PackagesError("not_found", status: 404) }
        return value
    }

    public func packages() async throws -> [MarketPackage] {
        try await call("GET", "/api/market/packages").maps("packages").compactMap(MarketPackage.init(json:))
    }

    public func installCounts() async throws -> [String: PackageCounts] {
        var out: [String: PackageCounts] = [:]
        for (key, value) in try await call("GET", "/api/market/installs").obj("packages") where PackagesText.isPackageId(key) {
            out[key] = PackageCounts(json: value.objectOrEmpty)
        }
        return out
    }

    public func marketSummary() async throws -> MarketSummary {
        MarketSummary(json: try await call("GET", "/api/market/reviews/summary"))
    }

    public func ratingSummaries() async throws -> [String: RatingSummary] {
        try await marketSummary().ratings
    }

    public func organizationPackages(_ organization: String) async throws -> OrgPackageState {
        OrgPackageState(json: try await call("GET", "/api/plugins/" + Self.segment(organization)))
    }

    public func install(_ packageId: String, organization: String) async throws -> OrgPackageState {
        guard PackagesText.isPackageId(packageId) else { throw PackagesError("plugin_not_found") }
        let state = OrgPackageState(
            json: try await call("POST", "/api/plugins/\(try Self.segment(organization))/\(packageId)/install", body: [:]))
        try? await reportUse(packageId)
        return state
    }

    public func uninstall(_ packageId: String, organization: String) async throws -> PackageRemoval {
        guard PackagesText.isPackageId(packageId) else { throw PackagesError("plugin_not_found") }
        let org = try Self.segment(organization)
        return PackageRemoval(
            json: try await call("POST", "/api/plugins/\(org)/\(packageId)/uninstall", body: [:]), packageId: packageId,
            organization: org)
    }

    public func reportUse(_ packageId: String, market: String = "forkmesh") async throws {
        try await call(
            "POST", "/api/market/reviews/uses",
            body: ["market": .string(market), "package": .string(packageId), "source": "mobile"])
    }

    public func reportActivity(_ packageId: String, kind: PackageActivityKind, market: String = "forkmesh") async throws {
        try await call(
            "POST", "/api/market/reviews/activity",
            body: ["market": .string(market), "package": .string(packageId), "kind": .string(kind.rawValue), "source": "mobile"])
    }

    public func packageActivity(_ packageId: String, market: String = "forkmesh", page: Int = 1) async throws -> PackageActivityPage {
        PackageActivityPage(
            json: try await call(
                "GET", "/api/market/reviews/\(market)/\(packageId)/activity", query: [("page", String(max(1, page))), ("limit", "50")]))
    }

    public func reviews(_ packageId: String, market: String = "forkmesh", sort: String = "newest", page: Int = 1)
        async throws -> ReviewPage
    {
        ReviewPage(
            json: try await call(
                "GET", "/api/market/reviews/\(market)/\(packageId)",
                query: [("sort", sort), ("page", String(max(1, page))), ("limit", "20")]))
    }

    public func rate(_ packageId: String, market: String = "forkmesh", draft: ReviewDraft) async throws -> MarketReview {
        let reply = try await call("POST", "/api/market/reviews/\(market)/\(packageId)", body: draft.body)
        return MarketReview(json: reply.obj("review"))
    }

    public func setHelpful(_ reviewId: String, helpful: Bool) async throws -> (helpful: Int, mine: Bool) {
        let reply = try await call(helpful ? "POST" : "DELETE", "/api/market/reviews/items/\(try Self.item(reviewId))/helpful", body: [:])
        return (reply.int("helpful"), reply.bool("viewerFoundHelpful"))
    }

    public func reportReview(_ reviewId: String, reason: String) async throws {
        try await call(
            "POST", "/api/market/reviews/items/\(try Self.item(reviewId))/report",
            body: ["reason": .string(String(reason.prefix(500)))])
    }

    public func crmOrganizations() async throws -> [PackageOrganization] {
        PackageOrganization.list(try await call("GET", "/api/crm"))
    }

    public func crm(
        _ organization: String, since: String = "", offsetMinutes: Int = TimeZone.current.secondsFromGMT() / 60
    ) async throws -> CrmOrganizationData? {
        var query = since.isEmpty ? [] : [("since", since)]
        query.append(("tz", String(offsetMinutes)))
        let reply = try await call("GET", "/api/crm/" + Self.segment(organization), query: query)
        if reply.bool("unchanged") { return nil }
        return await Task.detached(priority: .userInitiated) { CrmOrganizationData(json: reply) }.value
    }

    public func crmContact(_ ref: CrmContactRef) async throws -> CrmContact {
        CrmContact(
            json: try await call(
                "GET", "/api/crm/\(try Self.segment(ref.organization))/contacts/\(try Self.item(ref.contactId))")
                .obj("contact"))
    }

    public func updateCrmContact(_ ref: CrmContactRef, changes: JSONObject) async throws -> CrmContact {
        CrmContact(
            json: try await call(
                "PATCH", "/api/crm/\(try Self.segment(ref.organization))/contacts/\(try Self.item(ref.contactId))",
                body: changes
            ).obj("contact"))
    }

    public func createCrmContact(_ organization: String, fields: JSONObject) async throws -> CrmContact {
        CrmContact(json: try await call("POST", "/api/crm/\(try Self.segment(organization))/contacts", body: fields).obj("contact"))
    }

    public func deleteCrmContact(_ ref: CrmContactRef) async throws {
        try await call(
            "DELETE", "/api/crm/\(try Self.segment(ref.organization))/contacts/\(try Self.item(ref.contactId))", body: [:])
    }

    public func marketingOrganizations() async throws -> [PackageOrganization] {
        PackageOrganization.list(try await call("GET", "/api/marketing"))
    }

    public func marketing(_ organization: String) async throws -> MarketingOverview {
        MarketingOverview(json: try await call("GET", "/api/marketing/" + Self.segment(organization)))
    }

    public func setContentStatus(_ organization: String, contentId: String, status: String) async throws
        -> MarketingContent
    {
        guard MarketingContent.statuses.contains(status) else { throw PackagesError("invalid_status") }
        var body: JSONObject = ["status": .string(status)]
        if status == "published" { body["publishedAt"] = .int(Int64(Date().timeIntervalSince1970 * 1000)) }
        return MarketingContent(
            json: try await call(
                "PATCH", "/api/marketing/\(try Self.segment(organization))/content/\(try Self.item(contentId))", body: body
            ).obj("content"))
    }

    public func setAdStatus(_ organization: String, adId: String, status: String) async throws -> MarketingCampaignAd {
        guard MarketingCampaignAd.statuses.contains(status) else { throw PackagesError("invalid_status") }
        return MarketingCampaignAd(
            json: try await call(
                "PATCH", "/api/marketing/\(try Self.segment(organization))/ads/\(try Self.item(adId))",
                body: ["status": .string(status)]
            ).obj("ad"))
    }

    public func analyticsOrganizations() async throws -> [PackageOrganization] {
        PackageOrganization.list(try await call("GET", "/api/analytics"))
    }

    public func analyticsProjects(_ organization: String) async throws -> [AnalyticsProject] {
        try await call("GET", "/api/analytics/\(try Self.segment(organization))/projects").maps("projects")
            .map(AnalyticsProject.init(json:)).filter { PackagesText.isHexId($0.id) }
    }

    public func analyticsOverview(_ organization: String, projectId: String, range: AnalyticsRange, nowMs: Int64)
        async throws -> AnalyticsOverview
    {
        AnalyticsOverview(
            json: try await call(
                "POST", "/api/analytics/\(try Self.segment(organization))/projects/\(try Self.item(projectId))/query",
                body: range.query(nowMs: nowMs)))
    }

    public func competitorOrganizations() async throws -> [PackageOrganization] {
        PackageOrganization.list(try await call("GET", "/api/competitors"))
    }

    public func competitors(_ organization: String) async throws -> CompetitorBoard {
        CompetitorBoard(json: try await call("GET", "/api/competitors/" + Self.segment(organization)))
    }

    public func saveCompetitor(_ organization: String, _ competitor: Competitor) async throws -> Competitor {
        let base = "/api/competitors/\(try Self.segment(organization))/items"
        if competitor.id.isEmpty {
            return Competitor(json: try await call("POST", base, body: competitor.body).obj("competitor"))
        }
        return Competitor(
            json: try await call("PATCH", base + "/" + Self.item(competitor.id), body: competitor.body).obj("competitor"))
    }

    public func markCompetitorReviewed(_ organization: String, _ competitor: Competitor, nowMs: Int64) async throws
        -> Competitor
    {
        Competitor(
            json: try await call(
                "PATCH", "/api/competitors/\(try Self.segment(organization))/items/\(try Self.item(competitor.id))",
                body: ["lastReviewedAt": .int(nowMs)]
            ).obj("competitor"))
    }

    public func deleteCompetitor(_ organization: String, id: String) async throws {
        try await call("DELETE", "/api/competitors/\(try Self.segment(organization))/items/\(try Self.item(id))", body: [:])
    }

    public func isAdReviewer() async throws -> Bool {
        try await call("GET", "/api/ads/mine").bool("isReviewer")
    }

    public func adReviewQueue(status: String = "pending") async throws -> AdReviewQueue {
        AdReviewQueue(json: try await call("GET", "/api/ads/review", query: [("status", status)]))
    }

    public func reviewAd(_ ad: OwnedAd, decision: AdReviewDecision) async throws {
        guard decision.isComplete else { throw PackagesError("reason_required") }
        try await call(
            "POST", "/api/ads/\(try Self.item(ad.id))/review", body: decision.body(contentHash: ad.contentHash))
    }

    public func ledger(after cursor: LedgerCursor? = nil) async throws -> LedgerPage {
        LedgerPage(json: try await call("GET", "/api/ledger", query: [("limit", "40")] + (cursor?.query ?? [])))
    }

    public func rewardEstimates(repository: String? = nil) async throws -> [RewardEstimate] {
        var query: [(String, String)] = []
        if let repository { query = [("ownerScope", "1"), ("repository", repository.lowercased())] }
        return try await call("GET", "/api/rewards/estimates", query: query).maps("rewards")
            .map(RewardEstimate.init(json:)).filter { !$0.rewardId.isEmpty }
    }
}
