package com.forkmesh.nativeapp.control

import com.forkmesh.nativeapp.agents.AgentChipSlot
import com.forkmesh.nativeapp.agents.AgentDeleteTarget
import com.forkmesh.nativeapp.agents.AgentLineChips
import com.forkmesh.nativeapp.agents.AgentReadState
import com.forkmesh.nativeapp.agents.AgentSwipeDelete
import com.forkmesh.nativeapp.model.AgentSession
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class OtherDesktopAgentsTest {
    private fun session(id: Int, status: String = "running", at: Long = 0) =
        AgentSession(id = id, status = status, createdAtMs = at, startedAtMs = at, promptTokens = 400, completionTokens = 30)

    private val snapshot = LocalAgentsSnapshot(
        sessions = listOf(
            LocalAgentRow("me", "app", session(5, "done", 10), prompt = "Old work"),
            LocalAgentRow("me", "app", session(6, "running", 20), prompt = "Live work", tokensIn = 900, tokensOut = 70),
        ),
    )

    fun testAnotherDesktopsRowsAreTaggedWithItsKeyAndName() {
        val other = OtherDesktopAgents.listed(snapshot, "KEY2", "studio", ControlTransport.Relay)
        assertEq(listOf("KEY2:desktop:6", "KEY2:desktop:5"), other.rows.map { it.id })
        expect(other.rows.all { it.elsewhere && it.desktopKey == "KEY2" && it.node == "studio" })
        assertEq(1, other.running)
        assertEq(900, other.rows[0].tokensIn)
        assertEq(30, other.rows[1].tokensOut)
        assertEq("studio · relay", other.rows[0].badgeLabel)
        val host = AgentLineChips.chips(AgentLineChips.facts(other.rows[0]), 0).first { it.slot == AgentChipSlot.Host }
        assertEq("studio", host.word)
    }

    fun testOtherDesktopRowsNeverMatchTheActiveDesktopsSessions() {
        val other = OtherDesktopAgents.listed(snapshot, "KEY2", "studio", ControlTransport.Relay).rows
        val mine = UnifiedAgentRow.fromDesktop(LocalAgentRow("me", "app", session(6)), ControlTransport.Lan, "laptop")
        val rows = listOf(mine) + other
        assertEq(mine, liveDesktopRow(rows, "app", 6))
        assertEq(other.map { it.id }.toSet(), desktopAgents(rows, listOf("app" to 6)).map { it.id }.toSet())
        assertEq(0, AgentLineChips.facts(other[0]).desktopSessionId)
        assertEq(6, AgentLineChips.facts(mine).desktopSessionId)
        val read = AgentReadState.facts(other[0])
        assertEq("desktop:KEY2:app#6", read?.key)
        assertEq(0, read?.desktopId)
        assertEq(AgentDeleteTarget.Desktop(6, "app", "KEY2"), AgentSwipeDelete.forRow(other[0]))
        assertEq(AgentDeleteTarget.Desktop(6, "app", ""), AgentSwipeDelete.forRow(mine))
    }

    fun testARelayRunIsFoldedIntoTheDesktopThatRunsIt() {
        val other = OtherDesktopAgents.listed(snapshot, "KEY2", "studio", ControlTransport.Relay).rows
        val mine = UnifiedAgentRow.fromDesktop(LocalAgentRow("me", "app", session(6)), ControlTransport.Lan, "laptop")
        val onStudio = UnifiedAgentRow(
            id = "node:abc", badge = AgentTransportBadge.Node, title = "t", repoOwner = "me", repoName = "app",
            node = "studio", status = "running", statusLabel = "Running", isActive = true, localId = 5, cloudSessionId = "abc",
        )
        val onLaptop = onStudio.copy(id = "node:def", node = "laptop", localId = 6, cloudSessionId = "def")
        val elsewhere = onStudio.copy(id = "node:ghi", node = "other", localId = 6, cloudSessionId = "ghi")
        val merged = mergeAgentRows(listOf(mine) + other, listOf(onStudio, onLaptop, elsewhere), "laptop")
        expect(merged.none { it.id == "node:abc" || it.id == "node:def" })
        expect(merged.any { it.id == "node:ghi" })
    }

    fun testAnAlertsIdFindsItsRowOnAnyDesktopOrTheQueue() {
        val other = OtherDesktopAgents.listed(snapshot, "KEY2", "studio", ControlTransport.Relay).rows
        val run = UnifiedAgentRow(
            id = "node:abc", badge = AgentTransportBadge.Runner, title = "t", repoOwner = "me", repoName = "app",
            node = "box", status = "running", statusLabel = "Running", isActive = true, cloudSessionId = "abc",
        )
        val rows = other + run
        assertEq("KEY2:desktop:6", agentRowForRoute("desktop:6", rows)?.id)
        assertEq(run, agentRowForRoute("node:abc", rows))
        assertEq(null, agentRowForRoute("desktop:9", rows))
        val twice = rows + OtherDesktopAgents.listed(snapshot, "KEY3", "den", ControlTransport.Relay).rows
        assertEq(null, agentRowForRoute("desktop:6", twice))
        assertEq("KEY3:desktop:6", agentRowForRoute("KEY3:desktop:6", twice)?.id)
        assertEq("KEY3:desktop:6", agentRouteRowId("KEY3:desktop:6"))
        assertEq("desktop:6", agentRouteRowId("6"))
    }
}
