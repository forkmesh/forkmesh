package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.agentwatch.ControlWatchFeeds
import com.forkmesh.nativeapp.control.AgentTranscriptDelta
import com.forkmesh.nativeapp.control.AgentTransportBadge
import com.forkmesh.nativeapp.control.UnifiedAgentRow
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class AgentEagleFeedTest {
    private fun row(id: String, localId: Int, repo: String = "forkmesh", active: Boolean = true) = UnifiedAgentRow(
        id = id, badge = AgentTransportBadge.DesktopLan, title = id, repoOwner = "forkmesh", repoName = repo, node = "studio",
        status = if (active) "running" else "done", statusLabel = if (active) "Running" else "Done", isActive = active, localId = localId,
    )

    fun testDialogIsTheLatestFinishedProseTheAgentSaid() {
        val transcript = "Looking at the **list** view.\n\n[Read AgentLineRow.swift]\n\nNow wiring the `ticker`\nacross the card.\n\nhalf a li"
        assertEq("Now wiring the ticker across the card.", AgentEagle.dialog(transcript))
        assertEq("Done.", AgentEagle.dialog("Done.\n\n[Read a.swift]\n"))
        assertEq("Checked the list", AgentEagle.dialog("- Checked the **list**\n\n[Bash swift test]\n!! exit 1\n==> Task done: build\n"))
        assertEq("", AgentEagle.dialog("> fix it\n\n[Bash swift test]\n\n```\nlet a = 1\n```\n"))
        assertEq("", AgentEagle.dialog("still typing"))
        assertEq("", AgentEagle.dialog(""))
        assertEq("word word word word…", AgentEagle.dialog("word ".repeat(60) + "\n", limit = 20))
    }

    fun testDialogOfACloudRunPrefersItsLiveOutputAndKeepsItsLastWordAfter() {
        val said = AgentLaunchMessage(role = "assistant", text = "Opened the pull request.")
        var session = AgentLaunchSession(sessionId = "s1", status = "running", history = listOf(AgentLaunchMessage(role = "user", text = "fix it"), said))
        assertEq("Opened the pull request.", AgentEagle.dialog(session))
        session = session.copy(liveText = "Running the tests\n")
        assertEq("Running the tests", AgentEagle.dialog(session))
        session = session.copy(status = "completed")
        assertEq("Opened the pull request.", AgentEagle.dialog(session))
    }

    fun testListViewDefaultsToEagleOnce() {
        assertEq(AgentListView.Eagle, AgentListView.Full.next)
        assertEq(AgentListView.Compact, AgentListView.Eagle.next)
        assertEq(AgentListView.Eagle, AgentListView.fromKey(null))
        assertEq(AgentListView.Eagle, AgentListView.fromKey("grid"))
        assertEq(AgentListView.Eagle, AgentListView.stored("zen", eagleDefaulted = false))
        assertEq(AgentListView.Zen, AgentListView.stored("zen", eagleDefaulted = true))
    }

    fun testHandsEachRunningDesktopRowItsLatestLine() {
        val deltas = HashMap<String, (AgentTranscriptDelta) -> Unit>()
        val feeds = ControlWatchFeeds(starter = { it.run() }) { row, _, onDelta ->
            deltas[row.id] = onDelta
            null
        }
        val live = row("desktop:7", 7, repo = "ForkMesh")
        val idle = row("desktop:9", 9, active = false)
        val feed = AgentEagleFeed()
        var changes = 0
        feed.onChange = { changes++ }
        feed.follow(listOf(live, idle), { feeds })
        assertEq(listOf("desktop:7"), deltas.keys.sorted())
        deltas["desktop:7"]!!(AgentTranscriptDelta("Running the tests.\n\n[Bash swift test]\n", 38, reset = true, active = true, status = "running"))
        assertEq("desktop:7", feed.desktopRowId("forkmesh", 7))
        assertEq("Running the tests.", feed.dialog("desktop:7"))
        assertEq("", feed.dialog("desktop:9"))
        assertEq(1, changes)
        val tools = "[Bash swift test]\n\n".repeat(600)
        deltas["desktop:7"]!!(AgentTranscriptDelta(tools, 38 + tools.length, reset = false, active = true, status = "running"))
        assertEq("Running the tests.", feed.dialog("desktop:7"))
        assertEq(1, changes)
        feed.stop()
        assertEq("", feed.dialog("desktop:7"))
        assertEq(null, feed.desktopRowId("forkmesh", 7))
    }

    fun testKeepsTheLastLineOfAStoppedRunAndRecallsEachStoppedRowOnce() {
        val deltas = HashMap<String, (AgentTranscriptDelta) -> Unit>()
        val feeds = ControlWatchFeeds(starter = { it.run() }) { row, _, onDelta ->
            deltas[row.id] = onDelta
            null
        }
        val live = row("desktop:7", 7)
        val done = live.copy(isActive = false, status = "done")
        val idle = row("desktop:9", 9, active = false)
        val fetched = ArrayList<String>()
        val feed = AgentEagleFeed(runner = { it.run() })
        feed.follow(listOf(live, idle), { feeds }) { r ->
            fetched += r.id
            "> fix it\n\nShipped the fix.\n\n[Bash swift test]\n"
        }
        deltas["desktop:7"]!!(AgentTranscriptDelta("Running the tests.\n\n[Bash swift test]\n", 38, reset = true, active = true, status = "running"))
        feed.follow(listOf(done, idle), { feeds })
        assertEq("Running the tests.", feed.dialog("desktop:7"))
        feed.recall("desktop:7", active = false)
        feed.recall("desktop:9", active = true)
        expect(fetched.isEmpty())
        feed.recall("desktop:9", active = false)
        feed.recall("desktop:9", active = false)
        assertEq("Shipped the fix.", feed.dialog("desktop:9"))
        assertEq(listOf("desktop:9"), fetched)
        feed.recall("desktop:11", active = false)
        feed.recall("desktop:11", active = false, row = row("desktop:11", 11, active = false))
        assertEq(listOf("desktop:9", "desktop:11"), fetched)
        feed.stop()
    }

    fun testLaunchedRowsKeepTheirLastWord() {
        val feed = AgentEagleFeed()
        val running = AgentLaunchSession(sessionId = "s1", status = "running", liveText = "Reading the code\n")
        assertEq("Reading the code", feed.launchedDialog("s1", running))
        assertEq("Reading the code", feed.launchedDialog("s1", running.copy(liveText = "")))
    }
}
