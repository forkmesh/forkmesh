import FMApi
import FMBase
import Foundation
import XCTest

@testable import FMPackages

final class AppStoreStatusTests: XCTestCase {
    static let docExample = """
    {
      "format": "forkmesh-app-store-status-v1",
      "generatedAt": 1760000000000,
      "app": {"id": "6752000000", "bundleId": "com.forkmesh.app"},
      "attention": ["1.4.0 was rejected: Apple's message is in App Store Connect -> App Review"],
      "versions": [
        {"version": "1.4.0", "build": "19", "releaseType": "AFTER_APPROVAL",
         "createdAt": "2026-09-16T09:45:00Z",
         "status": {"state": "REJECTED", "label": "Rejected", "tone": "action"}}
      ],
      "submissions": [
        {"id": "s1", "submittedAt": "2026-10-08T18:29:00Z",
         "status": {"state": "UNRESOLVED_ISSUES", "label": "Unresolved issues", "tone": "action"},
         "items": [{"name": "1.4.0", "status": {"state": "REJECTED", "label": "Rejected", "tone": "action"}}]}
      ],
      "downloads": {
        "available": true, "note": "", "since": "2026-08",
        "total": {"downloads": 18, "redownloads": 2, "updates": 7},
        "recent": [{"days": 7, "downloads": 1, "redownloads": 0, "updates": 2},
                   {"days": 30, "downloads": 104, "redownloads": 0, "updates": 2}]
      },
      "builds": [
        {"version": "1.4.0", "build": "19", "uploadedAt": "2026-10-05T12:51:00Z", "expired": false,
         "processing": {"state": "VALID", "label": "Valid", "tone": "good"},
         "internal": {"state": "IN_BETA_TESTING", "label": "In beta testing", "tone": "good"},
         "external": {"state": "WAITING_FOR_BETA_REVIEW", "label": "Waiting for beta review", "tone": "pending"},
         "betaReview": {"state": "", "label": "Not submitted", "tone": "neutral"},
         "groups": ["forkmesh-team"]}
      ],
      "groups": [
        {"name": "outside", "internal": false, "testers": 1, "allBuilds": false,
         "publicLinkEnabled": true, "publicLink": "https://testflight.apple.com/join/x",
         "publicLinkLimit": 0, "feedbackEnabled": true}
      ],
      "feedback": [
        {"kind": "screenshot", "createdAt": "2026-09-01T03:06:00Z", "from": "t@example.com",
         "device": "iPhone16_2 27.0", "comment": "Show time as 5:45pm"}
      ],
      "reviews": [
        {"createdAt": "2026-10-01T10:00:00Z", "rating": 5, "title": "Great", "territory": "USA",
         "body": "Works well"}
      ],
      "unavailable": {"feedback": "ERROR: App Store Connect GET answered 404"}
    }
    """

    func snapshot(_ text: String = AppStoreStatusTests.docExample) throws -> AppStoreSnapshot {
        try AppStoreSnapshot.decode(Data(text.utf8))
    }

