import FMApi
import FMBase
import Foundation
import Observation

public enum FoodNutrient: String, CaseIterable, Sendable {
    case energy, protein, fiber, iron, calcium, vitaminD, vitaminC, potassium, magnesium, folate, vitaminB12
    case sodium, sugar, saturatedFat

    public static let needs: [FoodNutrient] = allCases.filter { $0 != .energy }

    public var name: String {
        switch self {
        case .energy: return "Calories"
        case .protein: return "Protein"
        case .fiber: return "Fiber"
        case .iron: return "Iron"
        case .calcium: return "Calcium"
        case .vitaminD: return "Vitamin D"
        case .vitaminC: return "Vitamin C"
        case .potassium: return "Potassium"
        case .magnesium: return "Magnesium"
        case .folate: return "Folate"
        case .vitaminB12: return "Vitamin B12"
        case .sodium: return "Sodium"
        case .sugar: return "Sugar"
        case .saturatedFat: return "Saturated fat"
        }
    }

    public var lowerName: String {
        switch self {
        case .vitaminD, .vitaminC, .vitaminB12: return name
        default: return name.lowercased()
        }
    }

    public var unit: String {
        switch self {
        case .energy: return "kcal"
        case .protein, .fiber, .sugar, .saturatedFat: return "g"
        case .iron, .calcium, .vitaminC, .potassium, .magnesium, .sodium: return "mg"
        case .vitaminD, .folate, .vitaminB12: return "mcg"
        }
    }

    public var dailyValue: Double {
        switch self {
        case .energy: return 2000
        case .protein: return 50
        case .fiber: return 28
        case .iron: return 18
        case .calcium: return 1300
        case .vitaminD: return 20
        case .vitaminC: return 90
        case .potassium: return 4700
        case .magnesium: return 420
        case .folate: return 400
        case .vitaminB12: return 2.4
        case .sodium: return 2300
        case .sugar: return 50
        case .saturatedFat: return 20
        }
    }

    public var isLimit: Bool { self == .sodium || self == .sugar || self == .saturatedFat }

    public var sources: String {
        switch self {
        case .energy: return ""
        case .protein: return "chicken, fish, eggs, Greek yogurt, tofu and lentils"
        case .fiber: return "oats, beans, lentils, berries, whole grains and vegetables"
        case .iron: return "lentils, beans, spinach, lean beef, sardines and bran flakes"
        case .calcium: return "yogurt, milk, cheese, sardines, tofu and kale"
        case .vitaminD: return "salmon, sardines, tuna, eggs and fortified milk"
        case .vitaminC: return "peppers, strawberries, oranges, broccoli and kale"
        case .potassium: return "beans, lentils, potatoes, spinach, bananas and salmon"
        case .magnesium: return "pumpkin seeds, almonds, spinach, beans and whole grains"
        case .folate: return "lentils, beans, spinach, edamame and avocado"
        case .vitaminB12: return "fish, beef, eggs, yogurt and milk"
        case .sodium: return "home-cooked meals with fresh ingredients"
        case .sugar: return "meals built on vegetables, protein and whole grains"
        case .saturatedFat: return "fish, beans, lean poultry and olive oil"
        }
    }

    public func amount(_ value: Double) -> String { "\(FoodText.number(value)) \(unit)" }
}

public enum FoodFlag: String, CaseIterable, Sendable {
    case meat, fish, shellfish, dairy, egg, gluten, nuts, peanuts, soy
}

public enum FoodDiet: String, CaseIterable, Identifiable, Sendable {
    case vegetarian, vegan, dairyFree, glutenFree, noFish, nutFree

    public var id: String { rawValue }

    public var label: String {
        switch self {
        case .vegetarian: return "Vegetarian"
        case .vegan: return "Vegan"
        case .dairyFree: return "Dairy-free"
        case .glutenFree: return "Gluten-free"
        case .noFish: return "No fish or shellfish"
        case .nutFree: return "No nuts or peanuts"
        }
    }

    public var excludes: Set<FoodFlag> {
        switch self {
        case .vegetarian: return [.meat, .fish, .shellfish]
        case .vegan: return [.meat, .fish, .shellfish, .dairy, .egg]
        case .dairyFree: return [.dairy]
        case .glutenFree: return [.gluten]
        case .noFish: return [.fish, .shellfish]
        case .nutFree: return [.nuts, .peanuts]
        }
    }
}

