package com.forkmesh.nativeapp.health

import com.forkmesh.nativeapp.testing.assertEq
import java.time.Instant

class HeartTapTest {
    private val origin = Instant.ofEpochSecond(1_800_000_000)

    private fun times(gaps: List<Double>): List<Instant> {
        var at = origin
        val result = arrayListOf(at)
        for (gap in gaps) {
            at = at.plusMillis((gap * 1000).toLong())
            result.add(at)
        }
        return result
    }

    private fun model(): Pair<HeartTapModel, (Double) -> Unit> {
        var clock = origin
        return HeartTapModel { clock } to { seconds -> clock = clock.plusMillis((seconds * 1000).toLong()) }
    }

    fun testSteadyTapsGiveTheRate() {
        assertEq(60, HeartTap.bpm(times(listOf(1.0, 1.0, 1.0, 1.0))))
        assertEq(75, HeartTap.bpm(times(listOf(0.8, 0.8, 0.8, 0.8, 0.8))))
    }

    fun testTooFewTapsGiveNothing() {
        assertEq(null, HeartTap.bpm(times(listOf(1.0, 1.0, 1.0))))
        assertEq(null, HeartTap.bpm(emptyList()))
    }

    fun testAMissedBeatIsLeftOutAndOddRatesAreRejected() {
        assertEq(60, HeartTap.bpm(times(listOf(1.0, 1.0, 2.0, 1.0, 1.0))))
        assertEq(null, HeartTap.bpm(times(listOf(0.2, 0.2, 0.2, 0.2))))
        assertEq(null, HeartTap.bpm(times(listOf(2.4, 2.4, 2.4, 2.4))))
    }

    fun testOnlyTheLatestIntervalsCount() {
        val gaps = List(4) { 1.0 } + List(HeartTap.MAX_INTERVALS) { 0.5 }
        assertEq(120, HeartTap.bpm(times(gaps)))
    }

    fun testTappingCountsDownThenShowsTheRateAndAPauseStartsOver() {
        val (heart, advance) = model()
        assertEq(HeartTapModel.IDLE_LINE, heart.line)
        heart.tap()
        assertEq("Keep going: 4 more taps.", heart.line)
        repeat(3) {
            advance(0.75)
            heart.tap()
        }
        assertEq("Keep going: 1 more tap.", heart.line)
        advance(0.75)
        heart.tap()
        assertEq(80, heart.bpm)
        assertEq("80 BPM from 5 taps.", heart.line)
        advance(5.0)
        heart.tap()
        assertEq(null, heart.bpm)
        assertEq(1, heart.taps.size)
        heart.reset()
        assertEq(HeartTapModel.IDLE_LINE, heart.line)
    }
}
