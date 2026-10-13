package com.forkmesh.nativeapp.control

import com.forkmesh.nativeapp.agents.AgentChipSlot
import com.forkmesh.nativeapp.agents.AgentLaunchMessage
import com.forkmesh.nativeapp.agents.AgentLaunchSession
import com.forkmesh.nativeapp.agents.AgentLineChips
import com.forkmesh.nativeapp.agents.RunnerDiffStats
import com.forkmesh.nativeapp.agents.RunnerMergeState
import com.forkmesh.nativeapp.model.AgentSession
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class UnifiedAgentRowBranchTest {
    fun testRowsCarryTheBranchTheirDiffCompares() {
        val desk = UnifiedAgentRow.fromDesktop(
            LocalAgentRow("alice", "app", AgentSession(id = 9, status = "completed", branchName = "agent/9-fix", baseRef = "main")),
            ControlTransport.Lan,
            "desk",
        )
        assertEq("agent/9-fix", desk.branchName)
        assertEq("main", desk.baseBranch)
        val node = UnifiedAgentRow.fromCloud(
            AgentLaunchSession(sessionId = "s3", owner = "alice", repo = "site", status = "completed", branchName = "agent/s3", baseBranch = "dev"),
        )
        assertEq("agent/s3", node.branchName)
        assertEq("dev", node.baseBranch)
    }

    fun testConflictedRowsAndThePullAgent() {
        val desk = UnifiedAgentRow.fromDesktop(
            LocalAgentRow("alice", "app", AgentSession(id = 9, status = "completed", branchName = "agent/9-fix", conflicted = true)),
            ControlTransport.Lan,
            "desk",
        )
        expect(desk.conflicted)
        val merged = UnifiedAgentRow.fromDesktop(
            LocalAgentRow("alice", "app", AgentSession(id = 8, status = "completed", branchName = "agent/8", conflicted = true, merged = true)),
            ControlTransport.Lan,
            "desk",
        )
        expect(!merged.conflicted)
        val node = UnifiedAgentRow.fromCloud(
            AgentLaunchSession(sessionId = "s3", owner = "alice", repo = "app", status = "completed", branchName = "agent/s3", prNumber = 42),
        )
        val rows = listOf(desk, node)
        assertEq(node, AgentSessionLookup.forPull(rows, "alice", "app", 42, "feature"))
        assertEq(desk, AgentSessionLookup.forPull(rows, "alice", "app", 7, "agent/9-fix"))
        assertEq(null, AgentSessionLookup.forPull(rows, "bob", "app", 42, ""))
        assertEq(null, AgentSessionLookup.forPull(rows, "alice", "app", 0, ""))
    }

    fun testRouteIdsNameTheUnifiedRow() {
        assertEq("desktop:12", agentRouteRowId("12"))
        assertEq("desktop:12", agentRouteRowId("desktop:12"))
        assertEq("node:s3", agentRouteRowId("s3"))
        assertEq("node:s3", agentRouteRowId("node:s3"))
        assertEq("outside:4", agentRouteRowId("outside:4"))
    }

    fun testRunnerRowsLookLikeDesktopRows() {
        val session = AgentLaunchSession(
            sessionId = "s4", owner = "alice", repo = "site", status = "completed", localAgentId = 3,
            branchName = "agent/4-fix", baseBranch = "main", runner = "fmnext-r1", prNumber = 8, canMerge = true,
            diffStats = RunnerDiffStats(files = 3, added = 1200, removed = 7, ahead = 2, behind = 0, known = true),
        )
        val row = UnifiedAgentRow.fromCloud(session)
        assertEq(AgentTransportBadge.Runner, row.badge)
        assertEq("Runner \u00B7 fmnext-r1", row.badgeLabel)
        expect(!row.isDesktop && row.isRunner && row.hasBranchOps && row.canMergeBranch)
        expect(row.canContinue && row.canPrompt)
        assertEq(3, row.filesChanged)
        assertEq(8, row.prNumber)
        val chips = AgentLineChips.chips(AgentLineChips.facts(row), 0).associate { it.slot to it.word }
        assertEq("fmnext-r1", chips[AgentChipSlot.Host])
        val listChips = AgentLineChips.chips(AgentLineChips.facts(session, cloud = true), 0).associate { it.slot to it.word }
        assertEq("fmnext-r1", listChips[AgentChipSlot.Host])
        assertEq("very-long…", AgentLineChips.runnerWord("very-long-runner-name"))
        assertEq("Runner", AgentLineChips.runnerWord(" "))
        assertEq("3", chips[AgentChipSlot.Files])
        assertEq("+1.2k\u22127", chips[AgentChipSlot.Churn])
        val running = UnifiedAgentRow.fromCloud(session.copy(status = "running"))
        expect(!running.canContinue && running.canStop && !running.canMergeBranch)
        val merged = UnifiedAgentRow.fromCloud(session.copy(merge = RunnerMergeState(state = "merged", branchDeleted = false)))
        expect(merged.merged && merged.needsCleanup && !merged.canMergeBranch)
        assertEq("Merged", AgentLineChips.chips(AgentLineChips.facts(merged), 0).first().word)
        val node = UnifiedAgentRow.fromCloud(session.copy(runner = ""))
        assertEq("Node", node.badgeLabel)
    }

    fun testCloudTranscriptKeepsStructureForTheRenderer() {
        val session = AgentLaunchSession(
            sessionId = "s5", status = "failed", lastError = "boom",
            history = listOf(
                AgentLaunchMessage(role = "user", text = "fix it\nplease"),
                AgentLaunchMessage(role = "assistant", text = "## Done\n| a | b |\n|---|---|"),
            ),
        )
        assertEq("> fix it\n> please\n\n## Done\n| a | b |\n|---|---|\n\n!! boom", cloudTranscript(session))
    }

    fun testCloudRowsCarryThePullAndCreatePr() {
        val open = UnifiedAgentRow.fromCloud(
            AgentLaunchSession(sessionId = "s1", owner = "alice", repo = "app", status = "completed", branchName = "agent/x", runner = "fmnext-r1", canOpenPull = true),
        )
        expect(open.canOpenPull && open.canCreatePull)
        assertEq(0, open.prNumber)
        val linked = UnifiedAgentRow.fromCloud(
            AgentLaunchSession(sessionId = "s2", owner = "alice", repo = "app", status = "completed", branchName = "agent/y", runner = "fmnext-r1", prNumber = 9),
        )
        assertEq(9, linked.prNumber)
        expect(!linked.canCreatePull)
        expect(!open.copy(prNumber = 3).canCreatePull)
        expect(!open.copy(isActive = true).canCreatePull)
        expect(!open.copy(merged = true).canCreatePull)
        expect(!open.copy(branchName = "").canCreatePull)
    }
}