public enum FoodGoal: String, CaseIterable, Identifiable, Sendable {
    case loseWeight, maintain, buildMuscle, eatHealthier

    public var id: String { rawValue }

    public var label: String {
        switch self {
        case .loseWeight: return "Lose weight"
        case .maintain: return "Keep my weight"
        case .buildMuscle: return "Build muscle"
        case .eatHealthier: return "Eat healthier"
        }
    }

    public var line: String {
        switch self {
        case .loseWeight: return "Filling meals high in protein and fiber that stay inside your calorie budget."
        case .maintain: return "Balanced meals that match your calorie budget."
        case .buildMuscle: return "Meals with 30 g of protein or more, sized to your calorie budget."
        case .eatHealthier: return "More vegetables, fiber and potassium, less sodium and saturated fat."
        }
    }
}

public enum FoodMeal: String, CaseIterable, Identifiable, Sendable {
    case breakfast, lunch, dinner

    public var id: String { rawValue }

    public var title: String {
        switch self {
        case .breakfast: return "Breakfast"
        case .lunch: return "Lunch"
        case .dinner: return "Dinner"
        }
    }

    public var share: Double {
        switch self {
        case .breakfast: return 0.25
        case .lunch: return 0.35
        case .dinner: return 0.40
        }
    }

    public var endsAtMinute: Int {
        switch self {
        case .breakfast: return 10 * 60 + 30
        case .lunch: return 15 * 60
        case .dinner: return 21 * 60
        }
    }
}

public struct FoodProduct: Equatable, Identifiable, Sendable {
    public let id: String
    public let aldiName: String
    public let per100g: [FoodNutrient: Double]
    public let flags: Set<FoodFlag>

    public init(_ id: String, _ aldiName: String, _ values: [Double], flags: Set<FoodFlag> = []) {
        self.id = id
        self.aldiName = aldiName
        var per100g: [FoodNutrient: Double] = [:]
        for (nutrient, value) in zip(FoodNutrient.allCases, values) where value > 0 {
            per100g[nutrient] = value
        }
        self.per100g = per100g
        self.flags = flags
    }
}

public struct FoodPortion: Equatable, Sendable {
    public let product: String
    public let grams: Double

    public init(_ product: String, _ grams: Double) {
        self.product = product
        self.grams = grams
    }
}

public struct FoodRecipe: Equatable, Identifiable, Sendable {
    public let id: String
    public let title: String
    public let url: String
    public let site: String
    public let meals: Set<FoodMeal>
    public let minutes: Int
    public let portions: [FoodPortion]

    public init(
        _ id: String, _ title: String, site: String, url: String, meals: Set<FoodMeal>, minutes: Int,
        _ portions: [FoodPortion]
    ) {
        self.id = id
        self.title = title
        self.url = url
        self.site = site
        self.meals = meals
        self.minutes = minutes
        self.portions = portions
    }

    public func nutrients(_ products: [String: FoodProduct]) -> [FoodNutrient: Double] {
        var total: [FoodNutrient: Double] = [:]
        for portion in portions {
            guard let product = products[portion.product] else { continue }
            for (nutrient, value) in product.per100g {
                total[nutrient, default: 0] += value * portion.grams / 100
            }
        }
        return total
    }

    public func flags(_ products: [String: FoodProduct]) -> Set<FoodFlag> {
        portions.reduce(into: Set<FoodFlag>()) { (flags: inout Set<FoodFlag>, portion: FoodPortion) in
            flags.formUnion(products[portion.product]?.flags ?? [])
        }
    }

    public func shopping(_ products: [String: FoodProduct]) -> [String] {
        var seen: Set<String> = []
        return portions.compactMap { (portion: FoodPortion) -> String? in
            guard let name = products[portion.product]?.aldiName, seen.insert(name).inserted else { return nil }
            return name
        }
    }
}

public struct FoodNeed: Equatable, Identifiable, Sendable {
    public let nutrient: FoodNutrient
    public let average: Double
    public let days: Int

    public init(nutrient: FoodNutrient, average: Double, days: Int) {
        self.nutrient = nutrient
        self.average = average
        self.days = days
    }

