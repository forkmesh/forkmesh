package com.forkmesh.nativeapp.health

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.sin

fun strideSignal(from: Double, seconds: Double, cadence: Double, impact: Double): List<MotionSample> {
    val count = (seconds / StrideMeter.SAMPLE_INTERVAL).toInt()
    var seed = 0x9E3779B97F4A7C15UL.toLong()
    return (0 until count).map { index ->
        val t = from + index * StrideMeter.SAMPLE_INTERVAL
        seed = seed * 6364136223846793005L + 1442695040888963407L
        val noise = ((seed ushr 40).toDouble() / (1 shl 24).toDouble() - 0.5) * 0.02
        val bounce = if (cadence > 0) impact * sin(2 * PI * cadence * (t - from)) else 0.0
        MotionSample(0.04 + noise, -(1 + bounce), 0.1 - noise, t)
    }
}

class StrideMeterTest {
    private fun run(meter: StrideMeter, samples: List<MotionSample>): StrideReading {
        var reading = meter.reading
        for (sample in samples) reading = meter.add(sample)
        return reading
    }

    private fun near(expected: Double, actual: Double, accuracy: Double) =
        expect(abs(expected - actual) <= accuracy, "expected $expected within $accuracy but was $actual")

    fun testStillPhoneCountsNoSteps() {
        val reading = run(StrideMeter(), strideSignal(100.0, 6.0, 0.0, 0.0))
        assertEq(0.0, reading.cadence)
        assertEq(StrideGait.Still, reading.gait)
    }

    fun testWalkingCadenceFollowsTheBounce() {
        val reading = run(StrideMeter(), strideSignal(100.0, 6.0, 1.8, 0.35))
        near(1.8, reading.cadence, 0.05)
        assertEq(StrideGait.Walking, reading.gait)
    }

    fun testBriskWalkIsNotRunning() {
        val reading = run(StrideMeter(), strideSignal(100.0, 6.0, 2.3, 0.5))
        near(2.3, reading.cadence, 0.05)
        assertEq(StrideGait.Walking, reading.gait)
    }

    fun testRunningIsDetectedAndCadenceTracksInRealTime() {
        val meter = StrideMeter()
        var reading = run(meter, strideSignal(100.0, 5.0, 1.7, 0.3))
        assertEq(StrideGait.Walking, reading.gait)
        reading = run(meter, strideSignal(105.0, 3.0, 2.8, 1.5))
        near(2.8, reading.cadence, 0.1)
        assertEq(StrideGait.Running, reading.gait)
        expect(reading.impact > StrideMeter.RUN_ENTER_IMPACT, "impact ${reading.impact}")
    }

    fun testRunningEasesBackToWalkingAndStops() {
        val meter = StrideMeter()
        run(meter, strideSignal(100.0, 5.0, 2.8, 1.5))
        var reading = run(meter, strideSignal(105.0, 6.0, 1.8, 0.3))
        assertEq(StrideGait.Walking, reading.gait)
        near(1.8, reading.cadence, 0.05)
        reading = run(meter, strideSignal(111.0, 2.5, 0.0, 0.0))
        assertEq(0.0, reading.cadence)
        assertEq(StrideGait.Still, reading.gait)
    }

    fun testOutOfOrderSamplesAreIgnored() {
        val meter = StrideMeter()
        val before = run(meter, strideSignal(100.0, 4.0, 1.8, 0.35))
        val after = meter.add(MotionSample(0.0, -3.0, 0.0, 50.0))
        assertEq(before, after)
    }
}
