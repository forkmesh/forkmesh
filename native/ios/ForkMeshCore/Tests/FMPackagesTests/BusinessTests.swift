import FMBase
import XCTest

@testable import FMPackages

final class BusinessTests: XCTestCase {
    @MainActor func testMarketingOverviewAndQuickEdits() async throws {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/marketing", ["organizations": [["name": "acme", "installed": true]]])
        transport.on("GET", "/api/marketing/acme", [
            "organization": "acme", "role": "member",
            "content": [
                ["id": .string(hexA), "channel": "x", "status": "draft", "title": "Launch"],
                ["id": .string(hexB), "channel": "blog", "status": "queued", "title": "Post"],
            ],
            "ads": [[
                "id": .string(hexA), "channel": "google-ads", "status": "active", "name": "Search", "currency": "USD",
                "budgetCents": 10000, "spendCents": 2550, "impressions": 1000, "clicks": 25, "pacing": ["state": "under"],
            ]],
            "channelCatalog": [["id": "x", "name": "X", "category": "Social networks"]],
            "summary": ["published": 0, "queued": 1, "channels": [["channel": "x", "reach": 7]]],
        ])
        let client = PackagesClient(transport: transport)
        let model = OrgPackageModel<MarketingOverview>(
            organizations: { try await client.marketingOrganizations() }, load: { try await client.marketing($0) })
        await model.refresh()
        let overview = try XCTUnwrap(model.payload)
        XCTAssertEqual(overview.plan.map(\.status), ["draft", "queued"])
        XCTAssertEqual(overview.channelName("x"), "X")
        XCTAssertEqual(overview.channelName("blog"), "blog")
        XCTAssertEqual(overview.activeAds.first?.spendLabel, "25.50 USD of 100.00 USD")
        XCTAssertEqual(overview.activeAds.first?.clickRate ?? 0, 0.025, accuracy: 0.0001)

        transport.on("PATCH", "/api/marketing/acme/content/" + hexA, ["content": ["id": .string(hexA), "status": "published", "title": "Launch"]])
        let updated = try await client.setContentStatus("acme", contentId: hexA, status: "published")
        model.update { $0.replace(updated) }
        XCTAssertEqual(model.payload?.content.first?.status, "published")
        XCTAssertNotNil(transport.last("PATCH", "/api/marketing/acme/content/" + hexA)?.body?["publishedAt"])
        do {
            _ = try await client.setContentStatus("acme", contentId: hexA, status: "bogus")
            XCTFail("expected a refusal")
        } catch {}
    }

    func testMarketingChannelHealthAndCaptureLink() {
        let overview = MarketingOverview(json: [
            "settings": ["channels": [["id": "x", "profileUrl": "https://x.com/acme"], ["id": "reddit", "profileUrl": ""]]],
            "summary": ["channels": [
                ["channel": "x", "followers": 450, "rating": 4.5, "health": [
                    "state": "attention", "followersChange": -50, "engagementRate": 2.5, "postsLast30d": 3,
                    "checks": [["id": "link", "label": "Channel link added", "ok": true],
                               ["id": "growing", "label": "Audience growing over 30 days", "ok": false]],
                ]],
                ["channel": "reddit", "health": ["state": "setup"]],
                ["channel": "google-search", "visits": 6, "indexedPages": 1230, "health": ["state": "good"]],
                ["channel": "email", "outbound": 42, "closedDeals": 3, "closedValue": 2000, "health": ["state": "good"]],
                ["channel": "github", "followers": 7, "stars": 130, "forks": 9, "health": ["state": "good", "starsChange": 30]],
            ]],
        ])
        let x = overview.channels[0]
        XCTAssertEqual(x.healthTitle, "Needs attention")
        XCTAssertEqual(x.followersChange, -50)
        XCTAssertEqual(x.missingChecks, ["Audience growing over 30 days"])
        XCTAssertTrue(x.canCapture)
        XCTAssertEqual(x.captureURL(organization: "acme")?.absoluteString,
                       "https://app.forkmesh.com/dashboard/marketing?org=acme&page=channels&capture=x")
        XCTAssertFalse(overview.channels[1].canCapture)
        XCTAssertNil(overview.channels[1].followersChange)
        XCTAssertTrue(overview.channels[2].isGoogle)
        XCTAssertFalse(x.isGoogle)
        XCTAssertEqual(overview.channels[2].visits, 6)
        XCTAssertEqual(overview.channels[2].indexedPages, 1230)
        XCTAssertTrue(overview.channels[3].isEmail)
        XCTAssertEqual(overview.channels[3].outbound, 42)
        XCTAssertEqual(overview.channels[3].closedDeals, 3)
        XCTAssertEqual(overview.channels[3].closedValue, 2000)
        XCTAssertFalse(overview.channels[3].canCapture)
        XCTAssertTrue(overview.channels[4].isGitHub)
        XCTAssertFalse(x.isGitHub)
        XCTAssertEqual(overview.channels[4].stars, 130)
        XCTAssertEqual(overview.channels[4].forks, 9)
        XCTAssertEqual(overview.channels[4].starsChange, 30)
        XCTAssertNil(x.starsChange)
    }

    func testMoneyText() {
        XCTAssertEqual(MoneyText.cents(5, "EUR"), "0.05 EUR")
        XCTAssertEqual(MoneyText.cents(123456, ""), "1234.56")
    }

