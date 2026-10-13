import FMApi
import FMBase
import XCTest
@testable import FMTools

@MainActor
final class FakeSugar: SugarBridge {
    var available = true
    var allow = true
    var grams: [String: Double] = [:]
    var authorized = 0
    var reads: [(from: Date, to: Date)] = []

    func authorizeSugar() async -> Bool {
        authorized += 1
        return allow
    }

    func sugarGrams(from: Date, to: Date) async -> [String: Double] {
        reads.append((from, to))
        return grams
    }
}

@MainActor
final class SugarBreakTests: XCTestCase {
    let utc = TimeZone(identifier: "UTC")!

    func at(_ day: String, hour: Int = 12) -> Date {
        CaloriesText.tryParseDay(day, timeZone: utc)!.addingTimeInterval(TimeInterval(hour * 3600))
    }

    var activeOverview: JSONObject {
        [
            "ok": true, "installed": true, "actor": "alice", "day": "2026-10-11",
            "profile": ["defaultLengthDays": 21, "tzOffsetMinutes": 0],
            "lengths": [7, 14, 21, 30, 60, 90],
            "current": [
                "id": "b1", "startDay": "2026-10-02", "endDay": "2026-10-31", "lengthDays": 30,
                "startedAt": 1, "endedAt": 0, "endedDay": "", "status": "active",
            ],
            "days": [
                ["day": "2026-10-09", "status": "slip", "skips": 1, "slips": 1],
                ["day": "2026-10-10", "status": "sugar-free", "skips": 3, "slips": 0],
                ["day": "2026-10-11", "status": "open", "skips": 0, "slips": 0],
                ["day": "2026-10-12", "status": "upcoming", "skips": 0, "slips": 0],
            ],
            "today": ["day": "2026-10-11", "skips": 0, "slips": 0, "status": "open", "inBreak": true],
            "events": [],
            "summary": [
                "skips": 12, "slips": 1, "sugarFreeDays": 8, "slipDays": 1, "openDays": 1, "streak": 1,
                "bestStreak": 6, "dayNumber": 10, "lengthDays": 30, "remainingDays": 20, "percent": 26,
            ],
            "history": [
                [
                    "id": "b0", "startDay": "2026-09-01", "endDay": "2026-09-14", "lengthDays": 14,
                    "status": "completed",
                    "summary": ["skips": 20, "sugarFreeDays": 12, "bestStreak": 9],
                ],
            ],
        ]
    }

    func model(_ api: FakePackageApi, health: FakeSugar? = nil, prefs: KeyValueStore = InMemoryKeyValueStore()) -> SugarBreakModel {
        SugarBreakModel(
            api: SugarBreakAPI(call: api.call), health: health, prefs: prefs, timeZone: utc,
            now: { self.at("2026-10-11") })
    }

    func testNotInstalledShowsTheInstallPanel() async {
        let api = FakePackageApi()
        api.replies["GET"] = ["ok": true, "installed": false, "actor": "alice"]
        let sugar = model(api)
        await sugar.load()
        XCTAssertTrue(sugar.showsInstall)
        XCTAssertFalse(sugar.installed)
        XCTAssertEqual(api.calls.first?.suffix, "?day=2026-10-11")
        api.failures["GET ?day=2026-10-11"] = OrgApiException("plugin_not_installed", "plugin not installed")
        await sugar.load()
        XCTAssertTrue(sugar.showsInstall)
        await sugar.install()
        XCTAssertEqual(api.bodies("POST", "/install").count, 1)
    }

