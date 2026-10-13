// The Calorie Counter fed a fake package API
// answering from memory, driven through CaloriesModel and its sheets.

import FMApi
import FMBase
import XCTest
@testable import FMTools

@MainActor
final class FakeCaloriesApi {
    var installed: Bool
    private(set) var calls: [(method: String, suffix: String, body: JSONObject?)] = []
    private(set) var installs: [Int] = []
    var nextResult: JSONObject = ["ok": true]
    var failures: [String: OrgApiException] = [:]

    init(installed: Bool) { self.installed = installed }

    var api: CaloriesAPI {
        CaloriesAPI(
            overview: { [self] day in self.overview(day) },
            call: { [self] method, suffix, body in
                self.calls.append((method, suffix, body))
                if let failure = self.failures[suffix] { throw failure }
                return self.nextResult
            },
            install: { [self] offset in
                self.installs.append(offset)
                self.installed = true
            })
    }

    func overview(_ day: String) -> JSONObject {
        if !installed { return ["ok": true, "installed": false, "actor": "alice"] }
        let days: [JSONValue] = (0..<30).reversed().map { i in
            ["day": .string(day), "entries": .int(i % 2 == 0 ? 2 : 0), "calories": .int(i % 2 == 0 ? 1800 : 0),
             "proteinG": 80, "carbsG": 200, "fatG": 60]
        }
        return [
            "ok": true, "installed": true, "actor": "alice", "installedAt": 1, "today": .string(day),
            "profile": [
                "dailyCalories": 2000, "proteinG": 100, "carbsG": 250, "fatG": 65, "targetWeightKg": 75,
                "startWeightKg": 84, "heightCm": 178, "units": "metric", "tzOffsetMinutes": 0,
                "reminders": [["time": "12:30", "label": "Log lunch", "onlyIfUnlogged": true, "enabled": true]],
            ],
            "todayTotals": ["calories": 1310, "proteinG": 71, "carbsG": 140, "fatG": 38, "entries": 2],
            "entriesToday": [
                ["id": .string(String(repeating: "b", count: 32)), "day": .string(day), "meal": "lunch", "source": "barcode",
                 "calories": 890, "proteinG": 53, "carbsG": 80, "fatG": 26, "loggedAt": 2, "name": "Chicken wrap",
                 "portion": "1 serving", "brand": "Wrapsody", "notes": ""],
                ["id": .string(String(repeating: "a", count: 32)), "day": .string(day), "meal": "breakfast", "source": "photo",
                 "calories": 420, "proteinG": 18, "carbsG": 60, "fatG": 12, "loggedAt": 1, "name": "Oatmeal with berries",
                 "portion": "1 bowl", "brand": "", "notes": ""],
            ],
            "entries": [
                ["id": "e1", "day": .string(day), "meal": "lunch", "calories": 890, "proteinG": 53, "carbsG": 80, "fatG": 26,
                 "name": "Chicken wrap", "portion": "1 serving", "brand": "Wrapsody"],
                ["id": "e2", "day": .string(day), "meal": "breakfast", "calories": 420, "proteinG": 18, "carbsG": 60, "fatG": 12,
                 "name": "Oatmeal with berries", "portion": "1 bowl", "brand": ""],
                ["id": "e3", "day": "2026-09-13", "meal": "breakfast", "calories": 400, "proteinG": 17, "carbsG": 58, "fatG": 11,
                 "name": "oatmeal with berries ", "portion": "1 bowl", "brand": ""],
                ["id": "e4", "day": "2026-09-12", "meal": "breakfast", "calories": 420, "proteinG": 18, "carbsG": 60, "fatG": 12,
                 "name": "Oatmeal with berries", "portion": "1 bowl", "brand": ""],
                ["id": "e5", "day": "2026-09-11", "meal": "lunch", "calories": 890, "proteinG": 53, "carbsG": 80, "fatG": 26,
                 "name": "Chicken wrap", "portion": "1 serving", "brand": "Wrapsody"],
                ["id": "e6", "day": "2026-09-10", "meal": "snack", "calories": 150, "proteinG": 25, "carbsG": 10, "fatG": 0,
                 "name": "Skyr", "portion": "1 cup", "brand": ""],
                ["id": "e7", "day": "2026-09-09", "meal": "snack", "calories": 150, "proteinG": 25, "carbsG": 10, "fatG": 0,
                 "name": "Skyr", "portion": "1 cup", "brand": ""],
            ],
            "days": .array(days),
            "weights": [
                ["day": "2026-09-01", "weightKg": 84.0, "loggedAt": 1],
                ["day": .string(day), "weightKg": 81.2, "loggedAt": 2],
            ],
            "latestWeight": ["weightKg": 81.2, "day": .string(day)],
            "streaks": ["logged": 3, "onGoal": 2],
            "weekAverage": 1840,
            "challenges": [[
                "id": .string(String(repeating: "c", count: 32)), "kind": "weight_loss_percent", "status": "active",
                "title": "Spring shred", "description": "", "target": 0, "startsOn": "2026-09-01", "endsOn": "2026-09-30",
                "createdBy": "maya", "createdAt": 1, "isCreator": false, "myStatus": "invited", "participants": 1, "invited": 1,
            ]],
            "sources": ["openFoodFacts": true, "usda": false, "ai": true],
            "quickFoods": [
                ["name": "Skyr", "brand": "Siggi's", "portion": "1 cup", "calories": 150, "proteinG": 25, "carbsG": 10,
                 "fatG": 0, "source": "receipt"],
            ],
            "commonFoods": [
                ["name": "Egg", "portion": "1 large", "calories": 72, "proteinG": 6.3, "carbsG": 0.4, "fatG": 4.8, "category": "breakfast"],
                ["name": "Toast with butter", "portion": "1 slice", "calories": 120, "proteinG": 2.7, "carbsG": 13.0, "fatG": 6.0,
                 "category": "breakfast"],
                ["name": "Banana", "portion": "1 medium", "calories": 105, "proteinG": 1.3, "carbsG": 27.0, "fatG": 0.4, "category": "fruit"],
                ["name": "Coffee with milk", "portion": "1 mug (240 ml)", "calories": 30, "proteinG": 1.6, "carbsG": 2.4, "fatG": 1.5,
                 "category": "drinks"],
            ],
        ]
    }
}

