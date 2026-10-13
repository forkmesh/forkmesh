package com.forkmesh.nativeapp.calories

import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.notes.JsonObject
import com.forkmesh.nativeapp.notes.jsonString
import java.math.BigDecimal
import java.math.RoundingMode
import java.time.LocalDate

data class PromptWeight(val kg: Double, val day: String)

data class QuickSection(val kind: String, val title: String, val foods: List<JsonObject>, val empty: String)

object CaloriesPrompt {
    const val HINT = "What did you eat? “2 eggs and toast for breakfast”, “weighed 72.5 kg”, or attach a meal photo or receipt"
    const val TOOLTIP = "Log this to your Calorie Counter diary"
    const val EMPTY = "Type what you ate, or attach a meal photo or receipt."
    const val MAX_IMAGE_DATA_URL = 1_600_000
    const val MEAL_SIDE = 1024
    const val RECEIPT_SIDE = 1600

    private const val MEAL_PATTERN = "breakfast|brunch|lunch|dinner|supper|snacks?|dessert"
    private val MEAL_WORDS = mapOf(
        "breakfast" to "breakfast", "brunch" to "breakfast", "lunch" to "lunch", "dinner" to "dinner",
        "supper" to "dinner", "snack" to "snack", "snacks" to "snack", "dessert" to "snack",
    )
    private val NUMBER_WORDS = mapOf(
        "a" to 1.0, "an" to 1.0, "one" to 1.0, "two" to 2.0, "three" to 3.0, "four" to 4.0, "five" to 5.0,
        "six" to 6.0, "seven" to 7.0, "eight" to 8.0, "nine" to 9.0, "ten" to 10.0, "half" to 0.5,
        "a half" to 0.5, "couple" to 2.0, "a couple of" to 2.0, "a few" to 3.0,
    )
    private val IGNORE = RegexOption.IGNORE_CASE
    private val WEIGHT = Regex(
        "^(?:i\\s+)?(?:weigh(?:ed|t)?(?:\\s+in)?|my weight(?:\\s+is)?|weight(?:\\s+is)?)" +
            "\\s*(?:at|is|was|:|=)?\\s*(\\d{2,3}(?:[.,]\\d+)?)\\s*" +
            "(kg|kgs|kilos?|kilograms?|lb|lbs|pounds?)?\\s*" +
            "(?:today|yesterday|this morning)?\\.?$",
        IGNORE,
    )
    private val NUMERIC = Regex("^(\\d+/\\d+|\\d+(?:[.,]\\d+)?)\\s*(?:x\\s+)?")
    private val WORDED = Regex("^(a couple of|a few|a half|an|a|one|two|three|four|five|six|seven|eight|nine|ten|half|couple)\\s+")
    private val FILLER = Regex("^(?:(?:of|some|an?)\\s+)+")

    fun foodKey(name: Any?): String = jsonString(name).trim().lowercase()

    fun singular(word: String): String =
        word.replaceFirst(Regex("(ch|sh|ss|x)es$"), "$1").replaceFirst(Regex("([^s])s$"), "$1")

    fun normalise(name: Any?): String = jsonString(name).lowercase()
        .replace(Regex("[^a-z0-9 ]+"), " ")
        .split(Regex("\\s+"))
        .filter { it.isNotEmpty() }
        .joinToString(" ") { singular(it) }

    fun meal(text: String): String {
        val found = Regex("\\b($MEAL_PATTERN)\\b", IGNORE).find(text) ?: return ""
        return MEAL_WORDS[found.groupValues[1].lowercase()] ?: ""
    }

    fun day(text: String, fallback: String, today: LocalDate): String = when {
        Regex("\\byesterday\\b", IGNORE).containsMatchIn(text) -> today.minusDays(1).toString()
        Regex("\\btoday\\b|\\bthis (morning|afternoon|evening)\\b|\\btonight\\b", IGNORE).containsMatchIn(text) -> today.toString()
        else -> fallback
    }

    fun weight(text: String, imperial: Boolean, fallback: String, today: LocalDate): PromptWeight? {
        val found = WEIGHT.find(text.trim()) ?: return null
        val value = found.groupValues[1].replace(',', '.').toDoubleOrNull() ?: return null
        val unit = found.groupValues[2].lowercase()
        val kg = when {
            unit.startsWith("k") -> value
            unit.isNotEmpty() || imperial -> value * Calories.KG_PER_LB
            else -> value
        }
        if (!(kg > 0)) return null
        return PromptWeight(kg, day(text, fallback, today))
    }

