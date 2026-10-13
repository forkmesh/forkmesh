import FMBase
import XCTest

@testable import FMPackages

final class ReviewTests: XCTestCase {
    let problem = "The contact import stopped halfway through my CSV file and dropped rows."
    let improvement = "Skip blank lines instead of stopping, and show which rows failed to import."

    func testFiveStarsNeedNoReview() {
        XCTAssertTrue(ReviewDraft(stars: 5).issues(ReviewRules()).isEmpty)
    }

    func testBelowFiveRequiresBothFields() {
        let issues = ReviewDraft(stars: 3).issues(ReviewRules())
        XCTAssertEqual(issues["problem"], "required")
        XCTAssertEqual(issues["improvement"], "required")
    }

    func testShortAndWordyChecks() {
        var draft = ReviewDraft(stars: 2, problem: "Too short", improvement: improvement)
        XCTAssertEqual(draft.issues(ReviewRules())["problem"], "too_short")
        draft.problem = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa bbbbbbbb"
        XCTAssertEqual(draft.issues(ReviewRules())["problem"], "too_few_words")
        draft.problem = problem
        XCTAssertTrue(draft.issues(ReviewRules()).isEmpty)
        draft.improvement = "  " + problem.uppercased() + " "
        XCTAssertEqual(draft.issues(ReviewRules())["improvement"], "same_as_problem")
    }

    func testTooLongOnAnyField() {
        let draft = ReviewDraft(stars: 5, comment: String(repeating: "x", count: 4001))
        XCTAssertEqual(draft.issues(ReviewRules())["comment"], "too_long")
    }

    func testBodyTrimsAndClamps() {
        let body = ReviewDraft(stars: 9, problem: "  p  ", category: "bug").body
        XCTAssertEqual(body.int("stars"), 5)
        XCTAssertEqual(body.str("problem"), "p")
        XCTAssertEqual(body.str("category"), "bug")
        XCTAssertFalse(body.contains("comment"))
    }

    func testRulesFromServer() {
        let rules = ReviewRules(json: ["reviewRequiredBelow": 4, "minChars": 10, "categories": [["id": "ux", "label": "UX"]]])
        XCTAssertEqual(rules.reviewRequiredBelow, 4)
        XCTAssertEqual(rules.categories.map(\.id), ["ux"])
        XCTAssertTrue(ReviewDraft(stars: 4).issues(rules).isEmpty)
    }

    func testPageAndViewer() {
        let page = ReviewPage(json: [
            "summary": ["average": 3.5, "count": 2],
            "reviews": [[
                "id": .string(hexA), "stars": 2, "reviewer": ["name": "alice", "displayName": "Alice"],
                "problem": .string(problem), "reply": ["text": "Fixed", "by": "owner", "at": 5], "addressedIn": "v1.14",
            ]],
            "total": 2, "hasMore": true,
            "viewer": ["signedIn": true, "canRate": false, "reason": "package_use_required"],
        ])
        XCTAssertEqual(page.reviews.first?.reviewerLabel, "Alice")
        XCTAssertEqual(page.reviews.first?.reply?.text, "Fixed")
        XCTAssertEqual(page.viewer.cannotRateReason, "Install or use the package before rating it.")
    }

    @MainActor func testSubmitShowsServerFieldErrors() async {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/market/reviews/forkmesh/crm", ["viewer": ["signedIn": true, "canRate": true]])
        transport.on(
            "POST", "/api/market/reviews/forkmesh/crm", status: 400,
            ["error": "review_invalid", "fields": ["problem": "repetitive"]])
        let model = PackageReviewsModel(client: PackagesClient(transport: transport), packageId: "crm")
        await model.refresh()
        model.draft = ReviewDraft(stars: 2, problem: problem, improvement: improvement)
        XCTAssertTrue(model.canSubmit)
        let ok = await model.submit()
        XCTAssertFalse(ok)
        XCTAssertEqual(model.fieldIssues["problem"], "repetitive")
        XCTAssertEqual(model.errorText, "Some review fields need more detail.")
    }

