package com.forkmesh.nativeapp.health

import com.forkmesh.nativeapp.tools.ChangeNotifier
import java.time.Instant
import kotlin.math.abs
import kotlin.math.roundToInt

object HeartTap {
    const val MIN_TAPS = 5
    const val MAX_INTERVALS = 12
    const val RESET_AFTER = 2.5
    const val TOLERANCE = 0.3
    val RANGE = 30.0..220.0

    fun intervals(taps: List<Instant>): List<Double> =
        taps.zipWithNext { a, b -> secondsBetween(a, b) }.takeLast(MAX_INTERVALS)

    fun median(values: List<Double>): Double? {
        if (values.isEmpty()) return null
        val sorted = values.sorted()
        val mid = sorted.size / 2
        return if (sorted.size % 2 == 1) sorted[mid] else (sorted[mid - 1] + sorted[mid]) / 2
    }

    fun bpm(taps: List<Instant>): Int? {
        if (taps.size < MIN_TAPS) return null
        val gaps = intervals(taps)
        val middle = median(gaps) ?: return null
        if (middle <= 0) return null
        val steady = gaps.filter { abs(it - middle) <= middle * TOLERANCE }
        if (steady.size < MIN_TAPS - 1) return null
        val rate = 60 / (steady.sum() / steady.size)
        if (rate !in RANGE) return null
        return rate.roundToInt()
    }
}

class HeartTapModel(private val now: () -> Instant = { Instant.now() }) : ChangeNotifier() {
    var taps: List<Instant> = emptyList()
        private set
    var bpm: Int? = null
        private set

    val canReset: Boolean get() = taps.isNotEmpty()

    val line: String
        get() {
            val rate = bpm
            if (rate != null) return "$rate BPM from ${taps.size} taps."
            if (taps.isEmpty()) return IDLE_LINE
            if (taps.size < HeartTap.MIN_TAPS) {
                val left = HeartTap.MIN_TAPS - taps.size
                return "Keep going: $left more ${if (left == 1) "tap" else "taps"}."
            }
            return UNSTEADY_LINE
        }

    fun tap() {
        val at = now()
        val last = taps.lastOrNull()
        var next = if (last != null && (secondsBetween(last, at) > HeartTap.RESET_AFTER || at.isBefore(last))) emptyList() else taps
        next = (next + at).takeLast(HeartTap.MAX_INTERVALS + 1)
        taps = next
        bpm = HeartTap.bpm(next)
        notifyListeners()
    }

    fun reset() {
        taps = emptyList()
        bpm = null
        notifyListeners()
    }

    companion object {
        const val TITLE = "Heart rate"
        const val TAP_LABEL = "Tap with each heartbeat"
        const val IDLE_LINE = "Find your pulse on your wrist or neck, then tap the heart with each beat."
        const val UNSTEADY_LINE = "Keep tapping in time with your pulse."
    }
}
