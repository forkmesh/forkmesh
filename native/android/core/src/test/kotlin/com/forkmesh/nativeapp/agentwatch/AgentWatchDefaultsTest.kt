package com.forkmesh.nativeapp.agentwatch

import com.forkmesh.nativeapp.testing.assertEq

class AgentWatchDefaultsTest {
    fun testRemoveAnimationsCountsAsReducedMotion() {
        assertEq(true, AgentWatchDefaults.reducedMotion(0f))
        assertEq(true, AgentWatchDefaults.reducedMotion(1f, animatorsEnabled = false))
        assertEq(false, AgentWatchDefaults.reducedMotion(1f))
        assertEq(false, AgentWatchDefaults.reducedMotion(0.5f))
        assertEq(false, AgentWatchDefaults.reducedMotion(null))
    }

    fun testFeelIsOffByDefaultUnderReducedMotion() {
        assertEq(false, AgentWatchDefaults.feel(null, reducedMotion = true))
        assertEq(true, AgentWatchDefaults.feel(null, reducedMotion = false))
    }

    fun testAnExplicitChoiceWins() {
        assertEq(true, AgentWatchDefaults.feel(true, reducedMotion = true))
        assertEq(false, AgentWatchDefaults.feel(false, reducedMotion = false))
    }
}
