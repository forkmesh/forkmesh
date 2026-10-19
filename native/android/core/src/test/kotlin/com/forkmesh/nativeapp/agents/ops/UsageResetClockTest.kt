package com.forkmesh.nativeapp.agents.ops

import com.forkmesh.nativeapp.testing.assertEq
import java.time.ZoneId

class UsageResetClockTest {
    private val utc = ZoneId.of("UTC")
    private val now = 1_792_404_300_000L

    private fun at(days: Long, hour: Long, minute: Long): Long {
        val midnight = now - now % 86_400_000L
        return midnight + days * 86_400_000L + hour * 3_600_000L + minute * 60_000L
    }

    fun testTodayShowsTwelveHourClock() {
        assertEq("Resets 3:45 PM", UsageFormat.resetClock(at(0, 15, 45), now, utc))
        assertEq("Resets 11:59 PM", UsageFormat.resetClock(at(0, 23, 59), now, utc))
    }

    fun testLaterThisWeekNamesTheWeekday() {
        assertEq("Resets Tue 12:00 AM", UsageFormat.resetClock(at(1, 0, 0), now, utc))
        assertEq("Resets Tue 9:00 AM", UsageFormat.resetClock(at(1, 9, 0), now, utc))
        assertEq("Resets Sun 12:30 PM", UsageFormat.resetClock(at(6, 12, 30), now, utc))
    }

    fun testAWeekOrMoreNamesTheDate() {
        assertEq("Resets Oct 26, 10:00 AM", UsageFormat.resetClock(at(7, 10, 0), now, utc))
    }

    fun testNoUpcomingResetIsEmpty() {
        assertEq("", UsageFormat.resetClock(at(0, 9, 0), now, utc))
        assertEq("", UsageFormat.resetClock(0, now, utc))
        assertEq("", UsageWindowMeter("5h", "5-hour", 10, null, UsageState.Ok).resetClockLabel(now, utc))
        assertEq("Resets 3:45 PM", UsageWindowMeter("5h", "5-hour", 10, at(0, 15, 45), UsageState.Ok).resetClockLabel(now, utc))
    }

    fun testFollowsTheViewersTimeZone() {
        val evening = at(0, 17, 30)
        assertEq("Resets Tue 1:00 AM", UsageFormat.resetClock(evening + 2 * 3_600_000L, evening, ZoneId.of("Asia/Kolkata")))
    }

    fun testRefreshAsksTheDesktopToReadAgain() {
        val channel = FakeControlChannel()
        val client = AgentOpsClient(channel)
        client.usage()
        assertEq(null, channel.params("agentops.usage")?.get("refresh"))
        client.usage(refresh = true)
        assertEq(true, channel.params("agentops.usage")?.get("refresh"))
    }
}
