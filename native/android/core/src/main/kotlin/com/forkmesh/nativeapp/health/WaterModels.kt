package com.forkmesh.nativeapp.health

import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.model.OrgApiException
import java.math.BigDecimal
import java.math.RoundingMode
import java.time.Instant
import java.time.ZoneId
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

data class HealthNotice(val message: String, val isError: Boolean = false)

object HealthClock {
    private val months = listOf("Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")
    private val weekdays = listOf("Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun")

    fun time(instant: Instant, zone: ZoneId): String {
        val local = instant.atZone(zone)
        val hour = local.hour
        val minute = local.minute
        val twelve = if (hour % 12 == 0) 12 else hour % 12
        return "$twelve:${if (minute < 10) "0" else ""}$minute ${if (hour < 12) "AM" else "PM"}"
    }

    fun shortDay(day: String): String {
        val parts = day.split("-")
        if (parts.size != 3) return day
        val month = parts[1].toIntOrNull() ?: return day
        val date = parts[2].toIntOrNull() ?: return day
        return "$month/$date"
    }

    fun dayLabel(day: String): String {
        val date = HealthDates.parseDay(day) ?: return day
        return "${weekdays[date.dayOfWeek.value - 1]} ${months[date.monthValue - 1]} ${date.dayOfMonth}"
    }

    fun tzOffsetMinutes(zone: ZoneId, now: Instant): Int = -(zone.rules.getOffset(now).totalSeconds / 60)

    fun fixed(value: Double, digits: Int): String = BigDecimal(value).setScale(digits, RoundingMode.HALF_UP).toPlainString()

    fun parseNumber(text: String): Double = text.trim().replace(",", ".").toDoubleOrNull() ?: 0.0
}

data class WaterEntryRow(
    val id: String,
    val title: String,
    val subtitle: String,
    val amount: String,
    val fromHealth: Boolean,
)

data class WaterPreset(val milliliters: Int, val label: String)

data class WaterBar(
    val id: String,
    val label: String,
    val ratio: Double,
    val metGoal: Boolean,
    val caption: String,
)