final class CaloriesTests: XCTestCase {
    let utc = TimeZone(identifier: "UTC")!
    /// Monday 2026-09-14, 12:00 UTC.
    let now = Date(timeIntervalSince1970: 1_789_387_200)

    @MainActor
    private func loaded(installed: Bool = true) async -> (CaloriesModel, FakeCaloriesApi) {
        let fake = FakeCaloriesApi(installed: installed)
        let model = CaloriesModel(api: fake.api, timeZone: utc, now: { [now] in now })
        await model.load()
        return (model, fake)
    }

    @MainActor
    func testShowsTodaysTotalsEntriesAndTheChallengesTab() async {
        let (model, fake) = await loaded()
        XCTAssertEqual(model.day, "2026-09-14")
        XCTAssertEqual(model.caloriesText, "1310")
        XCTAssertEqual(model.goalLine, "of 2000 kcal")
        XCTAssertEqual(model.remainingLine, "690 kcal left today")
        XCTAssertEqual(model.ring.ratio, 0.655, accuracy: 1e-9)
        XCTAssertFalse(model.ring.over)
        // Breakfast before lunch, whatever order the Worker sent.
        XCTAssertEqual(model.entryRows.map(\.title), ["Oatmeal with berries", "Chicken wrap"])
        XCTAssertEqual(model.entryRows[1].trailing, "890 kcal")
        XCTAssertEqual(model.entryRows[1].subtitle, "Lunch · 1 serving · Wrapsody · P 53 · C 80 · F 26")
        XCTAssertEqual(model.entryRows[0].subtitle, "Breakfast · 1 bowl · P 18 · C 60 · F 12")
        XCTAssertEqual(model.macros[0], CaloriesMacro(label: "Protein", amounts: "71 g / 100 g", ratio: 0.71))
        XCTAssertEqual(model.streaksText, "3 days logged · 2 within goal")
        XCTAssertEqual(model.weightStat, "81.2 kg on Mon Sep 14")
        XCTAssertEqual(model.diaryHeader, "Diary for Mon Sep 14")
        XCTAssertNil(model.photoUnavailable)

        let challenges = model.challengeRows
        XCTAssertEqual(challenges.map(\.title), ["Spring shred"])
        XCTAssertTrue(challenges[0].invited)
        XCTAssertEqual(challenges[0].dates, "Tue Sep 1 → Wed Sep 30 · 1 in · 1 invited · by maya")
        XCTAssertEqual(challenges[0].kind, "Lose the most percent of body weight")

        XCTAssertEqual(model.days.count, 30)
        XCTAssertEqual(model.trendsNote, "Seven-day average 1840 kcal · goal 2000 kcal · blue within goal, orange over")
        XCTAssertEqual(model.dayRows.first?.calories, "1800 kcal")
        XCTAssertEqual(model.dayRows.last?.calories, "—")
        XCTAssertTrue(fake.calls.isEmpty)
    }

    @MainActor
    func testAddsAManualEntryThroughThePackageAPI() async {
        let (model, fake) = await loaded()
        let draft = model.entryDraft(nil)
        XCTAssertEqual(draft.sheetTitle, "Add food")
        XCTAssertEqual(draft.meal, "lunch")
        draft.name = "Banana"
        draft.calories = "105"
        await model.saveEntry(draft)

        XCTAssertEqual(fake.calls.count, 1)
        let call = fake.calls[0]
        XCTAssertEqual(call.method, "POST")
        XCTAssertEqual(call.suffix, "/entries")
        XCTAssertEqual(call.body?["name"], "Banana")
        XCTAssertEqual(call.body?["calories"], 105)
        XCTAssertEqual(call.body?["source"], "manual")
        XCTAssertEqual(call.body?["day"], "2026-09-14")
        XCTAssertEqual(model.notice?.message, "Saved to your diary.")
    }

    @MainActor
    func testEditingAndDeletingAnEntry() async {
        let (model, fake) = await loaded()
        let entry = model.entries[0]
        let draft = model.entryDraft(entry)
        XCTAssertEqual(draft.sheetTitle, "Edit food")
        XCTAssertEqual(draft.calories, "420")
        XCTAssertEqual(draft.protein, "18")
        XCTAssertEqual(draft.meal, "breakfast")
        draft.name = "  "
        await model.saveEntry(draft)
        XCTAssertTrue(fake.calls.isEmpty)
        draft.name = "Porridge"
        await model.saveEntry(draft)
        XCTAssertEqual(fake.calls.last?.method, "PATCH")
        XCTAssertEqual(fake.calls.last?.suffix, "/entries/\(String(repeating: "a", count: 32))")
        XCTAssertNil(fake.calls.last?.body?["source"])
        await model.deleteEntry(entry)
        XCTAssertEqual(fake.calls.last?.method, "DELETE")
        XCTAssertEqual(model.notice?.message, "Entry deleted.")
    }

