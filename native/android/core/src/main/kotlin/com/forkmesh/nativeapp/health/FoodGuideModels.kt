package com.forkmesh.nativeapp.health

import com.forkmesh.nativeapp.api.PrefStore
import java.time.Instant
import java.time.ZoneId
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min

enum class FoodNutrient(val raw: String, val title: String, val unit: String, val dailyValue: Double, val sources: String) {
    Energy("energy", "Calories", "kcal", 2000.0, ""),
    Protein("protein", "Protein", "g", 50.0, "chicken, fish, eggs, Greek yogurt, tofu and lentils"),
    Fiber("fiber", "Fiber", "g", 28.0, "oats, beans, lentils, berries, whole grains and vegetables"),
    Iron("iron", "Iron", "mg", 18.0, "lentils, beans, spinach, lean beef, sardines and bran flakes"),
    Calcium("calcium", "Calcium", "mg", 1300.0, "yogurt, milk, cheese, sardines, tofu and kale"),
    VitaminD("vitaminD", "Vitamin D", "mcg", 20.0, "salmon, sardines, tuna, eggs and fortified milk"),
    VitaminC("vitaminC", "Vitamin C", "mg", 90.0, "peppers, strawberries, oranges, broccoli and kale"),
    Potassium("potassium", "Potassium", "mg", 4700.0, "beans, lentils, potatoes, spinach, bananas and salmon"),
    Magnesium("magnesium", "Magnesium", "mg", 420.0, "pumpkin seeds, almonds, spinach, beans and whole grains"),
    Folate("folate", "Folate", "mcg", 400.0, "lentils, beans, spinach, edamame and avocado"),
    VitaminB12("vitaminB12", "Vitamin B12", "mcg", 2.4, "fish, beef, eggs, yogurt and milk"),
    Sodium("sodium", "Sodium", "mg", 2300.0, "home-cooked meals with fresh ingredients"),
    Sugar("sugar", "Sugar", "g", 50.0, "meals built on vegetables, protein and whole grains"),
    SaturatedFat("saturatedFat", "Saturated fat", "g", 20.0, "fish, beans, lean poultry and olive oil");

    val lowerName: String get() = if (this == VitaminD || this == VitaminC || this == VitaminB12) title else title.lowercase()

    val isLimit: Boolean get() = this == Sodium || this == Sugar || this == SaturatedFat

    fun amount(value: Double): String = "${FoodText.number(value)} $unit"

    companion object {
        val needs: List<FoodNutrient> = entries.filter { it != Energy }
    }
}

enum class FoodFlag { Meat, Fish, Shellfish, Dairy, Egg, Gluten, Nuts, Peanuts, Soy }

enum class FoodDiet(val raw: String, val label: String, val excludes: Set<FoodFlag>) {
    Vegetarian("vegetarian", "Vegetarian", setOf(FoodFlag.Meat, FoodFlag.Fish, FoodFlag.Shellfish)),
    Vegan("vegan", "Vegan", setOf(FoodFlag.Meat, FoodFlag.Fish, FoodFlag.Shellfish, FoodFlag.Dairy, FoodFlag.Egg)),
    DairyFree("dairyFree", "Dairy-free", setOf(FoodFlag.Dairy)),
    GlutenFree("glutenFree", "Gluten-free", setOf(FoodFlag.Gluten)),
    NoFish("noFish", "No fish or shellfish", setOf(FoodFlag.Fish, FoodFlag.Shellfish)),
    NutFree("nutFree", "No nuts or peanuts", setOf(FoodFlag.Nuts, FoodFlag.Peanuts));

    companion object {
        fun of(raw: String): FoodDiet? = entries.firstOrNull { it.raw == raw }
    }
}

