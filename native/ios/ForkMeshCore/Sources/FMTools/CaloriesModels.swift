// The state and decisions of the Calorie Counter screen: the Calorie Counter
// package on the account-level Worker API (`/api/calories`): install panel, Today, Trends, Goals
// and Challenges, and the sheets (entry, suggestions, search, portion, goals form, challenge,
// invite, board). Photos are picked and, when too large, re-encoded by the app (CaloriesPhoto);
// barcodes are typed.

import FMApi
import FMBase
import Foundation
import Observation

/// One inline notice to show. A new value replaces the one on screen.
public struct ToolsNotice: Equatable, Identifiable, Sendable {
    public let id: UUID
    public let message: String
    public let isError: Bool

    public init(_ message: String, isError: Bool = false) {
        id = UUID()
        self.message = message
        self.isError = isError
    }
}

/// The Calorie Counter API the screen calls (ApiService's `caloriesOverview`, `caloriesApi`,
/// `installCalories`), as closures so tests can answer from memory.
public struct CaloriesAPI {
    public var overview: @MainActor (_ day: String) async throws -> JSONObject
    public var call: ToolsApiCall
    public var install: @MainActor (_ tzOffsetMinutes: Int) async throws -> Void

    public init(overview: @escaping @MainActor (_ day: String) async throws -> JSONObject,
                call: @escaping ToolsApiCall,
                install: @escaping @MainActor (_ tzOffsetMinutes: Int) async throws -> Void) {
        self.overview = overview
        self.call = call
        self.install = install
    }

    @MainActor
    public init(api: ApiService) {
        overview = { day in try await api.caloriesOverview(day) }
        call = { method, suffix, body in try await api.caloriesApi(method, suffix, body) }
        install = { offset in try await api.installCalories(tzOffsetMinutes: offset) }
    }
}

public struct CaloriesEntryRow: Equatable, Identifiable, Sendable {
    public let id: String
    public let title: String
    public let subtitle: String
    public let trailing: String
    public let entry: JSONObject
}

public struct CaloriesMacro: Equatable, Sendable {
    public let label: String
    public let amounts: String
    public let ratio: Double
}

public struct CaloriesChallengeRow: Equatable, Identifiable, Sendable {
    public let id: String
    public let title: String
    public let status: String
    public let active: Bool
    public let kind: String
    public let dates: String
    public let invited: Bool
}

@MainActor
@Observable
public final class CaloriesModel {
    public static let title = "Calorie Counter"
    public static let tabs = ["Today", "Trends", "Goals", "Challenges"]
    public static let installTitle = "Install Calorie Counter for yourself"
    public static let installMessage =
        "Your food diary, goals, reminders, charts, and weigh-ins are private to your ForkMesh account and sync across web, desktop, and mobile. Every row is encrypted at rest with a key minted for your account alone. Invite friends to challenges by username."
    public static let installButton = "Install Calorie Counter"
    public static let unavailableTitle = "Calorie Counter is unavailable"
    public static let emptyDiary = "Nothing logged yet. Snap a photo, type a barcode, or add a meal."
    public static let challengesIntro =
        "Challenge friends by their ForkMesh username to lose weight or keep a logging streak. Standings show progress only, never anyone's diary."
    public static let noChallenges = "No challenges yet. Start one and invite your friends."

    @ObservationIgnored public let api: CaloriesAPI
    @ObservationIgnored public let timeZone: TimeZone
    @ObservationIgnored private let now: () -> Date
    @ObservationIgnored public private(set) var health: HealthBridge?
    @ObservationIgnored public private(set) var prefs: KeyValueStore = InMemoryKeyValueStore()
    public private(set) var healthEnabled = false

    public private(set) var overview: JSONObject?
    public private(set) var installed = false
    public var day: String
    public private(set) var loading = true
    public private(set) var error = ""
    public var tab = 0
    public var notice: ToolsNotice?
    public var quickFilter = ""
    public var quickMeal: String
    public private(set) var readingReceipt = false

    public init(api: CaloriesAPI, timeZone: TimeZone = .current, now: (() -> Date)? = nil) {
        self.api = api
        self.timeZone = timeZone
        let clock = now ?? { Date() }
        self.now = clock
        day = CaloriesText.isoDay(clock(), timeZone: timeZone)
        quickMeal = CaloriesText.guessMeal(now: clock(), timeZone: timeZone)
    }

    public convenience init(api: ApiService) {
        self.init(api: CaloriesAPI(api: api))
    }

    // MARK: Loading

    public func load() async {
        loading = true
        error = ""
        do {
            let overview = try await api.overview(day)
            let installed = overview["installed"]?.boolValue == true
            self.installed = installed
            self.overview = installed ? overview : nil
            loading = false
            if installed && healthEnabled { await reconcileHealth() }
        } catch let error as OrgApiException {
            if error.code == "plugin_not_installed" {
                installed = false
                loading = false
                overview = nil
                return
            }
            loading = false
            self.error = error.message
        } catch {
            loading = false
            self.error = message(error)
        }
    }

    func message(_ error: Error) -> String {
        (error as? OrgApiException)?.message ?? "Calorie Counter is unavailable right now."
    }

    func show(_ message: String, error: Bool = false) { notice = ToolsNotice(message, isError: error) }

    /// `_run`: the action, its success notice, then a reload; a failure shows an error notice instead.
    func run(success: String = "", _ action: () async throws -> Void) async {
        do {
            try await action()
            if !success.isEmpty { show(success) }
            await load()
        } catch {
            show(message(error), error: true)
        }
    }

    public func attachHealth(_ health: HealthBridge?, prefs: KeyValueStore) {
        self.health = health
        self.prefs = prefs
        healthEnabled = health?.available == true && prefs.bool(forKey: HealthPrefs.nutrition, or: false)
    }

    public var healthAvailable: Bool { health?.available == true }

    public var healthLine: String {
        if !healthAvailable { return "Apple Health is not available on this device." }
        if !healthEnabled { return "Turn on Apple Health to write the calories, protein, carbohydrates, and fat you log to Health." }
        return "Sharing with Apple Health: the last \(Self.healthDays) days of meals are written to Health as dietary energy and macros, and edits and deletions follow."
    }

    public static let healthDays = 14

    public func enableHealth() async {
        guard let health, health.available else {
            show("Apple Health is not available on this device.", error: true)
            return
        }
        guard await health.authorize([.nutrition]) else {
            show("ForkMesh was not allowed to write nutrition to Apple Health. Allow it in Settings > Health > Data Access & Devices.", error: true)
            return
        }
        prefs.set(true, forKey: HealthPrefs.nutrition)
        healthEnabled = true
        await reconcileHealth()
        show("Calorie Counter now writes your meals to Apple Health.")
    }