    @MainActor
    func testOffersAnyAccountThePersonalInstallPanel() async {
        let (model, fake) = await loaded(installed: false)
        XCTAssertTrue(model.showsInstall)
        XCTAssertFalse(CaloriesModel.installMessage.contains("organization"))
        XCTAssertTrue(CaloriesModel.installMessage.contains("private to your ForkMesh account"))
        await model.install()
        XCTAssertEqual(fake.installs, [0])
        XCTAssertTrue(fake.calls.isEmpty)
        XCTAssertFalse(model.showsInstall)
        XCTAssertEqual(model.entryRows.first?.title, "Oatmeal with berries")
        XCTAssertEqual(model.notice?.message, "Calorie Counter installed.")
    }

    @MainActor
    func testAnUninstalledPackageAnswerShowsThePanelNotAnError() async {
        let model = CaloriesModel(
            api: CaloriesAPI(overview: { _ in throw OrgApiException("plugin_not_installed", "not installed") },
                             call: { _, _, _ in [:] }, install: { _ in }),
            timeZone: utc, now: { [now] in now })
        await model.load()
        XCTAssertTrue(model.showsInstall)
        XCTAssertEqual(model.error, "")

        let broken = CaloriesModel(
            api: CaloriesAPI(overview: { _ in throw ApiException("offline") }, call: { _, _, _ in [:] }, install: { _ in }),
            timeZone: utc, now: { [now] in now })
        await broken.load()
        XCTAssertTrue(broken.showsError)
        XCTAssertEqual(broken.error, "Calorie Counter is unavailable right now.")
    }

    @MainActor
    func testInvitesChallengeFriendsByTypedUsername() async {
        let (model, fake) = await loaded()
        fake.nextResult = ["ok": true, "invited": ["maya", "sam"], "skipped": ["ghost"]]
        let draft = model.challengeDraft()
        XCTAssertEqual(draft.endsOn, "2026-10-12")
        XCTAssertEqual(draft.startsLabel, "Starts Mon Sep 14")
        draft.title = "Step up"
        draft.invite = "@Maya, sam ghost"
        await model.createChallenge(draft)

        XCTAssertEqual(fake.calls.count, 1)
        XCTAssertEqual(fake.calls[0].method, "POST")
        XCTAssertEqual(fake.calls[0].suffix, "/challenges")
        XCTAssertEqual(fake.calls[0].body?["invite"], ["maya", "sam", "ghost"])
        XCTAssertEqual(model.notice?.message, "Challenge created. Invited maya, sam. Skipped ghost.")
    }

    @MainActor
    func testTheBoardOffersWhatThisMemberMayDo() async {
        let (model, fake) = await loaded()
        fake.nextResult = ["ok": true, "challenge": [
            "id": "c1", "title": "Spring shred", "kind": "log_streak", "status": "active", "myStatus": "invited",
            "isCreator": false, "startsOn": "2026-09-20", "endsOn": "2026-09-30", "createdBy": "maya", "winner": "",
            "leaderboard": [
                ["rank": 1, "name": "maya", "measured": true, "score": 4, "detail": "4 days"],
                ["rank": 0, "name": "alice", "measured": false, "score": 0, "detail": ""],
            ],
            "pending": [["name": "sam", "status": "invited"]],
        ]]
        let board = await model.openChallenge("c1")!
        XCTAssertEqual(fake.calls.last?.suffix, "/challenges/c1")
        XCTAssertEqual(board.actions, [.accept, .decline])
        XCTAssertEqual(board.subtitle, "Most days logged · Sun Sep 20 → Wed Sep 30 · started by maya")
        XCTAssertEqual(board.members.map(\.rank), ["1", "–"])
        XCTAssertEqual(board.members[1].name, "alice (you)")
        XCTAssertEqual(board.members[0].progress, 1)
        XCTAssertEqual(board.pending, "Invited or left: sam (invited)")

        fake.nextResult = ["ok": true]
        await model.boardAction(.accept, board: board)
        XCTAssertEqual(fake.calls.last?.suffix, "/challenges/c1/respond")
        XCTAssertEqual(fake.calls.last?.body, ["accept": true])
        XCTAssertEqual(model.notice?.message, "You are in. Good luck!")

        // Ending before the start date ends on the start date.
        await model.boardAction(.close, board: board)
        XCTAssertEqual(fake.calls.last?.body, ["title": "Spring shred", "status": "closed", "endsOn": "2026-09-20"])

        let owned = CaloriesBoard(challenge: ["status": "closed", "isCreator": true, "myStatus": "accepted"], me: "alice", dayLabel: { $0 })
        XCTAssertEqual(owned.actions, [.delete])
        let running = CaloriesBoard(challenge: ["status": "active", "isCreator": true, "myStatus": "accepted"], me: "alice", dayLabel: { $0 })
        XCTAssertEqual(running.actions, [.invite, .close, .delete])
        let member = CaloriesBoard(challenge: ["status": "active", "isCreator": false, "myStatus": "accepted"], me: "alice", dayLabel: { $0 })
        XCTAssertEqual(member.actions, [.leave])
    }

