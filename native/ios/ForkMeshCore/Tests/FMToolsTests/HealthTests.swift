import FMApi
import FMBase
import XCTest
@testable import FMTools

@MainActor
final class FakeHealth: HealthBridge {
    var available = true
    var allow = true
    var authorized: [Set<HealthKind>] = []
    var stepDaysResult: [HealthStepDay] = []
    var stored: [HealthQuantity: [HealthSample]] = [:]
    var saved: [HealthWrite] = []
    var deleted: [(quantities: [HealthQuantity], ids: [String])] = []
    var liveUpdate: (@MainActor (HealthStepDay) -> Void)?
    var liveStopped = 0
    var motionAvailable = false
    var motionUpdate: (@MainActor ([MotionSample]) -> Void)?
    var motionStopped = 0

    func authorize(_ kinds: Set<HealthKind>) async -> Bool {
        authorized.append(kinds)
        return allow
    }

    func stepDays(_ count: Int, now: Date) async -> [HealthStepDay] { Array(stepDaysResult.suffix(count)) }

    func startLiveSteps(_ update: @escaping @MainActor (HealthStepDay) -> Void) -> Bool {
        liveUpdate = update
        return true
    }

    func stopLiveSteps() { liveStopped += 1 }

    func startMotionSamples(_ update: @escaping @MainActor ([MotionSample]) -> Void) -> Bool {
        motionUpdate = update
        return motionAvailable
    }

    func stopMotionSamples() { motionStopped += 1 }

    func samples(_ quantity: HealthQuantity, from: Date, to: Date) async throws -> [HealthSample] {
        (stored[quantity] ?? []).filter { $0.date >= from && $0.date <= to }
    }

    func save(_ writes: [HealthWrite]) async throws {
        saved.append(contentsOf: writes)
        for write in writes {
            for (quantity, amount) in write.values {
                stored[quantity, default: []].append(
                    HealthSample(entryId: write.entryId, version: write.version, amount: amount, date: write.date, ours: true))
            }
        }
    }

    func delete(_ quantities: [HealthQuantity], entryIds: [String], from: Date, to: Date) async throws {
        deleted.append((quantities, entryIds))
        for quantity in quantities {
            stored[quantity] = (stored[quantity] ?? []).filter { !($0.ours && entryIds.contains($0.entryId)) }
        }
    }
}

@MainActor
final class FakePackageApi {
    var calls: [(method: String, suffix: String, body: JSONObject?)] = []
    var replies: [String: JSONObject] = [:]
    var failures: [String: OrgApiException] = [:]

    var call: ToolsApiCall {
        { [self] method, suffix, body in
            self.calls.append((method, suffix, body))
            if let failure = self.failures[method + " " + suffix] { throw failure }
            return self.replies[method + " " + suffix] ?? self.replies[method] ?? ["ok": true]
        }
    }

    func bodies(_ method: String, _ suffix: String) -> [JSONObject] {
        calls.filter { $0.method == method && $0.suffix == suffix }.compactMap(\.body)
    }
}

@MainActor
final class HealthTests: XCTestCase {
    let utc = TimeZone(identifier: "UTC")!
    let now = Date(timeIntervalSince1970: 1_789_561_860)

    func testReconcilePlansSavesReplacementsAndDeletions() async {
        let date = now
        let desired = [
            HealthWrite(entryId: "a", version: "1", date: date, values: [.water: 250]),
            HealthWrite(entryId: "b", version: "2", date: date, values: [.water: 500]),
            HealthWrite(entryId: "c", version: "1", date: date, values: [.water: 100]),
            HealthWrite(entryId: "c", version: "9", date: date, values: [.water: 100]),
        ]
        let existing = [
            HealthSample(entryId: "a", version: "1", amount: 250, date: date, ours: true),
            HealthSample(entryId: "b", version: "1", amount: 400, date: date, ours: true),
            HealthSample(entryId: "gone", version: "1", amount: 50, date: date, ours: true),
            HealthSample(entryId: "UUID-1", version: "", amount: 300, date: date, ours: false),
        ]
        let plan = HealthReconcile.plan(desired: desired, existing: existing)
        XCTAssertEqual(plan.save.map(\.entryId), ["b", "c"])
        XCTAssertEqual(plan.delete, ["b", "gone"])
        XCTAssertTrue(HealthReconcile.plan(desired: [desired[0]], existing: [existing[0]]).isEmpty)
    }