    public var id: String { nutrient.rawValue }
    public var ratio: Double { average / nutrient.dailyValue }
    public var isLow: Bool { !nutrient.isLimit && ratio < FoodNeeds.lowRatio }
    public var isHigh: Bool { nutrient.isLimit && ratio > 1 }
    public var weight: Double { isLow ? 1 - ratio : (isHigh ? min(1, ratio - 1) : 0) }
    public var percent: Int { Int((ratio * 100).rounded()) }

    public var title: String {
        if isLow { return "Low on \(nutrient.lowerName)" }
        if isHigh { return "High in \(nutrient.lowerName)" }
        return nutrient.name
    }

    public var line: String {
        let base: String =
            "\(nutrient.amount(average)) a day on average over \(days) days, \(percent)% of the "
            + "\(nutrient.amount(nutrient.dailyValue)) daily value."
        if isLow { return base + " Suggestions favour \(nutrient.sources)." }
        if isHigh { return base + " Suggestions favour \(nutrient.sources)." }
        return base
    }
}

public enum FoodNeeds {
    public static let lowRatio: Double = 0.8
    public static let minDays: Int = 3
    public static let fullDayKcal: Double = 800
    public static let windowDays: Int = 14

    public static func averages(_ days: [String: [FoodNutrient: Double]], before today: String) -> [FoodNeed] {
        let complete: [[FoodNutrient: Double]] = days
            .filter { (entry: (key: String, value: [FoodNutrient: Double])) -> Bool in
                entry.key < today && (entry.value[.energy] ?? 0) >= fullDayKcal
            }
            .map(\.value)
        return FoodNutrient.needs.compactMap { (nutrient: FoodNutrient) -> FoodNeed? in
            let values: [Double] = complete.compactMap { $0[nutrient] }.filter { $0 > 0 }
            guard values.count >= minDays else { return nil }
            return FoodNeed(nutrient: nutrient, average: values.reduce(0, +) / Double(values.count), days: values.count)
        }
    }

    public static func gaps(_ needs: [FoodNeed]) -> [FoodNeed] {
        needs.filter { $0.isLow || $0.isHigh }.sorted { (a: FoodNeed, b: FoodNeed) -> Bool in
            a.weight == b.weight ? a.nutrient.rawValue < b.nutrient.rawValue : a.weight > b.weight
        }
    }

    public static let everyday: [FoodNeed] = [
        FoodNeed(nutrient: .fiber, average: FoodNutrient.fiber.dailyValue * 0.6, days: 0),
        FoodNeed(nutrient: .potassium, average: FoodNutrient.potassium.dailyValue * 0.6, days: 0),
    ]
}

public enum FoodBudgetSource: Equatable, Sendable {
    case calorieCounter
    case standard
}

public struct FoodBudget: Equatable, Sendable {
    public let daily: Int
    public let eaten: Int
    public let source: FoodBudgetSource

    public init(daily: Int, eaten: Int, source: FoodBudgetSource) {
        self.daily = daily
        self.eaten = eaten
        self.source = source
    }

    public var remaining: Int { max(0, daily - eaten) }

    public var line: String {
        "\(FoodText.kcal(daily)) kcal a day · \(FoodText.kcal(eaten)) eaten today · \(FoodText.kcal(remaining)) left"
    }

    public var sourceLine: String {
        switch source {
        case .calorieCounter: return "Budget from your Calorie Counter goal."
        case .standard:
            return "Using the standard 2,000 kcal daily value. Set a daily goal in the Calorie Counter to use your own budget."
        }
    }
}

public struct FoodMealPlan: Equatable, Identifiable, Sendable {
    public let meal: FoodMeal
    public let target: Int
    public let tomorrow: Bool

    public init(meal: FoodMeal, target: Int, tomorrow: Bool) {
        self.meal = meal
        self.target = target
        self.tomorrow = tomorrow
    }

    public var id: String { meal.rawValue }

    public var title: String { tomorrow ? "\(meal.title) tomorrow" : meal.title }

    public var line: String {
        if !tomorrow && target < FoodPlanner.lightMeal {
            return "\(FoodText.kcal(target)) kcal left for \(meal.rawValue). The lightest choices come first."
        }
        return "About \(FoodText.kcal(target)) kcal"
    }

