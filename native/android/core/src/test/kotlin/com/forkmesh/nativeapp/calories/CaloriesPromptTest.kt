package com.forkmesh.nativeapp.calories

import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.notes.JsonObject
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.time.LocalDate
import kotlin.math.abs

class CaloriesPromptTest {
    private fun food(name: String, portion: String, calories: Number, protein: Number, carbs: Number, fat: Number): JsonObject =
        linkedMapOf("name" to name, "portion" to portion, "calories" to calories, "proteinG" to protein, "carbsG" to carbs, "fatG" to fat)

    private val common = listOf(
        food("Egg", "1 large", 72L, 6.3, 0.4, 4.8),
        food("Toast with butter", "1 slice", 120L, 2.7, 13.0, 6.0),
        food("Banana", "1 medium", 105L, 1.3, 27.0, 0.4),
        food("Glass of wine", "150 ml", 125L, 0.1, 3.8, 0.0),
    )
    private val today = LocalDate.of(2026, 9, 27)

    fun testNormalisesNames() {
        assertEq("egg", CaloriesPrompt.singular("eggs"))
        assertEq("sandwich", CaloriesPrompt.singular("sandwiches"))
        assertEq("glass", CaloriesPrompt.singular("glass"))
        assertEq("toast with butter", CaloriesPrompt.normalise("Toast, with BUTTER!"))
        assertEq("greek yogurt", CaloriesPrompt.normalise("Greek yogurts"))
    }

    fun testMealAndDay() {
        assertEq("breakfast", CaloriesPrompt.meal("2 eggs for Brunch"))
        assertEq("dinner", CaloriesPrompt.meal("pizza at supper"))
        assertEq("", CaloriesPrompt.meal("a cookie"))
        assertEq("2026-09-26", CaloriesPrompt.day("a banana yesterday", "x", today))
        assertEq("2026-09-27", CaloriesPrompt.day("toast this morning", "x", today))
        assertEq("2026-09-20", CaloriesPrompt.day("toast", "2026-09-20", today))
        assertEq("2026-02-28", CaloriesPrompt.day("yesterday", "x", LocalDate.of(2026, 3, 1)))
    }

    fun testSplitsParts() {
        assertEq(listOf("2 eggs", "toast with butter"), CaloriesPrompt.parts("I had 2 eggs and toast with butter for breakfast"))
        assertEq(
            listOf("pizza", "cola", "apple", "banana", "grapes", "beer"),
            CaloriesPrompt.parts("lunch: pizza, cola; apple\nbanana + grapes & beer"),
        )
    }

    fun testMatchesATypedBreakfast() {
        val items = CaloriesPrompt.match("2 eggs and toast with butter for breakfast", listOf(emptyList(), emptyList(), common))!!
        assertEq(2, items.size)
        assertEq("Egg", items[0]["name"])
        assertEq(144L, items[0]["calories"])
        assertEq(12.6, items[0]["proteinG"])
        assertEq("2 × 1 large", items[0]["portion"])
        assertEq("Toast with butter", items[1]["name"])
        assertEq("1 slice", items[1]["portion"])
        assertEq(120L, items[1]["calories"])
    }

    fun testWordedAndFractionalAmounts() {
        for (text in listOf("half a banana", "1/2 banana", "0.5 bananas")) {
            val item = CaloriesPrompt.match(text, listOf(common))!!.single()
            assertEq("Banana", item["name"], text)
            assertEq(53L, item["calories"], text)
            assertEq(13.5, item["carbsG"], text)
            assertEq("0.5 × 1 medium", item["portion"], text)
        }
        assertEq(144L, CaloriesPrompt.match("a couple of eggs", listOf(common))!!.single()["calories"])
        assertEq(72L, CaloriesPrompt.match("an egg", listOf(common))!!.single()["calories"])
        assertEq("3 × 1 large", CaloriesPrompt.match("3x egg", listOf(common))!!.single()["portion"])
    }

    fun testGivesUpOnUnknownOrAbsurd() {
        assertEq(null, CaloriesPrompt.match("2 eggs and a dragonfruit", listOf(common)))
        assertEq(null, CaloriesPrompt.match("dragonfruit", listOf(common)))
        assertEq(null, CaloriesPrompt.match("25 eggs", listOf(common)))
        assertEq(null, CaloriesPrompt.match("1/0 egg", listOf(common)))
        assertEq(null, CaloriesPrompt.match("", listOf(common)))
    }

    fun testPrefersMyFoods() {
        val mine = listOf(food("Banana", "1 small", 90L, 1L, 23L, 0.3) + ("source" to "saved"))
        assertEq(90L, CaloriesPrompt.match("banana", listOf(mine, emptyList(), common))!!.single()["calories"])
    }