    func testImportsSkipOurOwnKnownAndEmptySamples() async {
        let samples = [
            HealthSample(entryId: "x", version: "", amount: 200, date: now, ours: false),
            HealthSample(entryId: "y", version: "", amount: 300, date: now.addingTimeInterval(-60), ours: false),
            HealthSample(entryId: "y", version: "", amount: 300, date: now, ours: false),
            HealthSample(entryId: "z", version: "", amount: 0, date: now, ours: false),
            HealthSample(entryId: "mine", version: "1", amount: 250, date: now, ours: true),
        ]
        XCTAssertEqual(HealthReconcile.imports(samples, known: ["x"]).map(\.entryId), ["y"])
    }

    func testSampleDatesStayOnTheirDayAndNeverInTheFuture() async {
        let at = HealthDates.epochMs(now.addingTimeInterval(-3600))
        XCTAssertEqual(HealthDates.sampleDate(day: "2026-09-16", atMs: at, timeZone: utc, now: now),
                       now.addingTimeInterval(-3600))
        let noon = HealthDates.sampleDate(day: "2026-09-10", atMs: at, timeZone: utc, now: now)
        XCTAssertEqual(noon.map { CaloriesText.isoDay($0, timeZone: utc) }, "2026-09-10")
        XCTAssertEqual(noon?.timeIntervalSince1970.truncatingRemainder(dividingBy: 86400), 43200)
        XCTAssertEqual(HealthDates.sampleDate(day: "2026-09-16", atMs: 0, timeZone: utc, now: now),
                       Date(timeIntervalSince1970: 1_789_560_000))
        let morning = now.addingTimeInterval(-4 * 3600)
        XCTAssertEqual(HealthDates.sampleDate(day: "2026-09-16", atMs: 0, timeZone: utc, now: morning), morning)
        XCTAssertNil(HealthDates.sampleDate(day: "2026-09-17", atMs: 0, timeZone: utc, now: now))
        XCTAssertNil(HealthDates.sampleDate(day: "soon", atMs: 0, timeZone: utc, now: now))
        XCTAssertEqual(HealthDates.days(ending: "2026-09-02", count: 3, timeZone: utc), ["2026-08-31", "2026-09-01", "2026-09-02"])
    }

    func testStepSyncChunksAndRespectsTheInterval() async throws {
        let api = FakePackageApi()
        api.replies["POST"] = ["ok": true, "synced": 62]
        let days = (0..<70).map { HealthStepDay(day: String(format: "2026-07-%02d", $0 % 28 + 1), steps: $0) }
        let synced = try await StepsHealthSync.push(days, device: "iphone", call: api.call)
        XCTAssertEqual(synced, 124)
        XCTAssertEqual(api.bodies("POST", "/sync").map { $0["days"]?.arrayValue?.count }, [62, 8])
        XCTAssertEqual(api.bodies("POST", "/sync").first?["source"], "health")

        let health = FakeHealth()
        health.stepDaysResult = [HealthStepDay(day: "2026-09-16", steps: 4000, distanceM: 3000.44)]
        let prefs = InMemoryKeyValueStore()
        let quiet = FakePackageApi()
        let off = await StepsHealthSync.runIfDue(health: health, prefs: prefs, call: quiet.call, now: now)
        XCTAssertEqual(off, 0)
        prefs.set(true, forKey: HealthPrefs.steps)
        quiet.replies["POST"] = ["ok": true, "synced": 1]
        let first = await StepsHealthSync.runIfDue(health: health, prefs: prefs, call: quiet.call, now: now)
        XCTAssertEqual(first, 1)
        XCTAssertEqual(quiet.bodies("POST", "/sync").first?["days"]?.arrayValue?.first?.objectValue?["distanceM"], .double(3000.4))
        let soon = await StepsHealthSync.runIfDue(health: health, prefs: prefs, call: quiet.call, now: now.addingTimeInterval(60))
        XCTAssertEqual(soon, 0)
        let forced = await StepsHealthSync.runIfDue(
            health: health, prefs: prefs, call: quiet.call, now: now.addingTimeInterval(60), force: true)
        XCTAssertEqual(forced, 1)
    }