    @MainActor
    func testWeighInsBarcodesAndSuggestions() async {
        let (model, fake) = await loaded()
        XCTAssertEqual(model.weighInTitle, "Weigh in · Mon Sep 14")
        XCTAssertEqual(model.weighInInitial, "81.2")
        await model.saveWeighIn("abc")
        XCTAssertTrue(fake.calls.isEmpty)
        await model.saveWeighIn(" 80.5 ")
        XCTAssertEqual(fake.calls.last?.method, "PUT")
        XCTAssertEqual(fake.calls.last?.suffix, "/weights/2026-09-14")
        XCTAssertEqual(fake.calls.last?.body, ["weightKg": 80.5])
        await model.removeWeighIn()
        XCTAssertEqual(fake.calls.last?.method, "DELETE")

        let short = await model.lookupBarcode("12-34")
        XCTAssertNil(short)
        fake.nextResult = ["ok": true, "product": [
            "name": "Oat milk", "brand": "Oatly", "barcode": "7394376616037", "quantity": "1 l",
            "servingSize": "250 ml", "perServing": ["calories": 120, "proteinG": 2.5, "carbsG": 16.25, "fatG": 5],
            "per100g": ["calories": 48, "proteinG": 1, "carbsG": 6.5, "fatG": 2],
        ]]
        let portion = await model.lookupBarcode("7394 3766 16037")!
        XCTAssertEqual(fake.calls.last?.suffix, "/barcode/7394376616037")
        XCTAssertEqual(portion.basis, "serving")
        XCTAssertEqual(portion.subtitle, "Oatly · 1 l · Open Food Facts")
        XCTAssertEqual(portion.bases.map(\.label), ["1 serving (250 ml)", "100 g", "1 g"])
        portion.amount = "2"
        XCTAssertEqual(portion.summary, "240 kcal · P 5 g · C 32.5 g · F 10 g")
        portion.basis = "1g"
        portion.amount = "30"
        XCTAssertEqual(portion.values()["portion"], "30 g")
        XCTAssertEqual(portion.values()["calories"], 14)
        fake.nextResult = ["ok": true]
        await model.addPortion(portion)
        XCTAssertEqual(fake.calls.last?.suffix, "/entries")
        XCTAssertEqual(fake.calls.last?.body?["source"], "barcode")
        XCTAssertEqual(fake.calls.last?.body?["meal"], "lunch")
        XCTAssertEqual(model.notice?.message, "1 item(s) added to Mon Sep 14.")

        fake.nextResult = ["ok": true, "source": "text", "summary": "Looks like breakfast.", "totalCalories": 300, "items": [
            ["name": "Eggs", "portion": "2", "calories": 150, "proteinG": 12, "carbsG": 1, "fatG": 10, "confidence": 0.9],
            ["name": "Toast", "portion": "1 slice", "calories": 150, "proteinG": 4, "carbsG": 20, "fatG": 5, "confidence": 0.5],
        ]]
        let suggestions = await model.describeMeal("2 eggs and toast")!
        XCTAssertEqual(fake.calls.last?.body, ["description": "2 eggs and toast"])
        XCTAssertEqual(suggestions.sheetTitle, "Suggested foods · about 300 kcal")
        XCTAssertEqual(suggestions.detail(0), "2 · confidence 90% · P 12 · C 1 · F 10")
        suggestions.chosen[1] = false
        suggestions.kcal[0] = "160"
        fake.nextResult = ["ok": true]
        await model.addSuggestions(suggestions)
        XCTAssertEqual(fake.calls.last?.body?["calories"], 160)
        XCTAssertEqual(fake.calls.last?.body?["source"], "text")
        XCTAssertEqual(fake.calls.last?.body?["name"], "Eggs")
    }

    @MainActor
    func testSearchRunsAsTheNameIsTyped() async {
        var queries: [String] = []
        let wrap: JSONObject = [
            "name": "Buffalo Style Chicken Wrap", "brand": "Trader Joe's", "servingSize": "1 wrap",
            "per100g": ["calories": 199], "perServing": ["calories": 452],
        ]
        let model = CaloriesSearchModel { (query: String) in
            queries.append(query)
            return [wrap]
        }
        model.query = "trader joes buffalo"
        model.queryChanged()
        model.query = "trader  joes buffalo chicken wrap "
        model.queryChanged()
        try? await Task.sleep(nanoseconds: CaloriesSearchModel.debounceNanoseconds + 200_000_000)
        XCTAssertEqual(queries, ["trader joes buffalo chicken wrap"])
        XCTAssertEqual(model.results.count, 1)
        XCTAssertEqual(model.subtitle(wrap), "Trader Joe's · 199 kcal / 100 g · 452 kcal per 1 wrap")
        XCTAssertEqual(model.submitted()?["name"], "Buffalo Style Chicken Wrap")

        model.query = "t"
        model.queryChanged()
        XCTAssertTrue(model.results.isEmpty)
        model.query = "trader joes buffalo chicken wrap"
        model.queryChanged()
        try? await Task.sleep(nanoseconds: 50_000_000)
        XCTAssertEqual(queries.count, 1)
        XCTAssertEqual(model.results.count, 1)
    }