    func testDecodesTheDocumentedExample() throws {
        let snap = try snapshot()
        XCTAssertEqual(snap.format, AppStoreSnapshot.format)
        XCTAssertEqual(snap.generatedAt, 1_760_000_000_000)
        XCTAssertEqual(snap.appId, "6752000000")
        XCTAssertEqual(snap.bundleId, "com.forkmesh.app")
        XCTAssertEqual(snap.attention.count, 1)

        XCTAssertEqual(snap.versions.count, 1)
        XCTAssertEqual(snap.versions[0].version, "1.4.0")
        XCTAssertEqual(snap.versions[0].build, "19")
        XCTAssertEqual(snap.versions[0].releaseType, "AFTER_APPROVAL")
        XCTAssertEqual(snap.versions[0].status, AppStoreState(state: "REJECTED", label: "Rejected", tone: .action))

        XCTAssertEqual(snap.submissions[0].id, "s1")
        XCTAssertEqual(snap.submissions[0].status.tone, .action)
        XCTAssertEqual(snap.submissions[0].items.map(\.name), ["1.4.0"])
        XCTAssertEqual(snap.submissions[0].items[0].status.label, "Rejected")

        XCTAssertTrue(snap.downloads.available)
        XCTAssertEqual(snap.downloads.since, "2026-08")
        XCTAssertEqual(snap.downloads.total, AppStoreDownloadFigures(downloads: 18, redownloads: 2, updates: 7))
        XCTAssertEqual(snap.downloads.recent.map(\.days), [7, 30])
        XCTAssertEqual(snap.downloads.recent[1].downloads, 104)

        let build = snap.builds[0]
        XCTAssertFalse(build.expired)
        XCTAssertEqual(build.processing.tone, .good)
        XCTAssertEqual(build.internalTesting.label, "In beta testing")
        XCTAssertEqual(build.externalTesting.tone, .pending)
        XCTAssertEqual(build.betaReview, AppStoreState(state: "", label: "Not submitted", tone: .neutral))
        XCTAssertEqual(build.groups, ["forkmesh-team"])

        let group = snap.groups[0]
        XCTAssertFalse(group.isInternal)
        XCTAssertEqual(group.testers, 1)
        XCTAssertTrue(group.feedbackEnabled)
        XCTAssertEqual(group.publicURL, URL(string: "https://testflight.apple.com/join/x"))

        XCTAssertEqual(snap.feedback[0].comment, "Show time as 5:45pm")
        XCTAssertEqual(snap.feedback[0].device, "iPhone16_2 27.0")
        XCTAssertEqual(snap.reviews[0].rating, 5)
        XCTAssertEqual(snap.reviews[0].stars, "\u{2605}\u{2605}\u{2605}\u{2605}\u{2605}")
        XCTAssertEqual(snap.unavailable(.feedback), "ERROR: App Store Connect GET answered 404")
        XCTAssertNil(snap.unavailable(.reviews))
    }

    func testDecodingIsTolerant() throws {
        let snap = try snapshot("""
        {"app": "nope", "attention": ["", "  ", "Fix it", 3],
         "versions": [{"version": 2, "build": 19, "status": {"state": "IN_REVIEW", "tone": "sparkly"}}, "junk",
                      {"status": "READY_FOR_SALE"}],
         "submissions": {"not": "a list"},
         "downloads": {"available": "true", "total": [1, 2]},
         "builds": [{"expired": 1, "groups": ["a", 2, "b"]}],
         "groups": [{"name": "x", "internal": true, "testers": "4", "publicLinkEnabled": true,
                     "publicLink": "javascript:alert(1)"}],
         "reviews": [{"rating": 9}, {"rating": -2}, {"rating": 3.7}],
         "unavailable": {"builds": "", "groups": 5, "reviews": "down"},
         "extra": {"ignored": true}}
        """)
        XCTAssertEqual(snap.format, "")
        XCTAssertEqual(snap.generatedAt, 0)
        XCTAssertEqual(snap.bundleId, "")
        XCTAssertEqual(snap.attention, ["Fix it"])
        XCTAssertEqual(snap.versions.count, 2)
        XCTAssertEqual(snap.versions[0].version, "2")
        XCTAssertEqual(snap.versions[0].build, "19")
        XCTAssertEqual(snap.versions[0].status.tone, .neutral)
        XCTAssertEqual(snap.versions[0].status.text, "In review")
        XCTAssertEqual(snap.versions[1].status.state, "READY_FOR_SALE")
        XCTAssertEqual(snap.versions[1].status.text, "Ready for sale")
        XCTAssertTrue(snap.submissions.isEmpty)
        XCTAssertTrue(snap.downloads.available)
        XCTAssertEqual(snap.downloads.total, AppStoreDownloadFigures())
        XCTAssertTrue(snap.downloads.recent.isEmpty)
        XCTAssertTrue(snap.builds[0].expired)
        XCTAssertEqual(snap.builds[0].groups, ["a", "b"])
        XCTAssertTrue(snap.builds[0].processing.isEmpty)
        XCTAssertEqual(snap.groups[0].testers, 4)
        XCTAssertNil(snap.groups[0].publicURL)
        XCTAssertEqual(snap.reviews.map(\.rating), [5, 0, 3])
        XCTAssertEqual(snap.reviews[2].stars, "\u{2605}\u{2605}\u{2605}\u{2606}\u{2606}")
        XCTAssertEqual(snap.unavailable, ["reviews": "down"])
        XCTAssertTrue(snap.feedback.isEmpty)
    }