    public func disableHealth() {
        prefs.set(false, forKey: HealthPrefs.nutrition)
        healthEnabled = false
        show("Apple Health sharing is off. Meals already written to Health stay there.")
    }

    public func reconcileHealth() async {
        guard let health, health.available, healthEnabled, let overview else { return }
        let today = CaloriesText.isoDay(now(), timeZone: timeZone)
        let days = HealthDates.days(ending: today, count: Self.healthDays, timeZone: timeZone)
        guard let first = days.first, let start = HealthDates.startOfDay(first, timeZone: timeZone) else { return }
        let end = now()
        let entries = ToolsJSON.maps(overview["entries"]).filter {
            let day = $0["day"]?.stringValue ?? ""
            return day >= first && day <= today
        }
        let desired = Self.healthWrites(entries, timeZone: timeZone, now: end)
        do {
            let existing = try await health.samples(.energy, from: start, to: end)
            let plan = HealthReconcile.plan(desired: desired, existing: existing)
            if !plan.delete.isEmpty {
                try await health.delete([.energy, .protein, .carbs, .fat], entryIds: plan.delete, from: start, to: end)
            }
            if !plan.save.isEmpty { try await health.save(plan.save) }
        } catch {
            return
        }
    }

    public static func healthWrites(_ entries: [JSONObject], timeZone: TimeZone, now: Date) -> [HealthWrite] {
        entries.compactMap { entry in
            guard let id = entry["id"]?.stringValue, !id.isEmpty,
                let date = HealthDates.sampleDate(
                    day: entry["day"]?.stringValue ?? "", atMs: ToolsJSON.epoch(entry["loggedAt"]), timeZone: timeZone, now: now)
            else { return nil }
            let kcal = CaloriesText.num(entry["calories"])
            let protein = CaloriesText.num(entry["proteinG"])
            let carbs = CaloriesText.num(entry["carbsG"])
            let fat = CaloriesText.num(entry["fatG"])
            guard kcal > 0 else { return nil }
            var values: [HealthQuantity: Double] = [.energy: kcal]
            if protein > 0 { values[.protein] = protein }
            if carbs > 0 { values[.carbs] = carbs }
            if fat > 0 { values[.fat] = fat }
            let version = [kcal, protein, carbs, fat].map { DartCore.toStringAsFixed($0, 1) }.joined(separator: "/")
                + "@\(HealthDates.epochMs(date))"
            return HealthWrite(entryId: id, version: version, date: date, name: entry["name"]?.stringValue ?? "", values: values)
        }
    }

    public var showsSpinner: Bool { loading && overview == nil }
    public var showsError: Bool { !error.isEmpty && overview == nil }
    public var showsInstall: Bool { !showsSpinner && !showsError && !installed }

    public func install() async {
        let offset = CaloriesText.tzOffsetMinutes(timeZone: timeZone, now: now())
        await run(success: "Calorie Counter installed.") { try await self.api.install(offset) }
    }

    public func pickDay(_ date: Date) async {
        day = CaloriesText.isoDay(date, timeZone: timeZone)
        await load()
    }

    public func goToToday() async {
        day = CaloriesText.isoDay(now(), timeZone: timeZone)
        await load()
    }

    /// The date picker's bounds: 2020-01-01 to tomorrow.
    public var dayRange: ClosedRange<Date> {
        var c = DateComponents()
        c.year = 2020
        c.month = 1
        c.day = 1
        let first = CaloriesText.calendar(timeZone).date(from: c) ?? Date(timeIntervalSince1970: 0)
        return first...now().addingTimeInterval(86400)
    }

    public var dayDate: Date { CaloriesText.parseDay(day, timeZone: timeZone, now: now()) }

    public func dayLabel(_ day: String) -> String { CaloriesText.dayLabel(day, timeZone: timeZone, now: now()) }

    // MARK: Profile and units

    public var profile: JSONObject { ToolsJSON.object(overview?["profile"]) }
    public var imperial: Bool { profile["units"]?.stringValue == "imperial" }
    public var weightUnit: String { imperial ? "lb" : "kg" }
    public func displayWeight(_ kg: Double) -> Double { imperial ? kg / CaloriesText.kgPerLb : kg }
    public func toKg(_ shown: Double) -> Double { imperial ? shown * CaloriesText.kgPerLb : shown }

    public func weightText(_ kg: JSONValue?) -> String {
        CaloriesText.num(kg) > 0 ? "\(CaloriesText.grams(displayWeight(CaloriesText.num(kg)))) \(weightUnit)" : "—"
    }

    // MARK: Today

    public var goal: Int { CaloriesText.int(profile["dailyCalories"]) }
    public var totals: JSONObject { ToolsJSON.object(overview?["todayTotals"]) }
    public var calories: Double { CaloriesText.num(totals["calories"]) }
    public var ring: CaloriesRing { CaloriesRing(calories: calories, goal: goal) }
    public var caloriesText: String { CaloriesText.kcal(calories) }
    public var goalLine: String { "of \(goal) kcal" }

    public var remainingLine: String {
        if goal <= 0 { return "Set a daily goal on the Goals tab." }
        let g = Double(goal)
        return calories <= g ? "\(CaloriesText.kcal(g - calories)) kcal left today" : "\(CaloriesText.kcal(calories - g)) kcal over goal"
    }

    public var macros: [CaloriesMacro] {
        [("Protein", "proteinG"), ("Carbs", "carbsG"), ("Fat", "fatG")].map { label, key in
            let value = totals[key], target = profile[key]
            let ratio = CaloriesText.num(target) > 0 ? min(max(CaloriesText.num(value) / CaloriesText.num(target), 0), 1) : 0
            return CaloriesMacro(label: label, amounts: "\(CaloriesText.grams(value)) g / \(CaloriesText.grams(target)) g", ratio: ratio)
        }
    }

    public var streaksText: String {
        let streaks = ToolsJSON.object(overview?["streaks"])
        return "\(CaloriesText.int(streaks["logged"])) days logged · \(CaloriesText.int(streaks["onGoal"])) within goal"
    }

    public var latestWeight: JSONObject { ToolsJSON.object(overview?["latestWeight"]) }

    public var weightStat: String {
        let latest = latestWeight
        guard CaloriesText.num(latest["weightKg"]) > 0 else { return "No weigh-in yet" }
        return "\(weightText(latest["weightKg"])) on \(dayLabel(ToolsJSON.show(latest["day"])))"
    }

    public var diaryHeader: String { "Diary for \(dayLabel(day))" }

