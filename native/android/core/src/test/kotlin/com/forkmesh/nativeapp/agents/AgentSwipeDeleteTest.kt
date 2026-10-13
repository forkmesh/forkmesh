package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.control.AgentTransportBadge
import com.forkmesh.nativeapp.control.UnifiedAgentRow
import com.forkmesh.nativeapp.model.AgentSession
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class AgentSwipeDeleteTest {
    private val repo = Repository("alice", "app")

    private fun desktopRow(localId: Int, repoName: String = "app", active: Boolean = false, outside: Boolean = false) = UnifiedAgentRow(
        id = if (outside) "outside:$localId" else "desktop:$localId",
        badge = AgentTransportBadge.DesktopRelay,
        title = "t",
        repoOwner = "alice",
        repoName = repoName,
        node = "studio",
        status = if (active) "running" else "completed",
        statusLabel = "",
        isActive = active,
        localId = localId,
        outside = outside,
    )

    fun testEveryDesktopRowDeletesOnTheDesktop() {
        assertEq(AgentDeleteTarget.Desktop(7, "app"), AgentSwipeDelete.forRow(desktopRow(7)))
        assertEq(AgentDeleteTarget.Desktop(7, "app"), AgentSwipeDelete.forRow(desktopRow(7, active = true)))
        assertEq(null, AgentSwipeDelete.forRow(desktopRow(0)))
        val target = AgentSwipeDelete.forRow(desktopRow(7, active = true))!!
        expect(target.onDesktop && !target.needsConfirmation)
        assertEq(7, target.desktopId)
    }

    fun testOutsideRowDeletesAsAnOutsideTargetAfterConfirming() {
        val target = AgentSwipeDelete.forRow(desktopRow(-3, active = true, outside = true))
        assertEq(AgentDeleteTarget.Outside(-3, "app"), target)
        expect(target!!.onDesktop && target.needsConfirmation)
        assertEq(-3, target.desktopId)
        assertEq(null, AgentSwipeDelete.forRow(desktopRow(0, outside = true)))
    }

    fun testCloudRunNeedsOnlyASessionId() {
        val done = UnifiedAgentRow.fromCloud(AgentLaunchSession("s1", owner = "alice", repo = "app", status = "completed", runner = "r1"))
        assertEq(AgentDeleteTarget.Run(done), AgentSwipeDelete.forRow(done))
        val running = UnifiedAgentRow.fromCloud(AgentLaunchSession("s2", status = "running"))
        assertEq(AgentDeleteTarget.Run(running), AgentSwipeDelete.forRow(running))
        expect(!AgentDeleteTarget.Run(running).onDesktop && !AgentDeleteTarget.Run(running).needsConfirmation)
        val blank = UnifiedAgentRow.fromCloud(AgentLaunchSession("", status = "failed"))
        assertEq(null, AgentSwipeDelete.forRow(blank))
    }

    fun testPushedDesktopEntryDeletesWithOrWithoutALiveRow() {
        val entry = AgentEntry.Desktop(repo, AgentSession(id = 7, status = "completed"))
        assertEq(AgentDeleteTarget.Desktop(7, "App"), AgentSwipeDelete.forEntry(entry, listOf(desktopRow(7, repoName = "App"))))
        assertEq(AgentDeleteTarget.Desktop(7, "app"), AgentSwipeDelete.forEntry(entry, emptyList()))
        assertEq(AgentDeleteTarget.Desktop(7, "app"), AgentSwipeDelete.forEntry(entry, listOf(desktopRow(8))))
        assertEq(AgentDeleteTarget.Desktop(7, "app"), AgentSwipeDelete.forEntry(entry, listOf(desktopRow(7, repoName = "other"))))
        assertEq(AgentDeleteTarget.Desktop(7, "app"), AgentSwipeDelete.forEntry(entry, listOf(desktopRow(7, outside = true))))
        assertEq(AgentDeleteTarget.Desktop(7, "App"), AgentSwipeDelete.forEntry(entry, listOf(desktopRow(7, repoName = "App", active = true))))
        val running = AgentEntry.Desktop(repo, AgentSession(id = 7, status = "running"))
        assertEq(AgentDeleteTarget.Desktop(7, "app"), AgentSwipeDelete.forEntry(running, emptyList()))
        assertEq(null, AgentSwipeDelete.forEntry(AgentEntry.Desktop(repo, AgentSession(id = 0)), emptyList()))
    }

    fun testLaunchedEntryDeletesItsRunWithTheRepoFilledIn() {
        val entry = AgentEntry.Launched(repo, AgentLaunchSession("s1", status = "failed", orgScope = true), emptyList(), cloud = false)
        val target = AgentSwipeDelete.forEntry(entry, emptyList())
        expect(target is AgentDeleteTarget.Run)
        val row = (target as AgentDeleteTarget.Run).row
        assertEq("alice", row.repoOwner)
        assertEq("app", row.repoName)
        assertEq("s1", row.cloudSessionId)
        assertEq(true, row.orgScope)
        val queued = AgentEntry.Launched(repo, AgentLaunchSession("s2", status = "queued"), emptyList(), cloud = true)
        val queuedTarget = AgentSwipeDelete.forEntry(queued, emptyList())
        expect(queuedTarget is AgentDeleteTarget.Run)
        expect((queuedTarget as AgentDeleteTarget.Run).row.isActive)
    }

    fun testForgetDropsOnlyTheDeletedAgentAndEmptyRepos() {
        val other = Repository("alice", "lib")
        val groups = listOf(
            AgentRepoGroup(repo, listOf(AgentSession(id = 7), AgentSession(id = 8)), listOf(AgentLaunchSession("s1"))),
            AgentRepoGroup(other, listOf(AgentSession(id = 7))),
        )
        val desktop = AgentSwipeDelete.forget(groups, AgentDeleteTarget.Desktop(7, "APP"))
        assertEq(listOf(8), desktop.first { it.repo == repo }.sessions.map { it.id })
        assertEq(listOf(7), desktop.first { it.repo == other }.sessions.map { it.id })
        val run = UnifiedAgentRow.fromCloud(AgentLaunchSession("s1", owner = "alice", repo = "app", status = "failed"))
        val launched = AgentSwipeDelete.forget(groups, AgentDeleteTarget.Run(run))
        assertEq(0, launched.first { it.repo == repo }.launched.size)
        val emptied = AgentSwipeDelete.forget(listOf(AgentRepoGroup(other, listOf(AgentSession(id = 7)))), AgentDeleteTarget.Desktop(7, "lib"))
        assertEq(0, emptied.size)
        val outside = AgentSwipeDelete.forget(groups, AgentDeleteTarget.Outside(-7, "app"))
        assertEq(listOf(7, 8), outside.first { it.repo == repo }.sessions.map { it.id })
    }
}