    func testNoBreakOffersLengthsWithAMonthByDefault() async {
        let api = FakePackageApi()
        api.replies["GET"] = [
            "ok": true, "installed": true, "profile": ["defaultLengthDays": 30], "lengths": [7, 14, 21, 30, 60, 90],
            "current": nil, "days": [], "events": [], "history": [],
            "summary": ["skips": 0, "sugarFreeDays": 0, "lengthDays": 0],
        ]
        let sugar = model(api)
        await sugar.load()
        XCTAssertTrue(sugar.showsStart)
        XCTAssertFalse(sugar.isActive)
        XCTAssertEqual(sugar.selectedLength, 30)
        XCTAssertEqual(sugar.lengths.map(\.days), [7, 14, 21, 30, 60, 90])
        XCTAssertEqual(sugar.lengths.first { $0.days == 30 }?.label, "1 month (30 days)")
        XCTAssertEqual(sugar.lengths.first { $0.days == 14 }?.label, "2 weeks (14 days)")
        XCTAssertEqual(sugar.lengths.first { $0.days == 90 }?.label, "3 months (90 days)")
        await sugar.startBreak()
        let body = api.bodies("POST", "/breaks").first
        XCTAssertEqual(body?["lengthDays"], .int(30))
        XCTAssertEqual(body?["startDay"], .string("2026-10-11"))
        await sugar.startBreak(lengthDays: 500)
        XCTAssertEqual(api.bodies("POST", "/breaks").last?["lengthDays"], .int(365))
    }

    func testActiveBreakShowsProgressStatsAndDays() async {
        let api = FakePackageApi()
        api.replies["GET"] = activeOverview
        let sugar = model(api)
        await sugar.load()
        XCTAssertTrue(sugar.isActive)
        XCTAssertFalse(sugar.showsStart)
        XCTAssertEqual(sugar.selectedLength, 21)
        XCTAssertEqual(sugar.headline, "Day 10 of 30")
        XCTAssertEqual(sugar.progressLine, "8 of 30 days sugar-free")
        XCTAssertEqual(sugar.remainingLine, "20 days left · ends Sat Oct 31")
        XCTAssertEqual(sugar.progressRatio, 8.0 / 30.0, accuracy: 0.0001)
        XCTAssertEqual(sugar.todayLine, SugarBreakModel.emptyToday)
        XCTAssertEqual(sugar.stats.map(\.value), ["12", "8 of 30", "1 day", "6 days", "20"])
        let days = sugar.days
        XCTAssertEqual(days.map(\.status), [.slip, .sugarFree, .open, .upcoming])
        XCTAssertEqual(days.map(\.number), [9, 10, 11, 12])
        XCTAssertEqual(days[2].isToday, true)
        XCTAssertEqual(days[1].caption, "Sat Oct 10, sugar-free, +3")
        XCTAssertEqual(sugar.history.first?.title, "Sep 1 – Sep 14")
        XCTAssertEqual(sugar.history.first?.detail, "12 of 14 days sugar-free · +20 · best streak 9 days")
        XCTAssertEqual(sugar.history.first?.status, "Completed")
    }

    func testSkipSlipUndoAndEndEarlyCallTheApi() async {
        let api = FakePackageApi()
        api.replies["GET"] = activeOverview
        let sugar = model(api)
        await sugar.load()
        await sugar.skip()
        XCTAssertEqual(api.bodies("POST", "/events").first?["kind"], .string("skip"))
        XCTAssertEqual(api.bodies("POST", "/events").first?["day"], .string("2026-10-11"))
        XCTAssertEqual(sugar.pendingSkips, 0)
        await sugar.slip()
        XCTAssertEqual(api.bodies("POST", "/events").last?["kind"], .string("slip"))
        await sugar.undo("e1")
        XCTAssertTrue(api.calls.contains { $0.method == "DELETE" && $0.suffix == "/events/e1" })
        await sugar.endEarly()
        XCTAssertEqual(api.bodies("PATCH", "/breaks/b1").first?["end"], .bool(true))
        await sugar.deleteBreak("b0")
        XCTAssertTrue(api.calls.contains { $0.method == "DELETE" && $0.suffix == "/breaks/b0" })
        api.failures["POST /events"] = OrgApiException("no_active_break", "no active break")
        await sugar.skip()
        XCTAssertEqual(sugar.notice?.message, "Start a sugar break first.")
    }