    fun testWeighIns() {
        val kg = CaloriesPrompt.weight("weighed 72.5 kg", imperial = true, fallback = "2026-09-20", today = today)!!
        assertEq(72.5, kg.kg)
        assertEq("2026-09-20", kg.day)
        expect(abs(CaloriesPrompt.weight("my weight is 160 lb", false, "d", today)!!.kg - 72.57) < 0.01)
        assertEq(72.5, CaloriesPrompt.weight("weight 72,5", false, "d", today)!!.kg)
        expect(abs(CaloriesPrompt.weight("weighed 160", true, "d", today)!!.kg - 72.57) < 0.01)
        assertEq("2026-09-26", CaloriesPrompt.weight("I weighed in at 80 kg yesterday", false, "d", today)!!.day)
        assertEq(null, CaloriesPrompt.weight("2 eggs", false, "d", today))
        assertEq(null, CaloriesPrompt.weight("weighed 72 kg of potatoes", false, "d", today))
    }

    fun testFrequentFoods() {
        val entries = listOf(
            food("Latte", "12 oz", 190L, 12L, 18L, 7L),
            food("Apple", "1 medium", 95L, 0.5, 25L, 0.3),
            food("latte ", "12 oz", 190L, 12L, 18L, 7L),
            food("Apple", "1 medium", 95L, 0.5, 25L, 0.3),
            food("Soup", "1 bowl", 200L, 8L, 20L, 9L),
            food("Soup", "1 bowl", 200L, 8L, 20L, 9L),
            food("Soup", "1 bowl", 200L, 8L, 20L, 9L),
            food("Pear", "1", 100L, 1L, 27L, 0.2),
            food("Oats", "1 cup", 166L, 5.9, 28L, 3.6),
            food("Oats", "1 cup", 166L, 5.9, 28L, 3.6),
        )
        val frequent = CaloriesPrompt.frequent(entries, listOf(mapOf("name" to "OATS")))
        assertEq(listOf("Soup", "Latte", "Apple"), frequent.map { it["name"] })
        assertEq(listOf(3L, 2L, 2L), frequent.map { it["times"] })
        expect(frequent.first().keys.containsAll(listOf("brand", "portion", "fatG")))
        assertEq("12 oz · 190 kcal · logged 2×", CaloriesPrompt.meta(frequent[1]))
    }

    fun testQuickSectionsAndMyFoods() {
        val mine = listOf(food("Oat milk", "250 ml", 120L, 3L, 16L, 5L) + ("source" to "receipt"))
        val often = listOf(food("Oat milk", "250 ml", 120L, 3L, 16L, 5L), food("Soup", "1 bowl", 200L, 8L, 20L, 9L))
        val sections = CaloriesPrompt.sections(mine, often, common, "")
        assertEq(listOf("My foods", "You log often", "Common foods"), sections.map { it.title })
        assertEq(listOf("Soup"), sections[1].foods.map { it["name"] })
        val filtered = CaloriesPrompt.sections(emptyList(), emptyList(), common, "ban")
        assertEq(listOf("mine", "common"), filtered.map { it.kind })
        assertEq("No match.", filtered[0].empty)
        assertEq(listOf("Banana"), filtered[1].foods.map { it["name"] })
        assertEq("Nothing saved yet. Read a receipt or star a food.", CaloriesPrompt.sections(emptyList(), emptyList(), common, "")[0].empty)
        val added = CaloriesPrompt.withFood(mine, common[2])!!
        assertEq(listOf("Banana", "Oat milk"), added.map { it["name"] })
        assertEq("receipt", added[1]["source"])
        assertEq(null, CaloriesPrompt.withFood(mine, mapOf("name" to "oat MILK")))
        assertEq(emptyList<JsonObject>(), CaloriesPrompt.withoutFood(mine, mine[0]))
        assertEq(null, CaloriesPrompt.withoutFood(mine, common[0]))
    }

    fun testMessages() {
        val items = listOf(mapOf("name" to "Egg", "calories" to 144L), mapOf("name" to "Toast with butter", "calories" to 120L))
        assertEq(
            "Logged to breakfast, today: Egg (144 kcal), Toast with butter (120 kcal). 264 kcal in all. Today: 1574 of 2000 kcal.",
            CaloriesPrompt.loggedMessage(items, "breakfast", "2026-09-27", "2026-09-27", 2000, 1574.0),
        )
        assertEq(
            "Logged to lunch, Sat Sep 26: Egg (144 kcal), Toast with butter (120 kcal). 264 kcal in all.",
            CaloriesPrompt.loggedMessage(items, "lunch", "2026-09-26", "2026-09-27"),
        )
        assertEq("Saved 72.5 kg for today.", CaloriesPrompt.weightSaved("72.5 kg", "2026-09-27", "2026-09-27"))
        assertEq("Logged Egg (72 kcal) to snack.", CaloriesPrompt.quickLogged(common[0], "snack"))
    }