    fun parts(text: String): List<String> {
        val cleaned = text.lowercase()
            .replace(Regex("\\b(for|at|with)\\s+($MEAL_PATTERN)\\b"), " ")
            .replace(Regex("\\b($MEAL_PATTERN)\\s*:"), " ")
            .replace(Regex("\\b(yesterday|today|tonight|this (morning|afternoon|evening))\\b"), " ")
            .replaceFirst(Regex("^\\s*(i\\s+)?(just\\s+)?(had|ate|drank|log|add)\\b"), " ")
        return cleaned.split(Regex(",|;|\\n|\\+|&|\\band\\b")).map { it.trim() }.filter { it.isNotEmpty() }
    }

    fun amount(digits: String): Double? {
        if (digits.contains('/')) {
            val halves = digits.split('/')
            val top = halves[0].toDoubleOrNull() ?: return null
            val bottom = halves[1].toDoubleOrNull() ?: return null
            return top / bottom
        }
        return digits.replace(',', '.').toDoubleOrNull()
    }

    private fun num(value: Any?): Double = Calories.num(value)

    private fun tenths(value: Double): Double = BigDecimal(value).setScale(1, RoundingMode.HALF_UP).toDouble()

    private fun shown(value: Double): String =
        if (value == Math.rint(value)) value.toLong().toString() else BigDecimal(value).setScale(1, RoundingMode.HALF_UP).toPlainString()

    fun match(text: String, lists: List<List<JsonObject>>): List<JsonObject>? {
        val index = LinkedHashMap<String, JsonObject>()
        for (list in lists) {
            for (food in list) {
                val key = normalise(food["name"])
                if (key.isNotEmpty() && key !in index) index[key] = food
            }
        }
        val matched = mutableListOf<JsonObject>()
        for (part in parts(text)) {
            var rest = part
            var amount: Double? = 1.0
            val counted = NUMERIC.find(rest)
            val word = WORDED.find(rest)
            if (counted != null) {
                amount = amount(counted.groupValues[1])
                rest = rest.substring(counted.value.length)
            } else if (word != null) {
                amount = NUMBER_WORDS[word.groupValues[1]]
                rest = rest.substring(word.value.length)
            }
            rest = rest.replaceFirst(FILLER, "")
            val food = index[normalise(rest)]
            if (food == null || amount == null || !(amount > 0) || amount > 20) return null
            val portion = jsonString(food["portion"])
            matched.add(
                linkedMapOf(
                    "name" to jsonString(food["name"]),
                    "brand" to jsonString(food["brand"]),
                    "portion" to if (amount == 1.0) portion else "${shown(amount)} × ${portion.ifEmpty { "serving" }}",
                    "calories" to Math.round(num(food["calories"]) * amount),
                    "proteinG" to tenths(num(food["proteinG"]) * amount),
                    "carbsG" to tenths(num(food["carbsG"]) * amount),
                    "fatG" to tenths(num(food["fatG"]) * amount),
                ),
            )
        }
        return matched.ifEmpty { null }
    }

    fun quickFoodFields(food: JsonObject): JsonObject = linkedMapOf(
        "name" to jsonString(food["name"]),
        "brand" to jsonString(food["brand"]),
        "portion" to jsonString(food["portion"]),
        "calories" to (food["calories"] ?: 0L),
        "proteinG" to (food["proteinG"] ?: 0L),
        "carbsG" to (food["carbsG"] ?: 0L),
        "fatG" to (food["fatG"] ?: 0L),
    )

    fun savedFields(food: JsonObject): JsonObject {
        val out = LinkedHashMap(quickFoodFields(food))
        if (food["source"] == "receipt") out["source"] = "receipt"
        return out
    }

    fun frequent(entries: List<JsonObject>, mine: List<JsonObject>): List<JsonObject> {
        val known = mine.map { foodKey(it["name"]) }.toSet()
        val order = mutableListOf<String>()
        val counts = HashMap<String, Int>()
        val foods = HashMap<String, JsonObject>()
        for (entry in entries) {
            val key = foodKey(entry["name"])
            if (key.isEmpty() || key in known) continue
            val seen = counts[key]
            if (seen != null) {
                counts[key] = seen + 1
                continue
            }
            order.add(key)
            counts[key] = 1
            foods[key] = quickFoodFields(entry)
        }
        return order.withIndex()
            .filter { (counts[it.value] ?: 0) > 1 }
            .sortedWith(compareByDescending<IndexedValue<String>> { counts[it.value] ?: 0 }.thenBy { it.index })
            .take(8)
            .map { LinkedHashMap(foods.getValue(it.value)).apply { put("times", (counts[it.value] ?: 0).toLong()) } }
    }

