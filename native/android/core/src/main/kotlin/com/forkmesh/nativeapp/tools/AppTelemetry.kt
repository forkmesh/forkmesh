package com.forkmesh.nativeapp.tools

import java.io.File
import java.net.URI
import java.util.EnumMap
import kotlin.math.max
import kotlin.math.min

enum class TelemetryMetric(
    val label: String,
    val title: String,
    val description: String,
    val unit: String = "",
    val floor: Double = 1.0,
    val peak: Boolean = false,
    val count: Boolean = false,
) {
    Fps("FPS", "Frames drawn", "frames drawn per second", floor = 60.0),
    Frame("FRAME", "Worst frame", "slowest frame build + raster in the second", unit = "ms", floor = 32.0, peak = true),
    Loop("LOOP", "Event loop lag", "worst UI-isolate event-loop delay in the second", unit = "ms", floor = 50.0, peak = true),
    Mem("MEM", "Memory", "resident set size of the app process", unit = "MB"),
    Net("NET", "HTTP requests", "API requests finished per second", count = true),
    Relay("RELAY", "Relay frames", "relay socket frames received", count = true),
    Log("LOG", "Log lines", "lines written to the in-app log", count = true),
    Stall("STALL", "Stalls", "UI stalls recorded (200 ms or longer)", count = true),
    ;

    fun format(value: Double): String {
        if (value.isNaN()) return "–"
        if (value.isInfinite()) return dartFixed(value, 1)
        if (this == Mem) return "${dartRound(value)}MB"
        if (unit == "ms") return "${dartRound(value)}ms"
        if (value >= 1000) return "${dartFixed(value / 1000, 1)}k"
        if (value != dartRound(value).toDouble()) return dartFixed(value, 1)
        return dartRound(value).toString()
    }

    val lowerIsWorse: Boolean get() = this == Fps

    val limits: Pair<Double, Double>
        get() = when (this) {
            Fps -> 45.0 to 24.0
            Frame -> 50.0 to 100.0
            Loop -> 50.0 to 200.0
            Mem -> 500.0 to 1000.0
            Net -> 10.0 to 30.0
            Relay -> 50.0 to 200.0
            Log -> 20.0 to 100.0
            Stall -> 1.0 to 1.0
        }

    fun health(values: DoubleArray): TelemetryHealth? {
        val sampled = values.filter { !it.isNaN() }
        val worst = (if (lowerIsWorse) sampled.minOrNull() else sampled.maxOrNull()) ?: return null
        val (warning, bad) = limits
        if (lowerIsWorse) return if (worst < bad) TelemetryHealth.Bad else if (worst < warning) TelemetryHealth.Warning else TelemetryHealth.Good
        return if (worst >= bad) TelemetryHealth.Bad else if (worst >= warning) TelemetryHealth.Warning else TelemetryHealth.Good
    }
}

enum class TelemetryHealth { Good, Warning, Bad }

enum class TelemetryRange(val label: String, val seconds: Long) {
    M1("1m", 60),
    M5("5m", 5 * 60),
    M15("15m", 15 * 60),
    H1("1h", 3600),
    H6("6h", 6 * 3600),
    H24("24h", 24 * 3600),
    ;

    val durationMs: Long get() = seconds * 1000

    val minutes: Int get() = (seconds / 60).toInt()

    val usesMinutes: Boolean get() = seconds > 3600
}

data class MetricSummary(
    val latest: Double,
    val min: Double,
    val avg: Double,
    val max: Double,
    val total: Double,
    val samples: Int,
)

class MetricHistory(val secondCapacity: Int = 3600, val minuteCapacity: Int = 1440) {
    private val secondRing = DoubleArray(secondCapacity)
    private var secondHead = 0
    private var secondCount = 0

    private val minuteAvg = DoubleArray(minuteCapacity)
    private val minuteMax = DoubleArray(minuteCapacity)
    private var minuteHead = 0
    private var minuteCount = 0

    private var pendingSeconds = 0
    private var pendingValid = 0
    private var pendingSum = 0.0
    private var pendingMax = Double.NEGATIVE_INFINITY

    val latest: Double
        get() {
            if (secondCount == 0) return Double.NaN
            return secondRing[(secondHead - 1 + secondCapacity) % secondCapacity]
        }

