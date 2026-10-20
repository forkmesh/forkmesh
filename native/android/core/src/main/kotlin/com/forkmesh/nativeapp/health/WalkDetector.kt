package com.forkmesh.nativeapp.health

import java.time.Instant
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

internal fun secondsBetween(from: Instant, to: Instant): Double = (to.toEpochMilli() - from.toEpochMilli()) / 1000.0

enum class WalkMotion {
    Walking, Running, Still, Unknown;

    val moving: Boolean get() = this == Walking || this == Running
}

data class WalkMotionSample(val motion: WalkMotion, val confident: Boolean, val date: Instant)

sealed class WalkEvent {
    data class Started(val at: Instant) : WalkEvent()
    data class Ended(val at: Instant) : WalkEvent()
}

class WalkDetector {
    var walkStartedAt: Instant? = null
        private set
    var movingSince: Instant? = null
        private set
    var stillSince: Instant? = null
        private set

    val walking: Boolean get() = walkStartedAt != null

    val nextCheck: Instant?
        get() = if (walking) stillSince?.plusMillis((END_AFTER * 1000).toLong()) else movingSince?.plusMillis((START_AFTER * 1000).toLong())

    fun copy(): WalkDetector {
        val other = WalkDetector()
        other.walkStartedAt = walkStartedAt
        other.movingSince = movingSince
        other.stillSince = stillSince
        return other
    }

    private fun clear() {
        walkStartedAt = null
        movingSince = null
        stillSince = null
    }

    fun observe(sample: WalkMotionSample, now: Instant): WalkEvent? {
        when (sample.motion) {
            WalkMotion.Unknown -> Unit
            WalkMotion.Walking, WalkMotion.Running -> if (sample.confident) {
                stillSince = null
                if (!walking && movingSince == null) movingSince = sample.date
            }
            WalkMotion.Still -> {
                val started = walkStartedAt
                if (started != null) {
                    if (stillSince == null) stillSince = if (sample.date.isAfter(started)) sample.date else started
                } else {
                    movingSince = null
                }
            }
        }
        return tick(now)
    }

    fun forgetPendingStart() {
        movingSince = null
    }

    fun tick(now: Instant): WalkEvent? {
        if (walking) {
            val still = stillSince ?: return null
            if (secondsBetween(still, now) < END_AFTER) return null
            clear()
            return WalkEvent.Ended(still)
        }
        val moving = movingSince ?: return null
        if (secondsBetween(moving, now) < START_AFTER) return null
        walkStartedAt = moving
        movingSince = null
        stillSince = null
        return WalkEvent.Started(moving)
    }

    fun replay(samples: List<WalkMotionSample>, until: Instant): WalkEvent? {
        for (sample in samples.sortedBy { it.date }) {
            val event = tick(sample.date) ?: observe(sample, sample.date)
            if (event != null) return event
        }
        return tick(until)
    }

    override fun equals(other: Any?): Boolean =
        other is WalkDetector && other.walkStartedAt == walkStartedAt && other.movingSince == movingSince && other.stillSince == stillSince

    override fun hashCode(): Int = listOf(walkStartedAt, movingSince, stillSince).hashCode()

    companion object {
        const val START_AFTER = 30.0
        const val END_AFTER = 3 * 60.0
    }
}

data class WalkProgress(
    val startedAt: Instant,
    var baseline: Int = 0,
    var steps: Int = 0,
    var distanceM: Double = 0.0,
) {
    val todaySteps: Int get() = baseline + steps

    fun record(steps: Int, distanceM: Double) {
        this.steps = max(this.steps, steps)
        this.distanceM = max(this.distanceM, distanceM)
    }
}

class WalkPace {
    var cadence = 0.0
        private set
    var updatedAt: Instant? = null
        private set
    private var lastSteps: Int? = null
    private var lastAt: Instant? = null

    val moving: Boolean get() = cadence > 0
    val stepsPerMinute: Int get() = (cadence * 60).roundToInt()
    val running: Boolean get() = StepsText.gait(stepsPerMinute) == "Running"
    val expiresAt: Instant? get() = if (moving) updatedAt?.plusMillis((STALE_AFTER * 1000).toLong()) else null

    fun record(steps: Int, cadence: Double, at: Instant) {
        var rate = cadence
        val previousSteps = lastSteps
        val previousAt = lastAt
        if (rate <= 0 && previousSteps != null && previousAt != null && steps > previousSteps) {
            val elapsed = secondsBetween(previousAt, at)
            if (elapsed >= 1 && elapsed <= STALE_AFTER * 2) rate = (steps - previousSteps) / elapsed
        }
        lastSteps = steps
        lastAt = at
        this.cadence = if (rate >= MIN_CADENCE) min(rate, MAX_CADENCE) else 0.0
        updatedAt = at
    }

    fun expire(now: Instant): Boolean {
        val expires = expiresAt ?: return false
        if (now.isBefore(expires)) return false
        cadence = 0.0
        return true
    }

    companion object {
        const val STALE_AFTER = 10.0
        const val MIN_CADENCE = 0.5
        const val MAX_CADENCE = 4.0
    }
}
