import FMApi
import FMBase
import XCTest
@testable import FMTools

@MainActor
final class FakeNutrition: NutritionBridge {
    var available = true
    var allow = true
    var days: [String: [FoodNutrient: Double]] = [:]
    var authorized = 0
    var reads: [(from: Date, to: Date)] = []

    func authorizeNutrition() async -> Bool {
        authorized += 1
        return allow
    }

    func nutrientDays(from: Date, to: Date) async -> [String: [FoodNutrient: Double]] {
        reads.append((from, to))
        return days
    }
}

@MainActor
final class FoodGuideTests: XCTestCase {
    let utc = TimeZone(identifier: "UTC")!

    func at(_ day: String, hour: Int = 12) -> Date {
        CaloriesText.tryParseDay(day, timeZone: utc)!.addingTimeInterval(TimeInterval(hour * 3600))
    }

    var products: [String: FoodProduct] {
        Dictionary(FoodCatalog.products.map { ($0.id, $0) }, uniquingKeysWith: { first, _ in first })
    }

    func lowIronDays() -> [String: [FoodNutrient: Double]] {
        var days: [String: [FoodNutrient: Double]] = [:]
        for day in ["2026-10-08", "2026-10-09", "2026-10-10", "2026-10-11", "2026-10-12"] {
            days[day] = [.energy: 1900, .iron: 8, .fiber: 26, .protein: 70, .sodium: 2100]
        }
        days["2026-10-07"] = [.energy: 300, .iron: 1]
        days["2026-10-13"] = [.energy: 600, .iron: 2]
        return days
    }

    func testNeedsAverageOnlyFullPastDays() {
        let needs = FoodNeeds.averages(lowIronDays(), before: "2026-10-13")
        let iron = needs.first { $0.nutrient == .iron }
        XCTAssertEqual(iron?.average, 8)
        XCTAssertEqual(iron?.days, 5)
        XCTAssertNil(needs.first { $0.nutrient == .calcium })
        XCTAssertEqual(FoodNeeds.gaps(needs).map(\.nutrient), [.iron])
        XCTAssertEqual(FoodNeeds.gaps(needs).first?.title, "Low on iron")
        XCTAssertTrue(FoodNeeds.gaps(needs).first?.line.contains("44% of the 18 mg daily value") == true)
    }

    func testTooFewDaysGiveNoNeeds() {
        let days: [String: [FoodNutrient: Double]] = [
            "2026-10-11": [.energy: 2000, .iron: 5], "2026-10-12": [.energy: 2000, .iron: 5],
        ]
        XCTAssertTrue(FoodNeeds.averages(days, before: "2026-10-13").isEmpty)
    }

    func testHighSodiumIsAGap() {
        let need = FoodNeed(nutrient: .sodium, average: 3450, days: 7)
        XCTAssertTrue(need.isHigh)
        XCTAssertFalse(need.isLow)
        XCTAssertEqual(need.title, "High in sodium")
    }

    func testMealPlansSplitWhatIsLeft() {
        let budget = FoodBudget(daily: 2000, eaten: 600, source: .calorieCounter)
        let plans = FoodMealPlan.plans(budget, minuteOfDay: 12 * 60)
        XCTAssertEqual(plans.map(\.meal), [.breakfast, .lunch, .dinner])
        XCTAssertEqual(plans[0].tomorrow, true)
        XCTAssertEqual(plans[0].target, 500)
        XCTAssertEqual(plans[0].title, "Breakfast tomorrow")
        XCTAssertEqual(plans[1].target, 653)
        XCTAssertEqual(plans[2].target, 747)
        XCTAssertFalse(plans[1].tomorrow)
    }

    func testLateEveningPlansTomorrow() {
        let budget = FoodBudget(daily: 1800, eaten: 1700, source: .standard)
        let plans = FoodMealPlan.plans(budget, minuteOfDay: 22 * 60)
        XCTAssertTrue(plans.allSatisfy(\.tomorrow))
        XCTAssertEqual(plans.map(\.target), [450, 630, 720])
    }