    fun isMine(food: JsonObject, mine: List<JsonObject>): Boolean {
        val key = foodKey(food["name"])
        return mine.any { foodKey(it["name"]) == key }
    }

    fun filter(foods: List<JsonObject>, query: String): List<JsonObject> {
        val q = foodKey(query)
        if (q.isEmpty()) return foods
        return foods.filter { foodKey(it["name"]).contains(q) || foodKey(it["brand"]).contains(q) }
    }

    fun sections(mine: List<JsonObject>, frequent: List<JsonObject>, common: List<JsonObject>, query: String): List<QuickSection> {
        val filtering = foodKey(query).isNotEmpty()
        val often = frequent.filter { !isMine(it, mine) }
        val out = mutableListOf(
            QuickSection("mine", "My foods", filter(mine, query), if (filtering) "No match." else "Nothing saved yet. Read a receipt or star a food."),
        )
        if (often.isNotEmpty()) out.add(QuickSection("frequent", "You log often", filter(often, query), "No match."))
        out.add(QuickSection("common", "Common foods", filter(common, query), "No match."))
        return out
    }

    fun meta(food: JsonObject): String {
        val times = jsonString(food["times"]).toLongOrNull() ?: 0L
        return listOf(
            jsonString(food["brand"]),
            jsonString(food["portion"]),
            "${Calories.kcal(food["calories"])} kcal",
            if (times > 0) "logged $times×" else "",
        ).filter { it.isNotEmpty() }.joinToString(" · ")
    }

    fun withFood(mine: List<JsonObject>, food: JsonObject): List<JsonObject>? {
        if (isMine(food, mine)) return null
        return listOf(quickFoodFields(food)) + mine.map(::savedFields)
    }

    fun withoutFood(mine: List<JsonObject>, food: JsonObject): List<JsonObject>? {
        if (!isMine(food, mine)) return null
        val key = foodKey(food["name"])
        return mine.filter { foodKey(it["name"]) != key }.map(::savedFields)
    }

    fun loggedMessage(items: List<JsonObject>, meal: String, day: String, today: String, goal: Long = 0, todayCalories: Double = 0.0): String {
        val total = items.sumOf { num(it["calories"]) }
        val onToday = day == today
        val listed = items.joinToString(", ") { "${jsonString(it["name"])} (${Calories.kcal(it["calories"])} kcal)" }
        val progress = if (onToday && goal > 0) " Today: ${Calories.kcal(todayCalories)} of $goal kcal." else ""
        return "Logged to $meal, ${if (onToday) "today" else Calories.dayLabel(day)}: $listed. ${Calories.kcal(total)} kcal in all.$progress"
    }

    fun quickLogged(food: JsonObject, meal: String): String =
        "Logged ${jsonString(food["name"])} (${Calories.kcal(food["calories"])} kcal) to $meal."

    fun weightSaved(shown: String, day: String, today: String): String =
        "Saved $shown for ${if (day == today) "today" else Calories.dayLabel(day)}."

    fun isReceipt(text: String, fileName: String): Boolean =
        Regex("\\b(receipt|grocer(y|ies)|shopping)\\b", IGNORE).containsMatchIn(text) ||
            (text.trim().isEmpty() && fileName.lowercase().contains("receipt"))

    fun receiptMessage(payload: JsonObject): String {
        val added = (payload["added"] as? List<*>)?.map { jsonString(it) } ?: emptyList()
        val found = (payload["found"] as? List<*>)?.map { jsonString(it) } ?: emptyList()
        val store = jsonString(payload["store"]).trim()
        val from = if (store.isEmpty()) "" else " from $store"
        return if (added.isNotEmpty()) {
            "Added ${added.size} food${if (added.size == 1) "" else "s"}$from to My foods: ${added.joinToString(", ")}."
        } else {
            "Read ${found.size} food${if (found.size == 1) "" else "s"}$from; all were already in My foods."
        }
    }

    private fun code(error: Throwable): String = (error as? OrgApiException)?.code ?: ""

    fun reason(error: Throwable): String = (error as? OrgApiException)?.message ?: "Calorie Counter is unavailable right now."

