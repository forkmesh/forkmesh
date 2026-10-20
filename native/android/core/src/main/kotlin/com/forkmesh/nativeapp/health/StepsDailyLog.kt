package com.forkmesh.nativeapp.health

import com.forkmesh.nativeapp.api.PrefStore
import java.time.Instant
import java.time.LocalDate
import java.time.ZoneId
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt
import kotlin.math.roundToLong

object StepsDailyLog {
    const val DAYS_KEY = "health.steps.android.days"
    const val LAST_KEY = "health.steps.android.last"
    const val STRIDE_M = 0.72
    const val KEEP_DAYS = 45
    const val WAKE_START_MINUTES = 7 * 60
    const val WAKE_END_MINUTES = 22 * 60
    const val SEED_WINDOW_MS = 36L * 3600 * 1000
    const val REBOOT_JITTER_MS = 60_000L
    const val MAX_SPAN_DAYS = 8L

    data class Reading(val counter: Long, val atMs: Long, val bootedAtMs: Long)

    fun days(prefs: PrefStore): Map<String, Int> {
        val raw = prefs.getString(DAYS_KEY) ?: return emptyMap()
        val result = sortedMapOf<String, Int>()
        for (entry in raw.split(";")) {
            val parts = entry.split("=")
            if (parts.size != 2) continue
            val steps = parts[1].toIntOrNull() ?: continue
            if (HealthDates.parseDay(parts[0]) != null && steps > 0) result[parts[0]] = steps
        }
        return result
    }

    private fun save(prefs: PrefStore, days: Map<String, Int>) {
        val kept = days.toSortedMap().entries.toList().takeLast(KEEP_DAYS)
        prefs.putString(DAYS_KEY, kept.joinToString(";") { "${it.key}=${it.value}" })
    }

    fun lastReading(prefs: PrefStore): Reading? {
        val parts = (prefs.getString(LAST_KEY) ?: return null).split(",")
        if (parts.size != 3) return null
        return Reading(parts[0].toLongOrNull() ?: return null, parts[1].toLongOrNull() ?: return null, parts[2].toLongOrNull() ?: return null)
    }

    fun steps(prefs: PrefStore, day: String): Int = days(prefs)[day] ?: 0

    fun record(prefs: PrefStore, reading: Reading, zone: ZoneId = ZoneId.systemDefault()): Int {
        val today = HealthDates.isoDay(Instant.ofEpochMilli(reading.atMs), zone)
        val last = lastReading(prefs)
        if (last != null && reading.atMs < last.atMs) return steps(prefs, today)
        prefs.putString(LAST_KEY, "${reading.counter},${reading.atMs},${reading.bootedAtMs}")
        if (last == null) {
            if (reading.counter > 0 && reading.atMs - reading.bootedAtMs <= SEED_WINDOW_MS) {
                add(prefs, reading.counter, reading.bootedAtMs, reading.atMs, zone)
            }
            return steps(prefs, today)
        }
        val rebooted = reading.counter < last.counter || reading.bootedAtMs - last.bootedAtMs > REBOOT_JITTER_MS
        val delta = if (rebooted) reading.counter else reading.counter - last.counter
        if (delta > 0) add(prefs, delta, if (rebooted) max(last.atMs, reading.bootedAtMs) else last.atMs, reading.atMs, zone)
        return steps(prefs, today)
    }

    private fun add(prefs: PrefStore, delta: Long, fromMs: Long, toMs: Long, zone: ZoneId) {
        val shares = distribute(delta, fromMs, toMs, zone)
        val stored = LinkedHashMap(days(prefs))
        for ((day, steps) in shares) stored[day] = (stored[day] ?: 0) + steps
        save(prefs, stored)
    }

    fun distribute(delta: Long, fromMs: Long, toMs: Long, zone: ZoneId): Map<String, Int> {
        val total = min(delta, Int.MAX_VALUE.toLong()).toInt()
        val end = Instant.ofEpochMilli(toMs).atZone(zone).toLocalDate()
        val start = Instant.ofEpochMilli(min(fromMs, toMs)).atZone(zone).toLocalDate()
        if (start == end || end.toEpochDay() - start.toEpochDay() > MAX_SPAN_DAYS) return mapOf(end.toString() to total)
        val weights = LinkedHashMap<String, Double>()
        var date: LocalDate = start
        while (!date.isAfter(end)) {
            val open = date.atStartOfDay(zone).plusMinutes(WAKE_START_MINUTES.toLong()).toInstant().toEpochMilli()
            val close = date.atStartOfDay(zone).plusMinutes(WAKE_END_MINUTES.toLong()).toInstant().toEpochMilli()
            val overlap = max(0L, min(close, toMs) - max(open, fromMs))
            if (overlap > 0) weights[date.toString()] = overlap.toDouble()
            date = date.plusDays(1)
        }
        val sum = weights.values.sum()
        if (sum <= 0) return mapOf(end.toString() to total)
        val shares = LinkedHashMap<String, Int>()
        var given = 0
        val keys = weights.keys.toList()
        for ((index, key) in keys.withIndex()) {
            val part = if (index == keys.size - 1) total - given else (total * weights.getValue(key) / sum).roundToInt()
            shares[key] = part
            given += part
        }
        return shares.filterValues { it > 0 }
    }

    fun stepDays(prefs: PrefStore, count: Int, nowMs: Long, zone: ZoneId = ZoneId.systemDefault()): List<HealthStepDay> {
        val today = HealthDates.isoDay(Instant.ofEpochMilli(nowMs), zone)
        val stored = days(prefs)
        return HealthDates.days(today, max(1, count)).mapNotNull { day ->
            val steps = stored[day] ?: 0
            if (steps > 0) dayOf(day, steps) else null
        }
    }

    fun dayOf(day: String, steps: Int): HealthStepDay =
        HealthStepDay(day, steps, distanceM = (steps * STRIDE_M * 10).roundToLong() / 10.0)
}
