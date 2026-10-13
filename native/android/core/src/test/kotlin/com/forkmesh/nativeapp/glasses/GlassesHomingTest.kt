package com.forkmesh.nativeapp.glasses

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import kotlin.math.abs
import kotlin.math.cos

class GlassesHomingTest {
    private fun seen(address: String, at: Long, rssi: Int = -60) = GlassesSighting(address, "Glasses $address", GlassesMaker.LUXOTTICA, false, rssi, at)

    private fun near(a: Double, b: Double, within: Double) = abs(GlassesHoming.unwrap(a, b) - a) <= within

    private fun turnAround(homing: GlassesHoming, source: Double, from: Long = 0): Long {
        var at = from
        for (step in 0 until 120) {
            val heading = step * 3.0
            val rssi = -70 + Math.round(10 * cos(Math.toRadians(heading - source))).toInt()
            homing.hear("A", rssi, heading, at)
            at += 100
        }
        return at
    }

    fun testFindsTheDirectionOfTheStrongestSignal() {
        val homing = GlassesHoming()
        homing.follow(listOf(seen("A", 0)), 0)
        val end = turnAround(homing, 100.0)
        val reading = homing.reading(end)!!
        val bearing = reading.bearing!!
        expect(near(bearing, 100.0, 15.0), "bearing $bearing should point near 100")
        expect(reading.confidence >= 0.5, "a full turn with a clear peak is confident")
        expect(near(reading.arrow(10.0)!!, 90.0, 15.0), "facing 10 the arrow turns right by about 90")
        assertEq("Strongest signal this way.", reading.hint)
        expect(reading.live, "a signal heard just now is live")
    }

    fun testNoDirectionUntilEnoughHeadingsAreHeard() {
        val homing = GlassesHoming()
        homing.follow(listOf(seen("A", 0)), 0)
        for (i in 0 until 10) homing.hear("A", -60, 45.0, i * 100L)
        val reading = homing.reading(1_000)!!
        assertEq(null, reading.bearing)
        assertEq("Hold the phone in front of you and turn slowly to find the direction.", reading.hint)
        val blind = GlassesHoming()
        blind.follow(listOf(seen("A", 0)), 0)
        blind.hear("A", -60, null, 0)
        assertEq("No compass reading, so only closeness is shown.", blind.reading(10)?.hint)
    }

    fun testTrendFollowsTheSignal() {
        val homing = GlassesHoming()
        homing.follow(listOf(seen("A", 0)), 0)
        for (i in 0..10) homing.hear("A", -80, null, i * 500L)
        for (i in 11..20) homing.hear("A", -62, null, i * 500L)
        assertEq(GlassesTrend.CLOSER, homing.reading(10_000)?.trend)
        val away = GlassesHoming()
        away.follow(listOf(seen("A", 0)), 0)
        for (i in 0..10) away.hear("A", -55, null, i * 500L)
        for (i in 11..20) away.hear("A", -75, null, i * 500L)
        assertEq(GlassesTrend.FARTHER, away.reading(10_000)?.trend)
    }

    fun testClosenessAndLiveness() {
        assertEq(0.0, GlassesHoming.closeness(-100))
        assertEq(1.0, GlassesHoming.closeness(-40))
        assertEq(0.5, GlassesHoming.closeness(-70))
        val homing = GlassesHoming(liveMs = 5_000)
        homing.follow(listOf(seen("A", 0)), 0)
        homing.hear("A", -64, null, 0)
        expect(homing.reading(4_000)!!.live, "heard 4 s ago is still live")
        val stale = homing.reading(6_000)!!
        expect(!stale.live, "heard 6 s ago is lost")
        assertEq("Signal lost. Walk around to pick it up again.", stale.hint)
    }

    fun testSticksToItsTargetAndIgnoresOthers() {
        val homing = GlassesHoming(stickyMs = 10_000)
        homing.follow(listOf(seen("A", 0, -70)), 0)
        homing.hear("B", -40, 0.0, 100)
        assertEq(-70, homing.reading(200)?.rssi)
        homing.follow(listOf(seen("B", 1_000, -50), seen("A", 900, -70)), 1_000)
        assertEq("A", homing.target?.address)
        homing.follow(listOf(seen("B", 14_000, -50), seen("A", 900, -70)), 14_000)
        assertEq("B", homing.target?.address)
        homing.follow(emptyList(), 15_000)
        assertEq(null, homing.reading(15_000))
    }

    fun testAngleHelpers() {
        assertEq(350.0, GlassesHoming.normalize(-10.0))
        assertEq(370.0, GlassesHoming.unwrap(350.0, 10.0))
        assertEq(-10.0, GlassesHoming.unwrap(0.0, 350.0))
        val smoothed = GlassesHoming.smoothHeading(350.0, 10.0, 0.5)
        expect(near(smoothed, 0.0, 0.01), "smoothing across north stays near north, got $smoothed")
        assertEq("at 12 o'clock", GlassesHoming.clock(355.0))
        assertEq("at 3 o'clock", GlassesHoming.clock(92.0))
        assertEq("at 9 o'clock", GlassesHoming.clock(-90.0))
        val flatNorth = floatArrayOf(1f, 0f, 0f, 0f, 1f, 0f, 0f, 0f, 1f)
        expect(near(GlassesHoming.heading(flatNorth)!!, 0.0, 0.01), "a flat phone with its top to north faces north")
        val uprightEast = floatArrayOf(0f, 0f, -1f, -1f, 0f, 0f, 0f, 1f, 0f)
        expect(near(GlassesHoming.heading(uprightEast)!!, 90.0, 0.01), "an upright phone with its back to east faces east")
    }
}
