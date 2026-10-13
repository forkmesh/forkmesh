package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.control.AgentTransportBadge
import com.forkmesh.nativeapp.control.UnifiedAgentRow
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.model.AgentSession
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class AgentReadStateTest {
    private fun desktopRow(
        localId: Int,
        finishedAtMs: Long,
        unread: Boolean? = null,
        active: Boolean = false,
        outside: Boolean = false,
    ) = UnifiedAgentRow(
        id = if (outside) "outside:$localId" else "desktop:$localId",
        badge = AgentTransportBadge.DesktopLan,
        title = "t",
        repoOwner = "alice",
        repoName = "App",
        node = "studio",
        status = if (active) "running" else "done",
        statusLabel = "",
        isActive = active,
        localId = localId,
        outside = outside,
        finishedAtMs = finishedAtMs,
        unread = unread,
    )

    fun testFinishAfterTheBaselineIsUnreadUntilOpened() {
        val state = AgentReadState.load(0, null, nowMs = 1000)
        assertEq(1000L, state.baselineMs)
        val facts = AgentReadFacts("run:a", finishedAtMs = 2000, active = false)
        expect(state.isUnread(facts))
        val opened = state.markRead(facts)
        expect(!opened.isUnread(facts))
        assertEq(2000L, opened.seen["run:a"])
        expect(opened.isUnread(facts.copy(finishedAtMs = 3000)))
    }

    fun testFinishBeforeTheBaselineCountsAsRead() {
        val state = AgentReadState.load(5000, null, nowMs = 9000)
        assertEq(5000L, state.baselineMs)
        expect(!state.isUnread(AgentReadFacts("run:a", finishedAtMs = 4000, active = false)))
        expect(!state.isUnread(AgentReadFacts("run:a", finishedAtMs = 5000, active = false)))
        expect(state.isUnread(AgentReadFacts("run:a", finishedAtMs = 5001, active = false)))
    }

    fun testRunningOrUnfinishedAgentsAreNotUnread() {
        val state = AgentReadState(baselineMs = 0)
        expect(!state.isUnread(AgentReadFacts("run:a", finishedAtMs = 2000, active = true)))
        expect(!state.isUnread(AgentReadFacts("run:a", finishedAtMs = 0, active = false)))
        expect(!state.isUnread(AgentReadFacts("", finishedAtMs = 2000, active = false)))
        expect(!state.isUnread(null))
    }

    fun testOpeningARunningAgentDoesNotHideItsNextFinish() {
        val running = AgentReadFacts("run:a", finishedAtMs = 0, active = true)
        val state = AgentReadState(baselineMs = 0).markRead(running)
        assertEq(0L, state.seen["run:a"])
        expect(state.isUnread(AgentReadFacts("run:a", finishedAtMs = 10, active = false)))
        assertEq(state, state.markRead(running))
    }

    fun testDesktopFlagWinsUnlessThePhoneOpenedThatFinish() {
        val state = AgentReadState(baselineMs = 10_000)
        val flagged = AgentReadState.facts(desktopRow(7, finishedAtMs = 2000, unread = true))!!
        assertEq("desktop:app#7", flagged.key)
        assertEq(7, flagged.desktopId)
        expect(state.isUnread(flagged))
        val opened = state.markRead(flagged)
        expect(!opened.isUnread(flagged))
        expect(opened.isUnread(AgentReadState.facts(desktopRow(7, finishedAtMs = 3000, unread = true))))
        val cleared = AgentReadState.facts(desktopRow(7, finishedAtMs = 20_000, unread = false))
        expect(!state.isUnread(cleared))
    }

    fun testOlderDesktopWithoutTheFlagUsesThePhoneRule() {
        val state = AgentReadState(baselineMs = 1000)
        expect(state.isUnread(AgentReadState.facts(desktopRow(7, finishedAtMs = 2000))))
        expect(!state.isUnread(AgentReadState.facts(desktopRow(7, finishedAtMs = 500))))
        expect(!state.isUnread(AgentReadState.facts(desktopRow(7, finishedAtMs = 0, active = true))))
        assertEq(null, AgentReadState.facts(desktopRow(-3, finishedAtMs = 2000, outside = true)))
        assertEq(null, AgentReadState.facts(desktopRow(0, finishedAtMs = 2000)))
    }

    fun testPushedDesktopSessionSharesTheLiveRowKey() {
        val session = AgentSession(id = 7, status = "done", finishedAtMs = 2000)
        val pushed = AgentReadState.facts("app", session, null)!!
        assertEq("desktop:app#7", pushed.key)
        assertEq(null, pushed.desktopUnread)
        assertEq(2000L, pushed.finishedAtMs)
        val live = AgentReadState.facts("app", session, desktopRow(7, finishedAtMs = 2500, unread = true))!!
        assertEq(pushed.key, live.key)
        assertEq(true, live.desktopUnread)
        assertEq(2500L, live.finishedAtMs)
        assertEq(0L, AgentReadState.facts("app", session.copy(status = "running"), null)!!.finishedAtMs)
    }

    fun testCloudRunsUseTheirSessionIdAndCompletion() {
        val done = AgentLaunchSession("s1", status = "completed", updatedAtMs = 900, completedAtMs = 800)
        val facts = AgentReadState.facts(done)!!
        assertEq("run:s1", facts.key)
        assertEq(800L, facts.finishedAtMs)
        assertEq(0, facts.desktopId)
        assertEq(900L, AgentReadState.runFinishedAt(done.copy(completedAtMs = 0)))
        assertEq(0L, AgentReadState.runFinishedAt(done.copy(status = "running")))
        assertEq(null, AgentReadState.facts(AgentLaunchSession("", status = "completed")))
        val row = UnifiedAgentRow.fromCloud(done)
        assertEq(800L, row.finishedAtMs)
        assertEq(facts, AgentReadState.facts(row))
    }

    fun testSeenStampsRoundTripThroughJson() {
        val state = AgentReadState(1, mapOf("run:a" to 5L, "desktop:app#7" to 9L))
        val text = state.encodeSeen()
        assertEq(state.seen, AgentReadState.decodeSeen(text))
        assertEq(state.seen, AgentReadState.load(1, text, 99).seen)
        assertEq(emptyMap<String, Long>(), AgentReadState.decodeSeen(null))
        assertEq(emptyMap<String, Long>(), AgentReadState.decodeSeen("not json"))
        assertEq(mapOf("ok" to 3L), AgentReadState.decodeSeen("""{"ok":3,"bad":"x","":4,"neg":-2}""").filterKeys { it == "ok" })
        assertEq(0L, AgentReadState.decodeSeen("""{"neg":-2}""")["neg"])
    }

    fun testSeenStampsStayBounded() {
        var state = AgentReadState(baselineMs = 0)
        for (i in 1..(AgentReadState.MAX_ENTRIES + 50)) {
            state = state.markRead(AgentReadFacts("run:$i", finishedAtMs = i.toLong(), active = false))
        }
        assertEq(AgentReadState.MAX_ENTRIES, state.seen.size)
        expect("run:1" !in state.seen)
        expect("run:${AgentReadState.MAX_ENTRIES + 50}" in state.seen)
        val big = (1..(AgentReadState.MAX_ENTRIES + 10)).associate { "k$it" to it.toLong() }
        assertEq(AgentReadState.MAX_ENTRIES, AgentReadState.decodeSeen(Json.stringify(big)).size)
    }
}