enum class FoodGoal(val raw: String, val label: String, val line: String) {
    LoseWeight("loseWeight", "Lose weight", "Filling meals high in protein and fiber that stay inside your calorie budget."),
    Maintain("maintain", "Keep my weight", "Balanced meals that match your calorie budget."),
    BuildMuscle("buildMuscle", "Build muscle", "Meals with 30 g of protein or more, sized to your calorie budget."),
    EatHealthier("eatHealthier", "Eat healthier", "More vegetables, fiber and potassium, less sodium and saturated fat.");

    companion object {
        fun of(raw: String): FoodGoal? = entries.firstOrNull { it.raw == raw }
    }
}

enum class FoodMeal(val raw: String, val title: String, val share: Double, val endsAtMinute: Int) {
    Breakfast("breakfast", "Breakfast", 0.25, 10 * 60 + 30),
    Lunch("lunch", "Lunch", 0.35, 15 * 60),
    Dinner("dinner", "Dinner", 0.40, 21 * 60),
}

class FoodProduct(val id: String, val aldiName: String, values: DoubleArray, val flags: Set<FoodFlag> = emptySet()) {
    val per100g: Map<FoodNutrient, Double>

    init {
        val map = LinkedHashMap<FoodNutrient, Double>()
        FoodNutrient.entries.forEachIndexed { index, nutrient ->
            val value = values.getOrNull(index) ?: return@forEachIndexed
            if (value > 0) map[nutrient] = value
        }
        per100g = map
    }
}

data class FoodPortion(val product: String, val grams: Double)

class FoodRecipe(
    val id: String,
    val title: String,
    val site: String,
    val url: String,
    val meals: Set<FoodMeal>,
    val minutes: Int,
    val portions: List<FoodPortion>,
) {
    fun nutrients(products: Map<String, FoodProduct>): Map<FoodNutrient, Double> {
        val total = LinkedHashMap<FoodNutrient, Double>()
        for (portion in portions) {
            val product = products[portion.product] ?: continue
            for ((nutrient, value) in product.per100g) {
                total[nutrient] = (total[nutrient] ?: 0.0) + value * portion.grams / 100
            }
        }
        return total
    }

    fun flags(products: Map<String, FoodProduct>): Set<FoodFlag> {
        val flags = HashSet<FoodFlag>()
        for (portion in portions) products[portion.product]?.let { flags.addAll(it.flags) }
        return flags
    }

    fun shopping(products: Map<String, FoodProduct>): List<String> {
        val seen = HashSet<String>()
        val names = ArrayList<String>()
        for (portion in portions) {
            val name = products[portion.product]?.aldiName ?: continue
            if (seen.add(name)) names.add(name)
        }
        return names
    }
}

data class FoodNeed(val nutrient: FoodNutrient, val average: Double, val days: Int) {
    val id: String get() = nutrient.raw
    val ratio: Double get() = average / nutrient.dailyValue
    val isLow: Boolean get() = !nutrient.isLimit && ratio < FoodNeeds.LOW_RATIO
    val isHigh: Boolean get() = nutrient.isLimit && ratio > 1
    val weight: Double get() = if (isLow) 1 - ratio else if (isHigh) min(1.0, ratio - 1) else 0.0
    val percent: Int get() = FoodText.round(ratio * 100)

    val title: String
        get() = when {
            isLow -> "Low on ${nutrient.lowerName}"
            isHigh -> "High in ${nutrient.lowerName}"
            else -> nutrient.title
        }

    val line: String
        get() {
            val base = "${nutrient.amount(average)} a day on average over $days days, $percent% of the ${nutrient.amount(nutrient.dailyValue)} daily value."
            return if (isLow || isHigh) "$base Suggestions favour ${nutrient.sources}." else base
        }
}

object FoodNeeds {
    const val LOW_RATIO = 0.8
    const val MIN_DAYS = 3
    const val FULL_DAY_KCAL = 800.0
    const val WINDOW_DAYS = 14