    func stepsOverview(steps: Int, synced: Int64 = 0) -> JSONObject {
        let days: [JSONValue] = (0..<30).map { index in
            .object(["day": .string(String(format: "2026-%02d-%02d", index < 14 ? 8 : 9, index < 14 ? index + 18 : index - 13)),
                     "steps": .int(Int64(index == 29 ? steps : 8000 + index * 100)), "distanceM": 5000])
        }
        var health: JSONObject = [:]
        if synced > 0 { health = ["updatedAt": .int(synced), "steps": .int(Int64(steps))] }
        return [
            "ok": true, "installed": true, "actor": "alice", "day": "2026-09-16",
            "profile": ["goal": 10000, "units": "metric", "unitsPreference": "auto"],
            "today": ["day": "2026-09-16", "steps": .int(Int64(steps)), "distanceM": 3200.5, "floors": 4, "activeKcal": 0,
                      "sources": .object(synced > 0 ? ["health": .object(health)] : [:])],
            "days": .array(days),
            "summary": ["weekSteps": 61204, "averageSteps": 9100, "streak": 1, "goalDays": 3, "bestDay": "2026-09-15", "bestSteps": 13050],
        ]
    }

    func testStepsModelInstallsTurnsOnHealthAndSyncs() async {
        let api = FakePackageApi()
        api.replies["GET ?day=2026-09-16"] = ["ok": true, "installed": false, "actor": "alice"]
        let health = FakeHealth()
        health.stepDaysResult = [HealthStepDay(day: "2026-09-15", steps: 12000), HealthStepDay(day: "2026-09-16", steps: 5400)]
        let prefs = InMemoryKeyValueStore()
        let model = StepsModel(api: StepsAPI(call: api.call), health: health, prefs: prefs, timeZone: utc, now: { [now] in now })
        await model.load()
        XCTAssertTrue(model.showsInstall)
        api.replies["GET ?day=2026-09-16"] = stepsOverview(steps: 0)
        api.replies["POST /sync"] = ["ok": true, "synced": 2]
        await model.install()
        XCTAssertEqual(api.bodies("POST", "/install").first?["tzOffsetMinutes"], 0)
        XCTAssertEqual(health.authorized, [[.steps]])
        XCTAssertTrue(model.healthEnabled)
        XCTAssertEqual(prefs.bool(forKey: HealthPrefs.steps), true)
        XCTAssertEqual(api.bodies("POST", "/sync").first?["days"]?.arrayValue?.count, 2)
        XCTAssertEqual(api.bodies("POST", "/sync").first?["device"], "iphone")

        api.replies["GET ?day=2026-09-16"] = stepsOverview(steps: 5400, synced: HealthDates.epochMs(now))
        await model.load()
        XCTAssertEqual(model.stepsText, "5,400")
        XCTAssertEqual(model.goalLine, "of 10,000 steps")
        XCTAssertEqual(model.detailLine, "4,600 to go · 3.20 km · 4 floors")
        XCTAssertEqual(model.ringRatio, 0.54, accuracy: 0.0001)
        XCTAssertEqual(model.sourceLine, "Counting with Apple Health · synced 12:31 PM")
        XCTAssertEqual(model.stats.map(\.value), ["61,204 steps", "9,100 steps", "1 day", "13,050 · Tue Sep 15"])
        XCTAssertEqual(model.bars.count, 30)
        XCTAssertEqual(model.bars.last?.steps, 5400)
        XCTAssertEqual(model.bars.first?.label, "8/18")

        model.startLive()
        health.liveUpdate?(HealthStepDay(day: "2026-09-16", steps: 6100, distanceM: 3900))
        health.liveUpdate?(HealthStepDay(day: "2026-09-15", steps: 99999))
        XCTAssertEqual(model.stepsText, "6,100")
        XCTAssertEqual(model.bars.last?.steps, 6100)
        await model.leave()
        XCTAssertEqual(health.liveStopped, 1)
        XCTAssertEqual(api.bodies("POST", "/sync").count, 2)
    }

