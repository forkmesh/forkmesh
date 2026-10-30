package com.forkmesh.nativeapp.health

import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.time.Instant
import java.time.ZoneOffset

class FoodGuideTest {
    private class MemoryPrefs : PrefStore {
        val values = HashMap<String, Any>()
        override fun getString(key: String): String? = values[key] as? String
        override fun putString(key: String, value: String) { values[key] = value }
        override fun getBool(key: String): Boolean? = values[key] as? Boolean
        override fun putBool(key: String, value: Boolean) { values[key] = value }
        override fun getDouble(key: String): Double? = values[key] as? Double
        override fun putDouble(key: String, value: Double) { values[key] = value }
        override fun contains(key: String): Boolean = key in values
        override fun remove(key: String) { values.remove(key) }
    }

    private fun at(day: String, hour: Int = 12): Instant = Instant.parse("${day}T00:00:00Z").plusSeconds(hour * 3600L)

    private val products: Map<String, FoodProduct> get() = FoodCatalog.products.associateBy { it.id }

    private fun lowIronDays(): Map<String, Map<FoodNutrient, Double>> {
        val days = LinkedHashMap<String, Map<FoodNutrient, Double>>()
        for (day in listOf("2026-10-08", "2026-10-09", "2026-10-10", "2026-10-11", "2026-10-12")) {
            days[day] = mapOf(
                FoodNutrient.Energy to 1900.0, FoodNutrient.Iron to 8.0, FoodNutrient.Fiber to 26.0,
                FoodNutrient.Protein to 70.0, FoodNutrient.Sodium to 2100.0,
            )
        }
        days["2026-10-07"] = mapOf(FoodNutrient.Energy to 300.0, FoodNutrient.Iron to 1.0)
        days["2026-10-13"] = mapOf(FoodNutrient.Energy to 600.0, FoodNutrient.Iron to 2.0)
        return days
    }

    fun testNeedsAverageOnlyFullPastDays() {
        val needs = FoodNeeds.averages(lowIronDays(), "2026-10-13")
        val iron = needs.firstOrNull { it.nutrient == FoodNutrient.Iron }
        assertEq(8.0, iron?.average)
        assertEq(5, iron?.days)
        assertEq(null, needs.firstOrNull { it.nutrient == FoodNutrient.Calcium })
        assertEq(listOf(FoodNutrient.Iron), FoodNeeds.gaps(needs).map { it.nutrient })
        assertEq("Low on iron", FoodNeeds.gaps(needs).first().title)
        expect(FoodNeeds.gaps(needs).first().line.contains("44% of the 18 mg daily value"))
    }

    fun testTooFewDaysGiveNoNeeds() {
        val days = mapOf(
            "2026-10-11" to mapOf(FoodNutrient.Energy to 2000.0, FoodNutrient.Iron to 5.0),
            "2026-10-12" to mapOf(FoodNutrient.Energy to 2000.0, FoodNutrient.Iron to 5.0),
        )
        expect(FoodNeeds.averages(days, "2026-10-13").isEmpty())
    }

    fun testHighSodiumIsAGap() {
        val need = FoodNeed(FoodNutrient.Sodium, 3450.0, 7)
        expect(need.isHigh)
        expect(!need.isLow)
        assertEq("High in sodium", need.title)
    }

    fun testMealPlansSplitWhatIsLeft() {
        val budget = FoodBudget(2000, 600, FoodBudgetSource.CalorieCounter)
        val plans = FoodMealPlan.plans(budget, 12 * 60)
        assertEq(listOf(FoodMeal.Breakfast, FoodMeal.Lunch, FoodMeal.Dinner), plans.map { it.meal })
        assertEq(true, plans[0].tomorrow)
        assertEq(500, plans[0].target)
        assertEq("Breakfast tomorrow", plans[0].title)
        assertEq(653, plans[1].target)
        assertEq(747, plans[2].target)
        assertEq(false, plans[1].tomorrow)
    }

    fun testLateEveningPlansTomorrow() {
        val budget = FoodBudget(1800, 1700, FoodBudgetSource.Standard)
        val plans = FoodMealPlan.plans(budget, 22 * 60)
        expect(plans.all { it.tomorrow })
        assertEq(listOf(450, 630, 720), plans.map { it.target })
    }

    fun testLittleLeftAsksForTheLightest() {
        val budget = FoodBudget(1800, 1650, FoodBudgetSource.CalorieCounter)
        val plans = FoodMealPlan.plans(budget, 18 * 60)
        assertEq(150, plans[2].target)
        expect(plans[2].line.contains("lightest"))
    }

    fun testCatalogIsWhole() {
        val known = products
        assertEq(FoodCatalog.products.size, known.size)
        for (meal in FoodMeal.entries) {
            expect(FoodCatalog.recipes.count { meal in it.meals } >= 6, meal.raw)
        }
        assertEq(FoodCatalog.recipes.size, FoodCatalog.recipes.map { it.id }.toSet().size)
        for (recipe in FoodCatalog.recipes) {
            expect(recipe.url.startsWith("https://"), recipe.id)
            expect(recipe.portions.isNotEmpty(), recipe.id)
            for (portion in recipe.portions) {
                expect(known[portion.product] != null, "${recipe.id} uses ${portion.product}")
            }
            val kcal = recipe.nutrients(known)[FoodNutrient.Energy] ?: 0.0
            expect(kcal > 150, recipe.id)
            expect(kcal < 950, recipe.id)
        }
    }