    fun averages(days: Map<String, Map<FoodNutrient, Double>>, before: String): List<FoodNeed> {
        val complete = days.filter { it.key < before && (it.value[FoodNutrient.Energy] ?: 0.0) >= FULL_DAY_KCAL }.map { it.value }
        return FoodNutrient.needs.mapNotNull { nutrient ->
            val values = complete.mapNotNull { it[nutrient] }.filter { it > 0 }
            if (values.size < MIN_DAYS) null else FoodNeed(nutrient, values.sum() / values.size, values.size)
        }
    }

    fun gaps(needs: List<FoodNeed>): List<FoodNeed> = needs.filter { it.isLow || it.isHigh }.sortedWith { a, b ->
        if (a.weight == b.weight) a.nutrient.raw.compareTo(b.nutrient.raw) else b.weight.compareTo(a.weight)
    }

    val everyday: List<FoodNeed> = listOf(
        FoodNeed(FoodNutrient.Fiber, FoodNutrient.Fiber.dailyValue * 0.6, 0),
        FoodNeed(FoodNutrient.Potassium, FoodNutrient.Potassium.dailyValue * 0.6, 0),
    )
}

enum class FoodBudgetSource { CalorieCounter, Standard }

data class FoodBudget(val daily: Int, val eaten: Int, val source: FoodBudgetSource) {
    val remaining: Int get() = max(0, daily - eaten)

    val line: String get() = "${FoodText.kcal(daily)} kcal a day · ${FoodText.kcal(eaten)} eaten today · ${FoodText.kcal(remaining)} left"

    val sourceLine: String
        get() = when (source) {
            FoodBudgetSource.CalorieCounter -> "Budget from your Calorie Counter goal."
            FoodBudgetSource.Standard -> "Using the standard 2,000 kcal daily value. Set a daily goal in the Calorie Counter to use your own budget."
        }
}

data class FoodMealPlan(val meal: FoodMeal, val target: Int, val tomorrow: Boolean) {
    val id: String get() = meal.raw

    val title: String get() = if (tomorrow) "${meal.title} tomorrow" else meal.title

    val line: String
        get() {
            if (!tomorrow && target < FoodPlanner.LIGHT_MEAL) {
                return "${FoodText.kcal(target)} kcal left for ${meal.raw}. The lightest choices come first."
            }
            return "About ${FoodText.kcal(target)} kcal"
        }

    companion object {
        fun plans(budget: FoodBudget, minuteOfDay: Int): List<FoodMealPlan> {
            val ahead = FoodMeal.entries.filter { minuteOfDay < it.endsAtMinute }
            val aheadShare = ahead.sumOf { it.share }
            return FoodMeal.entries.map { meal ->
                if (meal in ahead && aheadShare > 0) {
                    val target = budget.remaining * meal.share / aheadShare
                    FoodMealPlan(meal, FoodText.round(target), false)
                } else {
                    FoodMealPlan(meal, FoodText.round(budget.daily * meal.share), true)
                }
            }
        }
    }
}

class FoodSuggestion(
    val recipe: FoodRecipe,
    val nutrients: Map<FoodNutrient, Double>,
    val reasons: List<String>,
    val shopping: List<String>,
    val highlights: List<FoodNutrient>,
    val score: Double,
) {
    val id: String get() = recipe.id
    val kcal: Int get() = FoodText.round(nutrients[FoodNutrient.Energy] ?: 0.0)

    val metaLine: String
        get() = listOf("${FoodText.kcal(kcal)} kcal", if (recipe.minutes > 0) "${recipe.minutes} min" else "", recipe.site)
            .filter { it.isNotEmpty() }
            .joinToString(" · ")

    val nutrientLine: String
        get() = highlights.joinToString(" · ") { "${it.amount(nutrients[it] ?: 0.0)} ${it.lowerName}" }

    val shoppingLine: String get() = "At ALDI: " + shopping.joinToString(", ")
}

object FoodPlanner {
    const val LIGHT_MEAL = 250
    const val PER_MEAL = 3

