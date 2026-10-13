package com.forkmesh.nativeapp.agentwatch

import com.forkmesh.nativeapp.control.AgentTranscriptDelta
import com.forkmesh.nativeapp.control.AgentTransportBadge
import com.forkmesh.nativeapp.control.UnifiedAgentRow
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class ControlWatchFeedsTest {
    private class FakeStream : WatchTranscriptStream {
        override var isStreaming = false
        var started = 0
        var stopped = 0

        override fun start(): Boolean {
            started++
            isStreaming = true
            return true
        }

        override fun stop() {
            stopped++
            isStreaming = false
        }
    }

    private fun row(id: String, localId: Int, badge: AgentTransportBadge = AgentTransportBadge.DesktopLan) = UnifiedAgentRow(
        id = id, badge = badge, title = id, repoOwner = "me", repoName = "app", node = "desk",
        status = "running", statusLabel = "Running", isActive = true, localId = localId,
    )

    fun testStreamsDesktopRowsAndHandsWatchWhatTheyWrote() {
        val streams = HashMap<String, FakeStream>()
        val deltas = HashMap<String, (AgentTranscriptDelta) -> Unit>()
        val ends = HashMap<String, (String) -> Unit>()
        val feeds = ControlWatchFeeds(limit = 2, starter = { it.run() }) { row, onEnded, onDelta ->
            FakeStream().also {
                streams[row.id] = it
                deltas[row.id] = onDelta
                ends[row.id] = onEnded
            }
        }
        var changes = 0
        feeds.onChange = { changes++ }
        feeds.follow(listOf(row("d1", 1), row("d2", 2), row("d3", 3), row("n1", 0, AgentTransportBadge.Node)))
        feeds.follow(listOf(row("d1", 1)))
        assertEq(listOf("d1", "d2"), streams.keys.sorted())
        assertEq(1, streams["d1"]?.started)
        assertEq(null, feeds.text("d1"))
        expect(feeds.lagging)

        deltas["d1"]!!(AgentTranscriptDelta("[Bash ls]\n", 10, reset = true, active = true, status = "running"))
        deltas["d1"]!!(AgentTranscriptDelta("[Read a]\n", 19, reset = false, active = true, status = ""))
        assertEq(ControlWatchText("[Bash ls]\n[Read a]\n", "running"), feeds.text("d1"))
        assertEq(2, changes)
        expect(feeds.streaming("d1"))
        expect(!feeds.streaming("d3"))

        ends["d2"]!!("unknown_session")
        assertEq("done", feeds.text("d2")?.status)
        expect(feeds.streaming("d2"))
        expect(feeds.lagging)

        feeds.stop()
        assertEq(1, streams["d1"]?.stopped)
        assertEq(null, feeds.text("d1"))
        deltas["d1"]!!(AgentTranscriptDelta("late", 23, reset = false, active = true, status = "running"))
        assertEq(3, changes)
    }
}