    public static func plans(_ budget: FoodBudget, minuteOfDay: Int) -> [FoodMealPlan] {
        let ahead: [FoodMeal] = FoodMeal.allCases.filter { minuteOfDay < $0.endsAtMinute }
        let aheadShare: Double = ahead.reduce(0) { $0 + $1.share }
        return FoodMeal.allCases.map { (meal: FoodMeal) -> FoodMealPlan in
            if ahead.contains(meal) && aheadShare > 0 {
                let target: Double = Double(budget.remaining) * meal.share / aheadShare
                return FoodMealPlan(meal: meal, target: Int(target.rounded()), tomorrow: false)
            }
            return FoodMealPlan(meal: meal, target: Int((Double(budget.daily) * meal.share).rounded()), tomorrow: true)
        }
    }
}

public struct FoodSuggestion: Equatable, Identifiable, Sendable {
    public let recipe: FoodRecipe
    public let nutrients: [FoodNutrient: Double]
    public let reasons: [String]
    public let shopping: [String]
    public let highlights: [FoodNutrient]
    public let score: Double

    public var id: String { recipe.id }
    public var kcal: Int { Int((nutrients[.energy] ?? 0).rounded()) }
    public var metaLine: String {
        let parts: [String] = ["\(FoodText.kcal(kcal)) kcal", recipe.minutes > 0 ? "\(recipe.minutes) min" : "", recipe.site]
        return parts.filter { !$0.isEmpty }.joined(separator: " · ")
    }

    public var nutrientLine: String {
        highlights.map { (nutrient: FoodNutrient) -> String in
            "\(nutrient.amount(nutrients[nutrient] ?? 0)) \(nutrient.lowerName)"
        }.joined(separator: " · ")
    }

    public var shoppingLine: String { "At ALDI: " + shopping.joined(separator: ", ") }
}

public enum FoodPlanner {
    public static let lightMeal: Int = 250
    public static let perMeal: Int = 3

    public static func suggest(
        recipes: [FoodRecipe], products: [String: FoodProduct], plans: [FoodMealPlan], needs: [FoodNeed],
        goal: FoodGoal, diets: Set<FoodDiet>, day: String, rounds: [FoodMeal: Int] = [:]
    ) -> [FoodMeal: [FoodSuggestion]] {
        let excluded: Set<FoodFlag> = diets.reduce(into: Set<FoodFlag>()) { $0.formUnion($1.excludes) }
        let allowed: [FoodRecipe] = recipes.filter { $0.flags(products).isDisjoint(with: excluded) }
        var used: Set<String> = []
        var result: [FoodMeal: [FoodSuggestion]] = [:]
        for plan in plans {
            let ranked: [FoodSuggestion] = allowed
                .filter { $0.meals.contains(plan.meal) && !used.contains($0.id) }
                .map { score($0, products: products, plan: plan, needs: needs, goal: goal, day: day) }
                .sorted { (a: FoodSuggestion, b: FoodSuggestion) -> Bool in
                    a.score == b.score ? a.recipe.id < b.recipe.id : a.score > b.score
                }
            let picks: [FoodSuggestion] = page(ranked, round: rounds[plan.meal] ?? 0)
            used.formUnion(picks.map(\.id))
            result[plan.meal] = picks
        }
        return result
    }

    static func page(_ ranked: [FoodSuggestion], round: Int) -> [FoodSuggestion] {
        guard ranked.count > perMeal else { return ranked }
        let start: Int = (round * perMeal) % ranked.count
        return (0..<perMeal).map { ranked[(start + $0) % ranked.count] }
    }

