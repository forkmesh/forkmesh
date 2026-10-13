package com.forkmesh.nativeapp.calories

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.notes.JsonObject
import com.forkmesh.nativeapp.notes.jsonObject
import com.forkmesh.nativeapp.notes.jsonObjects
import com.forkmesh.nativeapp.notes.jsonString
import java.math.BigDecimal
import java.math.RoundingMode
import java.net.URLEncoder
import java.time.DayOfWeek
import java.time.LocalDate
import kotlin.math.abs
import kotlin.math.floor
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow

object Calories {
    const val KG_PER_LB = 0.45359237
    const val CM_PER_IN = 2.54
    val MEALS = listOf("breakfast", "lunch", "dinner", "snack")
    val KIND_LABELS = linkedMapOf(
        "weight_loss_percent" to "Lose the most percent of body weight",
        "weight_loss_kg" to "Lose the most weight",
        "calorie_streak" to "Most days within the calorie goal",
        "log_streak" to "Most days logged",
    )
    private val MONTHS = listOf("Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")
    private val WEEKDAYS = listOf("Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun")

    fun isoDay(date: LocalDate): String = date.toString()

    fun parseDay(day: String): LocalDate = try {
        LocalDate.parse(day)
    } catch (_: Exception) {
        LocalDate.now()
    }

    fun dayLabel(day: String): String {
        val date = parseDay(day)
        return "${WEEKDAYS[date.dayOfWeek.value - DayOfWeek.MONDAY.value]} ${MONTHS[date.monthValue - 1]} ${date.dayOfMonth}"
    }

    fun num(value: Any?): Double = when (value) {
        is Number -> value.toDouble()
        is String -> value.trim().toDoubleOrNull() ?: 0.0
        null -> 0.0
        else -> value.toString().toDoubleOrNull() ?: 0.0
    }

    fun int(value: Any?): Long {
        val n = num(value)
        return if (n.isNaN()) 0 else BigDecimal(n).setScale(0, RoundingMode.HALF_UP).toLong()
    }

    fun kcal(value: Any?): String = int(value).toString()

    fun fixed1(value: Double): String = BigDecimal(value).setScale(1, RoundingMode.HALF_UP).toPlainString()

    fun grams(value: Any?): String {
        val n = num(value)
        return if (n == floor(n)) int(n).toString() else fixed1(n)
    }

    fun guessMeal(hour: Int): String = when {
        hour < 11 -> "breakfast"
        hour < 15 -> "lunch"
        hour < 21 -> "dinner"
        else -> "snack"
    }

    fun cap(value: String): String = if (value.isEmpty()) value else value[0].uppercase() + value.substring(1)

    fun usernames(text: String): List<String> = text.split(Regex("[\\s,]+"))
        .map { it.replace(Regex("^@+"), "").lowercase() }
        .filter { it.isNotEmpty() }
        .distinct()

    fun sortedEntries(entries: List<JsonObject>): List<JsonObject> = entries.sortedWith { a, b ->
        val day = jsonString(b["day"], "null").compareTo(jsonString(a["day"], "null"))
        if (day != 0) return@sortedWith day
        val meal = MEALS.indexOf(jsonString(a["meal"], "null")).compareTo(MEALS.indexOf(jsonString(b["meal"], "null")))
        if (meal != 0) meal else int(a["loggedAt"]).compareTo(int(b["loggedAt"]))
    }

    fun dayTotals(entries: List<JsonObject>): LinkedHashMap<String, Double> {
        val out = LinkedHashMap<String, Double>()
        for (entry in entries) {
            val day = jsonString(entry["day"], "null")
            out[day] = (out[day] ?: 0.0) + num(entry["calories"])
        }
        return out
    }

    fun remainingText(goal: Long, calories: Double): String = when {
        goal <= 0 -> "Set a daily goal on the Goals tab."
        calories <= goal -> "${kcal(goal - calories)} kcal left today"
        else -> "${kcal(calories - goal)} kcal over goal"
    }

    fun ringRatio(goal: Long, calories: Double): Double = if (goal > 0) (calories / goal).coerceIn(0.0, 1.0) else 0.0

    fun entrySubtitle(entry: JsonObject): String = listOf(
        cap(jsonString(entry["meal"])),
        jsonString(entry["portion"]),
        jsonString(entry["brand"]),
        "P ${grams(entry["proteinG"])} · C ${grams(entry["carbsG"])} · F ${grams(entry["fatG"])}",
    ).filter { it.isNotEmpty() }.joinToString(" · ")

    fun diaryHeader(entries: Int, days: Int): String =
        if (entries == 0) "Diary" else "Diary · $entries entries over $days day${if (days == 1) "" else "s"}"

