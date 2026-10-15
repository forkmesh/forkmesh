import FMBase
import XCTest

@testable import FMPackages

final class MarketingAppStoreTests: XCTestCase {
    let utc = TimeZone(identifier: "UTC")!
    let published: Int64 = 1_791_000_000_000

    func days(from first: String, count: Int) -> [JSONValue] {
        (0..<count).map { index in
            let day = MarketingAppStore.shift(first, by: index)!
            return .object(["day": .string(day), "downloads": .int(Int64(index + 1)), "redownloads": 1, "updates": 2])
        }
    }

    func appStoreJSON() -> JSONObject {
        [
            "app": ["id": "6803060735", "name": "ForkMesh", "bundleId": "com.forkmesh.forkmesh"],
            "lookup": [
                "appId": "6803060735", "country": "US", "name": "ForkMesh", "seller": "ForkMesh Inc", "version": "2.4",
                "releasedAt": 1_790_000_000_000, "firstReleasedAt": 1_780_000_000_000, "genre": "Developer Tools", "price": "Free",
                "minimumOs": "17.0", "rating": 4.6, "ratings": 12, "versionRating": 4.8, "versionRatings": 5,
                "url": "https://apps.apple.com/us/app/forkmesh/id6803060735", "lookedUpAt": 1_791_000_000_000,
            ],
            "versions": [
                ["version": "2.4", "build": "120", "status": ["state": "READY_FOR_SALE", "label": "Ready for sale", "tone": "good"],
                 "createdAt": 1_790_000_000_000],
                ["version": "2.5", "status": ["state": "WAITING_FOR_REVIEW", "tone": "bogus"]],
                "not a version",
            ],
            "reviews": [["createdAt": 1_790_500_000_000, "rating": 9, "title": "Great", "territory": "USA", "body": "Works offline"]],
            "publishedAt": .int(published), "publishedBy": "alice", "fresh": true,
            "firstDay": "2026-09-01", "lastDay": "2026-10-14",
            "days": .array(days(from: "2026-09-01", count: 44) + [.object(["day": "garbage", "downloads": 99])]),
            "total": ["downloads": 990, "redownloads": 44, "updates": "88"],
            "bestDay": ["day": "2026-10-14", "downloads": 44],
            "windows": [
                ["days": 7, "from": "2026-10-08", "to": "2026-10-14", "downloads": 287, "redownloads": 7, "updates": 14,
                 "previous": ["downloads": 238, "redownloads": 7, "updates": 14], "comparable": true,
                 "territories": [["code": "US", "downloads": 200], ["code": "DE", "downloads": 87]], "territoryCount": 2,
                 "devices": [["name": "iPhone", "downloads": 250], ["name": "iPad", "downloads": 37]]],
                ["days": 30, "from": "2026-09-15", "to": "2026-10-14", "downloads": 885, "redownloads": 30, "updates": 60,
                 "previous": ["downloads": 105], "comparable": false, "territories": [["code": "US", "downloads": 30]],
                 "territoryCount": 2, "devices": [["name": "iPhone", "downloads": 60]]],
            ],
        ]
    }

    func testDecodesThePayloadLeniently() throws {
        let store = try MarketingAppStore.decode(appStoreJSON())
        XCTAssertEqual(store.app.id, "6803060735")
        XCTAssertEqual(store.app.bundleId, "com.forkmesh.forkmesh")
        XCTAssertEqual(store.lookup.rating, 4.6)
        XCTAssertEqual(store.lookup.ratings, 12)
        XCTAssertEqual(store.lookup.releasedAt, 1_790_000_000_000)
        XCTAssertEqual(store.lookup.price, "Free")
        XCTAssertNotNil(store.lookup.link)
        XCTAssertEqual(store.ratingText, "4.6\u{2605}")
        XCTAssertEqual(store.versions.count, 2)
        XCTAssertEqual(store.versions[0].title, "2.4 (120)")
        XCTAssertEqual(store.versions[0].status.tone, .good)
        XCTAssertEqual(store.versions[1].status.tone, .neutral)
        XCTAssertEqual(store.versions[1].status.text, "Waiting for review")
        XCTAssertEqual(store.reviews.first?.rating, 5)
        XCTAssertEqual(store.reviews.first?.territory, "USA")
        XCTAssertEqual(store.days.count, 44)
        XCTAssertEqual(store.days.last?.day, "2026-10-14")
        XCTAssertEqual(store.total, MarketingAppStoreCounts(downloads: 990, redownloads: 44, updates: 88))
        XCTAssertEqual(store.bestDay?.downloads, 44)
        XCTAssertEqual(store.windows.count, 2)
        XCTAssertEqual(store.window(days: 7)?.territories.map(\.name), ["US", "DE"])
        XCTAssertEqual(store.window(days: 7)?.devices.first?.name, "iPhone")
        XCTAssertEqual(store.window(days: 30)?.previous.redownloads, 0)
        XCTAssertNil(store.window(days: 90))
        XCTAssertFalse(store.isEmpty)
        XCTAssertEqual(store.sentLine(now: published + 3 * 86_400_000), "Sent from App Store Connect 3d ago by alice")
    }