    @MainActor
    func testGoalsFormBody() async {
        let (model, _) = await loaded()
        let goals = model.goalsModel()
        XCTAssertEqual(goals.calories, "2000")
        XCTAssertEqual(goals.target, "75")
        XCTAssertEqual(goals.height, "178")
        XCTAssertEqual(goals.unitsPreference, "metric")
        XCTAssertEqual(goals.bmiLine, "BMI 25.6 from your latest weigh-in.")
        XCTAssertEqual(goals.reminders.map(\.label), ["Log lunch"])
        goals.addReminder(hour: 7, minute: 5)
        XCTAssertEqual(goals.reminders.last?.time, "07:05")
        goals.setUnits("imperial")
        XCTAssertEqual(goals.weightUnit, "lb")
        goals.target = "150"
        let body = goals.body()
        XCTAssertEqual(body["dailyCalories"], 2000)
        XCTAssertEqual(body["unitsPreference"], "imperial")
        XCTAssertEqual(body["targetWeightKg"]?.doubleValue ?? 0, 150 * CaloriesText.kgPerLb, accuracy: 1e-9)
        XCTAssertEqual(body["tzOffsetMinutes"], 0)
        XCTAssertEqual(body["reminders"]?.arrayValue?.count, 2)
        XCTAssertEqual(goals.usdaLine.hasSuffix("No USDA key saved."), true)
        XCTAssertEqual(goals.unitChoices.first?.label, "Automatic (match my location)")
    }

    func testThePromptParserSplitsCountsAndMatches() {
        XCTAssertEqual(CaloriesQuick.parts("2 eggs and toast with butter for breakfast"), ["2 eggs", "toast with butter"])
        XCTAssertEqual(CaloriesQuick.parts("I just had coffee with milk, a banana + 2 eggs; toast & jam\nyesterday"),
                       ["coffee with milk", "a banana", "2 eggs", "toast", "jam"])
        XCTAssertEqual(CaloriesQuick.parts("Lunch: a banana"), ["a banana"])
        XCTAssertEqual(CaloriesQuick.meal("2 eggs for Breakfast"), "breakfast")
        XCTAssertEqual(CaloriesQuick.meal("pasta at supper"), "dinner")
        XCTAssertEqual(CaloriesQuick.meal("chips as snacks"), "snack")
        XCTAssertNil(CaloriesQuick.meal("a banana"))
        XCTAssertEqual(CaloriesQuick.day("a banana yesterday", logDay: "2026-09-10", today: "2026-09-14", timeZone: utc), "2026-09-13")
        XCTAssertEqual(CaloriesQuick.day("toast this morning", logDay: "2026-09-10", today: "2026-09-14", timeZone: utc), "2026-09-14")
        XCTAssertEqual(CaloriesQuick.day("toast", logDay: "2026-09-10", today: "2026-09-14", timeZone: utc), "2026-09-10")
        XCTAssertEqual(CaloriesQuick.singular("sandwiches"), "sandwich")
        XCTAssertEqual(CaloriesQuick.singular("boxes"), "box")
        XCTAssertEqual(CaloriesQuick.singular("glass"), "glass")
        XCTAssertEqual(CaloriesQuick.singular("eggs"), "egg")
        XCTAssertEqual(CaloriesQuick.normalise("  Toast-with  BUTTER! "), "toast with butter")

        let foods: [CaloriesQuickFood] = [
            CaloriesQuickFood(name: "Egg", portion: "1 large", calories: 72, proteinG: 6.3, carbsG: 0.4, fatG: 4.8),
            CaloriesQuickFood(name: "Toast with butter", portion: "1 slice", calories: 120, proteinG: 2.7, carbsG: 13, fatG: 6),
            CaloriesQuickFood(name: "Banana", portion: "1 medium", calories: 105, proteinG: 1.3, carbsG: 27, fatG: 0.4),
            CaloriesQuickFood(name: "Latte", calories: 190, proteinG: 12, carbsG: 18, fatG: 7),
        ]
        let breakfast = CaloriesQuick.match("2 eggs and toast with butter for breakfast", foods: foods)
        XCTAssertEqual(breakfast?.map(\.name), ["Egg", "Toast with butter"])
        XCTAssertEqual(breakfast?[0].portion, "2 × 1 large")
        XCTAssertEqual(breakfast?[0].calories, 144)
        XCTAssertEqual(breakfast?[0].proteinG, 12.6)
        XCTAssertEqual(breakfast?[0].fatG, 9.6)
        XCTAssertEqual(breakfast?[1].portion, "1 slice")
        XCTAssertEqual(breakfast?[1].calories, 120)

        let half = CaloriesQuick.match("half a banana", foods: foods)
        XCTAssertEqual(half?.first?.portion, "0.5 × 1 medium")
        XCTAssertEqual(half?.first?.calories, 53)
        XCTAssertEqual(half?.first?.carbsG, 13.5)
        XCTAssertEqual(CaloriesQuick.match("1/2 banana", foods: foods), half)
        XCTAssertEqual(CaloriesQuick.match("a couple of eggs", foods: foods)?.first?.calories, 144)
        XCTAssertEqual(CaloriesQuick.match("3x latte", foods: foods)?.first?.portion, "3 × serving")
        XCTAssertEqual(CaloriesQuick.match("an egg", foods: foods)?.first?.portion, "1 large")
        XCTAssertNil(CaloriesQuick.match("2 eggs and a unicorn steak", foods: foods))
        XCTAssertNil(CaloriesQuick.match("25 eggs", foods: foods))
        XCTAssertNil(CaloriesQuick.match("0 eggs", foods: foods))
        XCTAssertNil(CaloriesQuick.match("for breakfast", foods: foods))

        let kg = CaloriesQuick.weight("weighed 72.5 kg", imperial: false, logDay: "2026-09-10", today: "2026-09-14", timeZone: utc)
        XCTAssertEqual(kg, CaloriesWeighIn(kg: 72.5, day: "2026-09-10"))
        XCTAssertEqual(CaloriesQuick.weight("My weight is 72,5 yesterday", imperial: false, logDay: "2026-09-10",
                                            today: "2026-09-14", timeZone: utc)?.day, "2026-09-13")
        XCTAssertEqual(CaloriesQuick.weight("weight 160 lbs", imperial: false, logDay: "d", today: "d")?.kg ?? 0,
                       160 * CaloriesText.kgPerLb, accuracy: 1e-9)
        XCTAssertEqual(CaloriesQuick.weight("I weigh 150", imperial: true, logDay: "d", today: "d")?.kg ?? 0,
                       150 * CaloriesText.kgPerLb, accuracy: 1e-9)
        XCTAssertNil(CaloriesQuick.weight("had 2 eggs", imperial: false, logDay: "d", today: "d"))
        XCTAssertNil(CaloriesQuick.weight("weighed 7 kg of rice", imperial: false, logDay: "d", today: "d"))

        XCTAssertEqual(CaloriesQuick.loggedMessage(breakfast ?? [], meal: "breakfast", dayLabel: nil, goal: 2000, todayCalories: 1574),
                       "Logged to breakfast, today: Egg (144 kcal), Toast with butter (120 kcal). 264 kcal in all. Today: 1574 of 2000 kcal.")
        XCTAssertEqual(CaloriesQuick.loggedMessage(half ?? [], meal: "snack", dayLabel: "Sun Sep 13", goal: 2000, todayCalories: 10),
                       "Logged to snack, Sun Sep 13: Banana (53 kcal). 53 kcal in all.")
        XCTAssertEqual(CaloriesQuick.receiptMessage(["store": "Corner Shop", "found": ["Skyr", "Hummus"], "added": ["Hummus"]]),
                       "Added 1 food from Corner Shop to My foods: Hummus.")
        XCTAssertEqual(CaloriesQuick.receiptMessage(["store": "", "found": ["Skyr", "Hummus"], "added": []]),
                       "Read 2 foods; all were already in My foods.")
    }