    fun add(value: Double) {
        secondRing[secondHead] = value
        secondHead = (secondHead + 1) % secondCapacity
        if (secondCount < secondCapacity) secondCount++

        pendingSeconds++
        if (!value.isNaN()) {
            pendingValid++
            pendingSum += value
            pendingMax = max(pendingMax, value)
        }
        if (pendingSeconds == 60) {
            minuteAvg[minuteHead] = if (pendingValid == 0) Double.NaN else pendingSum / pendingValid
            minuteMax[minuteHead] = if (pendingValid == 0) Double.NaN else pendingMax
            minuteHead = (minuteHead + 1) % minuteCapacity
            if (minuteCount < minuteCapacity) minuteCount++
            pendingSeconds = 0
            pendingValid = 0
            pendingSum = 0.0
            pendingMax = Double.NEGATIVE_INFINITY
        }
    }

    fun addGap(seconds: Int) {
        if (seconds <= 0) return
        var remaining = seconds
        val align = min(remaining, (60 - pendingSeconds) % 60)
        repeat(align) { add(Double.NaN) }
        remaining -= align
        val minutes = remaining / 60
        if (minutes > 0) {
            repeat(min(minutes, minuteCapacity)) {
                minuteAvg[minuteHead] = Double.NaN
                minuteMax[minuteHead] = Double.NaN
                minuteHead = (minuteHead + 1) % minuteCapacity
                if (minuteCount < minuteCapacity) minuteCount++
            }
            val secondsFill = min(minutes.toLong() * 60, secondCapacity.toLong()).toInt()
            repeat(secondsFill) {
                secondRing[secondHead] = Double.NaN
                secondHead = (secondHead + 1) % secondCapacity
                if (secondCount < secondCapacity) secondCount++
            }
            remaining -= minutes * 60
        }
        repeat(remaining) { add(Double.NaN) }
    }

    fun lastSeconds(n: Int): DoubleArray = tail(secondRing, secondHead, secondCount, secondCapacity, n)

    fun lastMinutes(n: Int, peak: Boolean): DoubleArray =
        tail(if (peak) minuteMax else minuteAvg, minuteHead, minuteCount, minuteCapacity, n)

    private fun tail(ring: DoubleArray, head: Int, count: Int, capacity: Int, n: Int): DoubleArray {
        val take = min(n, count)
        return DoubleArray(take) { i -> ring[(head - take + i + capacity) % capacity] }
    }
}

