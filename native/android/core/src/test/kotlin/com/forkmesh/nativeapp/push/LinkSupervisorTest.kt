package com.forkmesh.nativeapp.push

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class LinkSupervisorTest {
    private var now = 1_000_000L
    private lateinit var supervisor: LinkSupervisor

    private fun fresh() {
        now = 1_000_000L
        supervisor = LinkSupervisor(clock = { now }, catchUpIntervalMs = 600_000)
    }

    fun testANeverConnectedProjectNeedsACatchUpRead() {
        fresh()
        expect(supervisor.catchUpDue("a"))
        supervisor.markCaughtUp("a")
        expect(!supervisor.catchUpDue("a"))
        now += 600_000
        expect(supervisor.catchUpDue("a"))
    }

    fun testAConnectedSocketNeedsNoCatchUp() {
        fresh()
        supervisor.onConnected("a")
        assertEq(setOf("a"), supervisor.connected())
        now += 3_600_000
        expect(!supervisor.catchUpDue("a"))
    }

    fun testADroppedSocketCountsDowntimeAndWaitsOutTheInterval() {
        fresh()
        supervisor.onConnected("a")
        now += 1_000
        supervisor.onDropped("a")
        now += 5_000
        assertEq(5_000L, supervisor.downForMs("a"))
        supervisor.onDropped("a")
        assertEq(5_000L, supervisor.downForMs("a"))
        expect(!supervisor.catchUpDue("a"))
        now += 600_000
        expect(supervisor.catchUpDue("a"))
        supervisor.onConnected("a")
        assertEq(0L, supervisor.downForMs("a"))
    }

    fun testASignedOutProjectGoesDarkUntilItConnectsAgain() {
        fresh()
        supervisor.onConnected("a")
        supervisor.onSignedOut("a")
        expect(supervisor.isSignedOut("a"))
        expect(supervisor.connected().isEmpty())
        expect(!supervisor.catchUpDue("a"))
        supervisor.onConnected("a")
        expect(!supervisor.isSignedOut("a"))
    }

    fun testForgetClearsEverything() {
        fresh()
        supervisor.onSignedOut("a")
        supervisor.forget("a")
        expect(!supervisor.isSignedOut("a"))
        expect(supervisor.catchUpDue("a"))
    }
}