class WaterModel(
    private val call: HealthCall,
    private val prefs: PrefStore? = null,
    val zone: ZoneId = ZoneId.systemDefault(),
    private val now: () -> Instant = { Instant.now() },
) {
    companion object {
        const val TITLE = "Water Intake"
        const val INSTALL_TITLE = "Track what you drink"
        const val INSTALL_MESSAGE =
            "Water Intake is private to your ForkMesh account. Log each glass here, on the web, or on your desktop; the same log appears everywhere. Every drink is encrypted at rest with a key minted for your account alone."
        const val INSTALL_BUTTON = "Install Water Intake"
        const val UNAVAILABLE_TITLE = "Water Intake is unavailable"
        const val EMPTY_LOG = "Nothing logged today. Tap a glass after each drink."
        val DRINKS = listOf("water", "sparkling", "tea", "coffee", "juice", "milk", "other")
        val DEFAULT_PRESETS = listOf(250, 500, 750)
    }

    var overview: HealthJson? = null
        private set
    var installed = false
        private set
    var loading = true
        private set
    var error = ""
        private set
    var busy = false
        private set
    var notice: HealthNotice? = null

    val today: String get() = HealthDates.isoDay(now(), zone)

    fun load() {
        loading = true
        error = ""
        try {
            fetch()
            loading = false
        } catch (failure: OrgApiException) {
            loading = false
            if (failure.code == "plugin_not_installed") {
                installed = false
                overview = null
            } else {
                error = message(failure)
            }
        } catch (failure: Exception) {
            loading = false
            error = message(failure)
        }
    }

    private fun fetch() {
        val payload = call.call("GET", "?day=$today", null)
        installed = payload["installed"] == true
        overview = if (installed) payload else null
    }

    fun message(failure: Throwable): String = (failure as? OrgApiException)?.message ?: "Water Intake is unavailable right now."

    private fun show(text: String, isError: Boolean = false) {
        notice = HealthNotice(text, isError)
    }

    val showsSpinner: Boolean get() = loading && overview == null && error.isEmpty()
    val showsError: Boolean get() = error.isNotEmpty() && overview == null
    val showsInstall: Boolean get() = !loading && error.isEmpty() && !installed

    fun install() {
        try {
            call.call("POST", "/install", linkedMapOf("tzOffsetMinutes" to HealthClock.tzOffsetMinutes(zone, now()).toLong()))
            show("Water Intake installed.")
            load()
        } catch (failure: Exception) {
            show(message(failure), true)
        }
    }

    fun add(milliliters: Int, drink: String = "water", source: String = "quick") {
        if (milliliters <= 0) {
            show("Enter an amount first.", true)
            return
        }
        busy = true
        try {
            call.call(
                "POST", "/entries",
                linkedMapOf(
                    "day" to today, "amountMl" to milliliters.toLong(), "drink" to drink,
                    "source" to source, "at" to HealthDates.epochMs(now()),
                ),
            )
            show("Logged ${WaterText.amount(milliliters.toDouble(), units)} of $drink.")
            load()
        } catch (failure: Exception) {
            show(message(failure), true)
        } finally {
            busy = false
        }
    }

    fun addTyped(text: String, drink: String) {
        val value = HealthClock.parseNumber(text)
        val milliliters = Math.round(value * WaterText.unitMilliliters(units)).toInt()
        add(milliliters, drink, "manual")
    }

    fun delete(id: String) {
        busy = true
        try {
            call.call("DELETE", "/entries/$id", null)
            load()
        } catch (failure: Exception) {
            show(message(failure), true)
        } finally {
            busy = false
        }
    }

    fun saveGoal(goalMl: Int, units: String) {
        try {
            call.call("PUT", "/profile", linkedMapOf("goalMl" to max(0, goalMl).toLong(), "unitsPreference" to units))
            show("Goal saved.")
            fetch()
        } catch (failure: Exception) {
            show(message(failure), true)
        }
    }

    fun uninstall(): Boolean = try {
        call.call("POST", "/uninstall", emptyMap())
        prefs?.putBool(HealthPrefs.WATER, false)
        show("Water Intake uninstalled from your account.")
        load()
        true
    } catch (failure: Exception) {
        show(message(failure), true)
        false
    }

    val profile: HealthJson get() = HealthJsonRead.obj(overview?.get("profile"))
    val summary: HealthJson get() = HealthJsonRead.obj(overview?.get("summary"))
    val units: String get() = (profile["units"] as? String) ?: "metric"
    val imperial: Boolean get() = units == "imperial"
    val goalMl: Int get() = HealthJsonRead.int(profile["goalMl"]).toInt()
    val unitsPreference: String get() = (profile["unitsPreference"] as? String) ?: "auto"
    val totalMl: Int get() = HealthJsonRead.int(HealthJsonRead.obj(overview?.get("today"))["totalMl"]).toInt()
    val totalText: String get() = WaterText.amount(totalMl.toDouble(), units)
    val ratio: Double get() = if (goalMl > 0) min(1.0, totalMl.toDouble() / goalMl) else 0.0
    val goalMet: Boolean get() = goalMl > 0 && totalMl >= goalMl
    val unit: String get() = if (units == "cups") "cups" else if (imperial) "fl oz" else "ml"
    val amountHint: String get() = if (units == "cups") "1.5" else if (imperial) "12" else "330"

    val goalLine: String
        get() {
            if (goalMl <= 0) return "No daily goal"
            if (goalMet) return "Goal of ${WaterText.amount(goalMl.toDouble(), units)} reached"
            return "${WaterText.amount((goalMl - totalMl).toDouble(), units)} to go of ${WaterText.amount(goalMl.toDouble(), units)}"
        }

    val presets: List<WaterPreset>
        get() {
            val raw = (profile["presetsMl"] as? List<*>)?.mapNotNull { (it as? Number)?.toInt() } ?: emptyList()
            return (if (raw.isEmpty()) DEFAULT_PRESETS else raw).filter { it > 0 }.map {
                WaterPreset(it, WaterText.amount(it.toDouble(), units))
            }
        }

    val rows: List<WaterEntryRow>
        get() = HealthJsonRead.objects(overview?.get("entries")).reversed().map { entry ->
            val at = HealthJsonRead.int(entry["at"])
            val time = if (at > 0) HealthClock.time(Instant.ofEpochMilli(at), zone) else ""
            val source = (entry["source"] as? String) ?: "manual"
            val drink = WaterText.drinkLabel((entry["drink"] as? String) ?: "water")
            WaterEntryRow(
                id = (entry["id"] as? String) ?: "",
                title = drink,
                subtitle = listOf(time, if (source == "health") "From Apple Health" else "").filter { it.isNotEmpty() }.joinToString(" · "),
                amount = WaterText.amount(HealthJsonRead.int(entry["amountMl"]).toDouble(), units),
                fromHealth = source == "health",
            )
        }

    val bars: List<WaterBar>
        get() {
            val days = HealthJsonRead.objects(overview?.get("days"))
            var peak = max(1000.0, goalMl * 1.15)
            for (day in days) peak = max(peak, HealthJsonRead.double(day["totalMl"]) * 1.05)
            return days.mapIndexed { index, day ->
                val id = (day["day"] as? String) ?: index.toString()
                val total = HealthJsonRead.int(day["totalMl"]).toInt()
                WaterBar(
                    id = id,
                    label = if (index % 5 == 0 || index == days.size - 1) HealthClock.shortDay(id) else "",
                    ratio = total / peak,
                    metGoal = goalMl > 0 && total >= goalMl,
                    caption = "$id: ${WaterText.amount(total.toDouble(), units)}",
                )
            }
        }

    val summaryLine: String
        get() {
            val streak = HealthJsonRead.int(summary["streak"])
            val goalDays = HealthJsonRead.int(summary["goalDays"])
            return "Average ${WaterText.amount(HealthJsonRead.double(summary["averageMl"]), units)} · goal met on $goalDays of 30 days · streak $streak ${if (streak == 1L) "day" else "days"}"
        }
}

object WaterText {
    const val ML_PER_FL_OZ = 29.5735295625
    const val ML_PER_CUP = 8 * ML_PER_FL_OZ

    fun unitMilliliters(units: String): Double = if (units == "cups") ML_PER_CUP else if (units == "imperial") ML_PER_FL_OZ else 1.0

    fun amount(milliliters: Double, units: String): String {
        if (units == "cups") {
            var text = HealthClock.fixed(milliliters / ML_PER_CUP, 1)
            if (text.endsWith(".0")) text = text.dropLast(2)
            return "$text ${if (text == "1") "cup" else "cups"}"
        }
        if (units == "imperial") {
            val ounces = milliliters / ML_PER_FL_OZ
            return "${if (ounces >= 10) ounces.roundToInt().toString() else HealthClock.fixed(ounces, 1)} fl oz"
        }
        if (milliliters >= 1000) {
            var text = HealthClock.fixed(milliliters / 1000, 2)
            while (text.endsWith("0")) text = text.dropLast(1)
            if (text.endsWith(".")) text = text.dropLast(1)
            return "$text L"
        }
        return "${milliliters.roundToInt()} ml"
    }

    fun drinkLabel(drink: String): String = if (drink.isEmpty()) "Water" else drink.substring(0, 1).uppercase() + drink.substring(1)
}