    public static func score(
        _ recipe: FoodRecipe, products: [String: FoodProduct], plan: FoodMealPlan, needs: [FoodNeed], goal: FoodGoal,
        day: String
    ) -> FoodSuggestion {
        let nutrients: [FoodNutrient: Double] = recipe.nutrients(products)
        let share: Double = plan.meal.share
        var score: Double = 0
        var reasons: [String] = []
        var highlights: [FoodNutrient] = []

        let kcal: Double = nutrients[.energy] ?? 0
        let target: Double = Double(max(plan.target, 1))
        let over: Double = (kcal - target) / target
        let overWeight: Double = goal == .loseWeight ? 3 : (goal == .buildMuscle ? 1.5 : 2)
        let fit: Double = over > 0 ? max(0, 1 - over * overWeight) : max(0, 1 + over * 1.2)
        score += 3 * fit
        if plan.target < lightMeal {
            score -= kcal / 400
        }
        if abs(over) <= 0.15 {
            reasons.append("\(FoodText.kcal(Int(kcal.rounded()))) kcal fits your \(FoodText.kcal(plan.target)) kcal \(plan.meal.rawValue)")
        } else if over < 0 {
            reasons.append("\(FoodText.kcal(Int(kcal.rounded()))) kcal, under your \(FoodText.kcal(plan.target)) kcal \(plan.meal.rawValue)")
        }

        for need in needs where need.isLow || need.isHigh {
            let amount: Double = nutrients[need.nutrient] ?? 0
            let portion: Double = amount / (need.nutrient.dailyValue * share)
            if need.isLow {
                score += 4 * need.weight * min(1, portion)
                if portion >= 0.6 {
                    let percent: Int = Int((amount / need.nutrient.dailyValue * 100).rounded())
                    reasons.append("\(need.nutrient.amount(amount)) \(need.nutrient.lowerName), \(percent)% of the daily value")
                    highlights.append(need.nutrient)
                }
            } else {
                score -= 3 * need.weight * max(0, min(2, portion) - 0.6)
                if portion <= 0.5 {
                    reasons.append("Low in \(need.nutrient.lowerName): \(need.nutrient.amount(amount))")
                }
            }
        }

        let protein: Double = nutrients[.protein] ?? 0
        let fiber: Double = nutrients[.fiber] ?? 0
        let potassium: Double = nutrients[.potassium] ?? 0
        let sodium: Double = nutrients[.sodium] ?? 0
        let saturated: Double = nutrients[.saturatedFat] ?? 0
        switch goal {
        case .loseWeight:
            score += 1.5 * min(1, protein / 30) + min(1, fiber / 8)
            if protein >= 25 || fiber >= 8 {
                reasons.append("\(FoodText.number(protein)) g protein and \(FoodText.number(fiber)) g fiber to keep you full")
            }
        case .buildMuscle:
            score += 3 * min(1, protein / 35)
            if protein >= 30 { reasons.append("\(FoodText.number(protein)) g protein for building muscle") }
        case .eatHealthier:
            score += 1.5 * min(1, fiber / 8) + min(1, potassium / (FoodNutrient.potassium.dailyValue * share))
            score -= max(0, sodium / (FoodNutrient.sodium.dailyValue * share) - 1)
            score -= max(0, saturated / (FoodNutrient.saturatedFat.dailyValue * share) - 1)
            if fiber >= 8 { reasons.append("\(FoodText.number(fiber)) g fiber") }
        case .maintain:
            score += 0.75 * min(1, fiber / 8) + 0.75 * min(1, protein / 25)
        }
        score += 0.6 * FoodText.jitter(day + recipe.id)

        var shown: [FoodNutrient] = [.protein, .fiber]
        for nutrient in highlights where !shown.contains(nutrient) {
            shown.append(nutrient)
        }
        var seen: Set<String> = []
        let unique: [String] = reasons.filter { seen.insert($0).inserted }
        return FoodSuggestion(
            recipe: recipe, nutrients: nutrients, reasons: Array(unique.prefix(3)), shopping: recipe.shopping(products),
            highlights: Array(shown.prefix(4)), score: score)
    }
}

public enum FoodText {
    public static func number(_ value: Double) -> String {
        if value >= 10 || value == value.rounded() { return String(Int(value.rounded())) }
        let tenths: Double = (value * 10).rounded() / 10
        return tenths == tenths.rounded() ? String(Int(tenths)) : String(tenths)
    }

    public static func kcal(_ value: Int) -> String {
        let digits: String = String(abs(value))
        var grouped: String = ""
        for (index, character) in digits.enumerated() {
            if index > 0 && (digits.count - index) % 3 == 0 { grouped.append(",") }
            grouped.append(character)
        }
        return value < 0 ? "-" + grouped : grouped
    }

    static func jitter(_ text: String) -> Double {
        var hash: UInt64 = 1_469_598_103_934_665_603
        for byte in text.utf8 {
            hash = (hash ^ UInt64(byte)) &* 1_099_511_628_211
        }
        return Double(hash % 1000) / 1000
    }
}