    fun suggest(
        recipes: List<FoodRecipe>,
        products: Map<String, FoodProduct>,
        plans: List<FoodMealPlan>,
        needs: List<FoodNeed>,
        goal: FoodGoal,
        diets: Set<FoodDiet>,
        day: String,
        rounds: Map<FoodMeal, Int> = emptyMap(),
    ): Map<FoodMeal, List<FoodSuggestion>> {
        val excluded = HashSet<FoodFlag>()
        for (diet in diets) excluded.addAll(diet.excludes)
        val allowed = recipes.filter { recipe -> recipe.flags(products).none { it in excluded } }
        val used = HashSet<String>()
        val result = LinkedHashMap<FoodMeal, List<FoodSuggestion>>()
        for (plan in plans) {
            val ranked = allowed
                .filter { plan.meal in it.meals && it.id !in used }
                .map { score(it, products, plan, needs, goal, day) }
                .sortedWith { a, b -> if (a.score == b.score) a.recipe.id.compareTo(b.recipe.id) else b.score.compareTo(a.score) }
            val picks = page(ranked, rounds[plan.meal] ?: 0)
            used.addAll(picks.map { it.id })
            result[plan.meal] = picks
        }
        return result
    }

    fun page(ranked: List<FoodSuggestion>, round: Int): List<FoodSuggestion> {
        if (ranked.size <= PER_MEAL) return ranked
        val start = (round * PER_MEAL) % ranked.size
        return (0 until PER_MEAL).map { ranked[(start + it) % ranked.size] }
    }

    fun score(
        recipe: FoodRecipe,
        products: Map<String, FoodProduct>,
        plan: FoodMealPlan,
        needs: List<FoodNeed>,
        goal: FoodGoal,
        day: String,
    ): FoodSuggestion {
        val nutrients = recipe.nutrients(products)
        val share = plan.meal.share
        var score = 0.0
        val reasons = ArrayList<String>()
        val highlights = ArrayList<FoodNutrient>()

        val kcal = nutrients[FoodNutrient.Energy] ?: 0.0
        val target = max(plan.target, 1).toDouble()
        val over = (kcal - target) / target
        val overWeight = if (goal == FoodGoal.LoseWeight) 3.0 else if (goal == FoodGoal.BuildMuscle) 1.5 else 2.0
        val fit = if (over > 0) max(0.0, 1 - over * overWeight) else max(0.0, 1 + over * 1.2)
        score += 3 * fit
        if (plan.target < LIGHT_MEAL) {
            score -= kcal / 400
        }
        if (abs(over) <= 0.15) {
            reasons.add("${FoodText.kcal(FoodText.round(kcal))} kcal fits your ${FoodText.kcal(plan.target)} kcal ${plan.meal.raw}")
        } else if (over < 0) {
            reasons.add("${FoodText.kcal(FoodText.round(kcal))} kcal, under your ${FoodText.kcal(plan.target)} kcal ${plan.meal.raw}")
        }

        for (need in needs) {
            if (!(need.isLow || need.isHigh)) continue
            val amount = nutrients[need.nutrient] ?: 0.0
            val portion = amount / (need.nutrient.dailyValue * share)
            if (need.isLow) {
                score += 4 * need.weight * min(1.0, portion)
                if (portion >= 0.6) {
                    val percent = FoodText.round(amount / need.nutrient.dailyValue * 100)
                    reasons.add("${need.nutrient.amount(amount)} ${need.nutrient.lowerName}, $percent% of the daily value")
                    highlights.add(need.nutrient)
                }
            } else {
                score -= 3 * need.weight * max(0.0, min(2.0, portion) - 0.6)
                if (portion <= 0.5) {
                    reasons.add("Low in ${need.nutrient.lowerName}: ${need.nutrient.amount(amount)}")
                }
            }
        }

        val protein = nutrients[FoodNutrient.Protein] ?: 0.0
        val fiber = nutrients[FoodNutrient.Fiber] ?: 0.0
        val potassium = nutrients[FoodNutrient.Potassium] ?: 0.0
        val sodium = nutrients[FoodNutrient.Sodium] ?: 0.0
        val saturated = nutrients[FoodNutrient.SaturatedFat] ?: 0.0
        when (goal) {
            FoodGoal.LoseWeight -> {
                score += 1.5 * min(1.0, protein / 30) + min(1.0, fiber / 8)
                if (protein >= 25 || fiber >= 8) {
                    reasons.add("${FoodText.number(protein)} g protein and ${FoodText.number(fiber)} g fiber to keep you full")
                }
            }
            FoodGoal.BuildMuscle -> {
                score += 3 * min(1.0, protein / 35)
                if (protein >= 30) reasons.add("${FoodText.number(protein)} g protein for building muscle")
            }
            FoodGoal.EatHealthier -> {
                score += 1.5 * min(1.0, fiber / 8) + min(1.0, potassium / (FoodNutrient.Potassium.dailyValue * share))
                score -= max(0.0, sodium / (FoodNutrient.Sodium.dailyValue * share) - 1)
                score -= max(0.0, saturated / (FoodNutrient.SaturatedFat.dailyValue * share) - 1)
                if (fiber >= 8) reasons.add("${FoodText.number(fiber)} g fiber")
            }
            FoodGoal.Maintain -> {
                score += 0.75 * min(1.0, fiber / 8) + 0.75 * min(1.0, protein / 25)
            }
        }
        score += 0.6 * FoodText.jitter(day + recipe.id)

        val shown = arrayListOf(FoodNutrient.Protein, FoodNutrient.Fiber)
        for (nutrient in highlights) if (nutrient !in shown) shown.add(nutrient)
        val seen = HashSet<String>()
        val unique = reasons.filter { seen.add(it) }
        return FoodSuggestion(recipe, nutrients, unique.take(3), recipe.shopping(products), shown.take(4), score)
    }
}

