import FMApi
import FMBase
import XCTest
@testable import FMTools

@MainActor
final class FakeSleep: SleepBridge {
    var available = true
    var motionAvailable = true
    var allow = true
    var rest: [SleepRestSample]? = []
    var samples: [HealthSleepSample]? = []
    var saved: [HealthSleepWrite] = []
    var deleted: [String] = []
    var authorized: [Bool] = []

    func authorizeSleep(write: Bool) async -> Bool {
        authorized.append(write)
        return allow
    }

    func restHistory(from: Date, to: Date) async -> [SleepRestSample]? { rest }

    func sleepSamples(from: Date, to: Date) async -> [HealthSleepSample]? { samples }

    func saveSleep(_ writes: [HealthSleepWrite]) async throws { saved.append(contentsOf: writes) }

    func deleteSleep(entryIds: [String]) async throws { deleted.append(contentsOf: entryIds) }
}

@MainActor
final class SleepTests: XCTestCase {
    let utc = TimeZone(identifier: "UTC")!

    func at(_ day: String, _ clock: String) -> Date {
        let midnight = CaloriesText.tryParseDay(day, timeZone: utc)!
        return midnight.addingTimeInterval(TimeInterval((SleepSchedule.minutes(clock) ?? 0) * 60))
    }

    func still(_ day: String, _ clock: String, confident: Bool = true) -> SleepRestSample {
        SleepRestSample(start: at(day, clock), still: true, confident: confident)
    }

    func moving(_ day: String, _ clock: String) -> SleepRestSample {
        SleepRestSample(start: at(day, clock), still: false, confident: true)
    }

    var night: [SleepRestSample] {
        [
            moving("2026-09-15", "22:00"),
            still("2026-09-15", "23:10"),
            SleepRestSample(start: at("2026-09-16", "02:00"), still: false, confident: false),
            still("2026-09-16", "02:05"),
            moving("2026-09-16", "06:50"),
        ]
    }

    func testScheduleNightKeysAndWindows() {
        let schedule = SleepSchedule(bedtime: "23:00", wakeTime: "07:00")
        XCTAssertEqual(schedule.lengthMinutes, 480)
        XCTAssertEqual(schedule.nightKey(at("2026-09-15", "23:30"), timeZone: utc), "2026-09-16")
        XCTAssertEqual(schedule.nightKey(at("2026-09-16", "07:05"), timeZone: utc), "2026-09-16")
        XCTAssertEqual(schedule.nightKey(at("2026-09-16", "12:30"), timeZone: utc), "2026-09-17")
        let window = schedule.window(night: "2026-09-16", timeZone: utc)
        XCTAssertEqual(window?.start, at("2026-09-15", "19:00"))
        XCTAssertEqual(window?.end, at("2026-09-16", "12:00"))
        XCTAssertEqual(SleepSchedule(bedtime: "nope", wakeTime: "25:00"), SleepSchedule())
        XCTAssertEqual(SleepSchedule(bedtime: "02:30", wakeTime: "10:00").lengthMinutes, 450)
    }

    func testAPhoneLyingStillByTheBedCountsAsSleep() {
        let found = SleepDetector.detect(
            night: "2026-09-16", rest: night, usage: [], schedule: SleepSchedule(), timeZone: utc,
            now: at("2026-09-16", "09:00"))
        XCTAssertEqual(found?.bedAt, at("2026-09-15", "23:10"))
        XCTAssertEqual(found?.wakeAt, at("2026-09-16", "06:50"))
        XCTAssertEqual(found?.asleepMinutes, 445)
        XCTAssertEqual(found?.awakeMinutes, 15)
        XCTAssertEqual(found?.interruptions, 1)
        XCTAssertEqual(found?.confidence, "high")
        XCTAssertEqual(found?.body["asleepMinutes"], .int(445))
    }

    func testDetectingSeveralNightsMatchesDetectingEachInTurn() {
        let now = at("2026-09-16", "09:00")
        let keys = ["2026-09-15", "2026-09-16"]
        let found = SleepDetector.detect(
            nights: keys, rest: Array(night.reversed()), usage: [], schedule: SleepSchedule(), timeZone: utc, now: now)
        let one = SleepDetector.detect(
            night: "2026-09-16", rest: night, usage: [], schedule: SleepSchedule(), timeZone: utc, now: now)
        XCTAssertEqual(found.detected, [one].compactMap { $0 })
        XCTAssertEqual(found.bodies, [SleepNight.cleared("2026-09-15"), one?.body].compactMap { $0 })
    }

    func testUsingThePhoneAtNightIsAwakeTimeNotANewNight() {
        let usage = [SleepInterval(start: at("2026-09-16", "03:00"), end: at("2026-09-16", "03:30"))]
        let found = SleepDetector.detect(
            night: "2026-09-16", rest: night, usage: usage, schedule: SleepSchedule(), timeZone: utc,
            now: at("2026-09-16", "09:00"))
        XCTAssertEqual(found?.bedAt, at("2026-09-15", "23:10"))
        XCTAssertEqual(found?.wakeAt, at("2026-09-16", "06:50"))
        XCTAssertEqual(found?.asleepMinutes, 415)
        XCTAssertEqual(found?.interruptions, 2)
    }