    @MainActor func testOwnReviewPrefillsDraft() async {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/market/reviews/forkmesh/crm", [
            "viewer": ["signedIn": true, "canRate": true, "review": ["id": .string(hexB), "stars": 4, "problem": "p"]],
        ])
        let model = PackageReviewsModel(client: PackagesClient(transport: transport), packageId: "crm")
        await model.refresh()
        XCTAssertEqual(model.draft.stars, 4)
        XCTAssertEqual(model.draft.problem, "p")
    }

    func testActivityTotalsAndViewerCounts() {
        let page = ReviewPage(json: [
            "activity": ["views": 12, "opens": 30, "people": 4],
            "viewer": ["signedIn": true, "isMaintainer": true, "views": 3, "opens": 7],
        ])
        XCTAssertEqual(page.activity, PackageActivityTotals(views: 12, opens: 30, people: 4))
        XCTAssertEqual(page.viewer.views, 3)
        XCTAssertEqual(page.viewer.opens, 7)
        XCTAssertTrue(page.viewer.canSeePeople)
        XCTAssertEqual(ReviewPage(json: [:]).activity, PackageActivityTotals())
        XCTAssertFalse(ReviewPage(json: [:]).viewer.canSeePeople)
    }

    @MainActor func testActivityIsReportedAndThrottledPerPackageAndKind() async throws {
        var now: Int64 = 1_000
        let activity = PackageActivity(clock: { now })
        XCTAssertTrue(activity.shouldReport("crm", kind: .open))
        XCTAssertFalse(activity.shouldReport("crm", kind: .open))
        XCTAssertTrue(activity.shouldReport("crm", kind: .view))
        XCTAssertTrue(activity.shouldReport("notes", kind: .open))
        now += PackageActivity.window
        XCTAssertTrue(activity.shouldReport("crm", kind: .open))

        let transport = FakePackagesTransport()
        transport.on("POST", "/api/market/reviews/activity", ["ok": true, "views": 1, "opens": 0])
        try await PackagesClient(transport: transport).reportActivity("crm", kind: .view)
        let body = transport.last("POST", "/api/market/reviews/activity")?.body
        XCTAssertEqual(body?.str("package"), "crm")
        XCTAssertEqual(body?.str("kind"), "view")
        XCTAssertEqual(body?.str("source"), "mobile")
        XCTAssertEqual(body?.str("market"), "forkmesh")
    }

    @MainActor func testMaintainersLoadWhoOpenedThePackage() async {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/market/reviews/forkmesh/crm", ["viewer": ["signedIn": true, "isAdmin": true]])
        transport.on("GET", "/api/market/reviews/forkmesh/crm/activity", [
            "totals": ["views": 4, "opens": 2, "people": 2], "hasMore": false,
            "people": [["name": "alice", "views": 3, "opens": 2, "firstAt": 1, "lastAt": 9], ["name": "bob", "views": 1]],
        ])
        let model = PackageReviewsModel(client: PackagesClient(transport: transport), packageId: "crm")
        await model.refresh()
        await model.loadPeople()
        XCTAssertEqual(model.people?.people.map(\.name), ["alice", "bob"])
        XCTAssertEqual(model.people?.people.first?.opens, 2)
        XCTAssertEqual(model.people?.totals.people, 2)
    }

    @MainActor func testPeopleStayHiddenFromOrdinaryViewers() async {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/market/reviews/forkmesh/crm", ["viewer": ["signedIn": true, "canRate": true]])
        let model = PackageReviewsModel(client: PackagesClient(transport: transport), packageId: "crm")
        await model.refresh()
        await model.loadPeople()
        XCTAssertNil(model.people)
        XCTAssertNil(transport.last("GET", "/api/market/reviews/forkmesh/crm/activity"))
    }

    @MainActor func testQuickRateSubmitsFiveStarsAtOnce() async {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/market/reviews/forkmesh/crm", ["viewer": ["signedIn": true, "canRate": true]])
        transport.on("POST", "/api/market/reviews/forkmesh/crm", ["review": ["id": .string(hexA), "stars": 5]])
        let model = PackageReviewsModel(client: PackagesClient(transport: transport), packageId: "crm")
        await model.refresh()
        let outcome = await model.quickRate(5)
        XCTAssertEqual(outcome, .submitted)
        XCTAssertEqual(transport.last("POST", "/api/market/reviews/forkmesh/crm")?.body?.int("stars"), 5)
    }

    @MainActor func testQuickRateBelowFiveAsksForTheReview() async {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/market/reviews/forkmesh/crm", ["viewer": ["signedIn": true, "canRate": true]])
        let model = PackageReviewsModel(client: PackagesClient(transport: transport), packageId: "crm")
        await model.refresh()
        let outcome = await model.quickRate(3)
        XCTAssertEqual(outcome, .needsReview)
        XCTAssertEqual(model.draft.stars, 3)
        XCTAssertNil(transport.last("POST", "/api/market/reviews/forkmesh/crm"))
    }

    @MainActor func testQuickRateKeepsAnExistingWrittenReview() async {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/market/reviews/forkmesh/crm", [
            "viewer": ["signedIn": true, "canRate": true, "review": [
                "id": .string(hexB), "stars": 2, "problem": .string(problem), "improvement": .string(improvement),
            ]],
        ])
        transport.on("POST", "/api/market/reviews/forkmesh/crm", ["review": ["id": .string(hexB), "stars": 3]])
        let model = PackageReviewsModel(client: PackagesClient(transport: transport), packageId: "crm")
        await model.refresh()
        let outcome = await model.quickRate(3)
        XCTAssertEqual(outcome, .submitted)
        let body = transport.last("POST", "/api/market/reviews/forkmesh/crm")?.body
        XCTAssertEqual(body?.int("stars"), 3)
        XCTAssertEqual(body?.str("problem"), problem)
    }

    @MainActor func testQuickRateIsRefusedWhenTheViewerCannotRate() async {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/market/reviews/forkmesh/crm", ["viewer": ["signedIn": false]])
        let model = PackageReviewsModel(client: PackagesClient(transport: transport), packageId: "crm")
        await model.refresh()
        let outcome = await model.quickRate(5)
        XCTAssertEqual(outcome, .failed)
        XCTAssertNil(transport.last("POST", "/api/market/reviews/forkmesh/crm"))
    }
}