object FoodText {
    fun round(value: Double): Int = Math.round(value).toInt()

    fun number(value: Double): String {
        if (value >= 10 || value == Math.rint(value)) return Math.round(value).toString()
        val tenths = Math.round(value * 10) / 10.0
        return if (tenths == Math.round(tenths).toDouble()) Math.round(tenths).toString() else tenths.toString()
    }

    fun kcal(value: Int): String {
        val digits = abs(value).toString()
        val grouped = StringBuilder()
        digits.forEachIndexed { index, character ->
            if (index > 0 && (digits.length - index) % 3 == 0) grouped.append(',')
            grouped.append(character)
        }
        return if (value < 0) "-$grouped" else grouped.toString()
    }

    fun jitter(text: String): Double {
        var hash = 1469598103934665603UL
        for (byte in text.toByteArray(Charsets.UTF_8)) {
            hash = (hash xor (byte.toULong() and 0xFFUL)) * 1099511628211UL
        }
        return (hash % 1000UL).toDouble() / 1000
    }
}

object FoodGuidePrefs {
    const val GOAL = "foodGuide.goal"
    const val DIETS = "foodGuide.diets"

    fun readList(prefs: PrefStore?, key: String): List<String> {
        val text = prefs?.getString(key) ?: return emptyList()
        return Regex("\"((?:[^\"\\\\]|\\\\.)*)\"").findAll(text).map { it.groupValues[1] }.toList()
    }

    fun writeList(prefs: PrefStore?, key: String, values: List<String>) {
        prefs?.putString(key, values.joinToString(",", "[", "]") { "\"" + it.replace("\\", "\\\\").replace("\"", "\\\"") + "\"" })
    }
}

