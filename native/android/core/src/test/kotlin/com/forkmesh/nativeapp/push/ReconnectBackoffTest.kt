package com.forkmesh.nativeapp.push

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class ReconnectBackoffTest {
    fun testDoublesFromTheInitialDelayUpToTheCap() {
        val backoff = ReconnectBackoff(initialMs = 2_000, maxMs = 30_000, jitter = 0.3, random = { 0.5 })
        val delays = List(7) { backoff.nextDelayMs() }
        assertEq(listOf(2_000L, 4_000L, 8_000L, 16_000L, 30_000L, 30_000L, 30_000L), delays)
        assertEq(7, backoff.failures)
    }

    fun testJitterSpreadsEachDelayWithinItsBand() {
        val low = ReconnectBackoff(initialMs = 1_000, maxMs = 60_000, jitter = 0.3, random = { 0.0 })
        val high = ReconnectBackoff(initialMs = 1_000, maxMs = 60_000, jitter = 0.3, random = { 1.0 })
        assertEq(700L, low.nextDelayMs())
        assertEq(1_300L, high.nextDelayMs())
        assertEq(1_400L, low.nextDelayMs())
        assertEq(2_600L, high.nextDelayMs())
    }

    fun testTheCapHoldsEvenWithFullJitter() {
        val backoff = ReconnectBackoff(initialMs = 1_000, maxMs = 5_000, jitter = 1.0, random = { 1.0 })
        repeat(10) { expect(backoff.nextDelayMs() <= 5_000L) }
    }

    fun testResetReturnsToTheInitialDelay() {
        val backoff = ReconnectBackoff(initialMs = 2_000, maxMs = 300_000, jitter = 0.0)
        repeat(5) { backoff.nextDelayMs() }
        backoff.reset()
        assertEq(0, backoff.failures)
        assertEq(2_000L, backoff.nextDelayMs())
    }

    fun testDefaultsAreTwoSecondsToFiveMinutes() {
        val backoff = ReconnectBackoff(jitter = 0.0)
        val delays = List(12) { backoff.nextDelayMs() }
        assertEq(2_000L, delays.first())
        assertEq(300_000L, delays.last())
    }

    fun testRejectsNonsenseBounds() {
        assertFails<IllegalArgumentException> { ReconnectBackoff(initialMs = 0) }
        assertFails<IllegalArgumentException> { ReconnectBackoff(initialMs = 10, maxMs = 5) }
        assertFails<IllegalArgumentException> { ReconnectBackoff(jitter = 2.0) }
    }
}