    /// Today's entries by meal, then by the time they were logged.
    public var entries: [JSONObject] {
        let meals = CaloriesText.meals
        func mealIndex(_ entry: JSONObject) -> Int { meals.firstIndex(of: ToolsJSON.show(entry["meal"])) ?? -1 }
        return ToolsJSON.maps(overview?["entriesToday"]).enumerated().sorted { a, b in
            let ka = mealIndex(a.element), kb = mealIndex(b.element)
            let ta = CaloriesText.int(a.element["loggedAt"]), tb = CaloriesText.int(b.element["loggedAt"])
            let order = (ka < kb ? -1 : ka > kb ? 1 : 0) * 1000 + (ta < tb ? -1 : ta > tb ? 1 : 0)
            return order != 0 ? order < 0 : a.offset < b.offset
        }.map(\.element)
    }

    public var entryRows: [CaloriesEntryRow] {
        entries.map { entry in
            let parts = [
                CaloriesText.cap(ToolsJSON.show(entry["meal"])),
                ToolsJSON.show(entry["portion"]),
                ToolsJSON.show(entry["brand"]),
                "P \(CaloriesText.grams(entry["proteinG"])) · C \(CaloriesText.grams(entry["carbsG"])) · F \(CaloriesText.grams(entry["fatG"]))",
            ].filter { !$0.isEmpty }
            return CaloriesEntryRow(
                id: ToolsJSON.show(entry["id"]), title: ToolsJSON.show(entry["name"]),
                subtitle: parts.joined(separator: " · "), trailing: "\(CaloriesText.kcal(entry["calories"])) kcal", entry: entry)
        }
    }

    // MARK: Entries

    public func entryDraft(_ entry: JSONObject?) -> CaloriesEntryDraft {
        CaloriesEntryDraft(entry: entry, day: day, meal: CaloriesText.guessMeal(now: now(), timeZone: timeZone))
    }

    /// The entry sheet closed with Save (`fields`) or Delete.
    public func saveEntry(_ draft: CaloriesEntryDraft) async {
        guard let fields = draft.result() else { return }
        if let entry = draft.entry {
            await run(success: "Saved to your diary.") {
                _ = try await self.api.call("PATCH", "/entries/\(ToolsJSON.show(entry["id"]))", fields)
            }
        } else {
            await run(success: "Saved to your diary.") { _ = try await self.api.call("POST", "/entries", fields) }
        }
    }

    public func deleteEntry(_ entry: JSONObject) async {
        await run(success: "Entry deleted.") {
            _ = try await self.api.call("DELETE", "/entries/\(ToolsJSON.show(entry["id"]))", [:])
        }
    }

    public func addEntries(_ items: [JSONObject], meal: String, source: String) async {
        let label = dayLabel(day)
        await run(success: "\(items.count) item(s) added to \(label).") {
            for item in items {
                var body = item
                body["day"] = .string(self.day)
                body["meal"] = .string(meal)
                body["source"] = .string(source)
                _ = try await self.api.call("POST", "/entries", body)
            }
        }
    }

    // MARK: Photo, describe, barcode, search

    /// The notice instead of the photo picker when this host has no AI model.
    public var photoUnavailable: String? {
        if overview?["sources"]?.objectValue?["ai"]?.boolValue == false {
            return "AI estimates are unavailable on this host. Describe the meal or add it by hand."
        }
        return nil
    }

    public static let photoHintPrompt = (title: "Anything the model should know?", hint: "half portion, brown rice… (optional)")
    public static let describePrompt = (title: "Describe the meal", hint: "2 scrambled eggs, 1 slice of toast with butter")
    public static let barcodePrompt = (title: "Type the barcode digits", hint: "Printed under the barcode")

    public func photoUnreadable() { show("That photo could not be read.", error: true) }

    /// `POST /analyze` with a photo; the suggestions to confirm, or nil after a notice.
    public func analyzePhoto(dataURL: String, hint: String) async -> CaloriesSuggestionsModel? {
        show("Asking the model about the photo…")
        return await analyze(["image": .string(dataURL), "hint": .string(hint)])
    }

    public func describeMeal(_ description: String) async -> CaloriesSuggestionsModel? {
        if description.isEmpty { return nil }
        show("Asking the model about the meal…")
        return await analyze(["description": .string(description)])
    }

    private func analyze(_ body: JSONObject) async -> CaloriesSuggestionsModel? {
        do {
            let payload = try await api.call("POST", "/analyze", body)
            return CaloriesSuggestionsModel(payload: payload, meal: CaloriesText.guessMeal(now: now(), timeZone: timeZone))
        } catch {
            show(message(error), error: true)
            return nil
        }
    }

    /// The sheet's "Add selected to diary".
    public func addSuggestions(_ model: CaloriesSuggestionsModel) async {
        await addEntries(model.chosenItems(), meal: model.meal, source: model.source)
    }

    /// The typed barcode's product for the portion sheet, or nil (a notice for a miss).
    public func lookupBarcode(_ code: String) async -> CaloriesPortionModel? {
        let digits = code.filter { $0.isASCII && $0.isNumber }
        if digits.count < 6 || digits.count > 14 { return nil }
        do {
            let payload = try await api.call("GET", "/barcode/\(digits)", nil)
            return portion(ToolsJSON.object(payload["product"]), source: "barcode")
        } catch let error as OrgApiException {
            show(error.code == "product_not_found" ? "No product found for \(digits). Try a search or add it by hand." : error.message, error: true)
            return nil
        } catch {
            show(message(error), error: true)
            return nil
        }
    }

    public func searchModel() -> CaloriesSearchModel {
        let call = api.call
        return CaloriesSearchModel { query in
            let payload = try await call("GET", "/foods?q=\(CaloriesModel.encodeQueryComponent(query))", nil)
            return ToolsJSON.maps(payload["foods"])
        }
    }

    public func portion(_ product: JSONObject, source: String) -> CaloriesPortionModel? {
        product.isEmpty ? nil : CaloriesPortionModel(product: product, source: source,
                                                     meal: CaloriesText.guessMeal(now: now(), timeZone: timeZone))
    }

    /// The portion sheet's "Add to diary".
    public func addPortion(_ model: CaloriesPortionModel) async {
        var values = model.values()
        let meal = DartCore.toString(values.removeValue(forKey: "meal"))
        await addEntries([values], meal: meal, source: model.source)
    }

    /// `Uri.encodeQueryComponent`: spaces as '+'.
    nonisolated static func encodeQueryComponent(_ text: String) -> String {
        let allowed = CharacterSet(charactersIn: "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~*")
        let spaced = text.addingPercentEncoding(withAllowedCharacters: allowed.union(CharacterSet(charactersIn: " "))) ?? text
        return spaced.replacingOccurrences(of: " ", with: "+")
    }

    // MARK: Weigh-in

    public var weighInTitle: String { "Weigh in · \(dayLabel(day))" }