class FoodGuideModel(
    private val caloriesOverview: ((String) -> Map<String, Any?>)?,
    private val prefs: PrefStore? = null,
    private val recipes: List<FoodRecipe> = FoodCatalog.recipes,
    products: List<FoodProduct> = FoodCatalog.products,
    private val zone: ZoneId = ZoneId.systemDefault(),
    private val now: () -> Instant = { Instant.now() },
) {
    @Volatile var loading = false
        private set
    @Volatile var loaded = false
        private set
    @Volatile var calorieGoal = 0
        private set
    @Volatile var calorieEaten = 0
        private set
    @Volatile var plans: List<FoodMealPlan> = emptyList()
        private set
    @Volatile var suggestions: Map<FoodMeal, List<FoodSuggestion>> = emptyMap()
        private set
    @Volatile var diets: Set<FoodDiet>
        private set
    @Volatile var goal: FoodGoal
        private set
    @Volatile var onChange: (() -> Unit)? = null

    private val products: Map<String, FoodProduct> = LinkedHashMap<String, FoodProduct>().also { map ->
        for (product in products) if (product.id !in map) map[product.id] = product
    }
    private var rounds: Map<FoodMeal, Int> = emptyMap()

    init {
        goal = FoodGoal.of(prefs?.getString(FoodGuidePrefs.GOAL).orEmpty()) ?: FoodGoal.EatHealthier
        diets = FoodGuidePrefs.readList(prefs, FoodGuidePrefs.DIETS).mapNotNull { FoodDiet.of(it) }.toSet()
    }

    val today: String get() = HealthDates.isoDay(now(), zone)

    val budget: FoodBudget
        get() {
            val eaten = calorieEaten
            if (calorieGoal > 0) return FoodBudget(calorieGoal, eaten, FoodBudgetSource.CalorieCounter)
            return FoodBudget(FoodNutrient.Energy.dailyValue.toInt(), eaten, FoodBudgetSource.Standard)
        }

    fun suggestions(meal: FoodMeal): List<FoodSuggestion> = suggestions[meal] ?: emptyList()

    fun load() {
        loading = true
        changed()
        try {
            readCalories()
            rebuild()
        } finally {
            loading = false
            loaded = true
            changed()
        }
    }

    fun setGoal(value: FoodGoal) {
        goal = value
        prefs?.putString(FoodGuidePrefs.GOAL, value.raw)
        rounds = emptyMap()
        rebuild()
        changed()
    }

    fun toggleDiet(diet: FoodDiet) {
        diets = if (diet in diets) diets - diet else diets + diet
        FoodGuidePrefs.writeList(prefs, FoodGuidePrefs.DIETS, FoodDiet.entries.filter { it in diets }.map { it.raw })
        rounds = emptyMap()
        rebuild()
        changed()
    }

    fun moreIdeas(meal: FoodMeal) {
        rounds = rounds + (meal to ((rounds[meal] ?: 0) + 1))
        rebuild()
        changed()
    }

    fun rebuild() {
        val local = now().atZone(zone)
        plans = FoodMealPlan.plans(budget, local.hour * 60 + local.minute)
        suggestions = FoodPlanner.suggest(recipes, products, plans, FoodNeeds.everyday, goal, diets, today, rounds)
    }

    private fun readCalories() {
        val read = caloriesOverview ?: return
        try {
            val overview = read(today)
            calorieGoal = HealthJsonRead.int(HealthJsonRead.obj(overview["profile"])["dailyCalories"]).toInt()
            calorieEaten = FoodText.round(HealthJsonRead.double(HealthJsonRead.obj(overview["todayTotals"])["calories"]))
        } catch (_: Exception) {
            calorieGoal = 0
            calorieEaten = 0
        }
    }

    private fun changed() {
        onChange?.invoke()
    }

    companion object {
        const val TITLE = "Food Guide"
        const val INTRO = "Breakfast, lunch and dinner ideas from recipes on the web, made with groceries from ALDI, picked for your calorie budget, your goal and your food choices."
        const val NEEDS_LINE = "Suggestions follow your calorie budget and goal and lean on fiber and potassium. Android has no nutrition history to read yet, so they cannot fill in what you have been missing."
        const val DISCLAIMER = "Ideas, not medical advice. Nutrition is estimated from typical values for each ingredient; check ALDI labels for exact numbers. For a medical condition, pregnancy, an eating disorder, a child's diet or a food allergy, talk to a doctor or dietitian."
    }
}