    func testEmptyAndMissingKeysDefault() throws {
        let store = try MarketingAppStore.decode(["lookup": [:], "bestDay": .null, "windows": "nope", "days": [["day": 5]]])
        XCTAssertTrue(store.isEmpty)
        XCTAssertTrue(store.lookup.isEmpty)
        XCTAssertNil(store.bestDay)
        XCTAssertTrue(store.windows.isEmpty)
        XCTAssertTrue(store.days.isEmpty)
        XCTAssertEqual(store.ratingText, "")
        XCTAssertEqual(store.sentLine(now: published), "")
        XCTAssertTrue(store.chartDays(range: 30).isEmpty)
        XCTAssertTrue(MarketingAppStore.emptyText.hasPrefix("No downloads yet. Run native/ios/tools/asc_store_report.py --publish"))
        let reply = MarketingAppStoreReply(organization: "acme", json: ["ok": true])
        XCTAssertFalse(reply.canPublish)
        XCTAssertTrue(reply.appStore.isEmpty)
    }

    func testHelpersPickWindowsChangesAndChartDays() throws {
        let store = try MarketingAppStore.decode(appStoreJSON())
        let week = try XCTUnwrap(store.window(days: 7))
        XCTAssertEqual(try XCTUnwrap(week.downloadsChange), 20.588, accuracy: 0.01)
        XCTAssertEqual(week.downloadsChangeText, "+20.6%")
        XCTAssertNil(store.window(days: 30)?.downloadsChange)
        XCTAssertEqual(store.window(days: 30)?.downloadsChangeText, "")
        XCTAssertEqual(MarketingAppStore.percentChange(50, 100, comparable: true), -50)
        XCTAssertNil(MarketingAppStore.percentChange(50, 0, comparable: true))
        XCTAssertNil(MarketingAppStore.percentChange(50, 100, comparable: false))

        let month = store.chartDays(range: 30)
        XCTAssertEqual(month.count, 30)
        XCTAssertEqual(month.first?.day, "2026-09-15")
        XCTAssertEqual(month.last?.day, "2026-10-14")
        XCTAssertEqual(store.chartDays(range: 7).first?.day, "2026-10-08")
        XCTAssertEqual(store.chartDays(range: 90).count, 44)
        XCTAssertEqual(MarketingAppStore.shift("2026-03-01", by: -1), "2026-02-28")
        XCTAssertEqual(MarketingAppStore.shift("2026-12-31", by: 1), "2027-01-01")
        XCTAssertNil(MarketingAppStore.shift("2026-13-01", by: 1))
        XCTAssertEqual(MarketingAppStore.ranges.map(MarketingAppStore.rangeTitle), ["30D", "90D", "365D"])
    }

    func testChannelSummaryCarriesAppStoreCounts() {
        let stats = MarketingChannelStats(
            json: ["channel": "app-store", "downloads": 885, "redownloads": 30, "updates": 60, "ratings": 12, "rating": 4.6,
                   "health": ["state": "good", "source": "app-store-connect"]],
            settings: [:])
        XCTAssertTrue(stats.isAppStore)
        XCTAssertEqual(stats.downloads, 885)
        XCTAssertEqual(stats.redownloads, 30)
        XCTAssertEqual(stats.updates, 60)
        XCTAssertEqual(stats.ratings, 12)
        XCTAssertEqual(stats.source, "app-store-connect")

        let overview = MarketingOverview(json: [
            "organization": "acme", "summary": ["channels": [["channel": "app-store", "downloads": 885, "ratings": 12, "rating": 4.6]]],
        ])
        let draft = MarketingForms.metricDraft(overview, channel: "app-store", zone: utc)
        XCTAssertEqual(draft.fields.prefix(4).map(\.key), ["downloads", "redownloads", "updates", "ratings"])
        XCTAssertEqual(draft.original.int64("downloads"), 885)
        XCTAssertEqual(draft.original.int64("ratings"), 12)
    }

    @MainActor func testModelLoadsTheAppStoreForTheOrganization() async throws {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/marketing", ["organizations": [["name": "acme", "installed": true]]])
        transport.on("GET", "/api/marketing/acme", ["organization": "acme", "role": "admin"])
        transport.on("GET", "/api/marketing/acme/app-store", ["ok": true, "canPublish": true, "appStore": .object(appStoreJSON())])
        let model = MarketingModel(client: PackagesClient(transport: transport), zone: utc, clock: { 1_000 })
        await model.refresh()
        XCTAssertNil(model.appStore)
        await model.loadAppStore()
        XCTAssertEqual(transport.paths.last, "GET /api/marketing/acme/app-store")
        XCTAssertEqual(model.appStoreReply?.canPublish, true)
        XCTAssertEqual(model.appStore?.lastDay, "2026-10-14")
        XCTAssertEqual(model.appStoreChartDays.count, 30)
        XCTAssertEqual(model.appStoreWindow?.days, 30)
        model.appStoreRange = 7
        XCTAssertEqual(model.appStoreChartDays.count, 7)
        XCTAssertEqual(model.appStoreWindow?.downloads, 287)

        transport.routes["GET /api/marketing/acme/app-store"] = [PackagesReply(status: 403, json: ["error": "forbidden"])]
        await model.loadAppStore()
        XCTAssertFalse(model.errorText.isEmpty)
        XCTAssertEqual(model.appStore?.lastDay, "2026-10-14")

        do {
            _ = try await PackagesClient(transport: transport).appStore(org: "Bad Org")
            XCTFail("organization names are checked")
        } catch {}
    }
}
