package com.forkmesh.nativeapp.health

import com.forkmesh.nativeapp.api.PrefStore
import java.time.Instant
import java.time.ZoneId
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt
import kotlin.math.roundToLong

internal object HealthPrefList {
    fun read(prefs: PrefStore, key: String): List<String> = (prefs.getString(key) ?: "").split(";").filter { it.isNotEmpty() }

    fun write(prefs: PrefStore, key: String, values: List<String>) = prefs.putString(key, values.joinToString(";"))
}

data class SleepInterval(val start: Instant, val end: Instant) {
    val seconds: Double get() = max(0.0, secondsBetween(start, end))

    fun overlap(other: SleepInterval): Double {
        val from = if (start.isAfter(other.start)) start else other.start
        val to = if (end.isBefore(other.end)) end else other.end
        return max(0.0, secondsBetween(from, to))
    }
}

data class SleepRestSample(val start: Instant, val still: Boolean, val confident: Boolean)

data class SleepNight(
    val night: String,
    val bedAt: Instant,
    val wakeAt: Instant,
    val asleepMinutes: Int,
    val awakeMinutes: Int = 0,
    val interruptions: Int = 0,
    val confidence: String = "",
) {
    val body: HealthJson
        get() {
            val body = linkedMapOf<String, Any?>(
                "night" to night,
                "bedAt" to bedAt.toEpochMilli(),
                "wakeAt" to wakeAt.toEpochMilli(),
                "asleepMinutes" to max(0, asleepMinutes).toLong(),
                "awakeMinutes" to max(0, awakeMinutes).toLong(),
                "interruptions" to max(0, interruptions).toLong(),
            )
            if (confidence.isNotEmpty()) body["confidence"] = confidence
            return body
        }

    companion object {
        fun cleared(night: String): HealthJson = linkedMapOf("night" to night, "asleepMinutes" to 0L)
    }
}

data class SleepSchedule(val bedtime: Int = DEFAULT_BEDTIME, val wake: Int = DEFAULT_WAKE) {
    val lengthMinutes: Int
        get() {
            val length = ((wake - bedtime) % 1440 + 1440) % 1440
            return if (length == 0) 480 else length
        }

    val nightBoundary: Int get() = (wake + SleepDetector.LATE_MINUTES) % 1440

    fun nightKey(date: Instant, zone: ZoneId): String {
        val shift = ((1440 - nightBoundary) % 1440) * 60L
        return HealthDates.isoDay(date.plusSeconds(shift), zone)
    }

    fun planned(night: String, zone: ZoneId): SleepInterval? {
        val day = HealthDates.parseDay(night) ?: return null
        val wakeAt = day.atStartOfDay(zone).plusMinutes(wake.toLong()).toInstant()
        return SleepInterval(wakeAt.minusSeconds(lengthMinutes * 60L), wakeAt)
    }

    fun window(night: String, zone: ZoneId): SleepInterval? {
        val planned = planned(night, zone) ?: return null
        return SleepInterval(
            planned.start.minusSeconds(SleepDetector.EARLY_MINUTES * 60L),
            planned.end.plusSeconds(SleepDetector.LATE_MINUTES * 60L),
        )
    }

    companion object {
        const val DEFAULT_BEDTIME = 23 * 60
        const val DEFAULT_WAKE = 7 * 60

        fun of(bedtime: Int, wake: Int): SleepSchedule = SleepSchedule(((bedtime % 1440) + 1440) % 1440, ((wake % 1440) + 1440) % 1440)

        fun parse(bedtime: String, wakeTime: String): SleepSchedule =
            of(minutes(bedtime) ?: DEFAULT_BEDTIME, minutes(wakeTime) ?: DEFAULT_WAKE)

        fun minutes(clock: String): Int? {
            val parts = clock.split(":")
            if (parts.size != 2) return null
            val hour = parts[0].toIntOrNull() ?: return null
            val minute = parts[1].toIntOrNull() ?: return null
            if (hour !in 0..23 || minute !in 0..59) return null
            return hour * 60 + minute
        }
    }
}

object SleepDetector {
    const val EARLY_MINUTES = 4 * 60
    const val LATE_MINUTES = 5 * 60
    const val MERGE_GAP = 20.0 * 60
    const val WAKE_GAP = 75.0 * 60
    const val MINIMUM_OVERLAP = 60.0 * 60
    const val OFF_SCHEDULE_SLEEP = 5.0 * 3600
    const val INTERRUPTION_GAP = 2.0 * 60
    const val MINIMUM_SLEEP = 3.0 * 3600
    const val SETTLE_MINUTES = 10

    fun stillIntervals(samples: List<SleepRestSample>, within: SleepInterval): List<SleepInterval> {
        val sorted = samples.sortedBy { it.start }
        val result = ArrayList<SleepInterval>()
        for ((index, sample) in sorted.withIndex()) {
            if (!sample.still || !sample.confident) continue
            val next = if (index + 1 < sorted.size) sorted[index + 1].start else within.end
            val start = if (sample.start.isAfter(within.start)) sample.start else within.start
            val end = if (next.isBefore(within.end)) next else within.end
            if (!end.isAfter(start)) continue
            val last = result.lastOrNull()
            if (last != null && !last.end.isBefore(start)) {
                result[result.size - 1] = SleepInterval(last.start, if (last.end.isAfter(end)) last.end else end)
            } else {
                result.add(SleepInterval(start, end))
            }
        }
        return result
    }