    @MainActor
    func testQuickAddListsStarsRemovesAndReadsReceipts() async {
        let (model, fake) = await loaded()
        XCTAssertEqual(model.myFoods.map(\.name), ["Skyr"])
        XCTAssertEqual(model.myFoods.first?.source, "receipt")
        XCTAssertEqual(model.frequentFoods.map(\.name), ["Oatmeal with berries", "Chicken wrap"])
        XCTAssertEqual(model.frequentFoods.map(\.times), [3, 2])
        XCTAssertEqual(model.frequentFoods.first?.meta, "1 bowl · 420 kcal · logged 3×")
        XCTAssertEqual(model.commonFoods.count, 4)
        XCTAssertEqual(model.quickSections.map(\.kind), [.mine, .frequent, .common])
        XCTAssertEqual(model.quickHeader, "Quick add to today")
        XCTAssertNil(model.receiptUnavailable)
        model.quickFilter = "EGG"
        XCTAssertEqual(model.quickSections[0].foods, [])
        XCTAssertEqual(model.quickSections[0].empty, "No match.")
        XCTAssertEqual(model.quickSections[2].foods.map(\.name), ["Egg"])
        model.quickFilter = "siggi"
        XCTAssertEqual(model.quickSections[0].foods.map(\.name), ["Skyr"])
        model.quickFilter = ""

        let banana = model.commonFoods[2]
        model.quickMeal = "snack"
        await model.quickAdd(banana)
        XCTAssertEqual(fake.calls.last?.method, "POST")
        XCTAssertEqual(fake.calls.last?.suffix, "/entries")
        XCTAssertEqual(fake.calls.last?.body, [
            "name": "Banana", "brand": "", "portion": "1 medium", "calories": 105, "proteinG": 1.3, "carbsG": 27.0, "fatG": 0.4,
            "day": "2026-09-14", "meal": "snack", "source": "quick",
        ])
        XCTAssertEqual(model.notice?.message, "Logged Banana (105 kcal) to snack.")

        await model.starFood(model.myFoods[0])
        XCTAssertEqual(model.notice?.message, "Skyr is already in My foods.")
        let before = fake.calls.count
        fake.nextResult = ["ok": true, "quickFoods": [
            ["name": "Egg", "brand": "", "portion": "1 large", "calories": 72, "proteinG": 6.3, "carbsG": 0.4, "fatG": 4.8, "source": "saved"],
            ["name": "Skyr", "brand": "Siggi's", "portion": "1 cup", "calories": 150, "proteinG": 25, "carbsG": 10, "fatG": 0, "source": "receipt"],
        ]]
        await model.starFood(model.commonFoods[0])
        XCTAssertEqual(fake.calls.count, before + 1)
        XCTAssertEqual(fake.calls.last?.method, "PUT")
        XCTAssertEqual(fake.calls.last?.suffix, "/quick-foods")
        let sent = fake.calls.last?.body?["foods"]?.arrayValue?.compactMap(\.objectValue) ?? []
        XCTAssertEqual(sent.map { ToolsJSON.show($0["name"]) }, ["Egg", "Skyr"])
        XCTAssertNil(sent[0]["source"])
        XCTAssertEqual(sent[1]["source"], "receipt")
        XCTAssertEqual(model.myFoods.map(\.name), ["Egg", "Skyr"])
        XCTAssertTrue(model.isMyFood(model.commonFoods[0]))
        XCTAssertEqual(model.notice?.message, "Saved Egg to My foods.")

        fake.nextResult = ["ok": true, "quickFoods": [
            ["name": "Skyr", "brand": "Siggi's", "portion": "1 cup", "calories": 150, "source": "receipt"],
        ]]
        await model.removeMyFood(model.myFoods[0])
        XCTAssertEqual(fake.calls.last?.body?["foods"]?.arrayValue?.count, 1)
        XCTAssertEqual(model.myFoods.map(\.name), ["Skyr"])
        XCTAssertEqual(model.notice?.message, "Removed Egg from My foods.")

        fake.nextResult = ["ok": true, "store": "Corner Shop", "found": ["Skyr", "Hummus"], "added": ["Hummus"], "quickFoods": [
            ["name": "Hummus", "portion": "2 tbsp", "calories": 70, "source": "receipt"],
            ["name": "Skyr", "brand": "Siggi's", "portion": "1 cup", "calories": 150, "source": "receipt"],
        ]]
        await model.readReceipt(dataURL: "data:image/jpeg;base64,AQID")
        XCTAssertEqual(fake.calls.last?.suffix, "/receipt")
        XCTAssertEqual(fake.calls.last?.body, ["image": "data:image/jpeg;base64,AQID"])
        XCTAssertEqual(model.myFoods.map(\.name), ["Hummus", "Skyr"])
        XCTAssertEqual(model.notice?.message, "Added 1 food from Corner Shop to My foods: Hummus.")
        XCTAssertFalse(model.readingReceipt)

        for (code, message) in [
            ("rate_limited", "You have used this hour's AI reads. Try the receipt again later."),
            ("no_foods_on_receipt", "No foods were found on that receipt. Try a sharper, flatter photo."),
            ("ai_unavailable", "Receipt reading needs AI, which is unavailable on this host."),
            ("invalid_image", "Could not read the receipt: invalid image"),
        ] {
            fake.failures["/receipt"] = OrgApiException(code, code.replacingOccurrences(of: "_", with: " "))
            await model.readReceipt(dataURL: "data:image/jpeg;base64,AQID")
            XCTAssertEqual(model.notice?.message, message)
        }
    }

