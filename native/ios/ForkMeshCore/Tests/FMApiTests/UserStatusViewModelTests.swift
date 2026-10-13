// The user status sheet cases and the sponsored ad-slot rules on UserStatusSheetModel and
// SponsoredAdModel, over the real services on FakeHTTPClient.

import FMAccount
import FMBase
import FMNet
import XCTest
@testable import FMApi

private let fixedNow = Date(epochMillis: 1_770_000_000_000)

@MainActor
private func statusService(_ http: FakeHTTPClient) -> UserStatusService {
    let api = ApiService(settings: StaticApiSettings(), http: http, sleep: { _ in })
    api.sessionTokenProvider = { "session" }
    return UserStatusService(api: api, sessionToken: { "session" }, sleep: { _ in })
}

private func statusReply(_ status: JSONObject) -> HTTPResponse {
    HTTPResponse(status: 200, json: ["ok": true, "nodeName": "alice", "userStatus": .object(status)])
}

final class UserStatusViewModelTests: XCTestCase {
    @MainActor
    func testSaveSendsTheTrimmedStatusAndCloses() async throws {
        let http = FakeHTTPClient()
        http.on("POST", path: "/api/accounts/user-status") { _ in
            statusReply(["emoji": "🌴", "text": "Vacationing", "expiresAt": 0])
        }
        let service = statusService(http)
        let model = UserStatusSheetModel(service: service, now: { fixedNow }, timeZone: TimeZone(identifier: "UTC")!)
        XCTAssertEqual(model.clearAfter, .never)
        model.applyPreset(statusPresets[0])
        model.emoji = " 🌴 "
        model.text = " Vacationing "
        model.chooseClearAfter(.never)
        let closed = await model.save()
        let bodies = http.requests.map { JSON.parseObject($0.body ?? Data()) }
        XCTAssertTrue(closed)
        XCTAssertEqual(model.problem, "")
        XCTAssertEqual(bodies.last?["emoji"], "🌴")
        XCTAssertEqual(bodies.last?["text"], "Vacationing")
        XCTAssertEqual(bodies.last?["expiresAt"], .int(0))
        XCTAssertEqual(service.status.text, "Vacationing")
    }

    @MainActor
    func testRefusalsStayInTheSheet() async {
        let http = FakeHTTPClient()
        http.on("POST", path: "/api/accounts/user-status") { _ in
            HTTPResponse(status: 400, json: ["error": "status_text_too_long"])
        }
        let model = UserStatusSheetModel(service: statusService(http), now: { fixedNow })
        model.emoji = "not an emoji"
        let refusedLocally = await model.save()
        XCTAssertFalse(refusedLocally)
        XCTAssertEqual(model.problem, userStatusErrorMessage("bad_status_emoji"))
        XCTAssertTrue(http.requests.isEmpty)

        model.pickEmoji("🎧")
        XCTAssertEqual(model.problem, "")
        XCTAssertTrue(model.isSelected("🎧"))
        model.setCustom(fixedNow.addingTimeInterval(-60))
        let past = await model.save()
        XCTAssertFalse(past)
        XCTAssertEqual(model.problem, userStatusErrorMessage("status_expiry_in_past"))

        model.setCustom(fixedNow.addingTimeInterval(3600))
        XCTAssertEqual(model.clearAfter, .custom)
        let server = await model.save()
        XCTAssertFalse(server)
        XCTAssertEqual(model.problem, userStatusErrorMessage("status_text_too_long"))
        XCTAssertEqual(model.emoji, "🎧", "nothing typed is lost")
    }

    @MainActor
    func testEmptyFieldsClearTheStatusAndAwayRollsBackOnRefusal() async {
        let http = FakeHTTPClient()
        http.on("POST", path: "/api/accounts/user-status") { request in
            let body = JSON.parseObject(request.body ?? Data())
            if body["away"] != nil { return HTTPResponse(status: 503, json: ["error": "unavailable"]) }
            return statusReply([:])
        }
        let service = statusService(http)
        let model = UserStatusSheetModel(service: service, now: { fixedNow })
        let cleared = await model.save()
        XCTAssertTrue(cleared)
        XCTAssertEqual(http.requests.last.map { JSON.parseObject($0.body ?? Data()) }?["clear"], true)

        await model.setAway(true)
        XCTAssertFalse(model.away)
        XCTAssertFalse(model.problem.isEmpty)
        let notice = await setAwayFromSettings(service, true)
        XCTAssertEqual(notice, model.problem)
    }

    // MARK: sponsored_ad_card

    @MainActor
    func testTheAdSlotAsksOnceAndHidesForAWeek() async {
        let prefs = InMemoryKeyValueStore([:])
        let settings = SettingsService(prefs: prefs, secureStore: InMemorySecureStore([:]))
        let ad = SponsoredAd(id: "0123456789abcdef0123456789abcdef", headline: "Try it", url: "https://example.com/x")
        var fetches = 0
        let model = SponsoredAdModel(settings: settings, fetch: { fetches += 1; return ad }, clicked: { _ in })
        await model.load()
        await model.load()
        XCTAssertEqual(fetches, 1)
        XCTAssertEqual(model.visibleAd?.headline, "Try it")
        XCTAssertEqual(model.follow(ad), URL(string: "https://example.com/x"))
        model.hide()
        XCTAssertNil(model.visibleAd)
        XCTAssertTrue(settings.sponsoredMessagesHidden)
    }

    @MainActor
    func testTheAdSlotNeverAsksWhileSwitchedOff() async {
        let settings = SettingsService(prefs: InMemoryKeyValueStore([:]), secureStore: InMemorySecureStore([:]))
        settings.setShowSponsoredMessages(false)
        var fetches = 0
        let model = SponsoredAdModel(settings: settings, fetch: { fetches += 1; return nil }, clicked: { _ in })
        await model.load()
        XCTAssertEqual(fetches, 0)
        XCTAssertFalse(model.allowed)
        settings.setShowSponsoredMessages(true)
        await model.load()
        XCTAssertEqual(fetches, 1)
        XCTAssertNil(model.visibleAd)
    }
}