    func testHeldPhonesShortRestsAndDesksAreNotSleep() {
        let held = night.map { SleepRestSample(start: $0.start, still: $0.still, confident: false) }
        XCTAssertNil(SleepDetector.detect(
            night: "2026-09-16", rest: held, usage: [], schedule: SleepSchedule(), timeZone: utc,
            now: at("2026-09-16", "09:00")))
        let nap = [still("2026-09-15", "20:00"), moving("2026-09-15", "22:30")]
        XCTAssertNil(SleepDetector.detect(
            night: "2026-09-16", rest: nap, usage: [], schedule: SleepSchedule(), timeZone: utc,
            now: at("2026-09-16", "09:00")))
        let desk = [still("2026-09-16", "08:00"), moving("2026-09-16", "11:30")]
        XCTAssertNil(SleepDetector.detect(
            night: "2026-09-16", rest: desk, usage: [], schedule: SleepSchedule(), timeZone: utc,
            now: at("2026-09-16", "12:00")))
    }

    func testAppleHealthNightsMergeStagesAndSkipOurOwnSamples() {
        let samples = [
            HealthSleepSample(start: at("2026-09-15", "22:50"), end: at("2026-09-16", "07:05"), stage: .inBed),
            HealthSleepSample(start: at("2026-09-15", "23:10"), end: at("2026-09-16", "01:00"), stage: .core),
            HealthSleepSample(start: at("2026-09-16", "01:00"), end: at("2026-09-16", "02:00"), stage: .deep),
            HealthSleepSample(start: at("2026-09-16", "02:00"), end: at("2026-09-16", "02:10"), stage: .awake),
            HealthSleepSample(start: at("2026-09-16", "02:10"), end: at("2026-09-16", "03:00"), stage: .rem),
            HealthSleepSample(start: at("2026-09-16", "03:00"), end: at("2026-09-16", "06:55"), stage: .core),
            HealthSleepSample(
                start: at("2026-09-15", "23:00"), end: at("2026-09-16", "07:00"), stage: .asleep, ours: true,
                entryId: "sleep-2026-09-16", version: "1"),
        ]
        let nights = SleepDetector.healthNights(samples, schedule: SleepSchedule(), timeZone: utc)
        XCTAssertEqual(nights.count, 1)
        let found = nights[0]
        XCTAssertEqual(found.night, "2026-09-16")
        XCTAssertEqual(found.asleepMinutes, 455)
        XCTAssertEqual(found.awakeMinutes, 40)
        XCTAssertEqual(found.interruptions, 1)
        XCTAssertEqual([found.deepMinutes, found.remMinutes, found.coreMinutes], [60, 50, 345])
        let inBedOnly = SleepDetector.healthNights(
            [HealthSleepSample(start: at("2026-09-15", "23:00"), end: at("2026-09-16", "06:00"), stage: .inBed)],
            schedule: SleepSchedule(), timeZone: utc)
        XCTAssertEqual(inBedOnly.first?.asleepMinutes, 420)
    }

    func testWritesToAppleHealthAreReconciledByNightAndVersion() {
        let detected = SleepNight(
            night: "2026-09-16", bedAt: at("2026-09-15", "23:10"), wakeAt: at("2026-09-16", "06:50"),
            asleepMinutes: 445, awakeMinutes: 15, confidence: "high")
        let writes = SleepDetector.healthWrites([detected])
        XCTAssertEqual(writes.first?.entryId, "sleep-2026-09-16")
        XCTAssertEqual(writes.first?.start, at("2026-09-15", "23:25"))
        let stale = HealthSleepSample(
            start: at("2026-09-15", "23:00"), end: at("2026-09-16", "07:00"), stage: .asleep, ours: true,
            entryId: "sleep-2026-09-16", version: "old")
        let plan = SleepDetector.reconcile(desired: writes, existing: [stale])
        XCTAssertEqual(plan.delete, ["sleep-2026-09-16"])
        XCTAssertEqual(plan.save.count, 1)
        let current = HealthSleepSample(
            start: writes[0].start, end: writes[0].end, stage: .asleep, ours: true, entryId: writes[0].entryId,
            version: writes[0].version)
        let unchanged = SleepDetector.reconcile(desired: writes, existing: [current])
        XCTAssertTrue(unchanged.save.isEmpty && unchanged.delete.isEmpty)
    }

    func testUsageLogRecordsForegroundSessions() {
        let prefs = InMemoryKeyValueStore()
        SleepUsageLog.begin(prefs, now: at("2026-09-16", "03:00"))
        SleepUsageLog.end(prefs, now: at("2026-09-16", "03:20"))
        SleepUsageLog.begin(prefs, now: at("2026-09-16", "08:00"))
        let intervals = SleepUsageLog.intervals(prefs, now: at("2026-09-16", "08:05"))
        XCTAssertEqual(intervals, [
            SleepInterval(start: at("2026-09-16", "03:00"), end: at("2026-09-16", "03:20")),
            SleepInterval(start: at("2026-09-16", "08:00"), end: at("2026-09-16", "08:05")),
        ])
        SleepUsageLog.end(prefs, now: at("2026-09-25", "08:00"))
        XCTAssertEqual(SleepUsageLog.intervals(prefs, now: at("2026-09-25", "08:00")).count, 1)
    }

