package com.forkmesh.nativeapp.surfaces

import com.forkmesh.nativeapp.model.ForkNotification
import com.forkmesh.nativeapp.navigation.ComposeTarget
import com.forkmesh.nativeapp.navigation.InboxFilter
import com.forkmesh.nativeapp.navigation.RepoRef
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class SurfacesTest {
    fun testRunStatesFromStatusWords() {
        assertEq(AgentRunState.Queued, AgentRunState.from("claimed", true))
        assertEq(AgentRunState.Running, AgentRunState.from("in_progress", true))
        assertEq(AgentRunState.Completed, AgentRunState.from("merged", false))
        assertEq(AgentRunState.Failed, AgentRunState.from("ERROR", false))
        assertEq(AgentRunState.Stopped, AgentRunState.from("canceled", false))
        assertEq(AgentRunState.Running, AgentRunState.from("mystery", true))
        assertEq(AgentRunState.NeedsApproval, AgentRunState.from("done", false, needsApproval = true))
    }

    fun testAgentsSurfaceOrdersAndCounts() {
        val runs = listOf(
            AgentRunInput("a", "Old done", "fm", "app", "done", false, updatedAtMs = 100),
            AgentRunInput("b", "", "fm", "app", "running", true, updatedAtMs = 5, canStop = true),
            AgentRunInput("c", "Desk", "fm", "app", "running", true, localId = 7, isDesktop = true, updatedAtMs = 1),
            AgentRunInput("d", "Queued\nrun", "", "solo", "queued", true, updatedAtMs = 9),
            AgentRunInput("b", "dup", "fm", "app", "running", true),
            AgentRunInput("", "no id", "fm", "app", "running", true),
        )
        val approvals = listOf(
            AgentApprovalInput("ap1", 7, "Allow `rm`?", 50),
            AgentApprovalInput("ap0", 7, "older", 10),
            AgentApprovalInput("orphan", 99, "?", 1),
        )
        val surface = AgentsSurface.build(runs, approvals)
        assertEq(listOf("c", "b", "d", "a"), surface.runs.map { it.id })
        assertEq(AgentRunState.NeedsApproval, surface.runs[0].state)
        assertEq("Allow `rm`?", surface.runs[0].detail)
        assertEq(50L, surface.runs[0].updatedAtMs)
        assertEq(Route.Approval("ap1"), surface.runs[0].route)
        assertEq("Agent run", surface.runs[1].title)
        assertEq(true, surface.runs[1].canStop)
        assertEq("Queued run", surface.runs[2].title)
        assertEq("solo", surface.runs[2].repo)
        assertEq(3, surface.needsYou)
        assertEq(1, surface.running)
        assertEq(1, surface.queued)
        assertEq("3 need you, 1 running, 1 queued", surface.summaryLine)
        assertEq("No agents running", AgentsSurface.empty.summaryLine)
        assertEq(2, AgentsSurface.build(runs, approvals, limit = 2).runs.size)
        assertEq(Route.Agent("a"), surface.runs[3].route)
    }

    fun testInboxSurfaceTakesUnreadNewestFirst() {
        val notes = listOf(
            ForkNotification("1", "mention", "Hi", repo = "fm/app", href = "/fm/app/issues/2", ts = 1),
            ForkNotification("2", "code_push", "", ts = 3),
            ForkNotification("3", "mention", "read", ts = 9, readAt = 5),
        )
        val surface = InboxSurface.build(7, notes)
        assertEq(7, surface.unread)
        assertEq(listOf("2", "1"), surface.items.map { it.id })
        assertEq("Code push", surface.items[0].title)
        assertEq(Route.Inbox(InboxFilter.All), surface.items[0].destination)
        assertEq(Route.Issue(RepoRef("fm", "app"), 2), surface.items[1].destination)
        assertEq(0, InboxSurface.build(-3, emptyList()).unread)
    }

    fun testSnapshotRoundTripsAndReportsChangedSections() {
        val agents = AgentsSurface.build(
            listOf(AgentRunInput("a", "Fix", "fm", "app", "running", true, route = Route.LaunchedAgent(RepoRef("fm", "app"), "3"))),
            emptyList(),
        )
        val inbox = InboxSurface.build(1, listOf(ForkNotification("1", "mention", "Hi", href = "/fm/app/pulls/4", ts = 1)))
        val snapshot = SurfaceSnapshot(123, true, agents, inbox)
        val decoded = SurfaceSnapshot.decode(snapshot.encode())
        assertEq(snapshot, decoded)
        assertEq(emptySet<SurfaceSection>(), snapshot.changedSections(decoded))
        assertEq(setOf(SurfaceSection.Inbox), snapshot.copy(inbox = InboxSurface.empty).changedSections(snapshot))
        assertEq(SurfaceSection.entries.toSet(), SurfaceSnapshot.signedOut(1).changedSections(snapshot))
        assertEq(SurfaceSection.entries.toSet(), snapshot.changedSections(null))
        assertEq(null, SurfaceSnapshot.decode("{\"version\":2}"))
        assertEq(null, SurfaceSnapshot.decode("garbage"))
    }

    fun testSharedContentBuildsAPrompt() {
        val shared = SharedContent.from("Look at https://example.com/a. And ftp://x", subject = "Bug", repo = "fm/app")!!
        assertEq(listOf("https://example.com/a"), shared.urls)
        assertEq("Bug\n\nLook at https://example.com/a. And ftp://x", shared.prompt)
        assertEq(ComposeTarget(RepoRef("fm", "app"), text = shared.prompt), shared.composeTarget)
        val link = SharedContent.from("https://example.com/page")!!
        assertEq("", link.text)
        assertEq("https://example.com/page", link.prompt)
        assertEq(null, SharedContent.from("  ", null))
        assertEq(listOf("https://a.com", "http://b.org/x"), SharedContent.cleanUrls(listOf(" https://a.com ", "https://a.com", "javascript:x", "http://b.org/x", "https://")))
        expect(SharedContent.from("x".repeat(30_000))!!.text.length == SharedContent.MAX_TEXT_LENGTH)
    }

    fun testClip() {
        assertEq("a b", SurfaceText.clip(" a\nb ", 10))
        assertEq("abcd…", SurfaceText.clip("abcdefgh", 5))
    }
}