    @MainActor
    func testTypedMealsWeighInsAndEstimatesLogToTheDiary() async throws {
        let (model, fake) = await loaded()
        let breakfast = try await model.logTyped("2 eggs and toast with butter for breakfast")
        let posts = fake.calls.filter { $0.suffix == "/entries" }
        XCTAssertEqual(posts.count, 2)
        XCTAssertEqual(posts[0].body?["name"], "Egg")
        XCTAssertEqual(posts[0].body?["portion"], "2 × 1 large")
        XCTAssertEqual(posts[0].body?["calories"], 144)
        XCTAssertEqual(posts[0].body?["meal"], "breakfast")
        XCTAssertEqual(posts[0].body?["source"], "quick")
        XCTAssertEqual(posts[0].body?["day"], "2026-09-14")
        XCTAssertEqual(posts[1].body?["name"], "Toast with butter")
        XCTAssertEqual(breakfast,
                       "Logged to breakfast, today: Egg (144 kcal), Toast with butter (120 kcal). 264 kcal in all. Today: 1310 of 2000 kcal.")

        let half = try await model.logTyped("half a banana")
        XCTAssertEqual(fake.calls.last?.body?["meal"], "lunch")
        XCTAssertEqual(fake.calls.last?.body?["calories"], 53)
        XCTAssertTrue(half.hasPrefix("Logged to lunch, today: Banana (53 kcal)."))

        let oats = try await model.logTyped("oatmeal with berries yesterday")
        XCTAssertEqual(fake.calls.last?.body?["day"], "2026-09-13")
        XCTAssertEqual(fake.calls.last?.body?["brand"], "")
        XCTAssertEqual(oats, "Logged to lunch, Sun Sep 13: Oatmeal with berries (420 kcal). 420 kcal in all.")

        let weight = try await model.logTyped("weighed 72.5 kg")
        XCTAssertEqual(fake.calls.last?.method, "PUT")
        XCTAssertEqual(fake.calls.last?.suffix, "/weights/2026-09-14")
        XCTAssertEqual(fake.calls.last?.body, ["weightKg": 72.5])
        XCTAssertEqual(weight, "Saved 72.5 kg for today.")

        fake.nextResult = ["ok": true, "source": "text", "items": [
            ["name": "Unicorn steak", "portion": "1 steak", "calories": 700, "proteinG": 60, "carbsG": 0, "fatG": 50, "confidence": 0.4],
        ]]
        let guessed = try await model.logTyped("a unicorn steak for dinner")
        let analyze = fake.calls.first { $0.suffix == "/analyze" }
        XCTAssertEqual(analyze?.body, ["description": "a unicorn steak for dinner"])
        XCTAssertEqual(fake.calls.last?.suffix, "/entries")
        XCTAssertEqual(fake.calls.last?.body?["source"], "text")
        XCTAssertEqual(fake.calls.last?.body?["meal"], "dinner")
        XCTAssertEqual(fake.calls.last?.body?["confidence"], 0.4)
        XCTAssertTrue(guessed.hasPrefix("Logged to dinner, today: Unicorn steak (700 kcal)."))

        fake.failures["/analyze"] = OrgApiException("rate_limited", "rate limited")
        do {
            _ = try await model.logTyped("dragon fruit salad")
            XCTFail("expected a failure")
        } catch let failure as ShellPromptFailure {
            XCTAssertEqual(failure.message, "You have used this hour's AI estimates. Try again later, or use Quick add.")
        }
        do {
            _ = try await model.logTyped("   ")
            XCTFail("expected a failure")
        } catch let failure as ShellPromptFailure {
            XCTAssertEqual(failure.message, "Type what you ate, or what you weigh.")
        }
    }