    public var weighInInitial: String {
        let latest = CaloriesText.num(latestWeight["weightKg"])
        return latest > 0 ? CaloriesText.grams(displayWeight(latest)) : ""
    }

    public func saveWeighIn(_ text: String) async {
        guard let shown = DartCore.doubleTryParse(text.dartTrimmed), shown > 0 else { return }
        let kg = toKg(shown)
        await run(success: "Weigh-in saved.") { _ = try await self.api.call("PUT", "/weights/\(self.day)", ["weightKg": .double(kg)]) }
    }

    public func removeWeighIn() async {
        await run(success: "Weigh-in removed.") { _ = try await self.api.call("DELETE", "/weights/\(self.day)", [:]) }
    }

    public var todayDay: String { CaloriesText.isoDay(now(), timeZone: timeZone) }
    public var myFoods: [CaloriesQuickFood] { CaloriesQuick.foods(overview?["quickFoods"]) }
    public var commonFoods: [CaloriesQuickFood] { CaloriesQuick.foods(overview?["commonFoods"]) }

    public var frequentFoods: [CaloriesQuickFood] {
        let entries = overview?["entries"] != nil ? ToolsJSON.maps(overview?["entries"]) : ToolsJSON.maps(overview?["entriesToday"])
        return CaloriesQuick.frequent(entries: entries, mine: myFoods)
    }

    public var quickSections: [CaloriesQuickSection] {
        CaloriesQuick.sections(mine: myFoods, frequent: frequentFoods, common: commonFoods, filter: quickFilter)
    }

    public var quickHeader: String { "Quick add to \(day == todayDay ? "today" : dayLabel(day))" }

    public func isMyFood(_ food: CaloriesQuickFood) -> Bool { myFoods.contains { $0.id == food.id } }

    public func quickAdd(_ food: CaloriesQuickFood) async {
        let meal = quickMeal
        var body = food.json
        body["day"] = .string(day)
        body["meal"] = .string(meal)
        body["source"] = "quick"
        await run(success: "Logged \(food.name) (\(CaloriesText.kcal(food.calories)) kcal) to \(meal).") {
            _ = try await self.api.call("POST", "/entries", body)
        }
    }

    public func starFood(_ food: CaloriesQuickFood) async {
        let mine = myFoods
        if mine.contains(where: { $0.id == food.id }) {
            show("\(food.name) is already in My foods.")
            return
        }
        await saveMyFoods([food.json] + mine.map(\.savedJSON), note: "Saved \(food.name) to My foods.")
    }

    public func removeMyFood(_ food: CaloriesQuickFood) async {
        let mine = myFoods
        guard mine.contains(where: { $0.id == food.id }) else { return }
        await saveMyFoods(mine.filter { $0.id != food.id }.map(\.savedJSON), note: "Removed \(food.name) from My foods.")
    }

    private func saveMyFoods(_ foods: [JSONObject], note: String) async {
        do {
            let payload = try await api.call("PUT", "/quick-foods", ["foods": .array(foods.map { .object($0) })])
            overview?["quickFoods"] = payload["quickFoods"] ?? .array([])
            show(note)
        } catch {
            show("Could not save My foods: \(message(error))", error: true)
        }
    }

    public func readReceipt(dataURL: String) async {
        if readingReceipt { return }
        readingReceipt = true
        show("Reading the receipt…")
        do {
            let payload = try await api.call("POST", "/receipt", ["image": .string(dataURL)])
            overview?["quickFoods"] = payload["quickFoods"] ?? .array([])
            show(CaloriesQuick.receiptMessage(payload))
        } catch {
            show(CaloriesQuick.receiptError(error), error: true)
        }
        readingReceipt = false
    }

    public func receiptUnreadable() { show("That receipt photo could not be read.", error: true) }

    public var receiptUnavailable: String? {
        overview?["sources"]?.objectValue?["ai"]?.boolValue == false
            ? "Receipt reading needs AI, which is unavailable on this host." : nil
    }

    public func logTyped(_ raw: String) async throws -> String {
        let text = raw.dartTrimmed
        if !installed { throw ShellPromptFailure("Install Calorie Counter first.") }
        if text.isEmpty { throw ShellPromptFailure("Type what you ate, or what you weigh.") }
        let today = todayDay
        let meal = CaloriesQuick.meal(text) ?? CaloriesText.guessMeal(now: now(), timeZone: timeZone)
        let logDay = CaloriesQuick.day(text, logDay: day, today: today, timeZone: timeZone)
        if let weight = CaloriesQuick.weight(text, imperial: imperial, logDay: day, today: today, timeZone: timeZone) {
            do {
                _ = try await api.call("PUT", "/weights/\(weight.day)", ["weightKg": .double(weight.kg)])
            } catch {
                throw ShellPromptFailure("Could not save the weigh-in: \(message(error))")
            }
            let label = weight.day == today ? "today" : dayLabel(weight.day)
            let shown = "\(CaloriesText.grams(displayWeight(weight.kg))) \(weightUnit)"
            await load()
            return "Saved \(shown) for \(label)."
        }
        var items: [JSONObject]
        var source = "quick"
        if let matched = CaloriesQuick.match(text, foods: myFoods + frequentFoods + commonFoods) {
            items = matched.map(\.json)
        } else {
            source = "text"
            do {
                let payload = try await api.call("POST", "/analyze", ["description": .string(text)])
                items = ToolsJSON.maps(payload["items"])
            } catch {
                throw ShellPromptFailure(CaloriesQuick.estimateError(error))
            }
        }
        if items.isEmpty { throw ShellPromptFailure("Nothing recognisable as food there. Try naming the food and amount.") }
        do {
            for item in items {
                var body = item
                body["day"] = .string(logDay)
                body["meal"] = .string(meal)
                body["source"] = .string(source)
                _ = try await api.call("POST", "/entries", body)
            }
        } catch {
            await load()
            throw ShellPromptFailure("Could not log: \(message(error))")
        }
        await load()
        return CaloriesQuick.loggedMessage(
            items.map { CaloriesQuickFood(json: $0) }, meal: meal,
            dayLabel: logDay == today ? nil : dayLabel(logDay), goal: goal,
            todayCalories: day == today ? calories : nil)
    }

    // MARK: Trends

    public var days: [JSONObject] { ToolsJSON.maps(overview?["days"]) }
    public var weights: [JSONObject] { ToolsJSON.maps(overview?["weights"]) }
    public var targetWeightKg: Double { CaloriesText.num(profile["targetWeightKg"]) }

    public var trendsNote: String {
        "Seven-day average \(CaloriesText.kcal(overview?["weekAverage"])) kcal · goal \(goal) kcal · blue within goal, orange over"
    }