    func testEmptyObjectDecodesToDefaults() throws {
        let snap = try snapshot("{}")
        XCTAssertTrue(snap.versions.isEmpty)
        XCTAssertFalse(snap.downloads.available)
        XCTAssertTrue(snap.unavailable.isEmpty)
    }

    func testToneParsing() {
        XCTAssertEqual(AppStoreTone(raw: "action"), .action)
        XCTAssertEqual(AppStoreTone(raw: " Pending "), .pending)
        XCTAssertEqual(AppStoreTone(raw: "GOOD"), .good)
        XCTAssertEqual(AppStoreTone(raw: ""), .neutral)
        XCTAssertEqual(AppStoreTone(raw: "destructive"), .neutral)
    }

    func testResponseCarriesThePublishedStamp() throws {
        let body = "{\"ok\": true, \"actor\": \"jett\", \"published\": true, \"publishedAt\": 1760000300000, \"snapshot\": \(Self.docExample)}"
        let response = try AppStoreStatusResponse.decode(Data(body.utf8))
        XCTAssertTrue(response.published)
        XCTAssertEqual(response.publishedAt, 1_760_000_300_000)
        XCTAssertEqual(response.snapshot?.bundleId, "com.forkmesh.app")

        let iso = try AppStoreStatusResponse.decode(Data("{\"publishedAt\": \"2026-10-08T18:29:00Z\", \"snapshot\": {}}".utf8))
        XCTAssertEqual(iso.publishedAt, 1_791_484_140_000)
        XCTAssertTrue(iso.published)

        let seconds = try AppStoreStatusResponse.decode(Data("{\"publishedAt\": 1760000000, \"snapshot\": {}}".utf8))
        XCTAssertEqual(seconds.publishedAt, 1_760_000_000_000)

        let fallback = try AppStoreStatusResponse.decode(Data("{\"snapshot\": {\"generatedAt\": 1760000000000}}".utf8))
        XCTAssertEqual(fallback.publishedAt, 1_760_000_000_000)

        let empty = try AppStoreStatusResponse.decode(Data("{\"ok\": true, \"published\": false, \"publishedAt\": null, \"snapshot\": null}".utf8))
        XCTAssertFalse(empty.published)
        XCTAssertNil(empty.snapshot)
        XCTAssertEqual(empty.publishedAt, 0)
    }

    func testDecodesFromTheApiObject() throws {
        let object = JSON.parseObject("{\"ok\": true, \"publishedAt\": 1760000300000, \"snapshot\": \(Self.docExample)}")
        let response = try AppStoreStatusResponse.decode(object)
        XCTAssertEqual(response.snapshot?.versions.first?.status.tone, .action)
        XCTAssertEqual(response.snapshot?.downloads.recent.count, 2)
    }

    func testGroupsListInternalFirst() throws {
        let snap = try snapshot("""
        {"groups": [{"name": "b", "internal": false}, {"name": "a", "internal": true},
                    {"name": "c", "internal": false}, {"name": "d", "internal": true}]}
        """)
        XCTAssertEqual(snap.sortedGroups.map(\.name), ["a", "d", "b", "c"])
    }