    func testLittleLeftAsksForTheLightest() {
        let budget = FoodBudget(daily: 1800, eaten: 1650, source: .calorieCounter)
        let plans = FoodMealPlan.plans(budget, minuteOfDay: 18 * 60)
        XCTAssertEqual(plans[2].target, 150)
        XCTAssertTrue(plans[2].line.contains("lightest"))
    }

    func testCatalogIsWhole() {
        let known = products
        XCTAssertEqual(known.count, FoodCatalog.products.count)
        for meal in FoodMeal.allCases {
            XCTAssertGreaterThanOrEqual(FoodCatalog.recipes.filter { $0.meals.contains(meal) }.count, 6, meal.rawValue)
        }
        XCTAssertEqual(Set(FoodCatalog.recipes.map(\.id)).count, FoodCatalog.recipes.count)
        for recipe in FoodCatalog.recipes {
            XCTAssertTrue(recipe.url.hasPrefix("https://"), recipe.id)
            XCTAssertFalse(recipe.portions.isEmpty, recipe.id)
            for portion in recipe.portions {
                XCTAssertNotNil(known[portion.product], "\(recipe.id) uses \(portion.product)")
            }
            let kcal = recipe.nutrients(known)[.energy] ?? 0
            XCTAssertGreaterThan(kcal, 150, recipe.id)
            XCTAssertLessThan(kcal, 950, recipe.id)
        }
    }

    func testLowIronPutsIronRichMealsFirst() {
        let plans = FoodMealPlan.plans(FoodBudget(daily: 2000, eaten: 0, source: .standard), minuteOfDay: 7 * 60)
        let iron = [FoodNeed(nutrient: .iron, average: 6, days: 10)]
        let picks = FoodPlanner.suggest(
            recipes: FoodCatalog.recipes, products: products, plans: plans, needs: iron, goal: .maintain, diets: [],
            day: "2026-10-13")
        let none = FoodPlanner.suggest(
            recipes: FoodCatalog.recipes, products: products, plans: plans, needs: [], goal: .maintain, diets: [],
            day: "2026-10-13")
        for meal in FoodMeal.allCases {
            let ironFirst = picks[meal]!.map { $0.nutrients[.iron] ?? 0 }.reduce(0, +)
            let plain = none[meal]!.map { $0.nutrients[.iron] ?? 0 }.reduce(0, +)
            XCTAssertGreaterThanOrEqual(ironFirst, plain, meal.rawValue)
        }
        let top = picks[.dinner]!.first!
        XCTAssertTrue(top.reasons.contains { $0.contains("iron") }, top.reasons.joined(separator: "; "))
        XCTAssertTrue(top.highlights.contains(.iron))
        XCTAssertTrue(top.shoppingLine.hasPrefix("At ALDI: "))
    }

    func testEveryMealGetsThreeDifferentRecipes() {
        let plans = FoodMealPlan.plans(FoodBudget(daily: 2000, eaten: 0, source: .standard), minuteOfDay: 7 * 60)
        let picks = FoodPlanner.suggest(
            recipes: FoodCatalog.recipes, products: products, plans: plans, needs: FoodNeeds.everyday,
            goal: .eatHealthier, diets: [], day: "2026-10-13")
        let ids = FoodMeal.allCases.flatMap { picks[$0]!.map(\.id) }
        XCTAssertEqual(ids.count, 9)
        XCTAssertEqual(Set(ids).count, 9)
    }

    func testDietsLeaveOutWhatTheyExclude() {
        let plans = FoodMealPlan.plans(FoodBudget(daily: 2000, eaten: 0, source: .standard), minuteOfDay: 7 * 60)
        let known = products
        let picks = FoodPlanner.suggest(
            recipes: FoodCatalog.recipes, products: known, plans: plans, needs: [], goal: .buildMuscle,
            diets: [.vegetarian, .nutFree], day: "2026-10-13")
        for suggestion in FoodMeal.allCases.flatMap({ picks[$0]! }) {
            XCTAssertTrue(
                suggestion.recipe.flags(known).isDisjoint(with: [.meat, .fish, .shellfish, .nuts, .peanuts]),
                suggestion.id)
        }
    }