    @MainActor func testCompetitorsSaveAndReview() async throws {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/competitors/acme", [
            "organization": "acme",
            "competitors": [
                ["id": .string(hexA), "status": "primary", "name": "Rival", "lastReviewedAt": 0],
                ["id": .string(hexB), "status": "archived", "name": "Old"],
            ],
        ])
        let client = PackagesClient(transport: transport)
        let board = try await client.competitors("acme")
        XCTAssertEqual(board.grouped().map(\.status), ["primary", "archived"])
        XCTAssertTrue(board.competitors[0].needsReview(nowMs: 1))
        XCTAssertFalse(board.competitors[1].needsReview(nowMs: 1))

        var fresh = Competitor(name: " New ")
        fresh.website = "http://insecure.test"
        XCTAssertFalse(fresh.websiteIsValid)
        fresh.website = "https://new.test"
        transport.on("POST", "/api/competitors/acme/items", ["competitor": ["id": .string(hexB), "name": "New"]])
        let saved = try await client.saveCompetitor("acme", fresh)
        XCTAssertEqual(saved.id, hexB)
        XCTAssertEqual(transport.last("POST", "/api/competitors/acme/items")?.body?.str("name"), "New")
        transport.on("PATCH", "/api/competitors/acme/items/" + hexA, ["competitor": ["id": .string(hexA), "lastReviewedAt": 99]])
        let reviewed = try await client.markCompetitorReviewed("acme", board.competitors[0], nowMs: 99)
        XCTAssertEqual(reviewed.lastReviewedAt, 99)
    }

    @MainActor func testAnalyticsOverviewQuery() async throws {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/analytics/acme/projects", ["projects": [["id": .string(hexA), "siteDefault": true, "usage": ["events": 12]]]])
        transport.on("POST", "/api/analytics/acme/projects/\(hexA)/query", [
            "kind": "overview", "truncated": false,
            "result": [
                "totals": ["visitors": 10, "pageviews": 30, "sessions": 12, "bounceRate": 0.25],
                "previous": ["visitors": 5],
                "timeseries": ["labels": [1, 2], "series": [["name": "Visitors", "data": [4, 6], "total": 10]]],
                "breakdowns": ["pages": [["value": "/", "count": 20]], "referrers": [["value": "(direct)", "count": 9]]],
            ],
        ])
        let client = PackagesClient(transport: transport)
        let projects = try await client.analyticsProjects("acme")
        XCTAssertEqual(projects.first?.name, "Website")
        let overview = try await client.analyticsOverview("acme", projectId: hexA, range: .week, nowMs: 1_000_000_000)
        XCTAssertEqual(overview.totals.visitors, 10)
        XCTAssertEqual(AnalyticsTotals.change(overview.totals.visitors, overview.previous.visitors), 1.0)
        XCTAssertNil(AnalyticsTotals.change(1, 0))
        XCTAssertEqual(overview.series.first?.data, [4, 6])
        XCTAssertEqual(overview.pages.first?.value, "/")
        let body = transport.last("POST", "/api/analytics/acme/projects/\(hexA)/query")?.body
        XCTAssertEqual(body?.str("kind"), "overview")
        XCTAssertEqual(body?.int64("to"), 1_000_000_000)
        XCTAssertEqual(body?.int64("from"), 1_000_000_000 - 7 * 86_400_000)
    }

    @MainActor func testAdsReviewer() async {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/ads/mine", ["isReviewer": true])
        transport.on("GET", "/api/ads/review", [
            "status": "pending",
            "ads": [["id": .string(hexB), "status": "pending", "contentHash": "h1", "owner": "bob"]],
            "counts": ["pending": 1], "prohibited": [["id": "scam", "label": "Scams"]],
        ])
        transport.on("POST", "/api/ads/\(hexB)/review", ["ok": true])
        let model = AdsModel(client: PackagesClient(transport: transport))
        await model.refresh()
        XCTAssertTrue(model.isReviewer)
        XCTAssertTrue(model.checked)
        XCTAssertEqual(model.queue?.ads.first?.reviewActions, ["approve", "reject"])
        XCTAssertEqual(model.queue?.counts["pending"], 1)
        XCTAssertEqual(OwnedAd(json: ["id": .string(hexA), "delivery": "paused"]).deliveryText, "Paused")
        XCTAssertEqual(OwnedAd(json: ["id": .string(hexA), "delivery": "needs-membership"]).deliveryText, "Not running")

        let ad = model.queue!.ads[0]
        let refused = await model.review(ad, decision: AdReviewDecision(action: "reject"))
        XCTAssertFalse(refused)
        let approved = await model.review(ad, decision: AdReviewDecision(action: "approve"))
        XCTAssertTrue(approved)
        XCTAssertEqual(transport.last("POST", "/api/ads/\(hexB)/review")?.body?.str("contentHash"), "h1")
        XCTAssertTrue(model.queue?.ads.isEmpty ?? false)
    }

    @MainActor func testNonReviewerNeverLoadsQueue() async {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/ads/mine", ["ads": [], "isReviewer": false])
        let model = AdsModel(client: PackagesClient(transport: transport))
        await model.refresh()
        XCTAssertTrue(model.checked)
        XCTAssertFalse(model.isReviewer)
        XCTAssertFalse(transport.paths.contains("GET /api/ads/review"))
        let ok = await model.review(OwnedAd(json: ["id": .string(hexA), "status": "pending"]), decision: AdReviewDecision(action: "approve"))
        XCTAssertFalse(ok)
    }

    func testReviewDecisionBody() {
        let decision = AdReviewDecision(action: "reject", reason: "  misleading ", codes: ["scam", "scam", "adult"])
        XCTAssertTrue(decision.isComplete)
        let body = decision.body(contentHash: "h")
        XCTAssertEqual(body.str("reason"), "misleading")
        XCTAssertEqual(body.strings("codes"), ["adult", "scam"])
        XCTAssertNil(body["contentHash"])
    }
}