    func testText() {
        XCTAssertEqual(AppStoreStatusText.publishedLine(0), "")
        XCTAssertEqual(AppStoreStatusText.publishedLine(1_000_000, nowMillis: 1_000_000 + 5 * 60_000), "Published 5m ago")
        XCTAssertEqual(AppStoreStatusText.publishedLine(1_000_000, nowMillis: 1_000_500), "Published just now")
        XCTAssertEqual(AppStoreStatusText.stars(0), "\u{2606}\u{2606}\u{2606}\u{2606}\u{2606}")
        XCTAssertEqual(AppStoreStatusText.humanize("WAITING_FOR_REVIEW"), "Waiting for review")
        XCTAssertEqual(AppStoreStatusText.versionTitle(version: "1.4.0", build: "19"), "1.4.0 (19)")
        XCTAssertEqual(AppStoreStatusText.versionTitle(version: "1.4.0", build: ""), "1.4.0")
        XCTAssertEqual(AppStoreStatusText.releaseType("AFTER_APPROVAL"), "Releases after approval")
        XCTAssertEqual(AppStoreStatusText.releaseType("SOMETHING_NEW"), "Something new")
        XCTAssertEqual(AppStoreStatusText.spanTitle(AppStoreDownloadFigures(days: 7)), "Last 7 days")
        XCTAssertEqual(AppStoreStatusText.totalTitle(AppStoreDownloads(since: "2026-08")), "Total since 2026-08")
        XCTAssertEqual(AppStoreStatusText.totalTitle(AppStoreDownloads()), "Total")
        XCTAssertEqual(AppStoreStatusText.testers(1), "1 tester")
        XCTAssertEqual(AppStoreStatusText.testers(3), "3 testers")
        XCTAssertEqual(AppStoreState().text, "Unknown")
        XCTAssertNotNil(AppStoreStatusText.parseDate("2026-10-08T18:29:00Z"))
        XCTAssertNotNil(AppStoreStatusText.parseDate("2026-10-08T18:29:00.123-07:00"))
        XCTAssertNil(AppStoreStatusText.parseDate("yesterday"))
        XCTAssertEqual(AppStoreStatusText.date("yesterday"), "yesterday")
        let utc = TimeZone(identifier: "UTC")!
        let formatted = AppStoreStatusText.date("2026-10-08T18:29:00Z", timeZone: utc, locale: Locale(identifier: "en_US_POSIX"))
        XCTAssertTrue(formatted.contains("2026"), formatted)
        XCTAssertTrue(formatted.contains("Oct"), formatted)
    }

    func testRecognisesTheStatusShortcut() {
        XCTAssertTrue(AppStoreStatusText.isStatusShortcut(file: "ios-app-store-status.sh"))
        XCTAssertTrue(AppStoreStatusText.isStatusShortcut(file: ".forkmesh/shortcuts/ios-app-store-status.sh"))
        XCTAssertFalse(AppStoreStatusText.isStatusShortcut(file: "deploy-native-ios-app-store-mac.sh"))
        XCTAssertFalse(AppStoreStatusText.isStatusShortcut(file: ""))
    }

    func testErrorMessages() {
        XCTAssertEqual(
            AppStoreStatusText.message(for: OrgApiException("plugin_not_installed", "plugin not installed")),
            AppStoreStatusText.notInstalled)
        XCTAssertEqual(AppStoreStatusText.message(for: OrgApiException("unauthorized", "unauthorized")), "unauthorized")
        XCTAssertEqual(AppStoreStatusText.message(for: URLError(.timedOut)), AppStoreStatusText.genericError)
    }

    @MainActor func testModelLoadsRefreshesAndKeepsTheSnapshotOnError() async {
        var answers: [Result<JSONObject, Error>] = [
            .success(JSON.parseObject("{\"ok\": true, \"published\": false, \"snapshot\": null}")),
            .success(JSON.parseObject("{\"ok\": true, \"publishedAt\": 1760000300000, \"snapshot\": \(Self.docExample)}")),
            .failure(OrgApiException("http_502", "The server answered 502.")),
        ]
        let model = AppStoreStatusModel(api: AppStoreStatusAPI(fetch: { try answers.removeFirst().get() }))
        XCTAssertTrue(model.showsSpinner)

        await model.load()
        XCTAssertTrue(model.showsEmpty)
        XCTAssertFalse(model.showsSpinner)
        XCTAssertNil(model.snapshot)

        await model.load()
        XCTAssertFalse(model.showsEmpty)
        XCTAssertEqual(model.snapshot?.bundleId, "com.forkmesh.app")
        XCTAssertEqual(model.response?.publishedAt, 1_760_000_300_000)

        await model.load()
        XCTAssertEqual(model.snapshot?.bundleId, "com.forkmesh.app")
        XCTAssertFalse(model.showsError)
        XCTAssertEqual(model.refreshError, "The server answered 502.")
        model.dismissError()
        XCTAssertEqual(model.refreshError, "")
        XCTAssertFalse(model.loading)
    }

    @MainActor func testModelShowsTheFirstLoadError() async {
        let model = AppStoreStatusModel(api: AppStoreStatusAPI(fetch: {
            throw OrgApiException("plugin_not_installed", "plugin not installed")
        }))
        await model.load()
        XCTAssertTrue(model.showsError)
        XCTAssertFalse(model.showsSpinner)
        XCTAssertEqual(model.error, AppStoreStatusText.notInstalled)
    }
}