class AppTelemetry(
    val log: AppLog,
    val monitor: PerformanceMonitorService? = null,
    private val relayFrames: (() -> Long)? = null,
    val sampleIntervalMs: Long = 1000,
    private val now: () -> Long = { System.currentTimeMillis() },
    private val rssMegabytes: () -> Double = { ProcessMemory.rssMegabytes() },
) : ChangeNotifier() {
    private val lock = Any()

    val startedAtMs: Long = now()

    private val histories = EnumMap<TelemetryMetric, MetricHistory>(TelemetryMetric::class.java).apply {
        for (metric in TelemetryMetric.entries) put(metric, MetricHistory())
    }

    private var lastSampleAt: Long? = null
    private var framesThisSecond = 0
    private var worstFrameMs = 0.0
    private var worstLoopLagMs = 0.0
    private var requestsTotal = 0L
    private var inFlight = 0
    private var lastRequests = 0L
    private var lastRelayFrames = 0L
    private var lastLogLines = 0
    private var lastStalls = 0
    private var sent = 0L
    private var received = 0L

    val uptimeMs: Long get() = now() - startedAtMs

    val requestsInFlight: Int get() = synchronized(lock) { inFlight }

    val bytesSent: Long get() = synchronized(lock) { sent }

    val bytesReceived: Long get() = synchronized(lock) { received }

    fun history(metric: TelemetryMetric): MetricHistory = histories.getValue(metric)

    fun latest(metric: TelemetryMetric): Double = synchronized(lock) { histories.getValue(metric).latest }

    fun lastSeconds(metric: TelemetryMetric, n: Int): DoubleArray =
        synchronized(lock) { histories.getValue(metric).lastSeconds(n) }

    fun recordFrame(totalSpanMicros: Long) {
        synchronized(lock) {
            framesThisSecond++
            worstFrameMs = max(worstFrameMs, totalSpanMicros / 1000.0)
        }
    }

    fun recordLoopLag(lagMicros: Long) {
        synchronized(lock) { worstLoopLagMs = max(worstLoopLagMs, lagMicros / 1000.0) }
    }

    fun sampleNow() {
        val stalls = monitor?.stallCount ?: 0
        val relay = relayFrames?.invoke() ?: 0L
        val lines = log.totalLines
        val rss = rssMegabytes()
        synchronized(lock) {
            val at = now()
            val last = lastSampleAt
            lastSampleAt = at
            if (last != null) {
                val gapSeconds = (at - last).toDouble() / sampleIntervalMs
                val missing = min(dartRound(gapSeconds) - 1, 24L * 3600).toInt()
                for (history in histories.values) history.addGap(missing)
            }

            record(TelemetryMetric.Fps, framesThisSecond.toDouble())
            record(TelemetryMetric.Frame, worstFrameMs)
            record(TelemetryMetric.Loop, worstLoopLagMs)
            record(TelemetryMetric.Mem, rss)
            record(TelemetryMetric.Net, (requestsTotal - lastRequests).toDouble())
            record(TelemetryMetric.Relay, max(0L, relay - lastRelayFrames).toDouble())
            record(TelemetryMetric.Log, (lines - lastLogLines).toDouble())
            record(TelemetryMetric.Stall, max(0, stalls - lastStalls).toDouble())

            framesThisSecond = 0
            worstFrameMs = 0.0
            worstLoopLagMs = 0.0
            lastRequests = requestsTotal
            lastRelayFrames = relay
            lastLogLines = lines
            lastStalls = stalls
        }
        notifyListeners()
    }

    private fun record(metric: TelemetryMetric, value: Double) = histories.getValue(metric).add(value)

    fun series(metric: TelemetryMetric, range: TelemetryRange): DoubleArray = synchronized(lock) {
        val history = histories.getValue(metric)
        if (range.usesMinutes) history.lastMinutes(range.minutes, peak = metric.peak) else history.lastSeconds(range.seconds.toInt())
    }

    fun summary(metric: TelemetryMetric, range: TelemetryRange): MetricSummary {
        val points = series(metric, range)
        var lo = Double.POSITIVE_INFINITY
        var hi = Double.NEGATIVE_INFINITY
        var sum = 0.0
        var n = 0
        for (value in points) {
            if (value.isNaN()) continue
            lo = min(lo, value)
            hi = max(hi, value)
            sum += value
            n++
        }
        val perPoint = if (range.usesMinutes) 60 else 1
        return MetricSummary(
            latest = latest(metric),
            min = if (n == 0) Double.NaN else lo,
            avg = if (n == 0) Double.NaN else sum / n,
            max = if (n == 0) Double.NaN else hi,
            total = if (metric.peak) Double.NaN else sum * perPoint,
            samples = n,
        )
    }

    fun recordRequestStarted() {
        synchronized(lock) { inFlight++ }
    }

    fun recordRequestFinished(
        method: String,
        url: String,
        status: Int?,
        elapsedMs: Long,
        error: Throwable? = null,
        bytesOut: Long = 0,
        bytesIn: Long = 0,
    ) {
        synchronized(lock) {
            inFlight = max(0, inFlight - 1)
            requestsTotal++
            sent += bytesOut
            received += bytesIn
        }
        val base = "$method ${requestTarget(url)}"
        when {
            error != null -> log.add(LogCategory.Error, "$base failed after ${elapsedMs}ms: $error")
            status != null && status >= 400 -> log.add(LogCategory.Error, "$base HTTP $status in ${elapsedMs}ms")
            else -> log.add(LogCategory.Net, "$base $status in ${elapsedMs}ms")
        }
    }

    companion object {
        fun requestTarget(url: String): String = try {
            val uri = URI(url)
            "${uri.host ?: ""}${uri.rawPath ?: ""}"
        } catch (_: Exception) {
            url.substringAfter("://").substringBefore('?').substringBefore('#')
        }
    }
}

object ProcessMemory {
    fun rssMegabytes(status: File = File("/proc/self/status")): Double = try {
        status.useLines { lines -> lines.firstOrNull { it.startsWith("VmRSS:") } }
            ?.removePrefix("VmRSS:")?.trim()?.removeSuffix("kB")?.trim()?.toDouble()?.div(1024.0)
            ?: Double.NaN
    } catch (_: Exception) {
        Double.NaN
    }
}
