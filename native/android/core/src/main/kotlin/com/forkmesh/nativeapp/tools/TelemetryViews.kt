package com.forkmesh.nativeapp.tools

import kotlin.math.ceil
import kotlin.math.max

object TelemetryStripLayout {
    const val HEIGHT_DP = 34f
    const val SPARK_SECONDS = 30
    const val SQUARE_WIDTH_DP = 58f
}

fun compactCount(count: Int): String {
    if (count < 1000) return "$count"
    if (count < 10000) return "${dartFixed(count / 1000.0, 1)}k"
    return "${count / 1000}k"
}

fun formatUptime(uptimeMs: Long): String {
    val days = uptimeMs / 86_400_000
    val hours = uptimeMs / 3_600_000
    val minutes = uptimeMs / 60_000
    val seconds = uptimeMs / 1000
    if (days > 0) return "${days}d${hours % 24}h"
    if (hours > 0) return "${hours}h${minutes % 60}m"
    if (minutes > 0) return "${minutes}m${seconds % 60}s"
    return "${seconds}s"
}

fun downsample(values: DoubleArray, size: Int, peak: Boolean): DoubleArray {
    if (size <= 1) return values
    val out = ArrayList<Double>()
    var end = values.size
    while (end > 0) {
        val start = max(0, end - size)
        var sum = 0.0
        var hi = Double.NEGATIVE_INFINITY
        var n = 0
        for (i in start until end) {
            val v = values[i]
            if (v.isNaN()) continue
            sum += v
            hi = max(hi, v)
            n++
        }
        out.add(if (n == 0) Double.NaN else if (peak) hi else sum / n)
        end -= size
    }
    out.reverse()
    return out.toDoubleArray()
}

object TelemetryChart {
    fun scaleTop(values: DoubleArray, floor: Double): Double {
        var top = floor
        for (v in values) if (!v.isNaN()) top = max(top, v * 1.1)
        return top
    }

    fun step(width: Double, count: Int, slots: Int): Double {
        val span = max(slots, count) - 1
        return if (span == 0) 0.0 else width / span
    }

    fun xAt(index: Int, count: Int, width: Double, step: Double): Double = width - (count - 1 - index) * step

    fun yAt(value: Double, top: Double, height: Double, inset: Double): Double =
        height - inset - (max(0.0, value) / top).coerceIn(0.0, 1.0) * (height - inset * 2)

    fun runs(values: DoubleArray): List<IntRange> {
        val out = ArrayList<IntRange>()
        var i = 0
        val n = values.size
        while (i < n) {
            while (i < n && values[i].isNaN()) i++
            if (i >= n) break
            val start = i
            i++
            while (i < n && !values[i].isNaN()) i++
            out.add(start until i)
        }
        return out
    }

    fun indexAt(x: Double, width: Double, count: Int, slots: Int): Int? {
        if (count == 0 || width <= 0) return null
        val span = max(slots, count) - 1
        if (span == 0) return 0
        val dx = width / span
        val fromRight = dartRound((width - x) / dx)
        val index = count - 1 - fromRight
        return index.coerceIn(0L, (count - 1).toLong()).toInt()
    }
}

fun agoLabel(seconds: Long): String {
    if (seconds == 0L) return "now"
    if (seconds / 60 == 0L) return "${seconds}s ago"
    if (seconds / 3600 == 0L) return "${seconds / 60}m ago"
    return "${seconds / 3600}h${(seconds / 60) % 60}m ago"
}

class MetricCardModel private constructor(
    val metric: TelemetryMetric,
    val points: DoubleArray,
    val slots: Int,
    val secondsPerPoint: Int,
    val summary: MetricSummary,
) {
    fun scrubIndex(scrub: Int?): Int? = if (scrub != null && scrub < points.size) scrub else null

    fun valueText(scrub: Int?): String {
        val index = scrubIndex(scrub)
        return metric.format(if (index != null) points[index] else summary.latest)
    }

    fun whenText(scrub: Int?): String {
        val index = scrubIndex(scrub) ?: return "now"
        return agoLabel((points.size - 1 - index).toLong() * secondsPerPoint)
    }

    val footer: String
        get() = if (metric.count) {
            "total ${metric.format(summary.total)} · peak ${metric.format(summary.max)}/s"
        } else {
            "min ${metric.format(summary.min)} · avg ${metric.format(summary.avg)} · max ${metric.format(summary.max)}"
        }

    companion object {
        const val MAX_POINTS = 180

        fun of(telemetry: AppTelemetry, metric: TelemetryMetric, range: TelemetryRange): MetricCardModel {
            val raw = telemetry.series(metric, range)
            val expected = if (range.usesMinutes) range.minutes else range.seconds.toInt()
            val bucket = max(1, ceil(expected.toDouble() / MAX_POINTS).toInt())
            val points = downsample(raw, bucket, peak = metric.peak)
            val slots = ceil(expected.toDouble() / bucket).toInt()
            val secondsPerPoint = bucket * (if (range.usesMinutes) 60 else 1)
            return MetricCardModel(metric, points, slots, secondsPerPoint, telemetry.summary(metric, range))
        }
    }
}

object TelemetryScreenText {
    const val TITLE = "Diagnostics"
    const val COPY_TOOLTIP = "Copy the logs shown"

    fun statsHeader(uptimeMs: Long): String =
        "Up ${formatUptime(uptimeMs)} · sampled every second while the app is open · history since launch"

    fun statsColumns(widthDp: Float): Int = when {
        widthDp >= 720 -> 4
        widthDp >= 480 -> 3
        else -> 2
    }

    fun visibleEntries(
        log: AppLog,
        categories: Set<LogCategory>,
        range: TelemetryRange,
        nowMs: Long,
        pausedAtMs: Long?,
    ): List<AppLogEntry> {
        val anchor = pausedAtMs ?: nowMs
        val entries = log.entries(categories = categories.ifEmpty { null }, sinceMs = anchor - range.durationMs)
        if (pausedAtMs == null) return entries
        return entries.filter { it.timestampMs <= pausedAtMs }
    }

    fun filterCounts(log: AppLog, range: TelemetryRange, nowMs: Long, pausedAtMs: Long?): IntArray =
        log.countsSince((pausedAtMs ?: nowMs) - range.durationMs)

    fun allChipLabel(counts: IntArray): String = "All ${counts.sum()}"

    fun chipLabel(category: LogCategory, counts: IntArray): String = "${category.label} ${counts[category.ordinal]}"

    fun logsCaption(
        log: AppLog,
        categories: Set<LogCategory>,
        visibleCount: Int,
        range: TelemetryRange,
        stallLogPath: String?,
    ): String {
        val single = if (categories.size == 1) categories.first() else null
        if (single != null) {
            val also = if (single == LogCategory.Stall && stallLogPath != null) " Also written to $stallLogPath" else ""
            return "${single.description}. ${log.totalFor(single)} since launch.$also"
        }
        return "$visibleCount lines in the last ${range.label} · ${log.totalLines} since launch"
    }

    fun emptyLogs(range: TelemetryRange): String = "Nothing logged in the last ${range.label}"

    fun copyText(entries: List<AppLogEntry>): String = entries.asReversed().joinToString("\n") { it.toString() }

    fun copiedLines(count: Int): String = "Copied $count log lines"

    const val COPIED_LINE = "Copied log line"
}
