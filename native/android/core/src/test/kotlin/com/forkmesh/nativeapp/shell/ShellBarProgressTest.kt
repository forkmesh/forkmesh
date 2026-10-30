package com.forkmesh.nativeapp.shell

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class ShellBarProgressTest {
    fun testTheRingFillsWithElapsedOverExpectedAndStopsAt99() {
        val run = ShellBarProgress(startedAtMs = 1_000, expectedMs = 10_000)
        assertEq(run.fraction(6_000), 0.5)
        assertEq(run.fraction(500), 0.0)
        assertEq(run.fraction(60_000), 0.99)
        assertEq(run.spoken(6_000), "Running, 50 percent")
    }

    fun testARunWithNoEarlierDurationSpins() {
        val run = ShellBarProgress(startedAtMs = 1_000, expectedMs = -1)
        expect(!run.estimated)
        assertEq(run.fraction(6_000), null)
        assertEq(run.spoken(6_000), "Running")
    }

    fun testTheRunExpectedToFinishLastWinsUnlessOneCannotBeEstimated() {
        val early = ShellBarProgress(1_000, 5_000)
        val late = ShellBarProgress(2_000, 9_000)
        val unknown = ShellBarProgress(3_000, 0)
        assertEq(ShellBarProgress.latest(listOf(early, late)), late)
        assertEq(ShellBarProgress.latest(listOf(early, unknown, late)), unknown)
        assertEq(ShellBarProgress.latest(emptyList()), null)
    }
}