@MainActor
public protocol NutritionBridge: AnyObject {
    var available: Bool { get }
    func authorizeNutrition() async -> Bool
    func nutrientDays(from: Date, to: Date) async -> [String: [FoodNutrient: Double]]
}

public struct FoodGuideAPI {
    public var caloriesOverview: @MainActor (_ day: String) async throws -> JSONObject

    public init(caloriesOverview: @escaping @MainActor (_ day: String) async throws -> JSONObject) {
        self.caloriesOverview = caloriesOverview
    }

    @MainActor
    public init(api: ApiService) {
        caloriesOverview = { day in try await api.caloriesOverview(day) }
    }
}

public enum FoodGuidePrefs {
    public static let goal = "foodGuide.goal"
    public static let diets = "foodGuide.diets"
}

@MainActor
@Observable
public final class FoodGuideModel {
    public static let title = "Food Guide"
    public static let intro =
        "Breakfast, lunch and dinner ideas from recipes on the web, made with groceries from ALDI, picked for your calorie budget, your goal and what your diet has been short of."
    public static let healthOffLine =
        "Turn on Apple Health to read the last 14 days of nutrition that other apps logged, such as iron, fiber and calcium, so suggestions fill what you have been missing. It stays on this phone."
    public static let disclaimer =
        "Ideas, not medical advice. Nutrition is estimated from typical values for each ingredient; check ALDI labels for exact numbers. For a medical condition, pregnancy, an eating disorder, a child's diet or a food allergy, talk to a doctor or dietitian."

    public private(set) var loading = false
    public private(set) var loaded = false
    public private(set) var healthEnabled: Bool
    public private(set) var needs: [FoodNeed] = []
    public private(set) var healthEaten: Int = 0
    public private(set) var calorieGoal: Int = 0
    public private(set) var calorieEaten: Int = 0
    public private(set) var plans: [FoodMealPlan] = []
    public private(set) var suggestions: [FoodMeal: [FoodSuggestion]] = [:]
    public private(set) var diets: Set<FoodDiet>
    public private(set) var goal: FoodGoal
    public var notice: ToolsNotice?

    @ObservationIgnored private let api: FoodGuideAPI?
    @ObservationIgnored private let health: NutritionBridge?
    @ObservationIgnored private let prefs: KeyValueStore
    @ObservationIgnored private let timeZone: TimeZone
    @ObservationIgnored private let now: () -> Date
    @ObservationIgnored private let recipes: [FoodRecipe]
    @ObservationIgnored private let products: [String: FoodProduct]
    @ObservationIgnored private var rounds: [FoodMeal: Int] = [:]

    public init(
        api: FoodGuideAPI?, health: NutritionBridge?, prefs: KeyValueStore = InMemoryKeyValueStore(),
        recipes: [FoodRecipe] = FoodCatalog.recipes, products: [FoodProduct] = FoodCatalog.products,
        timeZone: TimeZone = .current, now: (() -> Date)? = nil
    ) {
        self.api = api
        self.health = health
        self.prefs = prefs
        self.recipes = recipes
        self.products = Dictionary(products.map { ($0.id, $0) }, uniquingKeysWith: { first, _ in first })
        self.timeZone = timeZone
        self.now = now ?? { Date() }
        healthEnabled = prefs.bool(forKey: HealthPrefs.foodGuide, or: false)
        goal = FoodGoal(rawValue: prefs.string(forKey: FoodGuidePrefs.goal, or: "")) ?? .eatHealthier
        diets = Set((prefs.stringList(forKey: FoodGuidePrefs.diets) ?? []).compactMap(FoodDiet.init(rawValue:)))
    }

    public var today: String { CaloriesText.isoDay(now(), timeZone: timeZone) }
    public var healthAvailable: Bool { health?.available == true }
    public var gaps: [FoodNeed] { FoodNeeds.gaps(needs) }

    public var budget: FoodBudget {
        let eaten: Int = max(calorieEaten, healthEaten)
        if calorieGoal > 0 { return FoodBudget(daily: calorieGoal, eaten: eaten, source: .calorieCounter) }
        return FoodBudget(daily: Int(FoodNutrient.energy.dailyValue), eaten: eaten, source: .standard)
    }