    func testCaloriesText() {
        XCTAssertEqual(CaloriesText.isoDay(now, timeZone: utc), "2026-09-14")
        XCTAssertEqual(CaloriesText.dayLabel("2026-09-14", timeZone: utc), "Mon Sep 14")
        XCTAssertEqual(CaloriesText.dayLabel("2026-09-13", timeZone: utc), "Sun Sep 13")
        XCTAssertNil(CaloriesText.tryParseDay("nope"))
        XCTAssertEqual(CaloriesText.grams(12.0), "12")
        XCTAssertEqual(CaloriesText.grams(12.25), "12.3")
        XCTAssertEqual(CaloriesText.int("105"), 105)
        XCTAssertEqual(CaloriesText.int("2.5"), 3)
        XCTAssertEqual(CaloriesText.int(JSONValue.string("oops")), 0)
        XCTAssertEqual(CaloriesText.guessMeal(hour: 7), "breakfast")
        XCTAssertEqual(CaloriesText.guessMeal(hour: 14), "lunch")
        XCTAssertEqual(CaloriesText.guessMeal(hour: 20), "dinner")
        XCTAssertEqual(CaloriesText.guessMeal(hour: 23), "snack")
        XCTAssertEqual(CaloriesText.usernames("@Maya, sam ghost,,@@sam"), ["maya", "sam", "ghost"])
        XCTAssertEqual(CaloriesText.inviteNote("Invitations sent.", [:]), "Invitations sent.")
        XCTAssertEqual(CaloriesText.tzOffsetMinutes(timeZone: TimeZone(secondsFromGMT: 7200)!, now: now), -120)
        XCTAssertEqual(CaloriesText.parseClock("12:30").hour, 12)
        XCTAssertEqual(CaloriesText.parseClock("x").minute, 0)
        XCTAssertEqual(CaloriesModel.encodeQueryComponent("oat milk&co"), "oat+milk%26co")
        XCTAssertEqual(CaloriesPhoto.dataURL(bytes: Data([1, 2, 3]), fileExtension: "JPG"), "data:image/jpeg;base64,AQID")
        XCTAssertNil(CaloriesPhoto.dataURL(bytes: Data([1, 2, 3]), fileExtension: "heic"))
        XCTAssertNil(CaloriesPhoto.dataURL(bytes: Data(count: CaloriesPhoto.limit + 1), fileExtension: "png"))
        XCTAssertEqual(CaloriesPhoto.receiptDataURL(jpeg: Data([1, 2, 3])), "data:image/jpeg;base64,AQID")
        XCTAssertNil(CaloriesPhoto.receiptDataURL(jpeg: Data(count: 1_200_000)))
    }

    func testChartGeometry() {
        let ring = CaloriesRing(calories: 2500, goal: 2000)
        XCTAssertEqual(ring.ratio, 1)
        XCTAssertTrue(ring.over)
        XCTAssertEqual(CaloriesRing(calories: 100, goal: 0).ratio, 0)

        let days: [JSONObject] = [
            ["day": "2026-09-13", "entries": 1, "calories": 2300],
            ["day": "2026-09-14", "entries": 0, "calories": 0],
        ]
        let chart = CaloriesDayChart(days: days, goal: 2000, width: 342, height: 200)
        XCTAssertEqual(chart.peak, 2300)
        XCTAssertEqual(chart.bars.count, 1)
        XCTAssertTrue(chart.bars[0].over)
        XCTAssertEqual(chart.bars[0].width, 14)
        XCTAssertEqual(chart.labels.last?.text, "09-14")
        XCTAssertEqual(chart.labels.first?.text, "0")
        XCTAssertNotNil(chart.goalY)
        XCTAssertEqual(chart.dashes.first?.0, 36)

        let weights: [JSONObject] = [["day": "2026-09-01", "weightKg": 84.0], ["day": "2026-09-14", "weightKg": 81.2]]
        let weight = CaloriesWeightChart(points: weights, target: 75, imperial: false, width: 348, height: 180, timeZone: utc)
        XCTAssertEqual(weight.low, 74)
        XCTAssertEqual(weight.high, 85)
        XCTAssertEqual(weight.points.first?.x, 40)
        XCTAssertEqual(weight.points.last?.x ?? 0, 340, accuracy: 1e-9)
        XCTAssertEqual(weight.grid.map(\.label), ["74", "77.7", "81.3", "85"])
        XCTAssertNotNil(weight.targetY)
    }
}