    func testMoreIdeasShowsOthers() async {
        let model = FoodGuideModel(api: nil, health: nil, timeZone: utc, now: { self.at("2026-10-13", hour: 7) })
        await model.load()
        let first = model.suggestions(for: .lunch).map(\.id)
        model.moreIdeas(.lunch)
        let next = model.suggestions(for: .lunch).map(\.id)
        XCTAssertEqual(next.count, 3)
        XCTAssertNotEqual(first, next)
    }

    func testBudgetComesFromTheCalorieCounter() async {
        let health = FakeNutrition()
        health.days = lowIronDays()
        let prefs = InMemoryKeyValueStore()
        prefs.set(true, forKey: HealthPrefs.foodGuide)
        let api = FoodGuideAPI(caloriesOverview: { (day: String) async throws -> JSONObject in
            XCTAssertEqual(day, "2026-10-13")
            return ["profile": ["dailyCalories": 1800], "todayTotals": ["calories": 450]]
        })
        let model = FoodGuideModel(
            api: api, health: health, prefs: prefs, timeZone: utc, now: { self.at("2026-10-13", hour: 12) })
        await model.load()
        XCTAssertEqual(model.budget, FoodBudget(daily: 1800, eaten: 600, source: .calorieCounter))
        XCTAssertEqual(model.budget.line, "1,800 kcal a day · 600 eaten today · 1,200 left")
        XCTAssertEqual(model.gaps.map(\.nutrient), [.iron])
        XCTAssertEqual(health.reads.first?.from, at("2026-09-29", hour: 0))
        XCTAssertEqual(model.plans.map(\.tomorrow), [true, false, false])
    }

    func testWithoutTheCalorieCounterTheStandardBudgetIsUsed() async {
        let api = FoodGuideAPI(caloriesOverview: { (_: String) async throws -> JSONObject in
            throw OrgApiException("plugin_not_installed", "not installed")
        })
        let model = FoodGuideModel(api: api, health: nil, timeZone: utc, now: { self.at("2026-10-13", hour: 7) })
        await model.load()
        XCTAssertEqual(model.budget, FoodBudget(daily: 2000, eaten: 0, source: .standard))
        XCTAssertEqual(model.needsLine, FoodGuideModel.healthOffLine)
        XCTAssertEqual(model.suggestions(for: .breakfast).count, 3)
    }

    func testHealthSwitchAndChoicesPersist() async {
        let health = FakeNutrition()
        health.days = lowIronDays()
        let prefs = InMemoryKeyValueStore()
        let model = FoodGuideModel(api: nil, health: health, prefs: prefs, timeZone: utc, now: { self.at("2026-10-13") })
        await model.enableHealth()
        XCTAssertEqual(health.authorized, 1)
        XCTAssertTrue(prefs.bool(forKey: HealthPrefs.foodGuide, or: false))
        XCTAssertEqual(model.gaps.map(\.nutrient), [.iron])
        model.disableHealth()
        XCTAssertFalse(prefs.bool(forKey: HealthPrefs.foodGuide, or: true))
        XCTAssertTrue(model.needs.isEmpty)

        health.allow = false
        await model.enableHealth()
        XCTAssertFalse(model.healthEnabled)
        XCTAssertEqual(model.notice?.isError, true)

        model.setGoal(.buildMuscle)
        model.toggleDiet(.vegan)
        let again = FoodGuideModel(api: nil, health: health, prefs: prefs, timeZone: utc, now: { self.at("2026-10-13") })
        XCTAssertEqual(again.goal, .buildMuscle)
        XCTAssertEqual(again.diets, [.vegan])
    }

    func testText() {
        XCTAssertEqual(FoodText.kcal(1234567), "1,234,567")
        XCTAssertEqual(FoodText.kcal(950), "950")
        XCTAssertEqual(FoodText.number(6.14), "6.1")
        XCTAssertEqual(FoodText.number(6.0), "6")
        XCTAssertEqual(FoodText.number(12.6), "13")
        XCTAssertEqual(FoodNutrient.vitaminD.amount(2.5), "2.5 mcg")
    }
}