    fun subtract(intervals: List<SleepInterval>, cuts: List<SleepInterval>): List<SleepInterval> {
        var pieces = intervals
        for (cut in cuts.sortedBy { it.start }) {
            if (!cut.end.isAfter(cut.start)) continue
            val next = ArrayList<SleepInterval>()
            for (piece in pieces) {
                if (!cut.end.isAfter(piece.start) || !cut.start.isBefore(piece.end)) {
                    next.add(piece)
                    continue
                }
                if (cut.start.isAfter(piece.start)) next.add(SleepInterval(piece.start, cut.start))
                if (cut.end.isBefore(piece.end)) next.add(SleepInterval(cut.end, piece.end))
            }
            pieces = next
        }
        return pieces
    }

    private class Block(var interval: SleepInterval, var restSeconds: Double, var gapSeconds: Double, var interruptions: Int)

    private fun joins(gap: SleepInterval, planned: SleepInterval): Boolean {
        if (gap.seconds <= MERGE_GAP) return true
        return gap.seconds <= WAKE_GAP && !gap.start.isBefore(planned.start) && !gap.end.isAfter(planned.end)
    }

    private fun blocks(intervals: List<SleepInterval>, planned: SleepInterval): List<Block> {
        val blocks = ArrayList<Block>()
        for (interval in intervals.sortedBy { it.start }) {
            val last = blocks.lastOrNull()
            val gapEnd = if (last != null && interval.start.isAfter(last.interval.end)) interval.start else last?.interval?.end
            if (last != null && gapEnd != null && joins(SleepInterval(last.interval.end, gapEnd), planned)) {
                val gap = max(0.0, secondsBetween(last.interval.end, interval.start))
                last.interval = SleepInterval(last.interval.start, if (interval.end.isAfter(last.interval.end)) interval.end else last.interval.end)
                last.restSeconds += interval.seconds
                last.gapSeconds += gap
                if (gap >= INTERRUPTION_GAP) last.interruptions += 1
            } else {
                blocks.add(Block(interval, interval.seconds, 0.0, 0))
            }
        }
        return blocks
    }

    fun detect(
        night: String,
        rest: List<SleepRestSample>,
        usage: List<SleepInterval>,
        schedule: SleepSchedule,
        zone: ZoneId,
        now: Instant,
    ): SleepNight? {
        val planned = schedule.planned(night, zone) ?: return null
        var window = schedule.window(night, zone) ?: return null
        window = SleepInterval(window.start, if (window.end.isBefore(now)) window.end else now)
        if (!window.end.isAfter(window.start)) return null
        val still = subtract(stillIntervals(rest, window), usage)
        val candidates = blocks(still, planned).filter { block ->
            block.interval.seconds >= MINIMUM_SLEEP &&
                (block.interval.overlap(planned) >= MINIMUM_OVERLAP || block.interval.seconds >= OFF_SCHEDULE_SLEEP)
        }
        val best = candidates.maxWithOrNull { left, right ->
            val a = left.interval.overlap(planned)
            val b = right.interval.overlap(planned)
            if (a == b) left.interval.seconds.compareTo(right.interval.seconds) else a.compareTo(b)
        } ?: return null
        val spanMinutes = (best.interval.seconds / 60).toInt()
        val gapMinutes = (best.gapSeconds / 60).roundToInt()
        val settle = min(SETTLE_MINUTES, max(0, spanMinutes - gapMinutes))
        val asleep = max(0, spanMinutes - gapMinutes - settle)
        val overlapRatio = best.interval.overlap(planned) / max(60.0, min(best.interval.seconds, planned.seconds))
        val confidence = if (overlapRatio >= 0.6 && best.interruptions <= 4 && best.restSeconds / best.interval.seconds >= 0.85) {
            "high"
        } else if (overlapRatio >= 0.3) {
            "medium"
        } else {
            "low"
        }
        return SleepNight(
            night, best.interval.start, best.interval.end, asleep, gapMinutes + settle, best.interruptions, confidence,
        )
    }

    fun detect(
        nights: List<String>,
        rest: List<SleepRestSample>,
        usage: List<SleepInterval>,
        schedule: SleepSchedule,
        zone: ZoneId,
        now: Instant,
    ): Pair<List<HealthJson>, List<SleepNight>> {
        val sorted = rest.sortedBy { it.start }
        val bodies = ArrayList<HealthJson>()
        val detected = ArrayList<SleepNight>()
        for (key in nights) {
            val night = detect(key, sorted, usage, schedule, zone, now)
            if (night != null) {
                detected.add(night)
                bodies.add(night.body)
            } else {
                bodies.add(SleepNight.cleared(key))
            }
        }
        return bodies to detected
    }
}