    func testSyncPostsPhoneNightsAndWritesThemToAppleHealth() async {
        let prefs = InMemoryKeyValueStore()
        prefs.set(true, forKey: HealthPrefs.sleep)
        prefs.set(true, forKey: HealthPrefs.sleepWriteHealth)
        let bridge = FakeSleep()
        bridge.rest = night
        let api = FakePackageApi()
        api.replies["POST /sync"] = ["ok": true, "synced": 1]
        let synced = await SleepHealthSync.runIfDue(
            bridge: bridge, prefs: prefs, call: api.call, now: at("2026-09-16", "09:00"), nights: 1, timeZone: utc)
        XCTAssertEqual(synced, 1)
        let body = api.bodies("POST", "/sync").first
        XCTAssertEqual(body?["source"], .string("phone"))
        let posted = ToolsJSON.maps(body?["nights"])
        XCTAssertEqual(posted.first?["night"], .string("2026-09-16"))
        XCTAssertEqual(posted.first?["asleepMinutes"], .int(445))
        XCTAssertEqual(bridge.saved.map(\.entryId), ["sleep-2026-09-16"])
        let again = await SleepHealthSync.runIfDue(
            bridge: bridge, prefs: prefs, call: api.call, now: at("2026-09-16", "09:05"), nights: 1, timeZone: utc)
        XCTAssertEqual(again, 0)
    }

    func testSyncDoesNothingWhenTrackingIsOff() async {
        let bridge = FakeSleep()
        bridge.rest = night
        let api = FakePackageApi()
        let synced = await SleepHealthSync.runIfDue(
            bridge: bridge, prefs: InMemoryKeyValueStore(), call: api.call, now: at("2026-09-16", "09:00"),
            timeZone: utc, force: true)
        XCTAssertEqual(synced, 0)
        XCTAssertTrue(api.calls.isEmpty)
    }

    func testModelShowsLastNightAndSummary() async {
        let api = FakePackageApi()
        api.replies["GET"] = [
            "ok": true, "installed": true, "night": "2026-09-16",
            "profile": ["goalMinutes": 480, "bedtime": "23:00", "wakeTime": "07:00"],
            "lastNight": [
                "night": "2026-09-16", "source": "phone", "asleepMinutes": 445, "awakeMinutes": 15,
                "interruptions": 1, "confidence": "high",
                "bedAt": .int(HealthDates.epochMs(at("2026-09-15", "23:10"))),
                "wakeAt": .int(HealthDates.epochMs(at("2026-09-16", "06:50"))),
            ],
            "summary": ["weekNights": 1, "weekAverageMinutes": 445, "debtMinutes": 35, "loggedNights": 1,
                        "averageMinutes": 445, "streak": 0, "averageBedtimeMinutes": 1390, "averageWakeMinutes": 410],
            "nights": [],
        ]
        let model = SleepModel(
            api: SleepAPI(call: api.call), bridge: FakeSleep(), prefs: InMemoryKeyValueStore(), timeZone: utc,
            now: { self.at("2026-09-16", "09:00") })
        await model.load(syncIfDue: false)
        XCTAssertTrue(model.installed)
        XCTAssertEqual(api.calls.first?.suffix, "?night=2026-09-16")
        XCTAssertEqual(model.durationText, "7 h 25 min")
        XCTAssertEqual(model.goalLine, "of 8 h")
        XCTAssertEqual(model.detailLine, "11:10 PM – 6:50 AM · 15 min awake · 1 interruption · 35 min short")
        XCTAssertEqual(model.stats.first { $0.label == "Average bedtime" }?.value, "11:10 PM")
        XCTAssertEqual(model.stats.first { $0.label == "Sleep debt" }?.value, "35 min")
        XCTAssertEqual(model.prefs.string(forKey: HealthPrefs.sleepBedtime), "23:00")
        XCTAssertEqual(model.nightLabel("2026-09-16"), "Last night")
        let span = model.span(night: "2026-09-16", bed: at("2026-01-01", "22:30"), wake: at("2026-01-01", "06:45"))
        XCTAssertEqual(span?.bedAt, at("2026-09-15", "22:30"))
        XCTAssertEqual(span?.wakeAt, at("2026-09-16", "06:45"))
    }

    func testSleepText() {
        XCTAssertEqual(SleepText.duration(45), "45 min")
        XCTAssertEqual(SleepText.duration(480), "8 h")
        XCTAssertEqual(SleepText.duration(-3), "0 min")
        XCTAssertEqual(SleepText.clock(minutes: 0), "12:00 AM")
        XCTAssertEqual(SleepText.clock(minutes: 13 * 60 + 5), "1:05 PM")
        XCTAssertEqual(SleepText.clock(minutes: -1), "—")
    }
}