    fun inviteNote(lead: String, result: JsonObject): String {
        fun names(value: Any?): List<String> = (value as? List<*>)?.map { jsonString(it, "null") } ?: emptyList()
        val parts = mutableListOf(lead)
        val invited = names(result["invited"])
        val skipped = names(result["skipped"])
        if (invited.isNotEmpty()) parts.add("Invited ${invited.joinToString(", ")}.")
        if (skipped.isNotEmpty()) parts.add("Skipped ${skipped.joinToString(", ")}.")
        return parts.joinToString(" ")
    }

    fun challengeLine(item: JsonObject): String {
        val invited = int(item["invited"])
        return "${dayLabel(jsonString(item["startsOn"]))} → ${dayLabel(jsonString(item["endsOn"]))} · ${jsonString(item["participants"], "null")} in" +
            (if (invited > 0) " · ${jsonString(item["invited"])} invited" else "") +
            " · by ${jsonString(item["createdBy"], "null")}"
    }

    fun boardSummary(challenge: JsonObject): String {
        val winner = jsonString(challenge["winner"])
        return "${KIND_LABELS[jsonString(challenge["kind"])] ?: jsonString(challenge["kind"], "null")} · " +
            "${dayLabel(jsonString(challenge["startsOn"]))} → ${dayLabel(jsonString(challenge["endsOn"]))} · " +
            "started by ${jsonString(challenge["createdBy"], "null")}" +
            (if (winner.isEmpty()) "" else " · winner: $winner")
    }

    fun boardMax(board: List<JsonObject>): Double {
        val scores = board.filter { it["measured"] == true }.map { abs(num(it["score"])) }
        return if (scores.isEmpty()) 1.0 else max(0.0001, scores.max())
    }

    fun portionValues(product: JsonObject, basis: String, amountText: String, meal: String): LinkedHashMap<String, Any?> {
        val per = jsonObject(if (basis == "serving") product["perServing"] else product["per100g"])
        val amount = if (num(amountText) <= 0) 1.0 else num(amountText)
        val factor = if (basis == "1g") amount / 100 else amount
        val serving = jsonString(product["servingSize"])
        val portion = if (basis == "serving") {
            "${grams(amount)} serving(s)${if (serving.isEmpty()) "" else " ($serving)"}"
        } else {
            "${grams(if (basis == "1g") amount else amount * 100)} g"
        }
        return linkedMapOf(
            "name" to product["name"],
            "brand" to product["brand"],
            "barcode" to product["barcode"],
            "portion" to portion,
            "calories" to int(num(per["calories"]) * factor),
            "proteinG" to fixed1(num(per["proteinG"]) * factor).toDouble(),
            "carbsG" to fixed1(num(per["carbsG"]) * factor).toDouble(),
            "fatG" to fixed1(num(per["fatG"]) * factor).toDouble(),
            "meal" to meal,
        )
    }

    fun productSource(product: JsonObject): String = listOf(
        jsonString(product["brand"]),
        jsonString(product["quantity"]),
        if (product["source"] == "usda") "USDA FoodData Central" else "Open Food Facts",
    ).filter { it.isNotEmpty() }.joinToString(" · ")

    fun barcodeDigits(text: String): String? {
        val digits = text.replace(Regex("\\D"), "")
        return if (digits.length in 6..14) digits else null
    }

    fun bmi(latestWeightKg: Double, heightCm: Double): String? {
        if (latestWeightKg <= 0 || heightCm <= 0) return null
        return fixed1(latestWeightKg / (heightCm / 100).pow(2))
    }

    fun reminderTime(hour: Int, minute: Int): String = "${hour.toString().padStart(2, '0')}:${minute.toString().padStart(2, '0')}"

    fun errorText(error: Throwable): String = (error as? OrgApiException)?.message ?: "Calorie Counter is unavailable right now."

    data class Bar(val index: Int, val heightRatio: Double, val over: Boolean)

    data class DayChart(val peak: Double, val bars: List<Bar>, val goalRatio: Double?)

    fun dayChart(days: List<JsonObject>, goal: Long): DayChart {
        var peak = max(500.0, goal * 1.15)
        for (day in days) peak = max(peak, num(day["calories"]))
        val bars = days.mapIndexedNotNull { index, day ->
            if (int(day["entries"]) == 0L) return@mapIndexedNotNull null
            val calories = num(day["calories"])
            Bar(index, calories / peak, goal > 0 && calories > goal)
        }
        return DayChart(peak, bars, if (goal > 0) goal / peak else null)
    }

    data class WeightChart(val low: Double, val high: Double, val points: List<Pair<Double, Double>>, val targetRatio: Double?)