    func testTodayEventsAreNewestFirst() async {
        let api = FakePackageApi()
        var overview = activeOverview
        overview["today"] = ["day": "2026-10-11", "skips": 2, "slips": 0, "status": "sugar-free"]
        overview["events"] = [
            ["id": "e1", "kind": "skip", "at": .int(HealthDates.epochMs(at("2026-10-11", hour: 9)))],
            ["id": "e2", "kind": "skip", "at": .int(HealthDates.epochMs(at("2026-10-11", hour: 15)))],
        ]
        api.replies["GET"] = overview
        let sugar = model(api)
        await sugar.load()
        XCTAssertEqual(sugar.events.map(\.id), ["e2", "e1"])
        XCTAssertEqual(sugar.events.first?.time, "3:00 PM")
        XCTAssertEqual(sugar.events.first?.title, "+1 Skipped sugar")
        XCTAssertEqual(sugar.todayLine, "+2 today. Sugar-free so far.")
    }

    func testFinishedBreakOffersANewOne() async {
        let api = FakePackageApi()
        var overview = activeOverview
        overview["current"] = [
            "id": "b1", "startDay": "2026-10-02", "endDay": "2026-10-31", "lengthDays": 30, "endedDay": "2026-10-09",
            "status": "ended",
        ]
        api.replies["GET"] = overview
        let sugar = model(api)
        await sugar.load()
        XCTAssertTrue(sugar.showsStart)
        XCTAssertEqual(sugar.headline, "Sugar break ended")
        XCTAssertEqual(sugar.remainingLine, "Oct 2 – ended early on Oct 9")
        XCTAssertEqual(sugar.resultLine, "You stopped with 8 of 30 days sugar-free and +12 in all. Best streak 6 days.")
    }

    func testAppleHealthSugarIsReadOnlyAndOffByDefault() async {
        let api = FakePackageApi()
        api.replies["GET"] = activeOverview
        let health = FakeSugar()
        health.grams = ["2026-10-09": 42.4, "2026-10-11": 18.2, "2026-10-10": 0]
        let prefs = InMemoryKeyValueStore()
        let sugar = model(api, health: health, prefs: prefs)
        await sugar.load()
        XCTAssertFalse(sugar.healthEnabled)
        XCTAssertTrue(health.reads.isEmpty)
        XCTAssertNil(sugar.days[0].healthGrams)
        await sugar.enableHealth()
        XCTAssertEqual(health.authorized, 1)
        XCTAssertTrue(prefs.bool(forKey: HealthPrefs.sugarBreak, or: false))
        XCTAssertEqual(health.reads.first?.from, at("2026-10-02", hour: 0))
        XCTAssertEqual(health.reads.first?.to, at("2026-10-11"))
        XCTAssertEqual(sugar.days[0].healthGrams, 42.4)
        XCTAssertNil(sugar.days[1].healthGrams)
        XCTAssertEqual(sugar.healthTodayLine, "Apple Health shows 18 g of sugar today")
        XCTAssertTrue(sugar.suggestsSlip)
        XCTAssertEqual(sugar.days[0].caption, "Fri Oct 9, had sugar, Apple Health 42 g sugar")
        sugar.disableHealth()
        XCTAssertFalse(sugar.suggestsSlip)
        XCTAssertNil(sugar.days[0].healthGrams)
        XCTAssertFalse(prefs.bool(forKey: HealthPrefs.sugarBreak, or: true))
    }

    func testAppleHealthRefusedOrMissingLeavesItOff() async {
        let api = FakePackageApi()
        api.replies["GET"] = activeOverview
        let health = FakeSugar()
        health.allow = false
        let sugar = model(api, health: health)
        await sugar.load()
        await sugar.enableHealth()
        XCTAssertFalse(sugar.healthEnabled)
        let none = model(api)
        await none.load()
        await none.enableHealth()
        XCTAssertEqual(none.notice?.message, "Apple Health is not available on this device.")
        XCTAssertEqual(none.healthLine, "Apple Health is not available on this device.")
    }