    /// "Day by day", newest first: (label, macros, calories).
    public var dayRows: [(label: String, macros: String, calories: String)] {
        days.reversed().map { d in
            (dayLabel(ToolsJSON.show(d["day"])),
             "P \(CaloriesText.grams(d["proteinG"])) · C \(CaloriesText.grams(d["carbsG"])) · F \(CaloriesText.grams(d["fatG"]))",
             CaloriesText.int(d["entries"]) > 0 ? "\(CaloriesText.kcal(d["calories"])) kcal" : "—")
        }
    }

    // MARK: Goals

    public func goalsModel() -> CaloriesGoalsModel {
        CaloriesGoalsModel(
            profile: profile,
            latestWeightKg: CaloriesText.num(latestWeight["weightKg"]),
            usdaSaved: overview?["sources"]?.objectValue?["usda"]?.boolValue == true,
            tzOffsetMinutes: CaloriesText.tzOffsetMinutes(timeZone: timeZone, now: now()))
    }

    /// Identifies the loaded profile, so the Goals form is rebuilt when it changes.
    public var goalsKey: String { "goals-\(ToolsJSON.show(overview?["today"]))-\(JSON.encode(profile))" }

    public func saveGoals(_ form: CaloriesGoalsModel) async {
        let body = form.body()
        await run(success: "Goals and reminders saved.") { _ = try await self.api.call("PUT", "/profile", body) }
    }

    public func saveUsdaKey(_ form: CaloriesGoalsModel) async {
        let key = form.usdaKey.dartTrimmed
        form.usdaKey = ""
        await run(success: "Food database settings saved.") {
            _ = try await self.api.call("PUT", "/settings", ["usdaApiKey": .string(key)])
        }
    }

    // MARK: Challenges

    public var challenges: [JSONObject] { ToolsJSON.maps(overview?["challenges"]) }

    public var challengeRows: [CaloriesChallengeRow] {
        challenges.map { item in
            let invited = CaloriesText.int(item["invited"])
            let dates = "\(dayLabel(ToolsJSON.show(item["startsOn"]))) → \(dayLabel(ToolsJSON.show(item["endsOn"]))) · \(ToolsJSON.show(item["participants"])) in\(invited > 0 ? " · \(ToolsJSON.show(item["invited"])) invited" : "") · by \(ToolsJSON.show(item["createdBy"]))"
            return CaloriesChallengeRow(
                id: ToolsJSON.show(item["id"]), title: ToolsJSON.show(item["title"]),
                status: CaloriesText.cap(ToolsJSON.show(item["status"])), active: item["status"]?.stringValue == "active",
                kind: CaloriesText.kindLabel(ToolsJSON.show(item["kind"])), dates: dates,
                invited: item["myStatus"]?.stringValue == "invited")
        }
    }

    public func challengeDraft() -> CaloriesChallengeDraft {
        CaloriesChallengeDraft(day: day, timeZone: timeZone, now: now())
    }

    public func createChallenge(_ draft: CaloriesChallengeDraft) async {
        guard let body = draft.result() else { return }
        await run {
            let result = try await self.api.call("POST", "/challenges", body)
            self.show(CaloriesText.inviteNote("Challenge created.", result))
        }
    }

    public func invite(challengeId: String, members: [String]) async {
        if members.isEmpty { return }
        await run {
            let result = try await self.api.call("POST", "/challenges/\(challengeId)/invite", ["members": JSONValue(members)])
            self.show(CaloriesText.inviteNote("Invitations sent.", result))
        }
    }

    /// The challenge's board sheet, or nil after a notice.
    public func openChallenge(_ id: String) async -> CaloriesBoard? {
        do {
            let payload = try await api.call("GET", "/challenges/\(id)", nil)
            return CaloriesBoard(challenge: ToolsJSON.object(payload["challenge"]),
                                 me: ToolsJSON.text(overview?["actor"], ""), dayLabel: { [weak self] in self?.dayLabel($0) ?? $0 })
        } catch {
            show(message(error), error: true)
            return nil
        }
    }

    /// A board action other than "invite" (which asks for names first, then calls `invite`).
    public func boardAction(_ action: CaloriesBoardAction, board: CaloriesBoard) async {
        let id = board.id
        let challenge = board.challenge
        switch action {
        case .accept, .decline, .leave:
            let accept = action == .accept
            await run(success: accept ? "You are in. Good luck!" : "Challenge left.") {
                _ = try await self.api.call("POST", "/challenges/\(id)/respond", ["accept": .bool(accept)])
            }
        case .invite:
            break
        case .close:
            let startsOn = ToolsJSON.show(challenge["startsOn"])
            let endsOn = day < startsOn ? startsOn : day
            await run(success: "Challenge ended.") {
                _ = try await self.api.call("PATCH", "/challenges/\(id)", [
                    "title": .string(ToolsJSON.show(challenge["title"])), "status": "closed", "endsOn": .string(endsOn),
                ])
            }
        case .delete:
            await run(success: "Challenge deleted.") { _ = try await self.api.call("DELETE", "/challenges/\(id)", [:]) }
        }
    }
}

// MARK: - Sheets

/// `_EntrySheet`: add or edit one food.
@MainActor
@Observable
public final class CaloriesEntryDraft {
    public let entry: JSONObject?
    public let day: String
    public var name: String
    public var portion: String
    public var calories: String
    public var protein: String
    public var carbs: String
    public var fat: String
    public var notes: String
    public var meal: String

    public init(entry: JSONObject?, day: String, meal guessed: String) {
        self.entry = entry
        self.day = day
        name = ToolsJSON.text(entry?["name"], "")
        portion = ToolsJSON.text(entry?["portion"], "")
        calories = entry == nil ? "" : CaloriesText.kcal(entry?["calories"])
        protein = entry == nil ? "" : CaloriesText.grams(entry?["proteinG"])
        carbs = entry == nil ? "" : CaloriesText.grams(entry?["carbsG"])
        fat = entry == nil ? "" : CaloriesText.grams(entry?["fatG"])
        notes = ToolsJSON.text(entry?["notes"], "")
        let chosen = ToolsJSON.text(entry?["meal"], guessed)
        meal = CaloriesText.meals.contains(chosen) ? chosen : "snack"
    }

    public var editing: Bool { entry != nil }
    public var sheetTitle: String { editing ? "Edit food" : "Add food" }

    /// The body to save, or nil while the name is empty.
    public func result() -> JSONObject? {
        let trimmedName = name.dartTrimmed
        if trimmedName.isEmpty { return nil }
        var body: JSONObject = [
            "day": .string(editing ? ToolsJSON.show(entry?["day"]) : day),
            "name": .string(trimmedName),
            "portion": .string(portion.dartTrimmed),
            "meal": .string(meal),
            "calories": .int(Int64(CaloriesText.int(calories))),
            "proteinG": .double(CaloriesText.num(protein)),
            "carbsG": .double(CaloriesText.num(carbs)),
            "fatG": .double(CaloriesText.num(fat)),
            "notes": .string(notes),
        ]
        if !editing { body["source"] = "manual" }
        return body
    }
}

