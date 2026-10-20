package com.forkmesh.nativeapp.health

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.PrefStore
import java.time.Instant
import java.time.LocalDate
import java.time.ZoneId
import java.time.format.DateTimeParseException
import kotlin.math.max
import kotlin.math.roundToLong

typealias HealthJson = Map<String, Any?>

fun interface HealthCall {
    fun call(method: String, suffix: String, body: HealthJson?): HealthJson
}

object HealthPrefs {
    const val STEPS = "health.steps.enabled"
    const val WATER = "health.water.enabled"
    const val NUTRITION = "health.calories.enabled"
    const val STEPS_SYNCED_AT = "health.steps.syncedAt"
    const val STEPS_IMPERIAL = "health.steps.imperial"
    const val SLEEP = "health.sleep.enabled"
    const val SLEEP_WRITE_HEALTH = "health.sleep.writeHealth"
    const val SLEEP_SYNCED_AT = "health.sleep.syncedAt"
    const val SLEEP_USAGE = "health.sleep.usage"
    const val SLEEP_USAGE_OPEN = "health.sleep.usageOpen"
    const val SLEEP_BEDTIME = "health.sleep.bedtime"
    const val SLEEP_WAKE_TIME = "health.sleep.wakeTime"
    const val SUGAR_BREAK = "health.sugarBreak.enabled"
    const val STRETCH = "health.stretch.enabled"
    const val FOOD_GUIDE = "health.foodGuide.enabled"
    const val SYNC_INTERVAL_SECONDS = 15 * 60.0

    fun bool(prefs: PrefStore?, key: String, or: Boolean = false): Boolean = prefs?.getBool(key) ?: or

    fun double(prefs: PrefStore?, key: String, or: Double = 0.0): Double = prefs?.getDouble(key) ?: or
}

class HealthApi(private val api: ApiService) {
    fun steps(): HealthCall = scoped("/api/steps")
    fun water(): HealthCall = scoped("/api/water")
    fun sleep(): HealthCall = scoped("/api/sleep")
    fun sugarBreak(): HealthCall = scoped("/api/sugar-break")
    fun stretch(): HealthCall = scoped("/api/stretch")

    private fun scoped(base: String): HealthCall = HealthCall { method, suffix, body -> api.accountApiCall(method, "$base$suffix", body) }
}

object HealthJsonRead {
    fun int(value: Any?): Long = when (value) {
        is Number -> value.toDouble().let { if (it.isFinite()) it.toLong() else 0L }
        is String -> value.trim().toDoubleOrNull()?.toLong() ?: 0L
        else -> 0L
    }

    fun double(value: Any?): Double = when (value) {
        is Number -> value.toDouble().takeIf { it.isFinite() } ?: 0.0
        is String -> value.trim().toDoubleOrNull() ?: 0.0
        else -> 0.0
    }

    fun string(value: Any?): String = when (value) {
        null -> ""
        is String -> value
        is Number -> if (value.toDouble() == value.toLong().toDouble()) value.toLong().toString() else value.toString()
        else -> value.toString()
    }

    fun bool(value: Any?): Boolean = when (value) {
        is Boolean -> value
        is Number -> value.toInt() != 0
        is String -> value == "true" || value == "1"
        else -> false
    }

    @Suppress("UNCHECKED_CAST")
    fun obj(value: Any?): HealthJson = (value as? Map<String, Any?>) ?: emptyMap()

    @Suppress("UNCHECKED_CAST")
    fun objects(value: Any?): List<HealthJson> = (value as? List<*>)?.mapNotNull { it as? Map<String, Any?> } ?: emptyList()
}

data class HealthStepDay(
    val day: String,
    val steps: Int,
    val distanceM: Double = 0.0,
    val floors: Int = 0,
    val activeKcal: Double = 0.0,
    val cadence: Double = 0.0,
    val pace: Double = 0.0,
) {
    val body: HealthJson
        get() = linkedMapOf(
            "day" to day,
            "steps" to max(0, steps).toLong(),
            "distanceM" to (max(0.0, distanceM) * 10).roundToLong() / 10.0,
            "floors" to max(0, floors).toLong(),
            "activeKcal" to (max(0.0, activeKcal) * 10).roundToLong() / 10.0,
        )

    fun merged(other: HealthStepDay): HealthStepDay = HealthStepDay(
        day, max(steps, other.steps), max(distanceM, other.distanceM), max(floors, other.floors),
        max(activeKcal, other.activeKcal), max(cadence, other.cadence), max(pace, other.pace),
    )
}

object HealthDates {
    fun isoDay(instant: Instant, zone: ZoneId = ZoneId.systemDefault()): String = instant.atZone(zone).toLocalDate().toString()

    fun parseDay(day: String): LocalDate? = try {
        LocalDate.parse(day)
    } catch (_: DateTimeParseException) {
        null
    }

    fun startOfDay(day: String, zone: ZoneId = ZoneId.systemDefault()): Instant? = parseDay(day)?.atStartOfDay(zone)?.toInstant()

    fun sampleDate(day: String, atMs: Long, zone: ZoneId = ZoneId.systemDefault(), now: Instant = Instant.now()): Instant? {
        val midnight = startOfDay(day, zone) ?: return null
        if (midnight.isAfter(now)) return null
        if (atMs > 0) {
            val at = Instant.ofEpochMilli(atMs)
            if (isoDay(at, zone) == day) return if (at.isAfter(now)) now else at
        }
        val noon = parseDay(day)!!.atStartOfDay(zone).plusHours(12).toInstant()
        return if (noon.isAfter(now)) now else noon
    }

    fun days(ending: String, count: Int): List<String> {
        val end = parseDay(ending) ?: return emptyList()
        if (count <= 0) return emptyList()
        return (count - 1 downTo 0).map { end.minusDays(it.toLong()).toString() }
    }

    fun epochMs(instant: Instant): Long = instant.toEpochMilli()
}

object StepsHealthSync {
    const val MAX_DAYS_PER_REQUEST = 62

    fun push(days: List<HealthStepDay>, device: String, call: HealthCall): Int {
        val usable = days.filter { it.day.isNotEmpty() }
        var synced = 0
        var index = 0
        while (index < usable.size) {
            val chunk = usable.subList(index, minOf(usable.size, index + MAX_DAYS_PER_REQUEST))
            val reply = call.call("POST", "/sync", linkedMapOf("source" to "health", "device" to device, "days" to chunk.map { it.body }))
            synced += HealthJsonRead.int(reply["synced"]).toInt()
            index += MAX_DAYS_PER_REQUEST
        }
        return synced
    }

    fun runIfDue(available: Boolean, collect: () -> List<HealthStepDay>, prefs: PrefStore, call: HealthCall, nowSeconds: Double, force: Boolean = false): Int {
        if (!HealthPrefs.bool(prefs, HealthPrefs.STEPS) || !available) return 0
        val last = HealthPrefs.double(prefs, HealthPrefs.STEPS_SYNCED_AT)
        if (!force && nowSeconds - last < HealthPrefs.SYNC_INTERVAL_SECONDS) return 0
        val collected = collect()
        if (collected.isEmpty()) return 0
        return try {
            val synced = push(collected, "android", call)
            prefs.putDouble(HealthPrefs.STEPS_SYNCED_AT, nowSeconds)
            synced
        } catch (_: Exception) {
            0
        }
    }
}