    func testStepsModelPaceFollowsLiveCadence() async {
        let api = FakePackageApi()
        api.replies["GET ?day=2026-09-16"] = stepsOverview(steps: 5400)
        let health = FakeHealth()
        let prefs = InMemoryKeyValueStore()
        var clock = now
        let model = StepsModel(api: StepsAPI(call: api.call), health: health, prefs: prefs, timeZone: utc, now: { clock })
        await model.load(syncIfDue: false)
        XCTAssertEqual(model.paceLine, StepsModel.paceOffMessage)
        prefs.set(true, forKey: HealthPrefs.steps)
        let enabled = StepsModel(api: StepsAPI(call: api.call), health: health, prefs: prefs, timeZone: utc, now: { clock })
        await enabled.load(syncIfDue: false)
        enabled.startLive()
        XCTAssertEqual(enabled.paceLine, "Standing still")
        health.liveUpdate?(HealthStepDay(day: "2026-09-16", steps: 5500, cadence: 1.8))
        XCTAssertEqual(enabled.stepsPerMinute, 108)
        XCTAssertEqual(enabled.paceLine, "Walking · 108 steps/min · 2.3 mph · 42 min to 10,000")
        clock = now.addingTimeInterval(4)
        health.liveUpdate?(HealthStepDay(day: "2026-09-16", steps: 5510))
        XCTAssertEqual(enabled.paceLine, "Running · 150 steps/min · 4.4 mph · 30 min to 10,000")
        XCTAssertTrue(enabled.running)
        clock = now.addingTimeInterval(60)
        health.liveUpdate?(HealthStepDay(day: "2026-09-16", steps: 5520))
        XCTAssertEqual(enabled.cadence, 0)
        health.liveUpdate?(HealthStepDay(day: "2026-09-16", steps: 5600, cadence: 1.4))
        XCTAssertEqual(enabled.paceLine, "Strolling · 84 steps/min · 1.8 mph · 53 min to 10,000")
        health.liveUpdate?(HealthStepDay(day: "2026-09-16", steps: 5610, cadence: 2, pace: 0.6))
        XCTAssertEqual(enabled.strideM, 1 / 1.2, accuracy: 0.001)
        XCTAssertEqual(enabled.paceLine, "Brisk walk · 120 steps/min · 3.7 mph · 37 min to 10,000")
        enabled.stopLive()
        XCTAssertEqual(enabled.paceLine, "Standing still")
        XCTAssertEqual(StepsText.gait(130), "Brisk walk")
        XCTAssertEqual(StepsText.speed(1, imperial: true), "2.2 mph")
        XCTAssertEqual(StepsText.minutesToGoal(5, goal: 10000), "5 min to 10,000")
    }

    func testStepsModelAccelerometerDrivesSpeedAndDetectsRunning() async {
        let api = FakePackageApi()
        api.replies["GET ?day=2026-09-16"] = stepsOverview(steps: 5400)
        let health = FakeHealth()
        health.motionAvailable = true
        let prefs = InMemoryKeyValueStore()
        prefs.set(true, forKey: HealthPrefs.steps)
        let model = StepsModel(api: StepsAPI(call: api.call), health: health, prefs: prefs, timeZone: utc, now: { self.now })
        await model.load(syncIfDue: false)
        model.startLive()
        XCTAssertTrue(model.sensing)
        health.motionUpdate?(strideSignal(from: 100, seconds: 5, cadence: 1.8, impact: 0.35))
        XCTAssertEqual(model.gait, .walking)
        XCTAssertEqual(model.stepsPerMinute, 108)
        XCTAssertFalse(model.running)
        health.liveUpdate?(HealthStepDay(day: "2026-09-16", steps: 5500, cadence: 0.5))
        XCTAssertEqual(model.stepsPerMinute, 108)
        XCTAssertEqual(model.paceLine, "Walking · 108 steps/min · 2.3 mph · 42 min to 10,000")
        health.motionUpdate?(strideSignal(from: 105, seconds: 4, cadence: 2.4, impact: 1.5))
        XCTAssertEqual(model.gait, .running)
        XCTAssertTrue(model.running)
        XCTAssertEqual(model.stepsPerMinute, 144)
        XCTAssertEqual(model.paceLine, "Running · 144 steps/min · 4.2 mph · 32 min to 10,000")
        health.motionUpdate?(strideSignal(from: 109, seconds: 3, cadence: 0, impact: 0))
        XCTAssertEqual(model.paceLine, "Standing still")
        XCTAssertFalse(model.running)
        model.stopLive()
        XCTAssertFalse(model.sensing)
        XCTAssertEqual(health.motionStopped, 1)
    }

