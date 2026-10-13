// PingSettings, the ping-settings API calls (`pingSettings` / `savePingSettings`) and
// PingSettingsModel's optimistic save.

import FMBase
import FMNet
import Foundation
import XCTest
@testable import FMApi

private struct Refused: Error, CustomStringConvertible {
    var description: String { "forbidden" }
}

@MainActor
final class PingSettingsTests: XCTestCase {
    // MARK: Parsing

    func testCodePushesAreOnUnlessTheRelaySaysOff() {
        // A relay that predates code-push pings leaves the field out: on, like every other kind.
        XCTAssertTrue(PingSettings(json: JSONObject()).codePushes)
        XCTAssertTrue(PingSettings(json: ["codePushes": true]).codePushes)
        XCTAssertFalse(PingSettings(json: ["codePushes": false]).codePushes)
    }

    func testTheWireShapeIsReadTolerantly() {
        let settings = PingSettings(json: [
            "emailNotifications": false,
            "notificationPreferences": ["mention": false, "junk": "yes"],
            "notificationDelivery": "instant",
            "notificationPauseUntil": 1_700_000_000_000,
            "pingMutedKinds": ["tip_sent", " ", "host_online"],
            "notificationQuietHours": [
                "enabled": true, "start": 1320, "end": 9999, "days": [6, 0, 9, 0], "timezone": "Europe/Berlin",
            ],
            "notificationQuietUntil": "1700000000500",
            "pingKindCatalog": [
                ["key": "g", "label": "Group", "kinds": [["kind": "mention", "email": true], ["label": "no kind"]]],
                ["key": "empty", "kinds": []],
            ],
            "statusMonitorAlerts": [["id": "api", "pings": true], ["label": "no id"]],
        ])
        XCTAssertFalse(settings.emailNotifications)
        XCTAssertEqual(settings.emailPreferences, ["mention": false])
        XCTAssertEqual(settings.delivery, "instant")
        XCTAssertEqual(settings.pauseUntil, 1_700_000_000_000)
        XCTAssertEqual(settings.mutedKinds, ["tip_sent", "host_online"])
        XCTAssertEqual(
            settings.quietHours,
            PingQuietHours(enabled: true, start: 1320, end: PingQuietHours.defaultEnd, days: [0, 6], timezone: "Europe/Berlin"))
        XCTAssertEqual(settings.quietUntil, 1_700_000_000_500)
        // Empty groups and kindless rows are dropped; a label falls back to the kind.
        XCTAssertEqual(settings.catalog, [
            PingKindGroup(key: "g", label: "Group", kinds: [PingKindOption(kind: "mention", label: "mention", email: true)]),
        ])
        XCTAssertEqual(settings.statusMonitorAlerts, [StatusMonitorAlert(id: "api", label: "api", pings: true)])
    }

    func testTheCatalogListsNoCreditsOrTips() {
        let settings = PingSettings(json: [
            "pingKindCatalog": [
                ["key": "rewards", "label": "Money & rewards", "kinds": [
                    ["kind": "bounty_paid", "label": "Bounties paid"],
                    ["kind": "credits_refilled", "label": "Credits refilled"],
                    ["kind": "tip_sent", "label": "Tips"],
                ]],
                ["key": "money", "kinds": [["kind": "tip_sent"]]],
            ],
        ])
        XCTAssertEqual(settings.catalog, [
            PingKindGroup(key: "rewards", label: "Rewards", kinds: [PingKindOption(kind: "bounty_paid", label: "Bounties paid")]),
        ])
        let fallback = PingSettings.fallbackCatalog.flatMap { $0.kinds.map(\.kind) }
        XCTAssertTrue(PingKindGroup.hiddenKinds.isDisjoint(with: fallback))
    }

    func testDefaultsAndTheFallbackCatalog() {
        let settings = PingSettings(json: JSONObject())
        XCTAssertTrue(settings.emailNotifications)
        XCTAssertEqual(settings.delivery, "digest")
        XCTAssertEqual(settings.quietHours, PingQuietHours())
        XCTAssertNil(settings.statusMonitorAlerts)
        XCTAssertEqual(settings.catalog, PingSettings.fallbackCatalog)
        XCTAssertEqual(settings.catalog.map(\.key), ["conversations", "repositories", "organizations", "rewards", "devices"])
        XCTAssertEqual(settings.option("repo_activity")?.label, "Pushes and merges")
        // Host pings do not mail unless switched on; everything else does.
        XCTAssertFalse(settings.emailEnabled("host_offline"))
        XCTAssertTrue(settings.emailEnabled("mention"))
    }

    // MARK: applyPatch

    func testAPatchMovesTheSwitchItNames() {
        let settings = PingSettings(json: [
            "pingMutedKinds": ["tip_sent"],
            "notificationQuietHours": ["enabled": true],
            "notificationQuietUntil": 5,
            "statusMonitorAlerts": [["id": "api"], ["id": "git"]],
        ])
        let muted = settings.applyPatch(["pingMutedKinds": ["tip_sent": true, "mention": false]])
        XCTAssertEqual(muted.mutedKinds, ["mention"])

        XCTAssertFalse(settings.applyPatch(["codePushes": false]).codePushes)
        XCTAssertTrue(settings.applyPatch(["codePushes": false]).applyPatch(["codePushes": true]).codePushes)
        XCTAssertTrue(settings.applyPatch(["emailNotifications": true]).codePushes)

        let quiet = settings.applyPatch(["notificationQuietHours": ["start": 60, "days": [1, 2]]])
        XCTAssertEqual(quiet.quietHours.start, 60)
        XCTAssertEqual(quiet.quietHours.days, [1, 2])
        XCTAssertTrue(quiet.quietHours.enabled)
        XCTAssertEqual(quiet.quietUntil, 5)
        // A window switched off is known not to be quiet any more.
        XCTAssertEqual(settings.applyPatch(["notificationQuietHours": ["enabled": false]]).quietUntil, 0)

        let monitor = settings.applyPatch(["statusMonitorAlerts": ["git": ["continual": true]]])
        XCTAssertEqual(monitor.statusMonitorAlerts?.map(\.continual), [false, true])

        let prefs = settings.applyPatch(["notificationPreferences": ["general_chat": false]])
        XCTAssertFalse(prefs.emailEnabled("general_chat"))
        XCTAssertEqual(settings.applyPatch(["notificationPauseUntil": 42]).pauseUntil, 42)
    }

