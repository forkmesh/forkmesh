package com.forkmesh.nativeapp.control

import com.forkmesh.nativeapp.agents.AgentLaunchSession
import com.forkmesh.nativeapp.agents.PromptAttachment
import com.forkmesh.nativeapp.model.AgentSession
import com.forkmesh.nativeapp.testing.assertEq

class AgentStopAllTest {
    private class FakeControl(override val rows: List<UnifiedAgentRow>, private val refuse: Set<String> = emptySet()) : AgentControlling {
        val stopped = ArrayList<String>()
        override val promptNotice: String = ""

        override fun refresh() {}

        override fun stop(row: UnifiedAgentRow) {
            if (row.id in refuse) throw LocalControlException("not_connected")
            stopped.add(row.id)
        }

        override fun prompt(row: UnifiedAgentRow, text: String, images: List<PromptAttachment>) {}

        override fun continueRun(row: UnifiedAgentRow) {}

        override fun transcript(row: UnifiedAgentRow, since: Int): AgentTranscriptDelta = throw UnsupportedOperationException()

        override fun startOnDesktop(owner: String, name: String, task: String, provider: String, images: List<PromptAttachment>): Int = 0
    }

    private fun cloud(id: String, status: String) = UnifiedAgentRow.fromCloud(AgentLaunchSession(sessionId = id, owner = "alice", repo = "site", status = status))

    private fun desk(id: Int, status: String, stoppable: Boolean) = UnifiedAgentRow.fromDesktop(
        LocalAgentRow("alice", "app", AgentSession(id = id, status = status), stoppable = stoppable),
        ControlTransport.Lan,
        "desk",
    )

    fun testStopAllStopsEveryRunningAgentAndNothingElse() {
        val outside = desk(7, "running", stoppable = true).copy(outside = true)
        val control = FakeControl(listOf(cloud("running", "running"), cloud("queued", "queued"), cloud("done", "completed"), outside, desk(8, "running", stoppable = true)))
        assertEq(listOf("node:running", "node:queued", "desktop:8"), AgentStopAll.targets(control.rows).map { it.id })
        val outcome = control.stopAll()
        assertEq(listOf("node:running", "node:queued", "desktop:8"), control.stopped)
        assertEq(AgentStopAllOutcome(3, 0), outcome)
        assertEq("Asked 3 agents to stop.", outcome.message)
    }

    fun testStopAllSaysWhatFailed() {
        val control = FakeControl(listOf(cloud("a", "running"), cloud("b", "running"), cloud("c", "running")), refuse = setOf("node:b"))
        val outcome = control.stopAll()
        assertEq(AgentStopAllOutcome(2, 1, "The desktop is not connected."), outcome)
        assertEq("Asked 2 agents to stop; 1 did not. The desktop is not connected.", outcome.message)
        assertEq("Could not stop 2 agents. The desktop is not connected.", AgentStopAllOutcome(0, 2, "The desktop is not connected.").message)
        assertEq("No agents are running.", AgentStopAllOutcome(0, 0).message)
        assertEq("Asked 1 agent to stop.", AgentStopAllOutcome(1, 0).message)
        assertEq("Stop the running agent?", AgentStopAll.question(1))
        assertEq("Stop all 3 running agents?", AgentStopAll.question(3))
    }
}