    func testStepsModelRefusedHealthStaysOffAndManualStepsConvertUnits() async {
        let api = FakePackageApi()
        var overview = stepsOverview(steps: 0)
        overview["profile"] = ["goal": 8000, "units": "imperial", "unitsPreference": "imperial"]
        api.replies["GET ?day=2026-09-16"] = overview
        let health = FakeHealth()
        health.allow = false
        let model = StepsModel(api: StepsAPI(call: api.call), health: health, timeZone: utc, now: { [now] in now })
        await model.load()
        await model.enableHealth()
        XCTAssertFalse(model.healthEnabled)
        XCTAssertTrue(model.notice?.message.contains("not allowed") == true)
        XCTAssertEqual(model.detailLine, "8,000 to go · 1.99 mi · 4 floors")
        await model.addSteps(day: "2026-09-15", steps: 3000, distance: 1)
        XCTAssertEqual(api.bodies("PUT", "/days/2026-09-15").first?["distanceM"], .double(1609.3))
        await model.saveGoal(12000, units: "metric")
        XCTAssertEqual(api.bodies("PUT", "/profile").first?["goal"], 12000)
        XCTAssertTrue(api.calls.allSatisfy { $0.suffix != "/sync" })
    }

    func testWaterModelWritesDrinksToHealthAndImportsOtherApps() async {
        let api = FakePackageApi()
        let entries: JSONValue = [
            ["id": "e1", "day": "2026-09-16", "amountMl": 250, "drink": "water", "source": "quick",
             "at": .int(HealthDates.epochMs(now.addingTimeInterval(-600)))],
            ["id": "e2", "day": "2026-09-15", "amountMl": 500, "drink": "tea", "source": "manual", "at": 0],
            ["id": "e3", "day": "2026-09-16", "amountMl": 300, "drink": "water", "source": "health", "externalId": "HK-1",
             "at": .int(HealthDates.epochMs(now.addingTimeInterval(-7200)))],
        ]
        api.replies["GET ?day=2026-09-16"] = [
            "ok": true, "installed": true, "actor": "alice",
            "profile": ["goalMl": 2000, "units": "metric", "unitsPreference": "auto", "presetsMl": [250, 500, 750]],
            "today": ["day": "2026-09-16", "totalMl": 550, "entries": 2], "entries": entries,
            "days": [["day": "2026-09-16", "totalMl": 550, "entries": 2]],
            "summary": ["averageMl": 1800, "goalDays": 12, "streak": 2],
        ]
        api.replies["GET /entries?from=2026-09-10&to=2026-09-16"] = ["ok": true, "entries": entries]
        api.replies["POST /import"] = ["ok": true, "imported": 1]
        let health = FakeHealth()
        health.stored[.water] = [
            HealthSample(entryId: "HK-1", version: "", amount: 300, date: now.addingTimeInterval(-7200), ours: false),
            HealthSample(entryId: "HK-2", version: "", amount: 330, date: now.addingTimeInterval(-3600), ours: false),
            HealthSample(entryId: "old", version: "1", amount: 100, date: now.addingTimeInterval(-9000), ours: true),
        ]
        let prefs = InMemoryKeyValueStore()
        let model = WaterModel(api: WaterAPI(call: api.call), health: health, prefs: prefs, timeZone: utc, now: { [now] in now })
        await model.load()
        XCTAssertFalse(model.healthEnabled)
        XCTAssertTrue(health.saved.isEmpty)
        await model.enableHealth()
        XCTAssertEqual(health.authorized, [[.water]])
        let imported = api.bodies("POST", "/import").first?["entries"]?.arrayValue ?? []
        XCTAssertEqual(imported.count, 1)
        XCTAssertEqual(imported.first?.objectValue?["externalId"], "HK-2")
        XCTAssertEqual(imported.first?.objectValue?["amountMl"], 330)
        XCTAssertEqual(Set(health.saved.map(\.entryId)), ["e1", "e2"])
        XCTAssertEqual(health.saved.first { $0.entryId == "e2" }?.values[.water], 500)
        XCTAssertEqual(health.deleted.flatMap(\.ids), ["old"])
        let savedCount = health.saved.count
        await model.load()
        XCTAssertEqual(health.saved.count, savedCount, "unchanged drinks are not written twice")

        XCTAssertEqual(model.totalText, "550 ml")
        XCTAssertEqual(model.goalLine, "1.45 L to go of 2 L")
        XCTAssertEqual(model.presets.map(\.label), ["250 ml", "500 ml", "750 ml"])
        XCTAssertEqual(model.rows.first?.title, "Water")
        XCTAssertEqual(model.rows.first?.subtitle, "10:31 AM · From Apple Health")
        XCTAssertEqual(model.summaryLine, "Average 1.8 L · goal met on 12 of 30 days · streak 2 days")

        await model.add(milliliters: 500, drink: "water")
        XCTAssertEqual(api.bodies("POST", "/entries").first?["amountMl"], 500)
        XCTAssertEqual(api.bodies("POST", "/entries").first?["source"], "quick")
    }