    fun testLowIronPutsIronRichMealsFirst() {
        val plans = FoodMealPlan.plans(FoodBudget(2000, 0, FoodBudgetSource.Standard), 7 * 60)
        val iron = listOf(FoodNeed(FoodNutrient.Iron, 6.0, 10))
        val picks = FoodPlanner.suggest(FoodCatalog.recipes, products, plans, iron, FoodGoal.Maintain, emptySet(), "2026-10-13")
        val none = FoodPlanner.suggest(FoodCatalog.recipes, products, plans, emptyList(), FoodGoal.Maintain, emptySet(), "2026-10-13")
        for (meal in FoodMeal.entries) {
            val ironFirst = picks[meal]!!.sumOf { it.nutrients[FoodNutrient.Iron] ?: 0.0 }
            val plain = none[meal]!!.sumOf { it.nutrients[FoodNutrient.Iron] ?: 0.0 }
            expect(ironFirst >= plain, meal.raw)
        }
        val top = picks[FoodMeal.Dinner]!!.first()
        expect(top.reasons.any { it.contains("iron") }, top.reasons.joinToString("; "))
        expect(FoodNutrient.Iron in top.highlights)
        expect(top.shoppingLine.startsWith("At ALDI: "))
    }

    fun testEveryMealGetsThreeDifferentRecipes() {
        val plans = FoodMealPlan.plans(FoodBudget(2000, 0, FoodBudgetSource.Standard), 7 * 60)
        val picks = FoodPlanner.suggest(FoodCatalog.recipes, products, plans, FoodNeeds.everyday, FoodGoal.EatHealthier, emptySet(), "2026-10-13")
        val ids = FoodMeal.entries.flatMap { meal -> picks[meal]!!.map { it.id } }
        assertEq(9, ids.size)
        assertEq(9, ids.toSet().size)
    }

    fun testDietsLeaveOutWhatTheyExclude() {
        val plans = FoodMealPlan.plans(FoodBudget(2000, 0, FoodBudgetSource.Standard), 7 * 60)
        val known = products
        val picks = FoodPlanner.suggest(
            FoodCatalog.recipes, known, plans, emptyList(), FoodGoal.BuildMuscle, setOf(FoodDiet.Vegetarian, FoodDiet.NutFree), "2026-10-13",
        )
        val banned = setOf(FoodFlag.Meat, FoodFlag.Fish, FoodFlag.Shellfish, FoodFlag.Nuts, FoodFlag.Peanuts)
        for (suggestion in FoodMeal.entries.flatMap { picks[it]!! }) {
            expect(suggestion.recipe.flags(known).none { it in banned }, suggestion.id)
        }
    }

    fun testMoreIdeasShowsOthers() {
        val model = FoodGuideModel(null, zone = ZoneOffset.UTC) { at("2026-10-13", 7) }
        model.load()
        val first = model.suggestions(FoodMeal.Lunch).map { it.id }
        model.moreIdeas(FoodMeal.Lunch)
        val next = model.suggestions(FoodMeal.Lunch).map { it.id }
        assertEq(3, next.size)
        expect(first != next)
    }

    fun testBudgetComesFromTheCalorieCounter() {
        var asked = ""
        val overview = { day: String ->
            asked = day
            mapOf<String, Any?>("profile" to mapOf("dailyCalories" to 1800), "todayTotals" to mapOf("calories" to 450))
        }
        val model = FoodGuideModel(overview, zone = ZoneOffset.UTC) { at("2026-10-13", 12) }
        model.load()
        assertEq("2026-10-13", asked)
        assertEq(FoodBudget(1800, 450, FoodBudgetSource.CalorieCounter), model.budget)
        assertEq("1,800 kcal a day · 450 eaten today · 1,350 left", model.budget.line)
        assertEq(listOf(true, false, false), model.plans.map { it.tomorrow })
    }

    fun testWithoutTheCalorieCounterTheStandardBudgetIsUsed() {
        val overview = { _: String -> throw OrgApiException("plugin_not_installed", "not installed") }
        val model = FoodGuideModel(overview, zone = ZoneOffset.UTC) { at("2026-10-13", 7) }
        model.load()
        assertEq(FoodBudget(2000, 0, FoodBudgetSource.Standard), model.budget)
        assertEq(3, model.suggestions(FoodMeal.Breakfast).size)
    }

    fun testChoicesPersist() {
        val prefs = MemoryPrefs()
        val model = FoodGuideModel(null, prefs, zone = ZoneOffset.UTC) { at("2026-10-13") }
        model.setGoal(FoodGoal.BuildMuscle)
        model.toggleDiet(FoodDiet.Vegan)
        model.toggleDiet(FoodDiet.NutFree)
        model.toggleDiet(FoodDiet.Vegan)
        assertEq("buildMuscle", prefs.getString(FoodGuidePrefs.GOAL))
        assertEq("[\"nutFree\"]", prefs.getString(FoodGuidePrefs.DIETS))
        val again = FoodGuideModel(null, prefs, zone = ZoneOffset.UTC) { at("2026-10-13") }
        assertEq(FoodGoal.BuildMuscle, again.goal)
        assertEq(setOf(FoodDiet.NutFree), again.diets)
        assertEq(FoodGoal.EatHealthier, FoodGuideModel(null, zone = ZoneOffset.UTC).goal)
    }

    fun testText() {
        assertEq("1,234,567", FoodText.kcal(1234567))
        assertEq("950", FoodText.kcal(950))
        assertEq("6.1", FoodText.number(6.14))
        assertEq("6", FoodText.number(6.0))
        assertEq("13", FoodText.number(12.6))
        assertEq("2.5 mcg", FoodNutrient.VitaminD.amount(2.5))
    }
}