    func testSugarBreakText() {
        XCTAssertEqual(SugarBreakText.length(7), "1 week (7 days)")
        XCTAssertEqual(SugarBreakText.length(21), "3 weeks (21 days)")
        XCTAssertEqual(SugarBreakText.length(60), "2 months (60 days)")
        XCTAssertEqual(SugarBreakText.length(45), "45 days")
        XCTAssertEqual(SugarBreakText.days(1), "1 day")
        XCTAssertEqual(SugarBreakText.grams(18.4), "18 g")
        XCTAssertEqual(SugarBreakText.grams(4.25), "4.3 g")
        XCTAssertEqual(SugarBreakText.grams(3), "3 g")
        XCTAssertEqual(SugarBreakText.monthDay("2026-12-05"), "Dec 5")
        XCTAssertEqual(SugarBreakText.monthDay("nope"), "nope")
    }

    func testContractCoversTheRestOfAnActiveBreak() async {
        let api = FakePackageApi()
        api.replies["GET"] = activeOverview
        let sugar = model(api)
        await sugar.load()
        let contract = sugar.contract()
        XCTAssertEqual(contract.startDay, "2026-10-11")
        XCTAssertEqual(contract.endDay, "2026-10-31")
        XCTAssertEqual(contract.days, 21)
        XCTAssertEqual(contract.signers, 2)
        XCTAssertEqual(contract.dateLine, "Oct 11 – Oct 31, 2026")
        XCTAssertEqual(contract.lengthLine, "21 days")
        XCTAssertEqual(contract.terms, "We, the undersigned, promise to eat no added sugar from Oct 11 through Oct 31, 2026.")
        XCTAssertEqual(
            contract.message, "No sugar for Oct 11 – Oct 31, 2026 (21 days). Sign the contract and go sugar-free with me.")
        XCTAssertEqual(contract.fileName, "no-sugar-contract-2026-10-11.png")
    }

    func testContractWithoutABreakUsesTheSelectedLength() async {
        let api = FakePackageApi()
        api.replies["GET"] = [
            "ok": true, "installed": true, "profile": ["defaultLengthDays": 7], "current": nil, "days": [],
            "events": [], "history": [], "summary": [:],
        ]
        let sugar = model(api)
        await sugar.load()
        let contract = sugar.contract(signers: 4)
        XCTAssertEqual(contract.startDay, "2026-10-11")
        XCTAssertEqual(contract.endDay, "2026-10-17")
        XCTAssertEqual(contract.days, 7)
        XCTAssertEqual(contract.lines.count, 4)
    }

    func testContractSignersNamesAndDates() async {
        var contract = SugarContract(startDay: "2026-12-24", endDay: "2026-12-20", signers: 0)
        XCTAssertEqual(contract.endDay, "2026-12-24")
        XCTAssertTrue(contract.isOneDay)
        XCTAssertEqual(contract.signers, 1)
        XCTAssertEqual(contract.dateLine, "Dec 24, 2026")
        XCTAssertEqual(contract.lengthLine, "1 day")
        XCTAssertEqual(contract.terms, "I, the undersigned, promise to eat no added sugar on Dec 24, 2026.")
        XCTAssertEqual(contract.message, "No sugar on Dec 24, 2026. Hold me to it.")
        contract.setName(0, "  Sam  ")
        contract.signers = 3
        XCTAssertEqual(contract.lines.map(\.caption), ["Sam", "Signature", "Signature"])
        contract.setName(2, String(repeating: "x", count: 99))
        XCTAssertEqual(contract.names[2].count, SugarContract.maxNameLength)
        contract.signers = 40
        XCTAssertEqual(contract.signers, 12)
        contract.signers = 2
        XCTAssertEqual(contract.names, ["  Sam  ", ""])
        contract.setEnd("2027-01-02")
        XCTAssertEqual(contract.days, 10)
        XCTAssertEqual(contract.dateLine, "Dec 24, 2026 – Jan 2, 2027")
        contract.setStart("2027-01-05")
        XCTAssertEqual(contract.endDay, "2027-01-05")
        contract.setEnd("2027-01-01")
        XCTAssertEqual(contract.endDay, "2027-01-05")
    }
}
