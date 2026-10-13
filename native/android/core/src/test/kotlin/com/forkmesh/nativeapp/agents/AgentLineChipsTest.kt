package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.control.AgentTransportBadge
import com.forkmesh.nativeapp.control.UnifiedAgentRow
import com.forkmesh.nativeapp.model.AgentSession
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class AgentLineChipsTest {
    private val now = 1_000_000_000_000L

    fun testListViewCyclesCompactZenFullAndReadsWhatWasStored() {
        assertEq(AgentListView.Zen, AgentListView.Compact.next)
        assertEq(AgentListView.Full, AgentListView.Zen.next)
        assertEq(AgentListView.Compact, AgentListView.Full.next)
        assertEq(AgentListView.Full, AgentListView.fromKey("full"))
        assertEq(AgentListView.Compact, AgentListView.fromKey(null))
        assertEq(AgentListView.Compact, AgentListView.fromKey("grid"))
        assertEq("agents/listView", AgentListView.PREF_KEY)
    }

    fun testFullChipsNameEachDetailInOneWordInColumnOrder() {
        val facts = AgentChipFacts(
            status = AgentLineStatus.Running,
            provider = "claude-code",
            updatedAtMs = now - 8 * 60_000,
            behind = 3,
            filesChanged = 4,
            linesAdded = 5285,
            linesRemoved = 74,
            branch = "agent/1",
            hasWorktree = true,
        )
        val chips = AgentLineChips.chips(facts, now)
        assertEq(
            listOf(
                AgentChipSlot.Status, AgentChipSlot.Provider, AgentChipSlot.Host, AgentChipSlot.Age,
                AgentChipSlot.Sync, AgentChipSlot.Files, AgentChipSlot.Churn, AgentChipSlot.Branch,
            ),
            chips.map { it.slot },
        )
        assertEq(
            listOf("Running", "Claude", "Desktop", "8m", "Behind", "4", "+5.2k−74", "Worktree"),
            chips.map { it.word },
        )
        for (chip in chips) expect(!chip.word.contains(' '), "one word: ${chip.word}")

        val quiet = facts.copy(status = AgentLineStatus.Success, provider = "codex", cloud = true, conflicted = true, branch = "")
        assertEq(
            listOf("Success", "Codex", "Cloud", "8m", "Conflicts", "4", "+5.2k−74"),
            AgentLineChips.chips(quiet, now).map { it.word },
        )
        assertEq("12k", AgentLineChips.compactCount(12_345))
        assertEq("999", AgentLineChips.compactCount(999))
        assertEq("120", AgentLineChips.count(120))
        assertEq("999+", AgentLineChips.count(1200))
    }

    fun testFactsReadEachRowKind() {
        val desktop = AgentLineChips.facts(
            AgentSession(id = 7, status = "running", provider = "codex", branchName = "agent/7", filesChanged = 2, createdAtMs = now),
        )
        assertEq(AgentLineStatus.Running, desktop.status)
        assertEq("agent/7", desktop.branch)
        assertEq(2, desktop.filesChanged)
        assertEq(AgentLineStatus.Merged, AgentLineChips.facts(AgentSession(id = 8, status = "completed", merged = true)).status)

        val launched = AgentLineChips.facts(AgentLaunchSession("s", status = "failed", updatedAtMs = now), cloud = true)
        assertEq(AgentLineStatus.Failed, launched.status)
        expect(launched.cloud, "a launched cloud run is on the cloud")

        val outside = AgentLineChips.facts(
            UnifiedAgentRow(
                id = "x", badge = AgentTransportBadge.DesktopLan, title = "t", repoOwner = "", repoName = "",
                node = "studio", status = "", statusLabel = "Running", isActive = true, outside = true,
            ),
        )
        assertEq(AgentLineStatus.Running, outside.status)
        assertEq("Outside", AgentLineChips.chips(outside, now).first { it.slot == AgentChipSlot.Host }.word)
    }
}