    public var needsLine: String {
        if !healthEnabled { return FoodGuideModel.healthOffLine }
        if needs.isEmpty {
            return "Apple Health has fewer than \(FoodNeeds.minDays) full days of nutrition in the last \(FoodNeeds.windowDays) days. Log meals in the Calorie Counter or another app and suggestions will follow what you have been missing."
        }
        if gaps.isEmpty { return "Over the last \(FoodNeeds.windowDays) days your logged nutrition meets the daily values Food Guide checks. Suggestions keep it balanced." }
        return "From the last \(FoodNeeds.windowDays) days of nutrition in Apple Health."
    }

    public func suggestions(for meal: FoodMeal) -> [FoodSuggestion] { suggestions[meal] ?? [] }

    public func load() async {
        loading = true
        defer {
            loading = false
            loaded = true
        }
        await readCalories()
        await readHealth()
        rebuild()
    }

    public func setGoal(_ value: FoodGoal) {
        goal = value
        prefs.set(value.rawValue, forKey: FoodGuidePrefs.goal)
        rounds = [:]
        rebuild()
    }

    public func toggleDiet(_ diet: FoodDiet) {
        if diets.contains(diet) {
            diets.remove(diet)
        } else {
            diets.insert(diet)
        }
        prefs.set(FoodDiet.allCases.filter { diets.contains($0) }.map(\.rawValue), forKey: FoodGuidePrefs.diets)
        rounds = [:]
        rebuild()
    }

    public func moreIdeas(_ meal: FoodMeal) {
        rounds[meal, default: 0] += 1
        rebuild()
    }

    public func enableHealth() async {
        guard let health, health.available else {
            show("Apple Health is not available on this device.", error: true)
            return
        }
        guard await health.authorizeNutrition() else {
            show("ForkMesh could not ask Apple Health for nutrition. Allow it in Settings > Health > Data Access & Devices.", error: true)
            return
        }
        prefs.set(true, forKey: HealthPrefs.foodGuide)
        healthEnabled = true
        await readHealth()
        rebuild()
        show("Food Guide now reads your nutrition history from Apple Health.")
    }

    public func disableHealth() {
        prefs.set(false, forKey: HealthPrefs.foodGuide)
        healthEnabled = false
        needs = []
        healthEaten = 0
        rebuild()
        show("Apple Health is off for Food Guide.")
    }

    func rebuild() {
        let date: Date = now()
        let parts: DateComponents = CaloriesText.calendar(timeZone).dateComponents([.hour, .minute], from: date)
        plans = FoodMealPlan.plans(budget, minuteOfDay: (parts.hour ?? 12) * 60 + (parts.minute ?? 0))
        let drivers: [FoodNeed] = gaps.isEmpty ? FoodNeeds.everyday : gaps
        suggestions = FoodPlanner.suggest(
            recipes: recipes, products: products, plans: plans, needs: drivers, goal: goal, diets: diets, day: today,
            rounds: rounds)
    }

    private func readCalories() async {
        guard let api else { return }
        do {
            let overview: JSONObject = try await api.caloriesOverview(today)
            calorieGoal = CaloriesText.int(ToolsJSON.object(overview["profile"])["dailyCalories"])
            calorieEaten = Int(CaloriesText.num(ToolsJSON.object(overview["todayTotals"])["calories"]).rounded())
        } catch {
            calorieGoal = 0
            calorieEaten = 0
        }
    }

    private func readHealth() async {
        guard healthEnabled, let health, health.available else {
            needs = []
            healthEaten = 0
            return
        }
        let calendar: Calendar = CaloriesText.calendar(timeZone)
        let date: Date = now()
        let midnight: Date = calendar.startOfDay(for: date)
        let start: Date = calendar.date(byAdding: .day, value: -FoodNeeds.windowDays, to: midnight) ?? midnight
        let days: [String: [FoodNutrient: Double]] = await health.nutrientDays(from: start, to: date)
        needs = FoodNeeds.averages(days, before: today)
        healthEaten = Int((days[today]?[.energy] ?? 0).rounded())
    }

    private func show(_ message: String, error: Bool = false) {
        notice = ToolsNotice(message, isError: error)
    }
}