    fun testReceiptsAndErrors() {
        expect(CaloriesPrompt.isReceipt("my grocery receipt", "photo.jpg"))
        expect(CaloriesPrompt.isReceipt("", "Receipt-0412.jpg"))
        expect(!CaloriesPrompt.isReceipt("lunch", "receipt.jpg"))
        expect(!CaloriesPrompt.isReceipt("", "photo.jpg"))
        assertEq(
            "Added 2 foods from Corner Market to My foods: Oat milk, Hummus.",
            CaloriesPrompt.receiptMessage(mapOf("store" to "Corner Market", "added" to listOf("Oat milk", "Hummus"), "found" to listOf("Oat milk", "Hummus", "Bread"))),
        )
        assertEq("Read 1 food; all were already in My foods.", CaloriesPrompt.receiptMessage(mapOf("added" to emptyList<String>(), "found" to listOf("Bread"))))
        expect(CaloriesPrompt.receiptError(OrgApiException("rate_limited", "rate limited")).contains("this hour"))
        expect(CaloriesPrompt.receiptError(OrgApiException("http_422", "")).contains("No foods were found"))
        expect(CaloriesPrompt.receiptError(OrgApiException("ai_unavailable", "")).contains("unavailable on this host"))
        assertEq("Could not read the receipt: bad gateway", CaloriesPrompt.receiptError(OrgApiException("http_502", "bad gateway")))
        expect(CaloriesPrompt.NOTHING_IDENTIFIED.startsWith("Nothing recognisable"))
        expect(CaloriesPrompt.estimateError(OrgApiException("http_429", "")).contains("Quick add"))
        assertEq("Receipt reading needs AI, which is unavailable on this host.", CaloriesPrompt.receiptUnavailable(mapOf("sources" to mapOf("ai" to false))))
        assertEq(null, CaloriesPrompt.receiptUnavailable(mapOf("sources" to mapOf("ai" to true))))
        assertEq(listOf(1600 to 82, 1600 to 65, 1200 to 65, 800 to 60), CaloriesPrompt.jpegLadder(1600))
        assertEq("data:image/jpeg;base64,QUJD", CaloriesPrompt.jpegDataUrl("QUJD"))
        assertEq(null, CaloriesPrompt.jpegDataUrl("A".repeat(CaloriesPrompt.MAX_IMAGE_DATA_URL)))
    }

    fun testSearchAsYouType() {
        val search = FoodSearch()
        val wrap = mapOf(
            "name" to "Buffalo Style Chicken Wrap", "brand" to "Trader Joe's", "servingSize" to "1 wrap",
            "per100g" to mapOf("calories" to 199L), "perServing" to mapOf("calories" to 452L),
        )
        assertEq(FoodSearchStep.Clear, search.changed("t"))
        val step = search.changed("trader  joes buffalo chicken wrap ") as FoodSearchStep.Run
        assertEq("trader joes buffalo chicken wrap", step.query)
        assertEq(FoodSearch.DEBOUNCE_MS, step.delayMs)
        assertEq(null, search.begin(step.query))
        assertEq("Searching…", search.status)
        expect(search.finished(step.query, listOf(wrap)))
        assertEq("1 match · Open Food Facts · tap one to log it.", search.status)
        assertEq(FoodSearchStep.Keep, search.changed("Trader joes buffalo chicken wrap"))
        assertEq(wrap, search.submitted("trader joes buffalo chicken wrap"))
        assertEq("Trader Joe's · 199 kcal / 100 g · 452 kcal per 1 wrap", FoodSearch.subtitle(wrap))
        assertEq(FoodSearchStep.Clear, search.changed("t"))
        assertEq(null, search.submitted("t"))
        assertEq(FoodSearchStep.Run("trader joes buffalo chicken wrap", 0), search.changed("trader joes buffalo chicken wrap"))
        assertEq(listOf(wrap), search.begin("trader joes buffalo chicken wrap"))
        search.changed("oat milk")
        search.begin("oat milk")
        expect(!search.finished("trader joes", emptyList()))
        expect(search.finished("oat milk", emptyList()))
        assertEq("Nothing found for “oat milk” yet. Keep typing, try the brand first, or add it by hand.", search.status)
        expect(search.failed("oat milk"))
        assertEq("Search failed.", search.status)
        assertEq("12 kcal / 100 g", FoodSearch.subtitle(mapOf("per100g" to mapOf("calories" to 12L))))
    }
}