    fun receiptError(error: Throwable): String = when (code(error)) {
        "rate_limited", "http_429" -> "You have used this hour's AI reads. Try the receipt again later."
        "no_foods_on_receipt", "http_422" -> "No foods were found on that receipt. Try a sharper, flatter photo."
        "ai_unavailable", "http_503" -> "Receipt reading needs AI, which is unavailable on this host."
        else -> "Could not read the receipt: ${reason(error)}"
    }

    fun estimateError(error: Throwable): String = when (code(error)) {
        "rate_limited", "http_429" -> "You have used this hour's AI estimates. Try again later, or use Quick add."
        "ai_unavailable", "http_503" -> "AI estimates are unavailable on this host. Use foods from Quick add, like “2 eggs and a banana”."
        "nothing_identified", "http_422" -> "Nothing recognisable as food there. Try naming the food and amount."
        else -> "Could not estimate: ${reason(error)}"
    }

    val NOTHING_IDENTIFIED: String get() = estimateError(OrgApiException("nothing_identified", ""))

    fun receiptUnavailable(overview: JsonObject?): String? {
        val sources = overview?.get("sources") as? Map<*, *> ?: return null
        return if (sources["ai"] == false) "Receipt reading needs AI, which is unavailable on this host." else null
    }

    fun jpegLadder(maxSide: Int): List<Pair<Int, Int>> =
        listOf(maxSide to 82, maxSide to 65, maxSide * 3 / 4 to 65, maxSide / 2 to 60)

    fun jpegDataUrl(base64: String): String? {
        val url = "data:image/jpeg;base64,$base64"
        return if (url.length <= MAX_IMAGE_DATA_URL) url else null
    }

    fun clip(text: String, max: Int): String = if (text.length > max) text.substring(0, max) else text
}

sealed class FoodSearchStep {
    data object Clear : FoodSearchStep()
    data object Keep : FoodSearchStep()
    data class Run(val query: String, val delayMs: Long) : FoodSearchStep()
}

class FoodSearch {
    private val cache = HashMap<String, List<JsonObject>>()
    var searched: String = ""
        private set
    var results: List<JsonObject> = emptyList()
        private set
    var status: String = START
        private set

    fun typed(raw: String): String = raw.trim().replace(Regex("\\s+"), " ")

    fun changed(raw: String): FoodSearchStep {
        val query = typed(raw)
        if (query.length < 2) {
            searched = ""
            results = emptyList()
            return FoodSearchStep.Clear
        }
        if (query.lowercase() == searched) return FoodSearchStep.Keep
        return FoodSearchStep.Run(query, if (cache.containsKey(query.lowercase())) 0 else DEBOUNCE_MS)
    }

    fun submitted(raw: String): JsonObject? {
        val query = typed(raw)
        return if (results.isNotEmpty() && query.lowercase() == searched) results.first() else null
    }

    fun begin(query: String): List<JsonObject>? {
        val key = query.lowercase()
        searched = key
        val cached = cache[key]
        if (cached != null) {
            show(query, cached)
            return cached
        }
        status = "Searching…"
        return null
    }

    fun finished(query: String, found: List<JsonObject>): Boolean {
        val key = query.lowercase()
        cache[key] = found
        if (searched != key) return false
        show(query, found)
        return true
    }

    fun failed(query: String): Boolean {
        if (searched != query.lowercase()) return false
        searched = ""
        status = "Search failed."
        return true
    }

    private fun show(query: String, found: List<JsonObject>) {
        results = found
        status = if (found.isEmpty()) {
            "Nothing found for “$query” yet. Keep typing, try the brand first, or add it by hand."
        } else {
            "${found.size} ${if (found.size == 1) "match" else "matches"} · Open Food Facts · tap one to log it."
        }
    }

    companion object {
        const val DEBOUNCE_MS = 300L
        const val TITLE = "Search foods"
        const val HINT = "trader joes buffalo chicken wrap…"
        const val START = "Start typing a food or brand, like trader joes buffalo chicken wrap."

        fun subtitle(food: JsonObject): String {
            val parts = mutableListOf(jsonString(food["brand"]), "${Calories.kcal((food["per100g"] as? Map<*, *>)?.get("calories"))} kcal / 100 g")
            val serving = food["perServing"] as? Map<*, *>
            if (serving != null) {
                val size = jsonString(food["servingSize"])
                parts.add("${Calories.kcal(serving["calories"])} kcal per ${size.ifEmpty { "serving" }}")
            }
            return parts.filter { it.isNotEmpty() }.joinToString(" · ")
        }
    }
}
