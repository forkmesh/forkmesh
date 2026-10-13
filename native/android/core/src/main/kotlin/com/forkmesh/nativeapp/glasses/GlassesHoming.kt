package com.forkmesh.nativeapp.glasses

import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.floor
import kotlin.math.sin
import kotlin.math.sqrt

enum class GlassesTrend(val label: String) {
    CLOSER("Getting closer"),
    FARTHER("Moving away"),
    STEADY("Holding steady"),
}

data class GlassesHomingReading(
    val address: String,
    val label: String,
    val rssi: Int,
    val closeness: Double,
    val trend: GlassesTrend?,
    val bearing: Double?,
    val confidence: Double,
    val live: Boolean,
    val compass: Boolean,
) {
    val distance: String get() = GlassesSignatures.distance(rssi)

    fun arrow(heading: Double?): Double? {
        val towards = bearing ?: return null
        val facing = heading ?: return null
        return GlassesHoming.normalize(towards - facing)
    }

    val hint: String
        get() = when {
            !live -> "Signal lost. Walk around to pick it up again."
            !compass -> "No compass reading, so only closeness is shown."
            bearing == null -> "Hold the phone in front of you and turn slowly to find the direction."
            confidence < 0.5 -> "Rough direction. Keep turning slowly to sharpen it."
            else -> "Strongest signal this way."
        }

    val summary: String
        get() = listOfNotNull(if (live) distance else "Signal lost", "$rssi dBm", trend?.takeIf { live }?.label).joinToString(" · ")
}

class GlassesHoming(
    val windowMs: Long = 20_000L,
    val liveMs: Long = 8_000L,
    val stickyMs: Long = 15_000L,
    val sectors: Int = 12,
) {
    private class Sample(val atMs: Long, val rssi: Int, val heading: Double?)

    private val samples = ArrayDeque<Sample>()
    private var smoothed: Double? = null

    var target: GlassesSighting? = null
        private set

    fun follow(nearby: List<GlassesSighting>, nowMs: Long) {
        val current = target
        val kept = current?.let { c -> nearby.firstOrNull { it.address == c.address } }
        if (kept != null && nowMs - kept.seenAtMs <= stickyMs) {
            target = kept
            return
        }
        val next = nearby.firstOrNull { nowMs - it.seenAtMs <= stickyMs }
        if (next?.address != current?.address) {
            samples.clear()
            smoothed = null
        }
        target = next
    }

    fun hear(address: String, rssi: Int, heading: Double?, nowMs: Long) {
        if (address != target?.address || rssi >= 0 || rssi < -127) return
        samples.addLast(Sample(nowMs, rssi, heading?.let(::normalize)))
        smoothed = smoothed?.let { it + (rssi - it) * SMOOTHING } ?: rssi.toDouble()
        prune(nowMs)
    }

    fun reset() {
        samples.clear()
        smoothed = null
        target = null
    }

    fun reading(nowMs: Long): GlassesHomingReading? {
        val sighting = target ?: return null
        prune(nowMs)
        val rssi = smoothed?.let { Math.round(it).toInt() } ?: sighting.rssi
        val last = samples.lastOrNull()?.atMs ?: sighting.seenAtMs
        val bearing = bearing()
        return GlassesHomingReading(
            address = sighting.address,
            label = sighting.label,
            rssi = rssi,
            closeness = closeness(rssi),
            trend = trend(nowMs),
            bearing = bearing?.first,
            confidence = bearing?.second ?: 0.0,
            live = nowMs - last <= liveMs,
            compass = samples.any { it.heading != null },
        )
    }

    private fun prune(nowMs: Long) {
        while (samples.isNotEmpty() && nowMs - samples.first().atMs > windowMs) samples.removeFirst()
    }

    private fun trend(nowMs: Long): GlassesTrend? {
        val recent = samples.filter { nowMs - it.atMs <= RECENT_MS }
        val earlier = samples.filter { nowMs - it.atMs in (RECENT_MS + 1000)..(RECENT_MS + 9000) }
        if (recent.size < 2 || earlier.size < 2) return null
        val change = recent.map { it.rssi }.average() - earlier.map { it.rssi }.average()
        return when {
            change >= TREND_DB -> GlassesTrend.CLOSER
            change <= -TREND_DB -> GlassesTrend.FARTHER
            else -> GlassesTrend.STEADY
        }
    }

    private fun bearing(): Pair<Double, Double>? {
        val width = 360.0 / sectors
        val sums = DoubleArray(sectors)
        val counts = IntArray(sectors)
        for (sample in samples) {
            val heading = sample.heading ?: continue
            val index = floor(heading / width).toInt().mod(sectors)
            sums[index] += sample.rssi
            counts[index]++
        }
        val averages = (0 until sectors).map { if (counts[it] >= MIN_SECTOR_SAMPLES) sums[it] / counts[it] else null }
        val covered = averages.count { it != null }
        if (covered < MIN_SECTORS) return null
        val known = averages.filterNotNull()
        val weakest = known.min()
        val best = averages.indices.maxBy { averages[it] ?: Double.NEGATIVE_INFINITY }
        val spread = averages[best]!! - weakest
        if (spread < MIN_SPREAD_DB) return null
        var x = 0.0
        var y = 0.0
        for (offset in -1..1) {
            val index = (best + offset).mod(sectors)
            val average = averages[index] ?: continue
            val weight = (average - weakest) * (average - weakest)
            val angle = Math.toRadians((index + 0.5) * width)
            x += weight * sin(angle)
            y += weight * cos(angle)
        }
        val angle = normalize(Math.toDegrees(atan2(x, y)))
        val confidence = minOf(1.0, spread / FULL_SPREAD_DB) * minOf(1.0, covered / (sectors / 2.0))
        return angle to confidence
    }

    companion object {
        const val SMOOTHING = 0.3
        const val RECENT_MS = 3_000L
        const val TREND_DB = 3.0
        const val MIN_SECTOR_SAMPLES = 2
        const val MIN_SECTORS = 3
        const val MIN_SPREAD_DB = 3.0
        const val FULL_SPREAD_DB = 12.0
        const val FAR_RSSI = -95.0
        const val NEAR_RSSI = -45.0

        fun normalize(degrees: Double): Double = ((degrees % 360.0) + 360.0) % 360.0

        fun clock(arrow: Double): String {
            val hour = Math.round(normalize(arrow) / 30.0).toInt() % 12
            return "at ${if (hour == 0) 12 else hour} o'clock"
        }

        fun closeness(rssi: Int): Double = ((rssi - FAR_RSSI) / (NEAR_RSSI - FAR_RSSI)).coerceIn(0.0, 1.0)

        fun unwrap(shown: Double, next: Double): Double {
            val delta = ((next - shown) % 360.0 + 540.0) % 360.0 - 180.0
            return shown + delta
        }

        fun smoothHeading(previous: Double?, next: Double, factor: Double = 0.2): Double {
            if (previous == null) return normalize(next)
            val a = Math.toRadians(previous)
            val b = Math.toRadians(next)
            val x = sin(a) * (1 - factor) + sin(b) * factor
            val y = cos(a) * (1 - factor) + cos(b) * factor
            return normalize(Math.toDegrees(atan2(x, y)))
        }

        fun heading(rotation: FloatArray): Double? {
            if (rotation.size < 9) return null
            val east = rotation[1] - rotation[2]
            val north = rotation[4] - rotation[5]
            if (sqrt(east * east + north * north) < 0.2f) return null
            return normalize(Math.toDegrees(atan2(east.toDouble(), north.toDouble())))
        }
    }
}
