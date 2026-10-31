package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.control.AgentTransportBadge
import com.forkmesh.nativeapp.control.DesktopTokenReading
import com.forkmesh.nativeapp.control.UnifiedAgentRow
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class AgentTokenFlowTest {
    private fun row(id: String, badge: AgentTransportBadge, tokensIn: Int, tokensOut: Int) = UnifiedAgentRow(
        id = id,
        badge = badge,
        title = id,
        repoOwner = "me",
        repoName = "app",
        node = "",
        status = "running",
        statusLabel = "Running",
        isActive = true,
        tokensIn = tokensIn,
        tokensOut = tokensOut,
    )

    fun testPaceStepsAndInputCountsTenTimesLess() {
        assertEq(AgentTokenPace.Idle, AgentTokenPace.of(0.5, input = false))
        assertEq(AgentTokenPace.Slow, AgentTokenPace.of(5.0, input = false))
        assertEq(AgentTokenPace.Steady, AgentTokenPace.of(30.0, input = false))
        assertEq(AgentTokenPace.Fast, AgentTokenPace.of(100.0, input = false))
        assertEq(AgentTokenPace.Blazing, AgentTokenPace.of(500.0, input = false))
        assertEq(AgentTokenPace.Slow, AgentTokenPace.of(100.0, input = true))
        assertEq(0.0, AgentTokenPace.travel(0.5, input = false))
        expect(AgentTokenPace.travel(1_000_000.0, input = false) <= AgentTokenPace.MAX_TRAVEL)
    }

    fun testSamplerMeasuresGrowthFadesAndSettles() {
        val sampler = AgentTokenSampler()
        sampler.sample(1_000, 10, 0)
        assertEq(0.0, sampler.outPerSecond)
        sampler.sample(3_000, 110, 2_000)
        assertEq(1_000.0, sampler.inPerSecond)
        assertEq(50.0, sampler.outPerSecond)
        assertEq(AgentTokenPace.Steady, sampler.outPace)
        sampler.hold(3_500)
        expect(sampler.outPerSecond < 50.0 && sampler.outPerSecond > 0.0, "a still second fades the rate")
        sampler.sample(3_000, 50, 4_000)
        assertEq(0.0, sampler.outPerSecond)
        sampler.sample(9_000, 150, 60_000)
        assertEq(12.5, sampler.outPerSecond)
        sampler.settle()
        assertEq(AgentTokenPace.Idle, sampler.inPace)
    }

    fun testClockMovesLanesOppositeWaysOnlyWhileRunning() {
        val clock = AgentTokenClock()
        clock.advance(10.0, running = true, inTravel = 4.0, outTravel = 4.0, wrap = 4.5)
        assertEq(0.0, clock.inPhase)
        clock.advance(10.1, running = true, inTravel = 4.0, outTravel = 4.0, wrap = 4.5)
        expect(clock.inPhase < 0 && clock.outPhase > 0)
        val held = clock.outPhase
        clock.advance(20.0, running = false, inTravel = 4.0, outTravel = 4.0, wrap = 4.5)
        clock.advance(30.0, running = true, inTravel = 4.0, outTravel = 4.0, wrap = 4.5)
        assertEq(held, clock.outPhase)
    }

    fun testSeriesAddsEachSourcesGrowthAndIgnoresNewOrShrinking() {
        val series = AgentTokenSeries()
        series.tick(mapOf("a" to listOf(100L, 10L)))
        assertEq(listOf(0.0), series.input)
        series.tick(mapOf("a" to listOf(150L, 15L), "b" to listOf(999L, 999L)))
        assertEq(listOf(0.0, 50.0), series.input)
        assertEq(listOf(0.0, 5.0), series.output)
        series.tick(mapOf("a" to listOf(140L, 20L), "b" to listOf(1_000L, 1_000L)))
        assertEq(1.0, series.input.last())
        assertEq(6.0, series.output.last())
        repeat(80) { series.tick(emptyMap()) }
        assertEq(AgentTokenSeries.SECONDS, series.input.size)
        assertEq("Agent tokens in 0 a second, out 0 a second", AgentTokenSeries.summary(series))
    }

    fun testTotalsUseTheDesktopFeedInsteadOfItsRows() {
        val rows = listOf(
            row("desktop:1", AgentTransportBadge.DesktopLan, 10, 1),
            row("node:x", AgentTransportBadge.Runner, 20, 2),
        )
        val fed = AgentTokenSeries.totals(listOf(500L, 50L), rows)
        assertEq(setOf(AgentTokenSeries.DESKTOP_KEY, "row:node:x"), fed.keys)
        val unfed = AgentTokenSeries.totals(emptyList(), rows)
        assertEq(listOf(10L, 1L), unfed["row:desktop:1"])
        assertEq(listOf(20L, 2L), unfed["row:node:x"])
    }

    fun testVitalsEventCarriesTotalsAndRunningAgents() {
        val reading = DesktopTokenReading.parse(
            mapOf(
                "memory" to 40,
                "tokensIn" to 1200.0,
                "tokensOut" to 80.0,
                "agentTokens" to mapOf("7" to listOf(1000.0, 60.0), "x" to listOf(1, 2), "8" to listOf(1)),
            ),
        )
        assertEq(listOf(1200L, 80L), reading?.totals)
        assertEq(mapOf(7 to listOf(1000L, 60L)), reading?.agents)
        assertEq(null, DesktopTokenReading.parse(mapOf("memory" to 40)))
    }

    fun testTileBadgesPutTheUrgentOnesFirst() {
        val facts = AgentChipFacts(
            status = AgentLineStatus.Running,
            conflicted = true,
            behind = 3,
            branch = "agent/1",
            hasWorktree = true,
            ahead = 2,
            prNumber = 9,
            cloud = true,
        )
        assertEq(
            listOf(
                AgentLineBadge.Conflicted, AgentLineBadge.Worktree, AgentLineBadge.Branch,
                AgentLineBadge.Ahead, AgentLineBadge.Pull, AgentLineBadge.Cloud,
            ),
            AgentLineBadge.of(facts),
        )
        assertEq(listOf(AgentLineBadge.Behind, AgentLineBadge.Runner), AgentLineBadge.of(AgentChipFacts(AgentLineStatus.Running, behind = 1, runner = true, cloud = true)))
    }
}