    func testWaterUnitsAndTypedAmounts() async {
        XCTAssertEqual(WaterText.amount(250, units: "metric"), "250 ml")
        XCTAssertEqual(WaterText.amount(1500, units: "metric"), "1.5 L")
        XCTAssertEqual(WaterText.amount(2000, units: "metric"), "2 L")
        XCTAssertEqual(WaterText.amount(237, units: "imperial"), "8.0 fl oz")
        XCTAssertEqual(WaterText.amount(710, units: "imperial"), "24 fl oz")
        XCTAssertEqual(WaterText.amount(237, units: "cups"), "1 cup")
        XCTAssertEqual(WaterText.amount(2000, units: "cups"), "8.5 cups")
        let api = FakePackageApi()
        api.replies["GET ?day=2026-09-16"] = [
            "ok": true, "installed": true, "profile": ["goalMl": 2000, "units": "imperial"],
            "today": ["day": "2026-09-16", "totalMl": 0, "entries": 0],
        ]
        let model = WaterModel(api: WaterAPI(call: api.call), health: nil, timeZone: utc, now: { [now] in now })
        await model.load()
        await model.addTyped("12", drink: "coffee")
        XCTAssertEqual(api.bodies("POST", "/entries").first?["amountMl"], 355)
        XCTAssertEqual(api.bodies("POST", "/entries").first?["drink"], "coffee")
        await model.addTyped("0", drink: "water")
        XCTAssertEqual(api.bodies("POST", "/entries").count, 1)
    }

    func testCaloriesWriteRecentMealsToHealthAndFollowEdits() async {
        let fake = FakeCaloriesApi(installed: true)
        let health = FakeHealth()
        let prefs = InMemoryKeyValueStore()
        let model = CaloriesModel(api: fake.api, timeZone: utc, now: { [now] in now })
        model.attachHealth(health, prefs: prefs)
        await model.load()
        XCTAssertTrue(health.saved.isEmpty)
        await model.enableHealth()
        XCTAssertEqual(health.authorized, [[.nutrition]])
        XCTAssertEqual(prefs.bool(forKey: HealthPrefs.nutrition), true)
        XCTAssertEqual(health.saved.map(\.entryId).sorted(), ["e1", "e2", "e3", "e4", "e5", "e6", "e7"])
        let wrap = health.saved.first { $0.entryId == "e1" }
        XCTAssertEqual(wrap?.values[.energy], 890)
        XCTAssertEqual(wrap?.values[.protein], 53)
        XCTAssertEqual(wrap?.name, "Chicken wrap")
        let count = health.saved.count
        await model.load()
        XCTAssertEqual(health.saved.count, count)

        health.stored[.energy] = (health.stored[.energy] ?? []) + [
            HealthSample(entryId: "deleted-meal", version: "1", amount: 300, date: now.addingTimeInterval(-60), ours: true),
        ]
        await model.load()
        XCTAssertEqual(health.deleted.last?.ids, ["deleted-meal"])
        XCTAssertEqual(health.deleted.last?.quantities, [.energy, .protein, .carbs, .fat])

        let second = CaloriesModel(api: fake.api, timeZone: utc, now: { [now] in now })
        second.attachHealth(health, prefs: prefs)
        XCTAssertTrue(second.healthEnabled, "the choice is kept on the device")
        second.disableHealth()
        XCTAssertFalse(second.healthEnabled)
    }
}

final class AppleHealthGuideTests: XCTestCase {
    func testEveryToolIsDescribedOnce() {
        XCTAssertEqual(AppleHealthGuide.uses.map(\.tool), AppleHealthTool.allCases)
        for tool in AppleHealthTool.allCases {
            let use = AppleHealthGuide.use(tool)
            XCTAssertEqual(use.tool, tool)
            XCTAssertFalse(use.name.isEmpty)
            XCTAssertFalse(use.reads.isEmpty)
            XCTAssertFalse(use.writes.isEmpty)
            XCTAssertFalse(use.account.isEmpty)
        }
    }

    func testTheGuideNamesAppleHealthAndHealthKit() {
        XCTAssertTrue(AppleHealthGuide.badge.contains("Apple Health"))
        XCTAssertTrue(AppleHealthGuide.framework.contains("HealthKit"))
        XCTAssertTrue(AppleHealthGuide.summary.contains("HealthKit"))
        XCTAssertTrue(AppleHealthGuide.promises.contains { $0.contains("advertising") })
    }
}