    fun weightChart(points: List<JsonObject>, targetKg: Double, imperial: Boolean): WeightChart? {
        if (points.isEmpty()) return null
        fun display(kg: Double) = if (imperial) kg / KG_PER_LB else kg
        var low = Double.POSITIVE_INFINITY
        var high = Double.NEGATIVE_INFINITY
        for (point in points) {
            val value = display(num(point["weightKg"]))
            low = min(low, value)
            high = max(high, value)
        }
        if (targetKg > 0) {
            low = min(low, display(targetKg))
            high = max(high, display(targetKg))
        }
        low = floor(low - 1)
        high = kotlin.math.ceil(high + 1)
        val first = parseDay(jsonString(points.first()["day"])).toEpochDay()
        val last = parseDay(jsonString(points.last()["day"])).toEpochDay()
        val span = max(1L, last - first).toDouble()
        val xy = points.map { point ->
            val x = (parseDay(jsonString(point["day"])).toEpochDay() - first) / span
            val y = (display(num(point["weightKg"])) - low) / (high - low)
            x to y
        }
        return WeightChart(low, high, xy, if (targetKg > 0) (display(targetKg) - low) / (high - low) else null)
    }
}

class CaloriesApi(private val api: ApiService) {
    fun call(method: String, suffix: String, body: JsonObject? = null): JsonObject =
        api.accountApiCall(method, "/api/calories$suffix", body)

    fun overview(day: String): JsonObject = call("GET", "?day=$day")

    fun install(tzOffsetMinutes: Int) {
        call("POST", "/install", mapOf("tzOffsetMinutes" to tzOffsetMinutes.toLong()))
    }

    fun searchFoods(query: String): List<JsonObject> =
        jsonObjects(call("GET", "/foods?q=${URLEncoder.encode(query, "UTF-8")}")["foods"])
}

class GoalsDraft(profile: JsonObject) {
    private val autoUnits = profile["autoUnits"]
    private val units = profile["units"]
    var unitsPreference: String = when (val stored = profile["unitsPreference"]) {
        "auto", "metric", "imperial" -> stored as String
        else -> if (units == "imperial") "imperial" else "metric"
    }
        private set
    var imperial: Boolean = units == "imperial"
        private set

    private val autoImperial: Boolean
        get() = if (autoUnits == null || autoUnits == "") units == "imperial" else autoUnits == "imperial"

    val autoLabel: String
        get() = when (autoUnits) {
            "imperial" -> "Automatic (lb, in)"
            "metric" -> "Automatic (kg, cm)"
            else -> "Automatic (match my location)"
        }

    var calories = Calories.kcal(profile["dailyCalories"])
    var protein = Calories.kcal(profile["proteinG"])
    var carbs = Calories.kcal(profile["carbsG"])
    var fat = Calories.kcal(profile["fatG"])
    var target = weight(profile["targetWeightKg"])
    var start = weight(profile["startWeightKg"])
    var height = if (Calories.num(profile["heightCm"]) > 0) {
        Calories.grams(if (imperial) Calories.num(profile["heightCm"]) / Calories.CM_PER_IN else profile["heightCm"])
    } else {
        ""
    }
    val reminders: MutableList<MutableMap<String, Any?>> = jsonObjects(profile["reminders"]).map { LinkedHashMap(it) }.toMutableList()

    private fun weight(kg: Any?): String =
        if (Calories.num(kg) > 0) Calories.grams(if (imperial) Calories.num(kg) / Calories.KG_PER_LB else kg) else ""

    fun setUnits(preference: String) {
        unitsPreference = preference
        imperial = if (preference == "auto") autoImperial else preference == "imperial"
    }

    val heightCm: Double get() = Calories.num(height) * (if (imperial) Calories.CM_PER_IN else 1.0)

    fun addReminder(hour: Int, minute: Int): Boolean {
        if (reminders.size >= 8) return false
        reminders.add(
            linkedMapOf("time" to Calories.reminderTime(hour, minute), "label" to "Time to log your meal", "onlyIfUnlogged" to false, "enabled" to true),
        )
        return true
    }

    fun body(tzOffsetMinutes: Int): JsonObject {
        val wf = if (imperial) Calories.KG_PER_LB else 1.0
        return linkedMapOf(
            "dailyCalories" to Calories.int(calories),
            "proteinG" to Calories.int(protein),
            "carbsG" to Calories.int(carbs),
            "fatG" to Calories.int(fat),
            "unitsPreference" to unitsPreference,
            "targetWeightKg" to Calories.num(target) * wf,
            "startWeightKg" to Calories.num(start) * wf,
            "heightCm" to heightCm,
            "tzOffsetMinutes" to tzOffsetMinutes.toLong(),
            "reminders" to reminders,
        )
    }
}
