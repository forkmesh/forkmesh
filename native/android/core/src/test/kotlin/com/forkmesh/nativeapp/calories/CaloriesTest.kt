package com.forkmesh.nativeapp.calories

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class CaloriesTest {
    fun testFormatting() {
        assertEq("Thu Sep 24", Calories.dayLabel("2026-09-24"))
        assertEq("12", Calories.grams(12.0))
        assertEq("12.5", Calories.grams(12.46))
        assertEq("13", Calories.kcal(12.5))
        assertEq("breakfast", Calories.guessMeal(7))
        assertEq("lunch", Calories.guessMeal(11))
        assertEq("dinner", Calories.guessMeal(20))
        assertEq("snack", Calories.guessMeal(22))
        assertEq(listOf("maya", "sam"), Calories.usernames(" maya, @Sam  maya"))
        assertEq("123456", Calories.barcodeDigits("12-34 56"))
        assertEq(null, Calories.barcodeDigits("12345"))
        assertEq("07:05", Calories.reminderTime(7, 5))
        assertEq("22.9", Calories.bmi(70.0, 175.0))
    }

    fun testDiaryOrderAndTotals() {
        val entries = listOf(
            mapOf("day" to "2026-09-23", "meal" to "dinner", "calories" to 500L, "loggedAt" to 3L),
            mapOf("day" to "2026-09-24", "meal" to "lunch", "calories" to 300L, "loggedAt" to 2L),
            mapOf("day" to "2026-09-24", "meal" to "breakfast", "calories" to 200.5, "loggedAt" to 9L),
        )
        val sorted = Calories.sortedEntries(entries)
        assertEq(listOf("breakfast", "lunch", "dinner"), sorted.map { it["meal"] })
        assertEq(mapOf("2026-09-24" to 500.5, "2026-09-23" to 500.0), Calories.dayTotals(sorted))
        assertEq("Diary · 3 entries over 2 days", Calories.diaryHeader(3, 2))
        assertEq("200 kcal left today", Calories.remainingText(2000, 1800.0))
        assertEq("100 kcal over goal", Calories.remainingText(2000, 2100.0))
        assertEq("Set a daily goal on the Goals tab.", Calories.remainingText(0, 10.0))
        assertEq(1.0, Calories.ringRatio(100, 250.0))
        assertEq("Lunch · 1 bowl · P 10 · C 2.5 · F 0", Calories.entrySubtitle(mapOf("meal" to "lunch", "portion" to "1 bowl", "brand" to "", "proteinG" to 10L, "carbsG" to 2.5, "fatG" to 0L)))
    }

    fun testPortionMath() {
        val product = mapOf(
            "name" to "Yogurt",
            "per100g" to mapOf("calories" to 60L, "proteinG" to 10.0, "carbsG" to 3.6, "fatG" to 0.4),
            "perServing" to mapOf("calories" to 90L, "proteinG" to 15.0, "carbsG" to 5.4, "fatG" to 0.6),
            "servingSize" to "150 g",
        )
        val serving = Calories.portionValues(product, "serving", "2", "snack")
        assertEq("2 serving(s) (150 g)", serving["portion"])
        assertEq(180L, serving["calories"])
        assertEq(30.0, serving["proteinG"])
        val grams = Calories.portionValues(product, "1g", "250", "lunch")
        assertEq("250 g", grams["portion"])
        assertEq(150L, grams["calories"])
        assertEq(9.0, grams["carbsG"])
        assertEq("100 g", Calories.portionValues(product, "100g", "0", "lunch")["portion"])
        assertEq("Open Food Facts", Calories.productSource(mapOf("brand" to "", "source" to "off")))
    }

    fun testChartsAndChallenges() {
        val chart = Calories.dayChart(listOf(mapOf("calories" to 2500L, "entries" to 2L), mapOf("calories" to 0L, "entries" to 0L)), 2000)
        assertEq(2500.0, chart.peak)
        assertEq(1, chart.bars.size)
        expect(chart.bars[0].over)
        assertEq(0.8, chart.goalRatio)
        val weights = Calories.weightChart(listOf(mapOf("day" to "2026-09-01", "weightKg" to 80.0), mapOf("day" to "2026-09-11", "weightKg" to 79.0)), 75.0, false)!!
        assertEq(74.0, weights.low)
        assertEq(81.0, weights.high)
        assertEq(1.0, weights.points.last().first)
        assertEq("Challenge created. Invited maya. Skipped sam.", Calories.inviteNote("Challenge created.", mapOf("invited" to listOf("maya"), "skipped" to listOf("sam"))))
        assertEq(2.0, Calories.boardMax(listOf(mapOf("measured" to true, "score" to -2.0), mapOf("measured" to false, "score" to 9.0))))
    }

    fun testGoalsDraftConvertsUnits() {
        val draft = GoalsDraft(mapOf("units" to "imperial", "unitsPreference" to "auto", "autoUnits" to "imperial", "heightCm" to 177.8, "targetWeightKg" to 45.359237, "dailyCalories" to 1800L))
        assertEq("70", draft.height)
        assertEq("100", draft.target)
        assertEq("Automatic (lb, in)", draft.autoLabel)
        draft.setUnits("metric")
        expect(!draft.imperial)
        draft.setUnits("auto")
        expect(draft.imperial)
        repeat(8) { draft.addReminder(12, 30) }
        expect(!draft.addReminder(1, 0))
        val body = draft.body(-120)
        assertEq(1800L, body["dailyCalories"])
        assertEq(true, kotlin.math.abs((body["heightCm"] as Double) - 177.8) < 1e-9)
        assertEq(-120L, body["tzOffsetMinutes"])
    }
}
