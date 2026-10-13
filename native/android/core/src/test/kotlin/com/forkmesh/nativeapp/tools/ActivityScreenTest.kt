package com.forkmesh.nativeapp.tools

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.DartException
import com.forkmesh.nativeapp.api.TestSettings
import com.forkmesh.nativeapp.model.FundsReceivedEntry
import com.forkmesh.nativeapp.model.NetworkLeaderboards
import com.forkmesh.nativeapp.model.NetworkStats
import com.forkmesh.nativeapp.model.PayoutNode
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class ActivityScreenTest {
    private class RetryingActivityApi : ApiService(TestSettings()) {
        var statsCalls = 0

        override fun networkStats(): NetworkStats {
            statsCalls += 1
            if (statsCalls == 1) throw DartException("network unavailable")
            return NetworkStats(nodesOnline = 2, hostsOnline = 1, repos = 3)
        }

        override fun networkLeaderboards(): NetworkLeaderboards = NetworkLeaderboards()
    }

    private class RetryingLeaderboardApi : ApiService(TestSettings()) {
        var leaderboardCalls = 0

        override fun networkStats(): NetworkStats = NetworkStats(nodesOnline = 2, hostsOnline = 1, repos = 3)

        override fun networkLeaderboards(): NetworkLeaderboards {
            leaderboardCalls += 1
            if (leaderboardCalls == 1) throw DartException("leaderboards unavailable")
            return NetworkLeaderboards(fundsMainnodes = listOf(FundsReceivedEntry(name = "mainnode-a", lamports = 42)))
        }
    }

    fun testNetworkActivityShowsAnErrorAndCanRetry() {
        val api = RetryingActivityApi()

        val first = ActivityRules.loadStats(api)
        assertEq(LoadState.Failed("Exception: network unavailable"), first)
        assertEq("Could not load network activity", ActivityRules.NETWORK_ERROR_TITLE)
        assertEq("Retry network activity", ActivityRules.RETRY_TOOLTIP)
        expect(ActivityRules.loadLeaderboards(api) is LoadState.Loaded)

        val retried = ActivityRules.loadStats(api)
        assertEq(2, api.statsCalls)
        val stats = (retried as LoadState.Loaded).value
        assertEq(2, stats.nodesOnline)
        assertEq(1, stats.hostsOnline)
        assertEq(3, stats.repos)
    }

    fun testLeaderboardActivityShowsAnErrorAndCanRetry() {
        val api = RetryingLeaderboardApi()

        assertEq(LoadState.Failed("Exception: leaderboards unavailable"), ActivityRules.loadLeaderboards(api))
        assertEq("Could not load funds received", ActivityRules.FUNDS_ERROR_TITLE)

        val boards = (ActivityRules.loadLeaderboards(api) as LoadState.Loaded).value
        assertEq(2, api.leaderboardCalls)
        expect(!boards.isEmpty)
        val entry = ActivityRules.visibleFunds(boards.fundsMainnodes).single()
        assertEq("mainnode-a", ActivityRules.fundsName(entry))
        assertEq("42 lamports", entry.amountLabel)
        assertEq(0, ActivityRules.visibleFunds(boards.fundsContributors).size)
    }

    fun testTheActivityLogShowsTheNewestTwelveUntilExpanded() {
        val log = (1..15).map { "12:00:%02d  event $it".format(it) }
        val collapsed = ActivityRules.visibleLogs(log, showAll = false)
        assertEq(12, collapsed.size)
        assertEq("12:00:15  event 15", collapsed.first())
        assertEq(15, ActivityRules.visibleLogs(log, showAll = true).size)
        expect(ActivityRules.hasMoreLogs(log))
        expect(!ActivityRules.hasMoreLogs(log.take(12)))
        assertEq("Show all 15 logs", ActivityRules.toggleLabel(showAll = false, count = 15))
        assertEq("Show less", ActivityRules.toggleLabel(showAll = true, count = 15))
        assertEq("15 events", ActivityRules.eventsLabel(15))
    }

    fun testLogRowsSplitTheStampAndPickATone() {
        assertEq(
            ActivityLogLine("12:00:01", "Connected to wss://relay.test/ws", ActivityLogLine.Tone.Connected),
            ActivityLogLine.parse("12:00:01  Connected to wss://relay.test/ws"),
        )
        assertEq(ActivityLogLine.Tone.Failure, ActivityLogLine.parse("12:00:02  Connect failed: timeout").tone)
        assertEq(ActivityLogLine.Tone.Failure, ActivityLogLine.parse("12:00:03  Decrypt error").tone)
        assertEq(ActivityLogLine.Tone.Left, ActivityLogLine.parse("12:00:04  alice left #general").tone)
        assertEq(ActivityLogLine.Tone.Event, ActivityLogLine.parse("12:00:05  bob joined").tone)
        assertEq(ActivityLogLine("", "short", ActivityLogLine.Tone.Event), ActivityLogLine.parse("short"))
    }

    fun testPayoutRowsListWhatIsKnown() {
        val node = PayoutNode(
            name = "",
            wallet = "ABCDEFGHIJKLMNOPQRSTUVWX",
            balanceLamports = 5,
            payoutEligible = true,
            relay = " relay.test ",
        )
        assertEq("Unnamed node", ActivityRules.nodeName(node))
        assertEq(listOf("eligible", "ABCDEF...STUVWX", "5 lamports", "relay.test"), ActivityRules.payoutDetails(node))
        assertEq(listOf("not eligible"), ActivityRules.payoutDetails(PayoutNode(name = "n1")))
        val nodes = (1..7).map { PayoutNode(name = "node-$it") }
        assertEq(5, ActivityRules.visiblePayoutNodes(nodes).size)
        assertEq("+2 more nodes", ActivityRules.moreNodesLabel(nodes))
        assertEq(null, ActivityRules.moreNodesLabel(nodes.take(5)))
    }

    fun testFundsGroupsShowTheirTopThree() {
        val entries = (1..5).map { FundsReceivedEntry(name = if (it == 2) "" else "p$it", lamports = it.toLong()) }
        val visible = ActivityRules.visibleFunds(entries)
        assertEq(3, visible.size)
        assertEq("Unnamed", ActivityRules.fundsName(visible[1]))
    }
}
