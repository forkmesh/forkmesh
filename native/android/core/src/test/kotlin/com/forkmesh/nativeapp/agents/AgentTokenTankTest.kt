package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import kotlin.math.hypot

class AgentTokenTankTest {
    private fun run(tank: AgentTokenTank, seconds: Double) {
        repeat((seconds * 30).toInt()) { tank.advance(1.0 / 30) }
    }

    private fun meanX(tank: AgentTokenTank): Double = (0 until tank.grains).sumOf { tank.xs[it] } / tank.grains

    private fun inside(tank: AgentTokenTank) {
        for (i in 0 until tank.grains) {
            expect(tank.xs[i] in 0.0..tank.width && tank.ys[i] in 0.0..tank.height, "grain $i left the tank")
        }
    }

    fun testDroppedGrainsRainInOverASecond() {
        val tank = AgentTokenTank(60.0, 12.0, leakSeconds = 1e9)
        expect(tank.idle)
        tank.drop(30.0)
        expect(!tank.idle)
        run(tank, 0.5)
        expect(tank.grains in 10..20, "half a second rained ${tank.grains}")
        run(tank, 0.6)
        assertEq(30, tank.grains)
    }

    fun testGrainsSettleOnTheFloor() {
        val tank = AgentTokenTank(60.0, 12.0, leakSeconds = 1e9)
        tank.drop(240.0)
        run(tank, 5.0)
        assertEq(240, tank.grains)
        inside(tank)
        for (i in 0 until tank.grains) expect(tank.ys[i] > 6.0, "grain $i floats at ${tank.ys[i]}")
    }

    fun testTiltPoursToTheLowSide() {
        val tank = AgentTokenTank(60.0, 12.0, leakSeconds = 1e9)
        tank.drop(200.0)
        run(tank, 3.0)
        expect(meanX(tank) in 25.0..35.0, "level tank centred at ${meanX(tank)}")
        tank.tilt(1.0, 0.0)
        run(tank, 4.0)
        expect(meanX(tank) > 45.0, "tilted right sits at ${meanX(tank)}")
        tank.tilt(-1.0, 0.0)
        run(tank, 4.0)
        expect(meanX(tank) < 15.0, "tilted left sits at ${meanX(tank)}")
        inside(tank)
    }

    fun testOverflowStopsAtCapacity() {
        val tank = AgentTokenTank(60.0, 12.0, leakSeconds = 1e9)
        assertEq(720, tank.capacity)
        tank.drop(5_000.0)
        run(tank, 3.0)
        assertEq(tank.capacity, tank.grains)
        inside(tank)
    }

    fun testLeakEmptiesTheTankOnceTokensStop() {
        val tank = AgentTokenTank(60.0, 12.0)
        tank.drop(200.0)
        run(tank, 1.5)
        val full = tank.grains
        run(tank, 10.0)
        expect(tank.grains < full, "no leak: ${tank.grains} of $full")
        run(tank, 120.0)
        assertEq(0, tank.grains)
        expect(tank.idle)
    }

    fun testPourScalesTheSecondToThePeak() {
        val peak = AgentTokenTank(60.0, 12.0)
        peak.pour(1_000.0, 1_000.0)
        run(peak, 1.1)
        expect(peak.grains in 55..60, "a peak second rained ${peak.grains}")
        val over = AgentTokenTank(60.0, 12.0)
        over.pour(9_000.0, 1_000.0)
        run(over, 1.1)
        assertEq(peak.grains, over.grains)
        val quarter = AgentTokenTank(60.0, 12.0)
        quarter.pour(250.0, 1_000.0)
        run(quarter, 1.1)
        expect(quarter.grains in 13..15, "a quarter second rained ${quarter.grains}")
        val none = AgentTokenTank(60.0, 12.0)
        none.pour(0.0, 1_000.0)
        expect(none.idle)
        assertEq(1_000.0, AgentTokenTank.scale(listOf(10.0, 400.0), 1_000.0))
        assertEq(4_000.0, AgentTokenTank.scale(listOf(10.0, 4_000.0), 1_000.0))
        assertEq(50.0, AgentTokenTank.scale(emptyList(), 50.0))
    }

    fun testTiltKeepsAFlatPhoneFallingDownAndCapsAShake() {
        val tank = AgentTokenTank(60.0, 12.0)
        tank.tilt(0.0, 1.0)
        assertEq(0.0, tank.gravityX)
        assertEq(AgentTokenTank.GRAVITY, tank.gravityY)
        tank.tilt(0.0, 0.0)
        expect(tank.gravityY > 0 && tank.gravityX == 0.0)
        tank.tilt(0.0, -1.0)
        expect(tank.gravityY < 0)
        tank.tilt(20.0, 0.0)
        expect(kotlin.math.abs(hypot(tank.gravityX, tank.gravityY) - AgentTokenTank.GRAVITY * AgentTokenTank.MAX_PULL) < 1e-9)
    }

    fun testEmptyTankIgnoresEverything() {
        val tank = AgentTokenTank(0.0, 12.0)
        assertEq(0, tank.capacity)
        tank.drop(10.0)
        tank.pour(100.0, 10.0)
        run(tank, 1.0)
        assertEq(0, tank.grains)
    }

    fun testThirtyFramesASecondRunTheSameSimulationAsTheDisplayRate() {
        assertEq(33_333_333L, AgentTokenTank.FRAME_NANOS)
        val paced = AgentTokenTank(60.0, 12.0, seed = 1L)
        val fast = AgentTokenTank(60.0, 12.0, seed = 1L)
        for (tank in listOf(paced, fast)) {
            tank.drop(300.0)
            tank.tilt(0.4, 0.1)
        }
        repeat(32 * 3) { paced.advance(1.0 / 32) }
        repeat(128 * 3) { fast.advance(1.0 / 128) }
        assertEq(fast.grains, paced.grains)
        for (i in 0 until paced.grains) {
            assertEq(fast.xs[i], paced.xs[i])
            assertEq(fast.ys[i], paced.ys[i])
        }
    }
}