object SleepUsageLog {
    const val KEEP = 8.0 * 86400

    fun begin(prefs: PrefStore, now: Instant) {
        if (HealthPrefs.double(prefs, HealthPrefs.SLEEP_USAGE_OPEN) > 0) end(prefs, now)
        prefs.putDouble(HealthPrefs.SLEEP_USAGE_OPEN, now.toEpochMilli() / 1000.0)
    }

    fun end(prefs: PrefStore, now: Instant) {
        val opened = HealthPrefs.double(prefs, HealthPrefs.SLEEP_USAGE_OPEN)
        prefs.putDouble(HealthPrefs.SLEEP_USAGE_OPEN, 0.0)
        val closed = now.toEpochMilli() / 1000.0
        if (opened <= 0 || closed <= opened) return
        val entries = HealthPrefList.read(prefs, HealthPrefs.SLEEP_USAGE).toMutableList()
        entries.add("${opened.toLong()}-${kotlin.math.ceil(closed).toLong()}")
        val cutoff = closed - KEEP
        HealthPrefList.write(prefs, HealthPrefs.SLEEP_USAGE, entries.filter { (parse(it)?.end?.toEpochMilli() ?: 0L) / 1000.0 >= cutoff })
    }

    fun intervals(prefs: PrefStore, now: Instant): List<SleepInterval> {
        val result = HealthPrefList.read(prefs, HealthPrefs.SLEEP_USAGE).mapNotNull(::parse).toMutableList()
        val opened = HealthPrefs.double(prefs, HealthPrefs.SLEEP_USAGE_OPEN)
        if (opened > 0 && opened < now.toEpochMilli() / 1000.0) result.add(SleepInterval(Instant.ofEpochMilli((opened * 1000).roundToLong()), now))
        return result
    }

    fun parse(entry: String): SleepInterval? {
        val parts = entry.split("-")
        if (parts.size != 2) return null
        val start = parts[0].toDoubleOrNull() ?: return null
        val end = parts[1].toDoubleOrNull() ?: return null
        if (end <= start) return null
        return SleepInterval(Instant.ofEpochMilli((start * 1000).roundToLong()), Instant.ofEpochMilli((end * 1000).roundToLong()))
    }
}

object SleepRestLog {
    const val LOG_KEY = "health.sleep.rest"
    const val LAST_KEY = "health.sleep.restLast"
    const val KEEP_MS = 8L * 86_400_000
    const val MOVING_STEPS_PER_SECOND = 0.02
    const val REBOOT_JITTER_MS = 60_000L

    private const val STILL = "S"
    private const val MOVING = "M"
    private const val UNKNOWN = "U"

    fun observe(prefs: PrefStore, reading: StepsDailyLog.Reading) {
        val raw = (prefs.getString(LAST_KEY) ?: "").split(",")
        prefs.putString(LAST_KEY, "${reading.counter},${reading.atMs},${reading.bootedAtMs}")
        if (raw.size != 3) return
        val lastCounter = raw[0].toLongOrNull() ?: return
        val lastAt = raw[1].toLongOrNull() ?: return
        val lastBoot = raw[2].toLongOrNull() ?: return
        if (reading.atMs <= lastAt) return
        val rebooted = reading.counter < lastCounter || reading.bootedAtMs - lastBoot > REBOOT_JITTER_MS
        val state = if (rebooted) {
            UNKNOWN
        } else {
            val delta = reading.counter - lastCounter
            val seconds = (reading.atMs - lastAt) / 1000.0
            if (delta > 0 && delta / seconds >= MOVING_STEPS_PER_SECOND) MOVING else STILL
        }
        append(prefs, lastAt, state, reading.atMs)
    }

    private fun append(prefs: PrefStore, atMs: Long, state: String, nowMs: Long) {
        val entries = HealthPrefList.read(prefs, LOG_KEY).toMutableList()
        if (entries.lastOrNull()?.substringAfter(":") == state) return
        entries.add("$atMs:$state")
        val cutoff = nowMs - KEEP_MS
        val kept = entries.filter { (it.substringBefore(":").toLongOrNull() ?: 0L) >= cutoff }
        HealthPrefList.write(prefs, LOG_KEY, if (kept.isEmpty()) entries.takeLast(1) else kept)
    }

    fun samples(prefs: PrefStore, fromMs: Long, toMs: Long): List<SleepRestSample> {
        val all = HealthPrefList.read(prefs, LOG_KEY).mapNotNull { entry ->
            val at = entry.substringBefore(":").toLongOrNull() ?: return@mapNotNull null
            at to entry.substringAfter(":")
        }.sortedBy { it.first }
        val lead = all.lastOrNull { it.first < fromMs }
        val inside = all.filter { it.first in fromMs until toMs }
        val picked = (listOfNotNull(lead?.let { fromMs to it.second }) + inside)
        return picked.map { (at, state) ->
            SleepRestSample(Instant.ofEpochMilli(at), state == STILL, state != UNKNOWN)
        }
    }
}
