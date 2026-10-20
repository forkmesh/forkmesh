package com.forkmesh.nativeapp.health

import kotlin.math.max
import kotlin.math.min
import kotlin.math.sqrt

enum class StrideGait { Still, Walking, Running }

data class MotionSample(val x: Double, val y: Double, val z: Double, val time: Double)

data class StrideReading(val cadence: Double = 0.0, val gait: StrideGait = StrideGait.Still, val impact: Double = 0.0)

class StrideMeter {
    private var gravity: Double? = null
    private var smooth = 0.0
    private var previous = 0.0
    private var previousTime = 0.0
    private var rising = false
    private var armed = true
    private var peakAverage = 0.0
    private val steps = ArrayList<Double>()
    private var lastTime: Double? = null

    var reading = StrideReading()
        private set

    fun reset() {
        gravity = null
        smooth = 0.0
        previous = 0.0
        previousTime = 0.0
        rising = false
        armed = true
        peakAverage = 0.0
        steps.clear()
        lastTime = null
        reading = StrideReading()
    }

    val threshold: Double get() = max(MIN_PEAK, peakAverage * 0.45)

    fun add(sample: MotionSample): StrideReading {
        val last = lastTime
        if (last != null && sample.time <= last) return reading
        val dt = if (last != null) min(0.5, sample.time - last) else SAMPLE_INTERVAL
        lastTime = sample.time
        val magnitude = sqrt(sample.x * sample.x + sample.y * sample.y + sample.z * sample.z)
        val base = gravity ?: magnitude
        val gravityNow = base + (magnitude - base) * min(1.0, dt / 1.0)
        gravity = gravityNow
        val dynamic = magnitude - gravityNow
        val value = smooth + (dynamic - smooth) * min(1.0, dt / 0.04)
        smooth = value
        if (value < 0) armed = true
        if (rising && value < previous && armed && previous >= threshold) step(previousTime, previous)
        rising = value > previous
        previous = value
        previousTime = sample.time
        settle(sample.time)
        return reading
    }

    private fun step(time: Double, height: Double) {
        if (steps.isNotEmpty() && time - steps.last() < MIN_STEP_GAP) return
        armed = false
        if (steps.isNotEmpty() && time - steps.last() > MAX_STEP_GAP) steps.clear()
        steps.add(time)
        steps.removeAll { time - it > WINDOW }
        while (steps.size > RECENT_STEPS) steps.removeAt(0)
        peakAverage = if (peakAverage == 0.0) height else peakAverage + (height - peakAverage) * 0.25
    }

    private fun settle(time: Double) {
        if (steps.size < 3) {
            if (steps.isNotEmpty() && time - steps.last() > MAX_STEP_GAP) steps.clear()
            reading = StrideReading(impact = peakAverage)
            return
        }
        val last = steps.last()
        val span = last - steps[0]
        val cadence = if (span > 0) (steps.size - 1).toDouble() / span else 0.0
        val gap = if (cadence > 0) 1 / cadence else MAX_STEP_GAP
        if (time - last > min(MAX_STEP_GAP, max(1.0, gap * 2.5))) {
            steps.clear()
            peakAverage *= 0.5
            reading = StrideReading(impact = peakAverage)
            return
        }
        reading = StrideReading(cadence = min(4.0, cadence), gait = gait(cadence), impact = peakAverage)
    }

    private fun gait(cadence: Double): StrideGait {
        if (reading.gait == StrideGait.Running) {
            return if (cadence >= RUN_LEAVE_CADENCE && peakAverage >= RUN_LEAVE_IMPACT) StrideGait.Running else StrideGait.Walking
        }
        val hard = cadence >= RUN_ENTER_CADENCE && peakAverage >= RUN_ENTER_IMPACT
        val fast = cadence >= RUN_FAST_CADENCE && peakAverage >= RUN_FAST_IMPACT
        return if (hard || fast) StrideGait.Running else StrideGait.Walking
    }

    companion object {
        const val SAMPLE_INTERVAL = 1.0 / 50
        const val MIN_PEAK = 0.08
        const val MIN_STEP_GAP = 0.25
        const val MAX_STEP_GAP = 2.0
        const val WINDOW = 4.0
        const val RECENT_STEPS = 6
        const val RUN_ENTER_CADENCE = 2.2
        const val RUN_ENTER_IMPACT = 1.0
        const val RUN_FAST_CADENCE = 2.6
        const val RUN_FAST_IMPACT = 0.6
        const val RUN_LEAVE_CADENCE = 2.0
        const val RUN_LEAVE_IMPACT = 0.75
    }
}