/// `_SuggestionsSheet`: what the model saw, to tick and correct before adding.
@MainActor
@Observable
public final class CaloriesSuggestionsModel {
    public let items: [JSONObject]
    public let summary: String
    public let total: Int
    public let source: String
    public var chosen: [Bool]
    public var kcal: [String]
    public var meal: String

    public init(payload: JSONObject, meal: String) {
        items = ToolsJSON.maps(payload["items"])
        summary = ToolsJSON.text(payload["summary"], "")
        total = CaloriesText.int(payload["totalCalories"])
        source = ToolsJSON.text(payload["source"], "photo")
        chosen = [Bool](repeating: true, count: items.count)
        kcal = items.map { String(CaloriesText.int($0["calories"])) }
        self.meal = meal
    }

    public var sheetTitle: String { "Suggested foods · about \(total) kcal" }
    public var intro: String { "Untick anything wrong and correct the calories before adding. \(summary)" }
    public static let addButton = "Add selected to diary"

    public func name(_ i: Int) -> String { ToolsJSON.show(items[i]["name"]) }

    public func detail(_ i: Int) -> String {
        let item = items[i]
        return "\(ToolsJSON.show(item["portion"])) · confidence \(TelemetryMath.round(CaloriesText.num(item["confidence"]) * 100))% · P \(CaloriesText.grams(item["proteinG"])) · C \(CaloriesText.grams(item["carbsG"])) · F \(CaloriesText.grams(item["fatG"]))"
    }

    public func chosenItems() -> [JSONObject] {
        items.indices.filter { chosen[$0] }.map { i in
            let item = items[i]
            return [
                "name": item["name"] ?? .null,
                "portion": item["portion"] ?? .null,
                "calories": .int(Int64(CaloriesText.int(kcal[i]))),
                "proteinG": item["proteinG"] ?? .null,
                "carbsG": item["carbsG"] ?? .null,
                "fatG": item["fatG"] ?? .null,
                "confidence": item["confidence"] ?? .null,
            ]
        }
    }
}

/// `_SearchSheet`: Open Food Facts (and USDA with a key) by name.
@MainActor
@Observable
public final class CaloriesSearchModel {
    public static let title = "Search foods"
    public static let hint = "trader joes buffalo chicken wrap…"
    public static let debounceNanoseconds: UInt64 = 300_000_000

    @ObservationIgnored private let search: @MainActor (String) async throws -> [JSONObject]
    @ObservationIgnored private var cache: [String: [JSONObject]] = [:]
    @ObservationIgnored private var pending: Task<Void, Never>?
    @ObservationIgnored private var searched = ""
    public var query = ""
    public private(set) var results: [JSONObject] = []
    public private(set) var status = "Start typing a food or brand, like trader joes buffalo chicken wrap."

    public init(search: @escaping @MainActor (String) async throws -> [JSONObject]) {
        self.search = search
    }

    private var typed: String {
        query.split(whereSeparator: { (c: Character) in c.isWhitespace }).joined(separator: " ")
    }

    public func queryChanged() {
        pending?.cancel()
        let q = typed
        if q.count < 2 {
            searched = ""
            results = []
            return
        }
        if q.lowercased() == searched { return }
        let wait: UInt64 = cache[q.lowercased()] == nil ? Self.debounceNanoseconds : 0
        pending = Task { [weak self] in
            if wait > 0 { try? await Task.sleep(nanoseconds: wait) }
            if Task.isCancelled { return }
            await self?.run()
        }
    }

    public func submitted() -> JSONObject? {
        if let first = results.first, typed.lowercased() == searched { return first }
        pending?.cancel()
        pending = Task { [weak self] in await self?.run() }
        return nil
    }

    public func run() async {
        let q = typed
        if q.count < 2 { return }
        let key = q.lowercased()
        searched = key
        if let cached = cache[key] {
            show(q, cached)
            return
        }
        status = "Searching…"
        do {
            let found = try await search(q)
            cache[key] = found
            guard searched == key else { return }
            show(q, found)
        } catch {
            guard searched == key else { return }
            searched = ""
            status = "Search failed."
        }
    }

    private func show(_ q: String, _ found: [JSONObject]) {
        results = found
        status = found.isEmpty
            ? "Nothing found for “\(q)” yet. Keep typing, try the brand first, or add it by hand."
            : "\(found.count) \(found.count == 1 ? "match" : "matches") · Open Food Facts · tap one to log it."
    }

    public func title(_ food: JSONObject) -> String { ToolsJSON.show(food["name"]) }

    public func subtitle(_ food: JSONObject) -> String {
        var parts: [String] = [ToolsJSON.show(food["brand"]),
                               "\(CaloriesText.kcal(food["per100g"]?.objectValue?["calories"])) kcal / 100 g"]
        if let serving = food["perServing"]?.objectValue {
            let size = ToolsJSON.show(food["servingSize"])
            parts.append("\(CaloriesText.kcal(serving["calories"])) kcal per \(size.isEmpty ? "serving" : size)")
        }
        return parts.filter { (part: String) in !part.isEmpty }.joined(separator: " · ")
    }
}

/// `_PortionSheet`: how much of a product.
@MainActor
@Observable
public final class CaloriesPortionModel {
    public let product: JSONObject
    public let source: String
    /// "serving", "100g" or "1g".
    public var basis: String
    public var amount = "1"
    public var meal: String

    public init(product: JSONObject, source: String, meal: String) {
        self.product = product
        self.source = source
        self.meal = meal
        basis = product["perServing"]?.objectValue != nil ? "serving" : "100g"
    }

    public var sheetTitle: String { ToolsJSON.show(product["name"]) }

    public var subtitle: String {
        [ToolsJSON.show(product["brand"]), ToolsJSON.show(product["quantity"]),
         product["source"]?.stringValue == "usda" ? "USDA FoodData Central" : "Open Food Facts"]
            .filter { !$0.isEmpty }.joined(separator: " · ")
    }

    /// The Basis choices: (value, label).
    public var bases: [(value: String, label: String)] {
        var out = [(value: String, label: String)]()
        if product["perServing"]?.objectValue != nil {
            let size = ToolsJSON.text(product["servingSize"], "")
            out.append(("serving", "1 serving\(size.isEmpty ? "" : " (\(size))")"))
        }
        out.append(("100g", "100 g"))
        out.append(("1g", "1 g"))
        return out
    }