    // MARK: API

    func testSettingsAreReadAndSavedThroughTheInboxRoute() async throws {
        let http = FakeHTTPClient()
        http.enqueue(status: 200, json: ["codePushes": false])
        http.enqueue(status: 200, json: ["codePushes": true])
        let api = ApiPortFixture.api(http)

        let loaded = try await api.pingSettings(node: " Alice ")
        XCTAssertFalse(loaded.codePushes)
        let get = try XCTUnwrap(http.requests.first)
        XCTAssertEqual(portMethodPath(get), "GET /api/notifications")
        XCTAssertEqual(portQuery(get), ["node": "alice", "settings": "1"])

        let saved = try await api.savePingSettings(node: "Alice", patch: ["codePushes": true])
        XCTAssertTrue(saved.codePushes)
        let post = try XCTUnwrap(http.requests.last)
        XCTAssertEqual(portMethodPath(post), "POST /api/notifications")
        XCTAssertEqual(portBody(post), ["node": "alice", "pingSettings": ["codePushes": true]])
    }

    func testARefusedSaveThrows() async {
        let http = FakeHTTPClient { _ in portJSON(["error": "forbidden"], 403) }
        do {
            _ = try await ApiPortFixture.api(http).savePingSettings(node: "alice", patch: ["codePushes": false])
            XCTFail("expected a refusal")
        } catch {}
    }

    // MARK: PingSettingsModel

    func testASwitchMovesAtOnceThenTakesTheWorkersAnswer() async {
        var patches: [JSONObject] = []
        let model = PingSettingsModel(
            load: { _ in PingSettings() },
            save: { (_: String, patch: JSONObject) in
                patches.append(patch)
                return PingSettings(codePushes: false)
            },
            node: { "alice" })
        await model.start()
        XCTAssertEqual(model.settings?.codePushes, true)
        await model.setCodePushes(false)
        XCTAssertEqual(patches, [["codePushes": false]])
        XCTAssertEqual(model.settings?.codePushes, false)
    }

    func testARefusedSavePutsTheSwitchBack() async {
        let model = PingSettingsModel(
            load: { _ in PingSettings() },
            save: { (_: String, _: JSONObject) in throw Refused() },
            node: { "alice" })
        await model.load()
        await model.setPingEnabled("mention", false)
        XCTAssertEqual(model.settings?.pingEnabled("mention"), true)
        XCTAssertEqual(model.saveError, "Could not save ping settings: forbidden")
    }

    func testALoadFailureIsShownWithItsReason() async {
        let model = PingSettingsModel(
            load: { _ in throw Refused() },
            save: { (_: String, _: JSONObject) in PingSettings() },
            node: { "alice" })
        await model.load()
        XCTAssertNil(model.settings)
        XCTAssertEqual(model.error, "forbidden")
        XCTAssertFalse(model.loading)
    }

    func testAPauseInThePastIsRefusedAndOneTooLongIsCapped() async {
        let now = Date(timeIntervalSince1970: 1_700_000_000)
        var patches: [JSONObject] = []
        let model = PingSettingsModel(
            load: { _ in PingSettings() },
            save: { (_: String, patch: JSONObject) in
                patches.append(patch)
                return PingSettings()
            },
            node: { "alice" },
            now: { now })
        await model.load()
        let refused = await model.pause(until: now.addingTimeInterval(-60))
        XCTAssertFalse(refused)
        XCTAssertTrue(patches.isEmpty)
        await model.pause(until: now.addingTimeInterval(365 * 24 * 60 * 60))
        let cap = now.addingTimeInterval(PingSettingsModel.maxPause).epochMillis
        XCTAssertEqual(patches.last, ["notificationPauseUntil": .int(cap)])
        await model.resume()
        XCTAssertEqual(patches.last, ["notificationPauseUntil": 0])
    }

    func testQuietDaysAreSentSorted() async {
        var patches: [JSONObject] = []
        let model = PingSettingsModel(
            load: { _ in PingSettings(quietHours: PingQuietHours(days: [4, 1])) },
            save: { (_: String, patch: JSONObject) in
                patches.append(patch)
                return PingSettings()
            },
            node: { "alice" })
        await model.load()
        await model.setQuietDay(0, true)
        XCTAssertEqual(patches, [["notificationQuietHours": ["days": [0, 1, 4]]]])
    }

    func testLabels() {
        XCTAssertEqual(PingSettingsModel.hhmm(7 * 60 + 5), "07:05")
        XCTAssertEqual(PingSettingsModel.daysLabel(PingQuietHours.allDays), "day")
        XCTAssertEqual(PingSettingsModel.daysLabel([0, 1, 2, 3, 4]), "weekday")
        XCTAssertEqual(PingSettingsModel.daysLabel([5, 6]), "weekend")
        XCTAssertEqual(PingSettingsModel.daysLabel([]), "no day")
        XCTAssertEqual(PingSettingsModel.daysLabel([0, 2]), "Mon, Wed")
    }
}