    public func values() -> JSONObject {
        let per = ToolsJSON.object(basis == "serving" ? product["perServing"] : product["per100g"])
        let typed = CaloriesText.num(amount)
        let amount = typed <= 0 ? 1.0 : typed
        let factor = basis == "1g" ? amount / 100 : amount
        let serving = ToolsJSON.text(product["servingSize"], "")
        let portion = basis == "serving"
            ? "\(CaloriesText.grams(amount)) serving(s)\(serving.isEmpty ? "" : " (\(serving))")"
            : "\(CaloriesText.grams(basis == "1g" ? amount : amount * 100)) g"
        func oneDecimal(_ v: Double) -> JSONValue { .double(Double(DartCore.toStringAsFixed(v, 1)) ?? v) }
        return [
            "name": product["name"] ?? .null,
            "brand": product["brand"] ?? .null,
            "barcode": product["barcode"] ?? .null,
            "portion": .string(portion),
            "calories": .int(Int64(TelemetryMath.round(CaloriesText.num(per["calories"]) * factor))),
            "proteinG": oneDecimal(CaloriesText.num(per["proteinG"]) * factor),
            "carbsG": oneDecimal(CaloriesText.num(per["carbsG"]) * factor),
            "fatG": oneDecimal(CaloriesText.num(per["fatG"]) * factor),
            "meal": .string(meal),
        ]
    }

    public var summary: String {
        let v = values()
        return "\(DartCore.toString(v["calories"])) kcal · P \(CaloriesText.grams(v["proteinG"])) g · C \(CaloriesText.grams(v["carbsG"])) g · F \(CaloriesText.grams(v["fatG"])) g"
    }
}

/// One meal reminder of the Goals form.
public struct CaloriesReminder: Equatable, Identifiable, Sendable {
    public let id: UUID
    public var time: String
    public var label: String
    public var onlyIfUnlogged: Bool
    public var enabled: Bool

    public init(time: String, label: String, onlyIfUnlogged: Bool, enabled: Bool) {
        id = UUID()
        self.time = time
        self.label = label
        self.onlyIfUnlogged = onlyIfUnlogged
        self.enabled = enabled
    }

    var json: JSONObject {
        ["time": .string(time), "label": .string(label), "onlyIfUnlogged": .bool(onlyIfUnlogged), "enabled": .bool(enabled)]
    }
}

/// `_GoalsForm`: daily goals, units, body measurements, reminders and the USDA key.
@MainActor
@Observable
public final class CaloriesGoalsModel {
    public static let foodDatabasesIntro =
        "Open Food Facts is always used for barcodes and search. Add your own free USDA FoodData Central API key to also search the USDA database. It is saved on your account only. "
    public static let remindersNote = "Delivered as pings in this phone's time zone. Up to eight per day."
    public static let maxReminders = 8

    public let profile: JSONObject
    public let latestWeightKg: Double
    public let usdaSaved: Bool
    public let tzOffsetMinutes: Int
    /// "auto", "metric" or "imperial": the stored pick. 'auto' follows the country the Worker saw
    /// this request arrive from, which it hands back as profile.units.
    public private(set) var unitsPreference: String
    public private(set) var imperial: Bool
    public var calories: String
    public var protein: String
    public var carbs: String
    public var fat: String
    public var target: String
    public var start: String
    public var height: String
    public var usdaKey = ""
    public var reminders: [CaloriesReminder]

    public init(profile: JSONObject, latestWeightKg: Double, usdaSaved: Bool, tzOffsetMinutes: Int) {
        self.profile = profile
        self.latestWeightKg = latestWeightKg
        self.usdaSaved = usdaSaved
        self.tzOffsetMinutes = tzOffsetMinutes
        let stored = profile["unitsPreference"]?.stringValue ?? ""
        let imperialUnits = profile["units"]?.stringValue == "imperial"
        unitsPreference = ["auto", "metric", "imperial"].contains(stored) ? stored : (imperialUnits ? "imperial" : "metric")
        imperial = imperialUnits
        calories = CaloriesText.kcal(profile["dailyCalories"])
        protein = CaloriesText.kcal(profile["proteinG"])
        carbs = CaloriesText.kcal(profile["carbsG"])
        fat = CaloriesText.kcal(profile["fatG"])
        func weight(_ kg: JSONValue?) -> String {
            let value = CaloriesText.num(kg)
            return value > 0 ? CaloriesText.grams(imperialUnits ? value / CaloriesText.kgPerLb : value) : ""
        }
        target = weight(profile["targetWeightKg"])
        start = weight(profile["startWeightKg"])
        let heightCm = CaloriesText.num(profile["heightCm"])
        height = heightCm > 0 ? CaloriesText.grams(imperialUnits ? heightCm / CaloriesText.cmPerIn : heightCm) : ""
        reminders = ToolsJSON.maps(profile["reminders"]).map { r in
            CaloriesReminder(time: ToolsJSON.show(r["time"]), label: ToolsJSON.text(r["label"], ""),
                             onlyIfUnlogged: r["onlyIfUnlogged"]?.boolValue == true, enabled: r["enabled"]?.boolValue != false)
        }
    }

    /// What 'auto' resolves to here.
    var autoImperial: Bool {
        let auto = profile["autoUnits"]
        if auto == nil || auto == .null || auto?.stringValue == "" { return profile["units"]?.stringValue == "imperial" }
        return auto?.stringValue == "imperial"
    }

    public var autoLabel: String {
        switch profile["autoUnits"]?.stringValue {
        case "imperial": return "Automatic (lb, in)"
        case "metric": return "Automatic (kg, cm)"
        default: return "Automatic (match my location)"
        }
    }

    /// The Units choices: (value, label).
    public var unitChoices: [(value: String, label: String)] {
        [("auto", autoLabel), ("metric", "Metric (kg, cm)"), ("imperial", "Imperial (lb, in)")]
    }

    public func setUnits(_ value: String) {
        unitsPreference = value
        imperial = value == "auto" ? autoImperial : value == "imperial"
    }

    public var weightUnit: String { imperial ? "lb" : "kg" }
    public var heightUnit: String { imperial ? "in" : "cm" }

    /// "BMI 25.6 from your latest weigh-in.", or nil without a weight and a height.
    public var bmiLine: String? {
        let heightCm = CaloriesText.num(height) * (imperial ? CaloriesText.cmPerIn : 1)
        guard latestWeightKg > 0, heightCm > 0 else { return nil }
        let bmi = latestWeightKg / ((heightCm / 100) * (heightCm / 100))
        return "BMI \(DartCore.toStringAsFixed(bmi, 1)) from your latest weigh-in."
    }

    public var usdaLine: String { CaloriesGoalsModel.foodDatabasesIntro + (usdaSaved ? "A USDA key is saved." : "No USDA key saved.") }

    public var canAddReminder: Bool { reminders.count < CaloriesGoalsModel.maxReminders }

    public func addReminder(hour: Int, minute: Int) {
        if !canAddReminder { return }
        reminders.append(CaloriesReminder(time: CaloriesText.clock(hour: hour, minute: minute),
                                          label: "Time to log your meal", onlyIfUnlogged: false, enabled: true))
    }

    public func removeReminder(_ id: UUID) { reminders.removeAll { $0.id == id } }

    public func setReminderTime(_ id: UUID, hour: Int, minute: Int) {
        guard let i = reminders.firstIndex(where: { $0.id == id }) else { return }
        reminders[i].time = CaloriesText.clock(hour: hour, minute: minute)
    }

    public func body() -> JSONObject {
        let wf = imperial ? CaloriesText.kgPerLb : 1.0
        return [
            "dailyCalories": .int(Int64(CaloriesText.int(calories))),
            "proteinG": .int(Int64(CaloriesText.int(protein))),
            "carbsG": .int(Int64(CaloriesText.int(carbs))),
            "fatG": .int(Int64(CaloriesText.int(fat))),
            "unitsPreference": .string(unitsPreference),
            "targetWeightKg": .double(CaloriesText.num(target) * wf),
            "startWeightKg": .double(CaloriesText.num(start) * wf),
            "heightCm": .double(CaloriesText.num(height) * (imperial ? CaloriesText.cmPerIn : 1.0)),
            "tzOffsetMinutes": .int(Int64(tzOffsetMinutes)),
            "reminders": .array(reminders.map { .object($0.json) }),
        ]
    }
}

/// `_ChallengeSheet`: a new challenge and whom to invite.
@MainActor
@Observable
public final class CaloriesChallengeDraft {
    public var title = ""
    public var description = ""
    public var kind = "weight_loss_percent"
    public var startsOn: String
    public var endsOn: String
    public var invite = ""
    @ObservationIgnored private let timeZone: TimeZone
    @ObservationIgnored private let now: Date

    public init(day: String, timeZone: TimeZone = .current, now: Date = Date()) {
        startsOn = day
        endsOn = CaloriesText.addDays(day, 28, timeZone: timeZone, now: now)
        self.timeZone = timeZone
        self.now = now
    }

    public var startsLabel: String { "Starts \(CaloriesText.dayLabel(startsOn, timeZone: timeZone, now: now))" }
    public var endsLabel: String { "Ends \(CaloriesText.dayLabel(endsOn, timeZone: timeZone, now: now))" }
    public var startDate: Date { CaloriesText.parseDay(startsOn, timeZone: timeZone, now: now) }
    public var endDate: Date { CaloriesText.parseDay(endsOn, timeZone: timeZone, now: now) }

    public func setStart(_ date: Date) { startsOn = CaloriesText.isoDay(date, timeZone: timeZone) }
    public func setEnd(_ date: Date) { endsOn = CaloriesText.isoDay(date, timeZone: timeZone) }

    public func result() -> JSONObject? {
        let trimmed = title.dartTrimmed
        if trimmed.isEmpty { return nil }
        return [
            "title": .string(trimmed),
            "kind": .string(kind),
            "startsOn": .string(startsOn),
            "endsOn": .string(endsOn),
            "description": .string(description),
            "invite": JSONValue(CaloriesText.usernames(invite)),
        ]
    }
}

public enum CaloriesBoardAction: String, Equatable, Sendable {
    case accept, decline, leave, invite, close, delete

    public var label: String {
        switch self {
        case .accept: return "Accept challenge"
        case .decline: return "Decline"
        case .leave: return "Leave challenge"
        case .invite: return "Invite"
        case .close: return "End now"
        case .delete: return "Delete"
        }
    }
}

/// `_BoardSheet`: one challenge's standings and what this member may do.
public struct CaloriesBoard {
    public struct Member: Equatable, Identifiable, Sendable {
        public let id: Int
        public let rank: String
        public let name: String
        public let progress: Double
        public let detail: String
    }

    public let challenge: JSONObject
    public let me: String
    public let id: String
    public let title: String
    public let subtitle: String
    public let description: String
    public let actions: [CaloriesBoardAction]
    public let members: [Member]
    public let pending: String?

    public static let nobody = "Nobody has accepted yet."

    public init(challenge: JSONObject, me: String, dayLabel: (String) -> String) {
        self.challenge = challenge
        self.me = me
        id = ToolsJSON.show(challenge["id"])
        title = ToolsJSON.show(challenge["title"])
        let winner = ToolsJSON.text(challenge["winner"], "")
        subtitle = "\(CaloriesText.kindLabel(ToolsJSON.show(challenge["kind"]))) · \(dayLabel(ToolsJSON.show(challenge["startsOn"]))) → \(dayLabel(ToolsJSON.show(challenge["endsOn"]))) · started by \(ToolsJSON.show(challenge["createdBy"]))\(winner.isEmpty ? "" : " · winner: \(winner)")"
        description = ToolsJSON.text(challenge["description"], "")
        let status = ToolsJSON.show(challenge["status"])
        let mine = ToolsJSON.show(challenge["myStatus"])
        // Only the creator may invite, end, or delete; the server enforces it.
        let owner = challenge["isCreator"]?.boolValue == true
        var actions = [CaloriesBoardAction]()
        if status != "closed" && mine == "invited" { actions += [.accept, .decline] }
        if status != "closed" && mine == "accepted" && !owner { actions.append(.leave) }
        if status != "closed" && owner { actions += [.invite, .close] }
        if owner { actions.append(.delete) }
        self.actions = actions
        let board = ToolsJSON.maps(challenge["leaderboard"])
        let scores = board.filter { $0["measured"]?.boolValue == true }.map { abs(CaloriesText.num($0["score"])) }
        let top = scores.isEmpty ? 1.0 : max(0.0001, scores.max() ?? 1)
        members = board.enumerated().map { index, m in
            let rank = CaloriesText.int(m["rank"]) > 0 ? ToolsJSON.show(m["rank"]) : "–"
            let name = ToolsJSON.show(m["name"])
            let progress = m["measured"]?.boolValue == true ? min(max(CaloriesText.num(m["score"]) / top, 0), 1) : 0
            return Member(id: index, rank: rank, name: "\(name)\(m["name"]?.stringValue == me ? " (you)" : "")",
                          progress: progress, detail: ToolsJSON.text(m["detail"], ""))
        }
        let pendingMembers = ToolsJSON.maps(challenge["pending"])
        pending = pendingMembers.isEmpty ? nil
            : "Invited or left: \(pendingMembers.map { "\(ToolsJSON.show($0["name"])) (\(ToolsJSON.show($0["status"])))" }.joined(separator: ", "))"
    }
}
